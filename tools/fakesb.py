#!/usr/bin/env python3
# Fake SIDBlaster on a pty, for testing HardSID.dll / SBTEST.EXE under Wine.
# usage: fakesb.py <link-path, e.g. ~/.wine/dosdevices/com5> <logfile>
# Logs every write/read/reset it receives; answers reads of $1B with random bytes.
import os, pty, sys, time, random, tty
master, slave = pty.openpty()
tty.setraw(master)
name = os.ttyname(slave)
link = sys.argv[1]
if os.path.lexists(link): os.unlink(link)
os.symlink(name, link)
log = open(sys.argv[2], 'w', buffering=1)
log.write(f"pty {name}\n")
t0 = time.time()
pending = None
buf = b''
while True:
    try:
        data = os.read(master, 4096)
    except OSError:
        time.sleep(0.01); continue
    for b in data:
        if pending is not None:
            log.write(f"{time.time()-t0:9.4f} W ${pending:02X}={b:02X}\n")
            pending = None
            continue
        if not (b & 0x80):
            log.write(f"{time.time()-t0:9.4f} junk {b:02X}\n"); continue
        reg = b & 0x1F
        if not (b & 0x20):
            log.write(f"{time.time()-t0:9.4f} RESET-low\n")
        if b & 0x40:
            pending = reg
        else:
            v = random.randrange(256) if reg == 0x1B else 0
            os.write(master, bytes([v]))
            log.write(f"{time.time()-t0:9.4f} R ${reg:02X} -> {v:02X}\n")
