/*
 * ============================================================================
 *  AD2S1210 <-> ESP32 (WeMos LOLIN32 V1.0.0) driver
 * ============================================================================
 *  Talks to the AD2S1210 resolver-to-digital converter on the
 *  EVAL-AD2S1210SDZ board via its J4 SERIAL interface connector.
 *
 *  Exposes read/write access to every user-accessible register:
 *    - Position (read)
 *    - Velocity (read)
 *    - Fault register (read + clear)
 *    - LOS / DOS overrange / DOS mismatch / DOS reset max / DOS reset min
 *      threshold registers (read/write)
 *    - LOT high / LOT low threshold registers (read/write)
 *    - Excitation frequency register (read/write)
 *    - Control register: resolution, hysteresis, phase-lock range,
 *      encoder-output resolution (read/write)
 *    - Software reset
 *
 *  Register map, protocol timing, and formulas are taken from the AD2S1210
 *  Rev. B datasheet ("Register Map" and "Serial Interface" sections).
 *
 * ----------------------------------------------------------------------------
 *  WIRING  (LOLIN32  ->  AD2S1210 / J4 on EVAL-AD2S1210SDZ)
 * ----------------------------------------------------------------------------
 *  LOLIN32 GPIO18  -> SCLK
 *  LOLIN32 GPIO23  -> SDI   (MOSI, ESP32 -> AD2S1210)
 *  LOLIN32 GPIO19  -> SDO   (MISO, AD2S1210 -> ESP32)
 *  LOLIN32 GPIO5   -> WR/FSYNC   (acts as SPI frame/chip-select)
 *  LOLIN32 GPIO4   -> SAMPLE
 *  LOLIN32 GPIO17  -> A0
 *  LOLIN32 GPIO16  -> A1
 *  LOLIN32 GPIO32  -> RESET   (active low)
 *  LOLIN32 GPIO35  -> LOT     (input only, AD2S1210 output, active low)
 *  LOLIN32 GPIO39  -> DOS     (input only, AD2S1210 output, active low)
 *
 *  Static ties (wire directly, not to the ESP32):
 *    AD2S1210 CS   -> GND (native chip-select unused in this mode)
 *    AD2S1210 SOE  -> GND (forces serial interface)
 *    AD2S1210 RD   -> VDRIVE (must be held high while SOE is low)
 *    AD2S1210 RES0/RES1 -> tie to match the resolution you configure
 *                           below (see RESOLUTION_BITS), OR wire to spare
 *                           GPIOs if you want to change resolution at
 *                           runtime via hardware pins as well.
 *
 *  Power:
 *    LOLIN32 3V3 -> AD2S1210 VDRIVE (via J704) so the serial interface
 *                   runs at 3.3 V logic - no level shifters needed.
 *    LOLIN32 GND -> common ground with the eval board.
 *    AVDD/DVDD (5 V) and the excitation drivers are powered from the
 *    eval board's own supply, NOT from the ESP32.
 *
 *  IMPORTANT: verify CLKIN_HZ below against the actual crystal/oscillator
 *  fitted to the eval board -- the excitation-frequency and velocity-
 *  scaling maths both depend on it.
 * ============================================================================
 */

#include <Arduino.h>   // required in PlatformIO .cpp files (implicit in Arduino IDE .ino files)
#include <SPI.h>

// ---------------------------------------------------------------------------
// Pin assignments
// ---------------------------------------------------------------------------
static const int PIN_SCLK    = 18;
static const int PIN_MOSI    = 23;   // SDI
static const int PIN_MISO    = 19;   // SDO
static const int PIN_WRFSYNC = 5;    // frame sync / chip select
static const int PIN_SAMPLE  = 4;
static const int PIN_A0      = 17;
static const int PIN_A1      = 16;
static const int PIN_RESET   = 32;
static const int PIN_LOT     = 35;   // input only
static const int PIN_DOS     = 39;   // input only

// ---------------------------------------------------------------------------
// Board configuration -- EDIT THESE to match your hardware
// ---------------------------------------------------------------------------
static const uint32_t CLKIN_HZ        = 8192000UL;  // verify against board crystal
static const uint8_t  RESOLUTION_BITS = 16;          // 10, 12, 14, or 16
static const uint32_t SPI_CLOCK_HZ    = 2000000UL;   // conservative; datasheet allows up to 25 MHz @5V VDRIVE, less at 3.3V

