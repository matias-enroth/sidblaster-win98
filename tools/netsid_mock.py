#!/usr/bin/env python3
"""Mock Network SID Device server, for testing HardSID.dll's NetSID backend
and SBTEST NET without a real server.

It answers the protocol subset the DLL uses and models the server's write
queue: queued delays drain at the C64 clock rate (the queue stalls when it
runs dry, like a real server), and TRY_WRITE / TRY_DELAY / TRY_READ get BUSY
while more than --cap-ms is queued. Each accepted write is logged with the
cycle it plays at, when it plays and when it arrived, and the lead left:

    cycle        play_s    recv_s     lead  what
    98525        1.1004    1.0002   100.2ms  W $18=0F

Underruns (the queue ran dry while writes kept coming) are logged as DRY;
a once-a-second summary goes to stderr.

    tools/netsid_mock.py                      # listen on 0.0.0.0:6581
    tools/netsid_mock.py --log mock.log --cap-ms 200 --start-ms 100

--start-ms N mimics SID-Device, which only starts playing once N ms are queued.
It also warns about requests split across recv() calls or pipelined into one:
SID-Device handles each recv() as exactly one request.
"""
import argparse, random, socket, sys, threading, time

PAL, NTSC = 985248, 1022727
CMD = {0: 'FLUSH', 1: 'TRY_SET_SID_COUNT', 2: 'MUTE', 3: 'TRY_RESET', 4: 'TRY_DELAY',
       5: 'TRY_WRITE', 6: 'TRY_READ', 7: 'GET_VERSION', 8: 'TRY_SET_SAMPLING',
       9: 'SET_CLOCKING', 10: 'GET_CONFIG_COUNT', 11: 'GET_CONFIG_INFO', 12: 'SET_SID_POSITION',
       13: 'SET_SID_LEVEL', 14: 'SET_SID_MODEL'}
OK, BUSY, ERROR, READ, VERSION, COUNT, INFO = range(7)
CONFIGS = [(0, b'Mock 6581'), (1, b'Mock 8580')]


