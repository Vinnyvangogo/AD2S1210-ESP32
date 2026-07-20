# AD2S1210-ESP32
Resolver to Digital conversion with ESP32 reading and writing to the AD2S1210 Eval board.

Wemos Lolin 32 - ESP32 board

<img width="1481" height="812" alt="image" src="https://github.com/user-attachments/assets/631ab4dd-bf29-420e-9477-62e59be31bc4" />



AD2S1210 Eval board

<img width="640" height="640" alt="image" src="https://github.com/user-attachments/assets/2f609706-403c-4192-bb02-a449b3ce02f0" />


wiring plan using the LOLIN32's hardware VSPI bus plus free GPIOs for the AD2S1210's control lines.

<img width="753" height="457" alt="image" src="https://github.com/user-attachments/assets/01f0de35-6122-4043-ba42-e69b582ecff8" />

<img width="755" height="364" alt="image" src="https://github.com/user-attachments/assets/d7fcc734-3ea6-4826-8c13-499af053185e" />

<img width="737" height="447" alt="image" src="https://github.com/user-attachments/assets/799675e9-c238-42b0-9f5a-feb6059583a0" />

<img width="760" height="256" alt="image" src="https://github.com/user-attachments/assets/5e6d7433-6b60-427d-bb05-d8f5e9a583b3" />

<img width="749" height="209" alt="image" src="https://github.com/user-attachments/assets/602ceeb6-1284-48f3-aeb4-e32113bd1e37" />

<img width="741" height="218" alt="image" src="https://github.com/user-attachments/assets/da8bf681-ade8-4ac6-97e9-49aa1e36c083" />

Notes on pin choice:

I avoided GPIO0, GPIO2, GPIO12, and GPIO15 — these are boot-strapping pins on the ESP32 and can interfere with the module entering normal run mode if pulled the wrong way at power-up.

AVDD/DVDD (5 V) still need to come from the eval board's own supply (9 V adapter or external 5 V) — VDRIVE is the only rail you're overriding to 3.3 V.

If you don't need to change resolution at runtime, you can skip GPIO26/27 and just hardwire RES0/RES1 directly to GND or VDRIVE for a fixed resolution (e.g., both high for 16-bit) — one less thing for firmware to manage.

full register map straight from the AD2S1210 Rev. B datasheet

<img width="731" height="541" alt="image" src="https://github.com/user-attachments/assets/15480965-dd70-4d77-8cd0-893c2a220fc0" />

It exposes an interactive serial menu (p, v, f, c, e, E<hz>, r<bits>, t, x, X, s) so you can poke at any register from the Serial Monitor, plus a continuous streaming mode for live angle/fault readout.
A few things worth double-checking before you power it up:

CLKIN_HZ — I defaulted to 8.192 MHz (the datasheet's reference value), but this must match whatever crystal/oscillator is actually on the eval board, since both the excitation-frequency math and velocity scaling depend on it.

SPI mode — I derived SPI_MODE1 (SDO changes on rising SCLK, SDI sampled on falling) from the datasheet timing diagrams. This is the part most worth scoping/verifying on first bring-up, since a wrong mode will look like garbage data rather than an obvious error.

RES0/RES1 pins — the chip uses these hardware pins for resolution in normal mode but the control-register bits in configuration mode. The code sets the control register to match RESOLUTION_BITS, but if you've hardwired RES0/RES1 on the board, make sure they're tied to the same resolution or you'll get inconsistent data (this is called out explicitly in the datasheet).

Excitation frequency default is 10 kHz in setup() — change the setExcitationFrequency(10000.0) line to 4747.0 (or whatever frequency your resolver uses) once you've settled on it.

I used SPI_CLOCK_HZ = 2 MHz conservatively — you can push this higher (the chip allows up to ~15–25 MHz depending on VDRIVE) once you've confirmed the basic protocol works.


Confirm PlatformIO's board ID is lolin32. Here's the full setup.

1. Install the PlatformIO extension
In VS Code: Extensions (Ctrl+Shift+X) → search "PlatformIO IDE" → Install. Restart VS Code when prompted. It'll take a minute to finish installing its own toolchains in the background (watch the PlatformIO icon in the bottom status bar / the "PIO Home" tab).

2. Create a new project
* Click the PlatformIO alien-head icon in the left sidebar → PIO Home → New Project
* Name: anything, e.g. ad2s1210-esp32
* Board: search for "WEMOS LOLIN32" and select it (this sets board = lolin32)
* Framework: Arduino
* Location: wherever you like
* Click Finish — PlatformIO creates the folder structure and downloads the ESP32/Arduino toolchain automatically.

3. Project structure

PlatformIO expects .cpp, not .ino. I've converted the sketch and generated the project files below — drop them straight in:
ad2s1210-esp32/
├── platformio.ini      <- replace with the one below
├── src/
│   └── main.cpp         <- replace the default with this

5. Wire it up

* Put platformio.ini in the project root (next to src/, lib/, etc. — replace the auto-generated one).
* Put main.cpp in src/ (replace the default src/main.cpp).
* The only real change from the original sketch is one added line: #include <Arduino.h>, since Arduino IDE adds that implicitly for .ino files but PlatformIO's .cpp files need it explicit. Everything else is unchanged.

5. Build and upload

* Connect the LOLIN32 via USB.
* Bottom status bar in VS Code (PlatformIO toolbar): click the checkmark (✓) icon to build, then the right-arrow (→) icon to upload.
* Click the plug icon to open the Serial Monitor (reads monitor_speed from platformio.ini, so it'll already be at 115200).
* If it can't find the port automatically, uncomment upload_port / monitor_port in platformio.ini and set it to your board's COM port / /dev/tty* device.

6. SPI library note

* The SPIClass vspi(VSPI) constructor and the vspi.begin(sck, miso, mosi, ss) call in the code are part of the ESP32 Arduino core, which PlatformIO's espressif32 platform pulls in automatically via framework = arduino — no extra lib_deps entry needed.
* One more thing worth doing once it's building: in platformio.ini you can bump -D CORE_DEBUG_LEVEL or add build_flags = -DCORE_DEBUG_LEVEL=3 if you want ESP32 core debug logging while you're bringing up the SPI link for the first time.

=== AD2S1210 command menu ===

 p  - read position
 
 v  - read velocity
 
 f  - read + clear fault register
 
 c  - print control register
 
 e  - print excitation frequency
 
 E<hz> - set excitation frequency, e.g. E4747
 
 r<bits> - set resolution (10/12/14/16), e.g. r16
 
 t  - print all threshold registers
 
 x  - software reset
 
 X  - hardware reset (RESET pin)
 
 s  - toggle continuous streaming of position/velocity/faults

 ==============================
 
 
Position: raw=0x0000  angle=0.000 deg

  Fault byte: 0x00

  (no faults)
  
Velocity: raw=0x0000  speed=0.000 rps

  Fault byte: 0x00
  
  (no faults)
  
Issuing hardware reset...

Done.


  Control byte: 0x00  (readback error flag: clear)
  
  Position/velocity resolution: 10-bit
  
  Encoder emulation resolution: 10-bit
  
  Hysteresis: disabled
  
  Phase lock range: +/-360 deg
  
Excitation frequency: 0.0 Hz

Fault register (read + cleared):

  Fault byte: 0x00
  
  (no faults)
  
LOT pin: FAULT(low)   DOS pin: FAULT(low)