// ---------------------------------------------------------------------------
// Register addresses (AD2S1210 datasheet, Table 10)
// ---------------------------------------------------------------------------
enum AD2S1210_Reg : uint8_t {
  REG_POSITION_MSB   = 0x80,
  REG_POSITION_LSB   = 0x81,
  REG_VELOCITY_MSB   = 0x82,
  REG_VELOCITY_LSB   = 0x83,
  REG_LOS_THRESH     = 0x88,
  REG_DOS_OVER_THRESH= 0x89,
  REG_DOS_MISM_THRESH= 0x8A,
  REG_DOS_RST_MAX    = 0x8B,
  REG_DOS_RST_MIN    = 0x8C,
  REG_LOT_HIGH       = 0x8D,
  REG_LOT_LOW        = 0x8E,
  REG_EXC_FREQ       = 0x91,
  REG_CONTROL        = 0x92,
  REG_SOFT_RESET     = 0xF0,
  REG_FAULT          = 0xFF
};

// Control register bit positions (within the 7 meaningful bits, D6..D0)
static const uint8_t CTRL_RESERVED_D6   = (1 << 6); // must always be 1
static const uint8_t CTRL_PHASELOCK_D5  = (1 << 5); // 0 = +/-360deg, 1 = +/-44deg
static const uint8_t CTRL_HYSTERESIS_D4 = (1 << 4); // 1 = enabled
static const uint8_t CTRL_ENRES1_D3     = (1 << 3);
static const uint8_t CTRL_ENRES0_D2     = (1 << 2);
static const uint8_t CTRL_RES1_D1       = (1 << 1);
static const uint8_t CTRL_RES0_D0       = (1 << 0);

// ---------------------------------------------------------------------------
// Low level SPI framing
//   WR/FSYNC low = one framed transfer (same shape as an SPI CS pulse).
//   Mode: SCLK idles low, SDO changes on rising edge / SDI sampled on
//   falling edge  -> SPI_MODE1.
// ---------------------------------------------------------------------------
SPIClass vspi(VSPI);
static SPISettings spiSettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE1);

static inline void frameLow()  { digitalWrite(PIN_WRFSYNC, LOW); }
static inline void frameHigh() { digitalWrite(PIN_WRFSYNC, HIGH); delayMicroseconds(2); }

// One 8-bit transfer inside its own WR/FSYNC frame.
static uint8_t spiFrame8(uint8_t out) {
  vspi.beginTransaction(spiSettings);
  frameLow();
  uint8_t in = vspi.transfer(out);
  frameHigh();
  vspi.endTransaction();
  return in;
}

// A single WR/FSYNC frame containing multiple bytes back-to-back
// (used for the 24-bit normal-mode position/velocity+fault read).
static void spiFrameN(uint8_t *buf, size_t n) {
  vspi.beginTransaction(spiSettings);
  frameLow();
  for (size_t i = 0; i < n; i++) buf[i] = vspi.transfer(buf[i]);
  frameHigh();
  vspi.endTransaction();
}

// ---------------------------------------------------------------------------
// Mode selection (A0 / A1)
// ---------------------------------------------------------------------------
static inline void modeConfig()          { digitalWrite(PIN_A1, HIGH); digitalWrite(PIN_A0, HIGH); }
static inline void modeNormalPosition()  { digitalWrite(PIN_A1, LOW);  digitalWrite(PIN_A0, LOW);  }
static inline void modeNormalVelocity()  { digitalWrite(PIN_A1, LOW);  digitalWrite(PIN_A0, HIGH); }

// SAMPLE pulse: latches position/velocity/fault into the output registers.
static void pulseSample() {
  digitalWrite(PIN_SAMPLE, LOW);
  delayMicroseconds(5);   // >> t16 (2*tCK + 20ns, a few hundred ns @8.192MHz)
  digitalWrite(PIN_SAMPLE, HIGH);
  delayMicroseconds(5);   // >> t17
}

// ---------------------------------------------------------------------------
// Configuration-mode register access
// ---------------------------------------------------------------------------

// Write one byte of data to a register.
void writeRegister(uint8_t addr, uint8_t data) {
  modeConfig();
  spiFrame8(addr);          // address phase (MSB=1, i.e. addr as given, e.g. 0x92)
  spiFrame8(data & 0x7F);   // data phase (MSB forced 0; chip computes parity itself)
}

// Read one byte of data back from a register.
// Returns the 7 data bits (D6..D0); sets *errorFlag if the readback parity
// (D7) indicated a mismatch with the last value written.
uint8_t readRegister(uint8_t addr, bool *errorFlag = nullptr) {
  modeConfig();
  spiFrame8(addr);              // address phase
  uint8_t raw = spiFrame8(0x00); // data phase (dummy out byte)
  if (errorFlag) *errorFlag = (raw & 0x80) != 0;
  return raw & 0x7F;
}

