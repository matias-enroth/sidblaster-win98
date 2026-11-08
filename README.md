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

It can also play through a **Network SID Device** server on another
computer instead ([SID-Device](https://github.com/WilfredC64/sid-device),
JSIDPlay2): the server buffers the writes and replays them in C64 time,
so Windows 98's stalls aren't heard. See [below](#network-sid-device).

Verified on Windows 98 SE, AMD K6-2, with a SIDBlaster-USB Tic-Tac Edition
and with JSIDPlay2's network SID device on a Mac.

## Quick start

Needs SIDPLAY/Windows and FTDI's Windows 98/ME VCP driver, version
1.09.06 (2004-11-25). FTDI's own file is `R10906.zip`; older drivers are
under *No longer supported* on its
[VCP drivers page](https://ftdichip.com/drivers/vcp-drivers/). The copy
used for testing came from
[this mirror](https://www.intelligent-appliance.com/product-551-%2810906_2004_08_10%29_USB_Virtual_COM_Port_Drivers.aspx)
(`Windows 98ME (1.09.06 2004-11-25).zip`).

1. Get `sidblaster-win98-1.1.zip` from
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
- `Backend=NetSID`: play through a Network SID Device server over TCP/IP
  instead, with a buffer that absorbs Windows stalls.
- A log for troubleshooting (`Log=1`), off by default.
- No C runtime, only Windows 95-era API imports, code for i586: runs on a
  Pentium or K6-2.

## Measured on real hardware

Windows 98 SE, AMD K6-2 at 368 MHz (Soyo 2A5LDS2FC, VIA Apollo VPX, 256 MB),
SIDBlaster-USB Tic-Tac Edition on COM2, found by auto-detect. Two sessions, about 18 minutes of play, with `Log=1`:

| | |
|---|---|
| USB batches | one per 50 Hz frame |
| Write-to-send latency | 0.6–0.8 ms median |
| Time per `WriteFile` | about 2.5 ms |
| Queue overflows, failed writes | none |
| Pitch | matches SIDPLAY's own emulation |

The only stutter heard was while a web browser was starting.

`Backend=NetSID`, same PC, with JSIDPlay2 4.14's JSIDDevice on an
Apple-silicon Mac over the LAN:

| | |
|---|---|
| Set-up | about 0.3 s, 37 SID configurations listed |
| Playing | no dropped writes, no queue overflows; `maxlat` 70–125 ms |
| Programs starting | no stutter for Explorer or Notepad++; brief stutters while RetroZilla loaded |
| Server stopped mid-tune | SIDPLAY plays on; the DLL reconnects within 5 s of the server's return |
| Pitch | right for PAL tunes on `Clock=PAL`; `Clock=NTSC` raises them about 4%, as it should |

JSIDDevice's buffer tends to sit at its 250 ms limit after a slow first
second, so most packets get one `BUSY` reply; that costs some network
traffic but doesn't affect the sound.

## Network SID Device

`Backend=NetSID` sends the writes over TCP/IP to a server speaking the
Network SID Device protocol (the one ACID64 uses), which emulates the SID
and plays it on that computer's sound output:
[SID-Device](https://github.com/WilfredC64/sid-device) (Windows, macOS,
Linux, Raspberry Pi) or JSIDPlay2's network SID device.

Why: SIDPLAY does its own timing, and on Windows 98 a program starting up
can hold it or the USB stack up long enough to hear. Here every write is
stamped with `QueryPerformanceCounter` as SIDPLAY makes it and sent with
its C64-cycle delay; the server queues them and replays them on its own
clock, `Lead=` milliseconds (100 by default) behind. A stall shorter than
that only delays the sending, not the music.

1. Start the server and let it accept connections from the network
   (SID-Device: *Allow external connections*; JSIDPlay2's JSIDDevice:
   *Allow external IP connections*). It listens on port 6581.
2. On the Windows 98 box (TCP/IP installed): `SBTEST NET 192.168.1.20`
   lists the server's SID configurations and plays the arpeggio through it.
3. In `SIDBLAST.INI`: `Backend=NetSID`, `Host=` the server's IP address,
   `Clock=PAL` or `NTSC`, and `Model=6581` or `8580` (or a config number).

The server emulates a real PAL or NTSC clock, so there's no pitch
correction. If the server isn't running, SIDPLAY finds no device (the
attempt takes at most half a second). If it goes away during play, the DLL
drops the writes and SIDPLAY carries on; while SIDPLAY plays, the DLL
tries to reconnect every 5 seconds, and the tune resumes where it is
once the server is back.

Each tune start (SIDPLAY's `MuteHardSID_Line(1)`) flushes the server's
queue and resets its SID, so stopping and changing tunes are immediate and the new tune
starts exactly one lead behind; the register state is sent again after
the flush. A gap of more than twice the lead (a pause) restarts the lead
too, rather than holding it up.

## Configuration

`SIDBLAST.INI`, next to the DLL:

| Key | Default | Meaning |
|---|---|---|
| `Backend` | `SIDBlaster` | `SIDBlaster`, or `NetSID` for a Network SID Device server |
| `Clock` | `PAL` | SIDBlaster: pitch correction, `PAL`, `NTSC` or `OFF`. NetSID: the clock the server emulates (`OFF` = PAL) |
| `Port` | empty | SIDBlaster: COM port; empty finds it from the FTDI entries in the registry |
| `SidClock` | `1000000` | SIDBlaster: its actual SID clock in Hz, to fine-tune a unit that's off |
| `Host` | empty | NetSID: the server's IP address (a host name works, but a lookup can stall) |
| `NetPort` | `6581` | NetSID: the server's port |
| `Lead` | `100` | NetSID: buffer in ms, 10 to 1000. SID-Device holds at most about 200 |
| `Model` | `0` | NetSID: the server's SID configuration number, or `6581` / `8580` for the first of that model |
| `MinSpacing` | `0` | NetSID: minimum C64 cycles between writes within a frame (they arrive microseconds apart) |
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
- **NetSID: no device.** The log says why: no `Host=`, can't load
  `WSOCK32.DLL` (TCP/IP not installed), connection refused (server not
  running, or not accepting outside connections) or timed out (wrong
  address). `SBTEST NET host` tries the same thing from a prompt.
- **NetSID: stutter.** In its stats lines, `busy` is the server saying
  its buffer is full (about 250 ms on JSIDDevice, 200 on SID-Device),
  which is harmless: it then has more in hand than `Lead=`, and `maxlat`
  just shows the DLL waiting for room. With `busy=0`, `maxlat` above the
  lead (in µs: 100 ms is 100000) means a write went out too late to play
  on time: raise `Lead=`. `dropped` counts writes lost while the link was
  down; `maxrtt` is the slowest request/response round trip.

## How it works

- Exports the four functions SIDPLAY/Windows looks up, all `__stdcall`:
  `GetHardSIDCount`, `WriteToHardSID`, `ReadFromHardSID`,
  `MuteHardSID_Line`.
- SIDPLAY does its own timing and calls `WriteToHardSID` in real time.
  Writes go into a queue, stamped with `QueryPerformanceCounter`; a writer
  thread hands whatever has gathered to the backend. The SIDBlaster gets it
  in one `WriteFile`; NetSID turns the stamps into C64-cycle delays (fixed
  point, no drift) and sends `TRY_WRITE` packets, one request in flight.
  Stats and log lines go to a low-priority thread, so the writer never
  touches the disk.
- `$00`–`$18` are write-only on a SID; reads return the last value
  written (SIDPLAY reads `$18` when pausing). `$19`–`$1C` are read from
  the chip.
- The DLL loads itself once more, so it stays in memory when SIDPLAY
  frees it, and its writer thread never runs in an unloaded image.

[docs/PROTOCOL.md](docs/PROTOCOL.md): the SIDBlaster's serial protocol,
how SIDPLAY/Windows uses the HardSID API, and why the official DLLs
don't load on Windows 9x.

## Limits

- **Digi (sample) tunes.** Most are RSID files in HVSC (Arkanoid, for
  one), which SIDPLAY/Windows 2002 can't load at all: "Could not determine
  file format". PSID tunes with samples load, but on the SIDBlaster their
  `$D418` writes are paced by Windows and 1 ms USB frames, which doesn't
  work in practice. NetSID keeps each write's timing as SIDPLAY made it;
  how well that works for samples hasn't been tried.
- **Clock is global.** The DLL can't see a tune's PAL/NTSC flag; set
  `Clock=` for what you mostly play.
- **NetSID: the SID model is global too.** The tune's 6581/8580 flag
  doesn't reach the DLL either, so `Model=` applies to every tune. An 8580
  tune on a 6581 configuration sounds harsh and overdriven (the 6581's
  filter and output stage are much more non-linear); switch `Model=` for
  such tunes.
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

Tests:

```sh
tests/run.sh                  # unit tests for the protocol code, built natively
tests/wine.sh                 # SBTEST NET and a SIDPLAY stand-in under Wine (Docker)
```

`tools/netsid_mock.py` is a Network SID Device server that models the
server's queue and logs when each write would play and how much lead was
left; `tests/wine.sh` runs everything against it. `tools/fakesb.py`
pretends to be a SIDBlaster on a pty, for testing under Wine.

## Credits

- **Davey** ([stg](https://github.com/stg/SIDBlaster-USB)), who designed
  the original SIDBlaster-USB (2010).
- **Andreas Schumm (gh0stless)** for the
  [SIDBlaster-USB Tic-Tac Edition](https://github.com/gh0stless/SIDBlaster-USB-Tic-Tac-Edition);
  the protocol here comes from its open PIC firmware.
- **Michael Schwendt and Adam Lorentzon** for SIDPLAY/Windows.
- **Hard Software** for the HardSID, whose API this speaks.
- **Ken Händel, Antti S. Lankila and Wilfred Bos** for the Network SID
  Device protocol; Wilfred Bos also for SID-Device and ACID64, Ken Händel
  for JSIDPlay2.

Developed with AI assistance (Claude).

## License

MIT, see [LICENSE](LICENSE).
