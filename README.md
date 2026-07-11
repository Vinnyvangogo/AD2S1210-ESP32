# AD2S1210-ESP32
Resolver to Digital conversion with ESP32 reading and writing to the AD2S1210 Eval board.

Wemos Lolin 32 - ESP32 board

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