// Initiate a software reset (address-only write, no data phase).
void softReset() {
  modeConfig();
  spiFrame8(REG_SOFT_RESET);
  // Allow the tracking loop to resettle; worst case (16-bit) is ~60 ms.
  delay(70);
}

// ---------------------------------------------------------------------------
// Normal-mode position / velocity (+ fault) read
// ---------------------------------------------------------------------------
struct SampleResult {
  uint16_t raw;     // raw 16-bit position or velocity code
  uint8_t  fault;   // fault register snapshot taken at the same SAMPLE edge
};

SampleResult readPosition() {
  modeNormalPosition();
  pulseSample();
  uint8_t buf[3] = {0, 0, 0};
  spiFrameN(buf, 3);
  SampleResult r;
  r.raw   = ((uint16_t)buf[0] << 8) | buf[1];
  r.fault = buf[2];
  return r;
}

SampleResult readVelocity() {
  modeNormalVelocity();
  pulseSample();
  uint8_t buf[3] = {0, 0, 0};
  spiFrameN(buf, 3);
  SampleResult r;
  r.raw   = ((uint16_t)buf[0] << 8) | buf[1];
  r.fault = buf[2];
  return r;
}

double positionToDegrees(uint16_t raw) {
  return (raw * 360.0) / 65536.0;
}

// Max tracking rate (rps) by resolution @ CLKIN = 8.192 MHz (datasheet Table 1),
// scaled linearly to the actual CLKIN in use.
double velocityToRPS(int16_t rawSigned, uint8_t resolutionBits) {
  double maxRpsAt8192k;
  switch (resolutionBits) {
    case 10: maxRpsAt8192k = 2500.0; break;
    case 12: maxRpsAt8192k = 1000.0; break;
    case 14: maxRpsAt8192k = 500.0;  break;
    case 16: maxRpsAt8192k = 125.0;  break;
    default: maxRpsAt8192k = 125.0;  break;
  }
  double maxRps = maxRpsAt8192k * (CLKIN_HZ / 8192000.0);
  return (rawSigned / 32768.0) * maxRps;
}

// ---------------------------------------------------------------------------
// Fault register
// ---------------------------------------------------------------------------
struct FaultBits {
  bool sinCosClipped;
  bool losThreshold;
  bool dosOverrange;
  bool dosMismatch;
  bool lotThreshold;
  bool velocityOverrange;
  bool phaseLockError;
  bool configParityError;
};

FaultBits decodeFault(uint8_t f) {
  FaultBits b;
  b.sinCosClipped      = f & (1 << 7);
  b.losThreshold        = f & (1 << 6);
  b.dosOverrange         = f & (1 << 5);
  b.dosMismatch          = f & (1 << 4);
  b.lotThreshold          = f & (1 << 3);
  b.velocityOverrange     = f & (1 << 2);
  b.phaseLockError         = f & (1 << 1);
  b.configParityError       = f & (1 << 0);
  return b;
}

void printFault(uint8_t f) {
  FaultBits b = decodeFault(f);
  Serial.printf("  Fault byte: 0x%02X\n", f);
  if (f == 0) { Serial.println("  (no faults)"); return; }
  if (b.sinCosClipped)      Serial.println("  - Sine/cosine input clipped");
  if (b.losThreshold)        Serial.println("  - Sine/cosine below LOS threshold");
  if (b.dosOverrange)         Serial.println("  - Sine/cosine exceeds DOS overrange threshold");
  if (b.dosMismatch)           Serial.println("  - Sine/cosine amplitude mismatch (DOS)");
  if (b.lotThreshold)           Serial.println("  - Tracking error exceeds LOT threshold");
  if (b.velocityOverrange)       Serial.println("  - Velocity exceeds maximum tracking rate");
  if (b.phaseLockError)           Serial.println("  - Phase error exceeds phase-lock range");
  if (b.configParityError)         Serial.println("  - Configuration register parity error");
}

// Full read-and-clear sequence per datasheet "Clearing the Fault Register".
uint8_t readAndClearFault() {
  pulseSample();                       // 1-2: latch current fault state
  bool err;
  uint8_t f = readRegister(REG_FAULT, &err); // 3-4: enter config mode, read fault reg
  pulseSample();                       // 5: second SAMPLE edge clears DOS/LOT latches
  return f;
}