class Conn:
    def __init__(self, sock, peer, args, log):
        self.s, self.peer, self.a, self.log = sock, peer, args, log
        self.clk = PAL
        self.t0 = time.monotonic()
        self.buffered = 0.0          # cycles queued, not yet played
        self.last = self.t0
        self.started = args.start_ms == 0
        self.timeline = 0            # cycles since connect (sum of all delays)
        self.playing = False         # had data since the last FLUSH/stop
        self.sec = {'writes': 0, 'busy': 0, 'dry': 0, 'pkts': 0, 'lead_min': None, 'lead_max': 0.0}
        self.total = {'writes': 0, 'busy': 0, 'dry': 0, 'pkts': 0}
        self.sec_t = self.t0

    def out(self, line):
        self.log.write(line + '\n')

    def now(self):
        return time.monotonic() - self.t0

    def drain(self):
        """Play the queue up to now; log an underrun if it ran dry."""
        t = time.monotonic()
        if self.started and self.buffered > 0:
            used = (t - self.last) * self.clk
            if used >= self.buffered:
                dry_at = self.last + self.buffered / self.clk - self.t0
                self.buffered = 0.0
                if self.playing:
                    self.out(f'{self.timeline:12d} {dry_at:9.4f}           DRY (queue empty)')
                    self.sec['dry'] += 1
                    self.total['dry'] += 1
                    self.playing = False
            else:
                self.buffered -= used
        self.last = t

    def lead_ms(self):
        return self.buffered / self.clk * 1000

    def queue(self, delay, what):
        self.buffered += delay
        self.timeline += delay
        if not self.started and self.lead_ms() >= self.a.start_ms:
            self.started = True
            self.last = time.monotonic()
        play = self.now() + self.buffered / self.clk
        lead = self.lead_ms()
        if what is not None:
            self.playing = True
            self.out(f'{self.timeline:12d} {play:9.4f} {self.now():9.4f} {lead:7.1f}ms  {what}')
            self.sec['writes'] += 1
            self.total['writes'] += 1
            mn = self.sec['lead_min']
            self.sec['lead_min'] = lead if mn is None else min(mn, lead)
            self.sec['lead_max'] = max(self.sec['lead_max'], lead)
        elif self.a.verbose:
            self.out(f'{self.timeline:12d} {play:9.4f} {self.now():9.4f} {lead:7.1f}ms  delay {delay}')

    def full(self):
        self.drain()
        if self.lead_ms() > self.a.cap_ms:
            self.sec['busy'] += 1
            self.total['busy'] += 1
            return True
        return False

    def summary(self, force=False):
        t = time.monotonic()
        if not force and t - self.sec_t < 1.0:
            return
        s = self.sec
        if s['pkts'] or force:
            lead = (f"lead {s['lead_min']:.0f}-{s['lead_max']:.0f}ms"
                    if s['lead_min'] is not None else 'no writes')
            print(f"[{self.peer}] {t - self.t0:7.1f}s pkts={s['pkts']} writes={s['writes']} "
                  f"busy={s['busy']} dry={s['dry']} {lead} queued={self.lead_ms():.0f}ms",
                  file=sys.stderr)
        self.sec = {'writes': 0, 'busy': 0, 'dry': 0, 'pkts': 0, 'lead_min': None, 'lead_max': 0.0}
        self.sec_t = t

    def handle(self, cmd, sid, data):
        name = CMD.get(cmd, f'cmd{cmd}')
        self.sec['pkts'] += 1
        self.total['pkts'] += 1
        if cmd in (5, 6):
            n = len(data) - (3 if cmd == 6 else 0)
            if n % 4 or n < 0:
                self.out(f'ERROR: {name} with {len(data)} data bytes')
                return bytes([ERROR]) + b'bad length'
            if self.full():
                return bytes([BUSY])
            for i in range(0, n, 4):
                d = data[i] << 8 | data[i + 1]
                self.queue(d, f'W ${data[i + 2]:02X}={data[i + 3]:02X}')
            if cmd == 5:
                return bytes([OK])
            d = data[n] << 8 | data[n + 1]
            reg = data[n + 2]
            self.queue(d, f'R ${reg:02X}')
            return bytes([READ, random.randrange(256) if reg == 0x1B else 0])
        if cmd == 4:
            if len(data) != 2 or (data[0] << 8 | data[1]) == 0:
                self.out(f'ERROR: TRY_DELAY {data.hex()}')
                return bytes([ERROR])
            if self.full():
                return bytes([BUSY])
            self.queue(data[0] << 8 | data[1], None)
            return bytes([OK])
        self.drain()
        self.out(f'{self.timeline:12d} {"":9s} {self.now():9.4f} {self.lead_ms():7.1f}ms  '
                 f'{name} sid={sid} {data.hex()}')
        if cmd == 0:
            self.buffered = 0.0
            self.playing = False
            self.started = self.a.start_ms == 0
            return bytes([OK])
        if cmd == 1:
            self.buffered = 0.0
            self.started = self.a.start_ms == 0
            return bytes([OK])
        if cmd == 7:
            return bytes([VERSION, self.a.version])
        if cmd == 9:
            if len(data) != 1:
                return bytes([ERROR])
            self.clk = NTSC if data[0] else PAL
            return bytes([OK])
        if cmd == 10:
            return bytes([COUNT, len(CONFIGS)])
        if cmd == 11:
            if sid >= len(CONFIGS):
                return bytes([ERROR]) + b'no such config'
            model, label = CONFIGS[sid]
            return bytes([INFO, model]) + label + b'\0'
        if cmd == 2:
            if len(data) != 2 or data[0] > (3 if self.a.version >= 3 else 2):
                return bytes([ERROR]) + b'bad voice'
            return bytes([OK])
        if cmd in (3, 8, 14):
            return bytes([OK]) if len(data) == 1 else bytes([ERROR])
        return bytes([ERROR]) + b'unknown command'

    def run(self):
        self.out(f'# connection from {self.peer}')
        buf = b''
        self.s.settimeout(0.25)
        while True:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                self.drain()
                self.summary()
                continue
            except OSError:
                break
            if not chunk:
                break
            if buf:
                self.out(f'WARN: request split across recv() calls (SID-Device would reject it)')
            buf += chunk
            npk = 0
            while len(buf) >= 4:
                n = buf[2] << 8 | buf[3]
                if len(buf) < 4 + n:
                    break
                cmd, sid, data, buf = buf[0], buf[1], buf[4:4 + n], buf[4 + n:]
                npk += 1
                resp = self.handle(cmd, sid, data)
                if self.a.slow_ms:
                    time.sleep(self.a.slow_ms / 1000)
                self.s.sendall(resp)
            if npk > 1:
                self.out(f'WARN: {npk} requests in one recv() (pipelined; SID-Device would drop all but one)')
            self.summary()
        self.drain()
        self.summary(force=True)
        t = self.total
        self.out(f'# closed: {t["pkts"]} packets, {t["writes"]} writes, {t["busy"]} BUSY, {t["dry"]} DRY')
        print(f'[{self.peer}] closed: {t["pkts"]} packets, {t["writes"]} writes, '
              f'{t["busy"]} BUSY, {t["dry"]} DRY', file=sys.stderr)
        self.log.flush()
        self.s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--host', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=6581)
    ap.add_argument('--cap-ms', type=float, default=200, help='BUSY above this much queued (200)')
    ap.add_argument('--start-ms', type=float, default=0, help='start playing once this much is queued')
    ap.add_argument('--version', type=int, default=5, help='protocol version to report (5)')
    ap.add_argument('--slow-ms', type=float, default=0, help='delay every response by this much')
    ap.add_argument('--log', help='write the event log here (default: stdout)')
    ap.add_argument('--verbose', action='store_true', help='log TRY_DELAYs too')
    a = ap.parse_args()
    log = open(a.log, 'w', buffering=1) if a.log else sys.stdout
    log.write('#      cycle    play_s    recv_s     lead  what\n')
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(4)
    print(f'netsid_mock listening on {a.host}:{a.port}', file=sys.stderr, flush=True)
    lock = threading.Lock()

    def serve(c, peer):
        with lock:      # one client at a time, like a real device
            Conn(c, f'{peer[0]}:{peer[1]}', a, log).run()

    while True:
        c, peer = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=serve, args=(c, peer), daemon=True).start()


if __name__ == '__main__':
    main()
