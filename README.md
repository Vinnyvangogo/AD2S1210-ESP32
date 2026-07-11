# AD2S1210-ESP32
Resolver to Digital conversion with ESP32 reading and writing to the AD2S1210 Eval board.

Wemos Lolin 32 - ESP32 board

<img width="1481" height="812" alt="image" src="https://github.com/user-attachments/assets/631ab4dd-bf29-420e-9477-62e59be31bc4" />

wiring plan using the LOLIN32's hardware VSPI bus plus free GPIOs for the AD2S1210's control lines.

<img width="755" height="364" alt="image" src="https://github.com/user-attachments/assets/d7fcc734-3ea6-4826-8c13-499af053185e" />

<img width="737" height="447" alt="image" src="https://github.com/user-attachments/assets/799675e9-c238-42b0-9f5a-feb6059583a0" />

<img width="760" height="256" alt="image" src="https://github.com/user-attachments/assets/5e6d7433-6b60-427d-bb05-d8f5e9a583b3" />

<img width="749" height="209" alt="image" src="https://github.com/user-attachments/assets/602ceeb6-1284-48f3-aeb4-e32113bd1e37" />

<img width="741" height="218" alt="image" src="https://github.com/user-attachments/assets/da8bf681-ade8-4ac6-97e9-49aa1e36c083" />

Notes on pin choice:

I avoided GPIO0, GPIO2, GPIO12, and GPIO15 — these are boot-strapping pins on the ESP32 and can interfere with the module entering normal run mode if pulled the wrong way at power-up.

AVDD/DVDD (5 V) still need to come from the eval board's own supply (9 V adapter or external 5 V) — VDRIVE is the only rail you're overriding to 3.3 V.

If you don't need to change resolution at runtime, you can skip GPIO26/27 and just hardwire RES0/RES1 directly to GND or VDRIVE for a fixed resolution (e.g., both high for 16-bit) — one less thing for firmware to manage.



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