// ---------------------------------------------------------------------------
// Excitation frequency register  (Address 0x91)
//   Excitation Frequency = fCW * CLKIN / 2^15,  fCW in [0x04, 0x50]
// ---------------------------------------------------------------------------
void setExcitationFrequency(double hz) {
  double fCWf = hz * 32768.0 / CLKIN_HZ;
  uint8_t fCW = (uint8_t) round(fCWf);
  if (fCW < 0x04) fCW = 0x04;
  if (fCW > 0x50) fCW = 0x50;
  writeRegister(REG_EXC_FREQ, fCW);
}

double getExcitationFrequency() {
  uint8_t fCW = readRegister(REG_EXC_FREQ);
  return (fCW * (double)CLKIN_HZ) / 32768.0;
}

// ---------------------------------------------------------------------------
// Control register  (Address 0x92)
// ---------------------------------------------------------------------------
uint8_t buildControlByte(uint8_t resolutionBits, bool hysteresis,
                          bool phaseLock44deg, uint8_t encoderResolutionBits) {
  uint8_t v = CTRL_RESERVED_D6; // D6 must be 1

  if (phaseLock44deg) v |= CTRL_PHASELOCK_D5;   // 1 = +/-44 deg (recommended default)
  if (hysteresis)      v |= CTRL_HYSTERESIS_D4;

  auto resBits = [](uint8_t bits) -> uint8_t {
    switch (bits) {
      case 10: return 0b00;
      case 12: return 0b01;
      case 14: return 0b10;
      case 16: return 0b11;
      default: return 0b11;
    }
  };

  uint8_t enRes = resBits(encoderResolutionBits);
  uint8_t res   = resBits(resolutionBits);

  if (enRes & 0b10) v |= CTRL_ENRES1_D3;
  if (enRes & 0b01) v |= CTRL_ENRES0_D2;
  if (res & 0b10)   v |= CTRL_RES1_D1;
  if (res & 0b01)   v |= CTRL_RES0_D0;

  return v;
}

void writeControlRegister(uint8_t resolutionBits, bool hysteresis,
                           bool phaseLock44deg, uint8_t encoderResolutionBits) {
  uint8_t v = buildControlByte(resolutionBits, hysteresis, phaseLock44deg, encoderResolutionBits);
  writeRegister(REG_CONTROL, v);
}

void printControlRegister() {
  bool err;
  uint8_t v = readRegister(REG_CONTROL, &err);
  static const char *resStr[4] = {"10-bit", "12-bit", "14-bit", "16-bit"};
  uint8_t res   = ((v & CTRL_RES1_D1) ? 2 : 0) | ((v & CTRL_RES0_D0) ? 1 : 0);
  uint8_t enRes = ((v & CTRL_ENRES1_D3) ? 2 : 0) | ((v & CTRL_ENRES0_D2) ? 1 : 0);
  Serial.printf("  Control byte: 0x%02X  (readback error flag: %s)\n", v, err ? "SET" : "clear");
  Serial.printf("  Position/velocity resolution: %s\n", resStr[res]);
  Serial.printf("  Encoder emulation resolution: %s\n", resStr[enRes]);
  Serial.printf("  Hysteresis: %s\n", (v & CTRL_HYSTERESIS_D4) ? "enabled" : "disabled");
  Serial.printf("  Phase lock range: %s\n", (v & CTRL_PHASELOCK_D5) ? "+/-44 deg" : "+/-360 deg");
}

// ---------------------------------------------------------------------------
// LOS / DOS threshold registers -- 0 to 4.82 V in ~38 mV steps (7-bit)
// ---------------------------------------------------------------------------
uint8_t voltsToThresholdCode(double volts) {
  int code = (int) round(volts / (4.82 / 127.0));
  if (code < 0) code = 0;
  if (code > 127) code = 127;
  return (uint8_t)code;
}

double thresholdCodeToVolts(uint8_t code) {
  return (code & 0x7F) * (4.82 / 127.0);
}

void setLOSThreshold(double volts)        { writeRegister(REG_LOS_THRESH,      voltsToThresholdCode(volts)); }
void setDOSOverrangeThreshold(double v)   { writeRegister(REG_DOS_OVER_THRESH, voltsToThresholdCode(v)); }
void setDOSMismatchThreshold(double v)    { writeRegister(REG_DOS_MISM_THRESH, voltsToThresholdCode(v)); }
void setDOSResetMaxThreshold(double v)    { writeRegister(REG_DOS_RST_MAX,     voltsToThresholdCode(v)); }
void setDOSResetMinThreshold(double v)    { writeRegister(REG_DOS_RST_MIN,     voltsToThresholdCode(v)); }

