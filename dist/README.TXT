SIDBlaster-USB HardSID.dll for SIDPLAY/Windows on Windows 98     version 1.0
===========================================================================
https://github.com/matias-enroth/sidblaster-win98

Lets SIDPLAY/Windows (the November 2002 win32 release) play through a
SIDBlaster-USB on Windows 98: SIDPLAY's HardSID option, on a real SID.

1. Driver
   Install FTDI's Windows 98/ME VCP driver, version 1.09.06 (2004-11-25;
   FTDI's file is R10906.zip). A copy, if FTDI no longer has it:
   www.intelligent-appliance.com, search for 1.09.06. Plug in the
   SIDBlaster; it appears under Device Manager > Ports as
   "USB Serial Port (COMx)".

2. Hardware test (from an MS-DOS Prompt in this folder)
     SBTEST          lists FTDI ports found in the registry + COM ports that open
     SBTEST COM2     resets the SID, plays an arpeggio, reads OSC3 back
   You should hear seven pulse-wave notes, and the OSC3 values should vary.

3. Install into SIDPLAY
   - Back up SIDPLAY's existing HardSID.dll, if any.
   - Copy HardSID.dll and SIDBLAST.INI into the SIDPLAY folder.
   - Set Port= in SIDBLAST.INI, or leave it empty for auto-detect.
   - Settings > Device > HardSID, device 0.

4. Settings (SIDBLAST.INI)
   Clock=PAL|NTSC|OFF  retunes frequency writes for the SIDBlaster's 1 MHz
                       SID clock. The DLL can't see the tune's own clock flag,
                       so this is a global choice; PAL suits most of HVSC.
   SidClock=1000000    fine-tune if your unit is slightly sharp or flat.
   Priority=15         writer thread priority; 15 is time-critical.

5. Troubleshooting
   Set Log=1 in SIDBLAST.INI. SIDBLAST.LOG, next to the DLL, then gets
   the init sequence (which port, what baud the driver accepted) and one
   stats line per second while playing. Log=2 adds every register write;
   that slows things down, so use it only to see what SIDPLAY sends.

   - HardSID greyed out in SIDPLAY: the DLL isn't in SIDPLAY's folder, or
     it failed to load. With Log=1, no SIDBLAST.LOG means it never loaded.
   - "No device found": the port wasn't found or didn't open; the log
     says which. Set Port=COMx, and check that SBTEST COMx plays.
   - Stutter: other programs starting (a browser, say) can starve USB on
     Windows 98. In the stats lines, qfull or wrfail above 0 means the
     DLL couldn't keep up; maxlat over 20000 us (a 50 Hz frame)
     means Windows held it up.

Limits
 - Digi/sample tunes: the SIDBlaster protocol carries no timing, so sample
   writes to $D418 are paced by Windows and USB 1.1 frames. Expect mush.
 - No mute relay on the SIDBlaster; line mute uses the volume nibble.

MIT License, (c) 2026 Matias Enroth.
