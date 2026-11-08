SIDBlaster-USB HardSID.dll for SIDPLAY/Windows on Windows 98     version 1.1
===========================================================================
https://github.com/matias-enroth/sidblaster-win98

Lets SIDPLAY/Windows (the November 2002 win32 release) play through a
SIDBlaster-USB on Windows 98: SIDPLAY's HardSID option, on a real SID.
It can also play through a Network SID Device server on another computer
(SID-Device, JSIDPlay2): see 6. below.

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

6. Network SID Device (Backend=NetSID)
   Instead of a SIDBlaster, play through a SID emulator on another computer
   on the network: SID-Device (Windows, Mac, Linux, Raspberry Pi;
   github.com/WilfredC64/sid-device) or JSIDPlay2's network SID device.
   The server buffers the writes and replays them in C64 time, so a Windows
   98 stall shorter than Lead= isn't heard. Needs TCP/IP on the Win98 box.
   - Start the server; allow connections from other computers (SID-Device:
     "Allow external connections"; JSIDDevice: "Allow
     external IP connections"). Port 6581.
   - Test from an MS-DOS Prompt:  SBTEST NET 192.168.1.20
     It lists the server's SID configs and plays the arpeggio through it.
   - In SIDBLAST.INI: Backend=NetSID, Host=<the server's IP address>,
     Clock=PAL or NTSC, Model=6581 or 8580 (or a config number).
   - Lead=100 is the buffer in ms. More rides out longer stalls; the music
     plays that much behind SIDPLAY's display either way.
   - If the server isn't running, SIDPLAY finds no device. If it goes away
     while playing, the DLL drops the writes (SIDPLAY carries on) and tries
     to reconnect every 5 seconds; the tune resumes once it's back.
   Log=1 stats lines: busy = the server's buffer was full (harmless; it
   then holds more than Lead). With busy=0, maxlat over Lead (in us:
   100 ms = 100000 us) means a write went out too late to be heard on
   time. dropped = writes lost while the link was down.

Limits
 - Digi/sample tunes: most are RSID files, which SIDPLAY/Windows can't
   load ("Could not determine file format"). For PSID tunes with samples,
   the SIDBlaster protocol carries no timing, so sample writes to $D418
   are paced by Windows and USB 1.1 frames. Expect mush.
 - No mute relay on the SIDBlaster; line mute uses the volume nibble.
 - NetSID: Model= applies to every tune (the DLL can't see a tune's
   6581/8580 flag). An 8580 tune on a 6581 sounds harsh and distorted.

MIT License, (c) 2026 Matias Enroth.
