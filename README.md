# sidblaster-win98

**A real SID chip in SIDPLAY/Windows on Windows 98: a drop-in `HardSID.dll`
that drives a SIDBlaster-USB.**

SIDPLAY/Windows (the November 2002 win32 release) can play through a
HardSID card, and only a HardSID card. The SIDBlaster-USB comes with a
HardSID-emulation DLL, but current builds are made for Vista and later
and don't load on Windows 9x, so SIDPLAY's HardSID option stays greyed
out. This `HardSID.dll` speaks the old HardSID API to SIDPLAY and the
SIDBlaster's serial protocol to the device, through FTDI's Windows 98
driver.

Verified on Windows 98 SE, AMD K6-2, SIDBlaster-USB Tic-Tac Edition.

## Quick start

Needs SIDPLAY/Windows and FTDI's Windows 98/ME VCP driver, version
1.09.06 (2004-11-25). FTDI's own file is `R10906.zip`; older drivers are
under *No longer supported* on its
[VCP drivers page](https://ftdichip.com/drivers/vcp-drivers/). The copy
used for testing came from
[this mirror](https://www.intelligent-appliance.com/product-551-%2810906_2004_08_10%29_USB_Virtual_COM_Port_Drivers.aspx)
(`Windows 98ME (1.09.06 2004-11-25).zip`).

1. Get `sidblaster-win98-1.0.zip` from
   [Releases](https://github.com/matias-enroth/sidblaster-win98/releases)
   and unpack it (on a newer PC if the Win98 box has no unzipper).
2. Install the FTDI driver and plug in the SIDBlaster. Device Manager →
   Ports shows `USB Serial Port (COMx)`.
3. From an MS-DOS Prompt: `SBTEST COMx`. You should hear a seven-note
   arpeggio, and the OSC3 values it reads back should vary. `SBTEST` on
   its own lists the FTDI ports it finds.
4. Copy `HardSID.dll` and `SIDBLAST.INI` into the SIDPLAY folder (back up
   any `HardSID.dll` already there).
5. In SIDPLAY: Settings → Device → HardSID, device 0.

The port is found by itself; if not, set `Port=COMx` in `SIDBLAST.INI`.

## What you get

- SIDPLAY/Windows' HardSID mode on a SIDBlaster-USB, under Windows 98.
- Pitch correction: the SIDBlaster runs its SID at 1 MHz rather than a
  PAL C64's 985,248 Hz, about 26 cents sharp. Frequency writes are
  rescaled, so tunes play in tune (`Clock=PAL`, `NTSC` or `OFF`).
- One USB transfer per frame: register writes are queued and sent in one
  batch by a time-critical thread, not one transfer each.
- Pause and resume work: reads of the SID's write-only registers are
  answered from what was written, as SIDPLAY expects of a HardSID.
- `SBTEST.EXE`: finds the port, plays a test arpeggio, reads OSC3 back.
- A log for troubleshooting (`Log=1`), off by default.
- No C runtime, only Windows 95-era API imports, code for i586: runs on a
  Pentium or K6-2.

## Measured on real hardware

Windows 98 SE, AMD K6-2, SIDBlaster-USB Tic-Tac Edition on COM2, found
by auto-detect. Two sessions, about 18 minutes of play, with `Log=1`:

| | |
|---|---|
| USB batches | one per 50 Hz frame |
| Write-to-send latency | 0.6–0.8 ms median |
| Time per `WriteFile` | about 2.5 ms |
| Queue overflows, failed writes | none |
| Pitch | matches SIDPLAY's own emulation |

The only stutter heard was while a web browser was starting.

## Configuration

`SIDBLAST.INI`, next to the DLL:

| Key | Default | Meaning |
|---|---|---|
| `Port` | empty | COM port; empty finds it from the FTDI entries in the registry |
| `Clock` | `PAL` | Pitch correction: `PAL`, `NTSC` or `OFF` |
| `SidClock` | `1000000` | The SIDBlaster's actual SID clock in Hz, to fine-tune a unit that's off |
| `Log` | `0` | 0 off; 1 init and a stats line per second; 2 also every register write |
| `Priority` | `15` | Writer thread priority: 15 time-critical, 2 highest, 1 above normal, 0 normal |

## Troubleshooting

Set `Log=1`. `SIDBLAST.LOG`, next to the DLL, then shows the init
sequence (which port, what baud rate the driver accepted) and a stats
line per second of play. `Log=2` adds every register write: big, and
slow enough to disturb playback, so only for seeing what SIDPLAY sends.

- **HardSID is greyed out in SIDPLAY.** The DLL isn't in SIDPLAY's folder,
  or it didn't load. With `Log=1`, no `SIDBLAST.LOG` means it never
  loaded.
- **"No device found. Run HardSIDConfig.exe".** The DLL loaded but found
  no port, or couldn't open it. The log says which; set `Port=COMx` and
  check that `SBTEST COMx` plays.
- **Stutter.** On Windows 98, a program starting up can hold up USB. In
  the stats lines, `qfull` or `wrfail` above 0 means the DLL couldn't keep
  up; `maxlat` above 20000 µs (a frame) means Windows held the writer up.
- **Out of tune.** Check `Clock=`; a unit whose oscillator is off can be
  corrected with `SidClock=` (higher if it plays sharp).

## How it works

- Exports the four functions SIDPLAY/Windows looks up, all `__stdcall`:
  `GetHardSIDCount`, `WriteToHardSID`, `ReadFromHardSID`,
  `MuteHardSID_Line`.
- SIDPLAY does its own timing and calls `WriteToHardSID` in real time.
  Writes go into a queue; a writer thread sends whatever has gathered in
  one `WriteFile`. Its stats go to a low-priority thread, so the writer
  never touches the disk.
- `$00`–`$18` are write-only on a SID; reads return the last value
  written (SIDPLAY reads `$18` when pausing). `$19`–`$1C` are read from
  the chip.
- The DLL loads itself once more, so it stays in memory when SIDPLAY
  frees it, and its writer thread never runs in an unloaded image.

[docs/PROTOCOL.md](docs/PROTOCOL.md): the SIDBlaster's serial protocol,
how SIDPLAY/Windows uses the HardSID API, and why the official DLLs
don't load on Windows 9x.

## Limits

- **Digi (sample) tunes** don't work in practice. The SIDBlaster protocol
  carries no timing, so writes to `$D418` are paced by Windows and 1 ms
  USB frames.
- **Clock is global.** The DLL can't see a tune's PAL/NTSC flag; set
  `Clock=` for what you mostly play.
- **Line mute** uses the volume nibble (the SIDBlaster has no mute
  relay), so a 6581 may click.
- Only Windows 98 SE has been tried. Windows ME should behave the same.
  Other SIDBlaster-USB builds use the same protocol and FTDI chip, but
  only the Tic-Tac Edition has been tested.

## Building

```sh
brew install mingw-w64        # or: apt install gcc-mingw-w64-i686
./build.sh                    # build/HardSID.dll, build/SBTEST.EXE
./release.sh                  # the same, checked for Win95 imports and i586, zipped
```

`tools/fakesb.py` pretends to be a SIDBlaster on a pty, for testing under
Wine.

## Credits

- **Davey** ([stg](https://github.com/stg/SIDBlaster-USB)), who designed
  the original SIDBlaster-USB (2010).
- **Andreas Schumm (gh0stless)** for the
  [SIDBlaster-USB Tic-Tac Edition](https://github.com/gh0stless/SIDBlaster-USB-Tic-Tac-Edition);
  the protocol here comes from its open PIC firmware.
- **Michael Schwendt and Adam Lorentzon** for SIDPLAY/Windows.
- **Hard Software** for the HardSID, whose API this speaks.

Developed with AI assistance (Claude).

## License

MIT, see [LICENSE](LICENSE).