double getLOSThreshold()      { return thresholdCodeToVolts(readRegister(REG_LOS_THRESH)); }
double getDOSOverrangeThresh(){ return thresholdCodeToVolts(readRegister(REG_DOS_OVER_THRESH)); }
double getDOSMismatchThresh() { return thresholdCodeToVolts(readRegister(REG_DOS_MISM_THRESH)); }
double getDOSResetMax()       { return thresholdCodeToVolts(readRegister(REG_DOS_RST_MAX)); }
double getDOSResetMin()       { return thresholdCodeToVolts(readRegister(REG_DOS_RST_MIN)); }

// ---------------------------------------------------------------------------
// LOT high/low threshold registers -- range & LSB depend on resolution
// (datasheet Table 19)
// ---------------------------------------------------------------------------
double lotLSBDegrees(uint8_t resolutionBits) {
  switch (resolutionBits) {
    case 10: return 0.35;
    case 12: return 0.14;
    case 14: return 0.09;
    case 16: return 0.09;
    default: return 0.09;
  }
}

void setLOTHighThreshold(double degrees, uint8_t resolutionBits) {
  int code = (int) round(degrees / lotLSBDegrees(resolutionBits));
  if (code < 0) code = 0;
  if (code > 127) code = 127;
  writeRegister(REG_LOT_HIGH, (uint8_t)code);
}

void setLOTLowThreshold(double degrees, uint8_t resolutionBits) {
  int code = (int) round(degrees / lotLSBDegrees(resolutionBits));
  if (code < 0) code = 0;
  if (code > 127) code = 127;
  writeRegister(REG_LOT_LOW, (uint8_t)code);
}

double getLOTHighThreshold(uint8_t resolutionBits) {
  return readRegister(REG_LOT_HIGH) * lotLSBDegrees(resolutionBits);
}

double getLOTLowThreshold(uint8_t resolutionBits) {
  return readRegister(REG_LOT_LOW) * lotLSBDegrees(resolutionBits);
}

// ---------------------------------------------------------------------------
// Hardware reset
// ---------------------------------------------------------------------------
void hardwareReset() {
  digitalWrite(PIN_RESET, LOW);
  delayMicroseconds(20);     // >> 10us minimum
  digitalWrite(PIN_RESET, HIGH);

  // Wait tTRACK for the tracking loop to settle after reset.
  uint32_t trackMs;
  switch (RESOLUTION_BITS) {
    case 10: trackMs = 10; break;
    case 12: trackMs = 20; break;
    case 14: trackMs = 25; break;
    case 16: trackMs = 60; break;
    default: trackMs = 60; break;
  }
  delay(trackMs + 5);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
unsigned long lastPoll = 0;

void printMenu() {
  Serial.println();
  Serial.println("=== AD2S1210 command menu ===");
  Serial.println(" p  - read position");
  Serial.println(" v  - read velocity");
  Serial.println(" f  - read + clear fault register");
  Serial.println(" c  - print control register");
  Serial.println(" e  - print excitation frequency");
  Serial.println(" E<hz> - set excitation frequency, e.g. E4747");
  Serial.println(" r<bits> - set resolution (10/12/14/16), e.g. r16");
  Serial.println(" t  - print all threshold registers");
  Serial.println(" x  - software reset");
  Serial.println(" X  - hardware reset (RESET pin)");
  Serial.println(" s  - toggle continuous streaming of position/velocity/faults");
  Serial.println("==============================");
}

bool streaming = false;

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  char c = cmd[0];
  switch (c) {
    case 'p': {
      SampleResult r = readPosition();
      Serial.printf("Position: raw=0x%04X  angle=%.3f deg\n", r.raw, positionToDegrees(r.raw));
      printFault(r.fault);
      break;
    }
    case 'v': {
      SampleResult r = readVelocity();
      double rps = velocityToRPS((int16_t)r.raw, RESOLUTION_BITS);
      Serial.printf("Velocity: raw=0x%04X  speed=%.3f rps\n", r.raw, rps);
      printFault(r.fault);
      break;
    }
    case 'f': {
      uint8_t f = readAndClearFault();
      Serial.println("Fault register (read + cleared):");
      printFault(f);
      Serial.printf("LOT pin: %s   DOS pin: %s\n",
                     digitalRead(PIN_LOT) ? "OK" : "FAULT(low)",
                     digitalRead(PIN_DOS) ? "OK" : "FAULT(low)");
      break;
    }
    case 'c':
      printControlRegister();
      break;
    case 'e':
      Serial.printf("Excitation frequency: %.1f Hz\n", getExcitationFrequency());
      break;
    case 'E': {
      double hz = cmd.substring(1).toFloat();
      if (hz >= 2000 && hz <= 20000) {
        setExcitationFrequency(hz);
        delay(5);
        Serial.printf("Excitation frequency set. Readback: %.1f Hz\n", getExcitationFrequency());
      } else {
        Serial.println("Excitation frequency must be 2000-20000 Hz.");
      }
      break;
    }
    case 'r': {
      int bits = cmd.substring(1).toInt();
      if (bits == 10 || bits == 12 || bits == 14 || bits == 16) {
        writeControlRegister(bits, true, true, bits);
        Serial.printf("Resolution set to %d-bit in control register.\n", bits);
        Serial.println("NOTE: if RES0/RES1 pins are hardwired, make sure they match!");
      } else {
        Serial.println("Resolution must be 10, 12, 14, or 16.");
      }
      break;
    }
    case 't':
      Serial.printf("LOS threshold:          %.3f V\n", getLOSThreshold());
      Serial.printf("DOS overrange threshold:%.3f V\n", getDOSOverrangeThresh());
      Serial.printf("DOS mismatch threshold: %.3f V\n", getDOSMismatchThresh());
      Serial.printf("DOS reset max:          %.3f V\n", getDOSResetMax());
      Serial.printf("DOS reset min:          %.3f V\n", getDOSResetMin());
      Serial.printf("LOT high threshold:     %.3f deg\n", getLOTHighThreshold(RESOLUTION_BITS));
      Serial.printf("LOT low threshold:      %.3f deg\n", getLOTLowThreshold(RESOLUTION_BITS));
      break;
    case 'x':
      Serial.println("Issuing software reset...");
      softReset();
      Serial.println("Done.");
      break;
    case 'X':
      Serial.println("Issuing hardware reset...");
      hardwareReset();
      Serial.println("Done.");
      break;
    case 's':
      streaming = !streaming;
      Serial.printf("Streaming %s\n", streaming ? "ON" : "OFF");
      break;
    default:
      printMenu();
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_WRFSYNC, OUTPUT); digitalWrite(PIN_WRFSYNC, HIGH);
  pinMode(PIN_SAMPLE,  OUTPUT); digitalWrite(PIN_SAMPLE, HIGH);
  pinMode(PIN_A0,       OUTPUT);
  pinMode(PIN_A1,       OUTPUT);
  pinMode(PIN_RESET,   OUTPUT); digitalWrite(PIN_RESET, HIGH);
  pinMode(PIN_LOT, INPUT);
  pinMode(PIN_DOS, INPUT);

  vspi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_WRFSYNC /* unused as HW CS, we drive it manually */);

  Serial.println("Resetting AD2S1210...");
  hardwareReset();

  Serial.println("Configuring AD2S1210...");
  writeControlRegister(RESOLUTION_BITS, /*hysteresis=*/true,
                        /*phaseLock44deg=*/true, /*encoderResolutionBits=*/RESOLUTION_BITS);
  setExcitationFrequency(10000.0); // adjust as needed, e.g. 4747.0

  // Reasonable default fault thresholds (datasheet power-up defaults);
  // override via the 't' command's setter functions if you need different
  // values for your resolver.
  setLOSThreshold(2.2);
  setDOSOverrangeThreshold(4.1);
  setDOSMismatchThreshold(0.38);
  setDOSResetMaxThreshold(2.28);
  setDOSResetMinThreshold(3.99);
  setLOTHighThreshold(12.5, RESOLUTION_BITS);
  setLOTLowThreshold(2.5, RESOLUTION_BITS);

  // Leave the fault register as the "last addressed" register so that
  // normal-mode reads automatically include fresh fault data (per datasheet
  // recommendation).
  readRegister(REG_FAULT);

  printMenu();
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleCommand(cmd);
  }

  if (streaming && millis() - lastPoll > 200) {
    lastPoll = millis();
    SampleResult r = readPosition();
    Serial.printf("[%lu ms] angle=%.2f deg  LOT=%d DOS=%d  fault=0x%02X\n",
                  millis(), positionToDegrees(r.raw),
                  digitalRead(PIN_LOT), digitalRead(PIN_DOS), r.fault);
  }
}
