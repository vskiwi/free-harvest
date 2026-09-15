#!/usr/bin/env python3
"""
A pretend Free Harvest adapter on a pseudo-terminal, for testing
tools/replay_capture.py --port without hardware.

Opens a pty pair, prints the slave device (e.g. /dev/ttys012), and behaves
like the adapter's outbound side well enough for the replay tool's verb
comparison:

  * REQINFO,                 -> WIFIINFO 1 0 "" 0 HR_000000000000 0 0 <s>
  * first inbound frame      -> hello burst STATE / UNIQUE / FDNAME / REQCFG / STATUS
  * every --heartbeat s      -> STATE 1 0   (default 15 s, scale with --speed)

Everything received is echoed to stderr. Stops after --duration seconds or
when the peer closes.

    python3 tools/fake_adapter_pty.py --speed 20 &
    python3 tools/replay_capture.py test/fixtures/capture-v2-sample.txt \\
        --port /dev/ttysNNN --speed 20 --settle 1

This exists to prove the replay tool's serial path (open, paced writes, read
back, compare) on a Mac with nothing plugged in. It is NOT a dryer or an
adapter simulator; see tools/mock_dryer.py and tools/host_replay.c.
"""

import argparse
import os
import select
import sys
import time

CR = b"\r"
HELLO = [b"STATE 1 0", b"UNIQUE", b"FDNAME", b"REQCFG", b"STATUS"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--speed", type=float, default=1.0,
                    help="same time compression as the replay tool")
    ap.add_argument("--heartbeat", type=float, default=15.0,
                    help="STATE interval in capture seconds")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="wall-clock seconds to run before exiting")
    ap.add_argument("--name-file", help="also write the slave path here")
    args = ap.parse_args()

    master, slave = os.openpty()
    name = os.ttyname(slave)
    print(name, flush=True)
    if args.name_file:
        with open(args.name_file, "w") as f:
            f.write(name + "\n")

    def send(frame):
        os.write(master, frame + CR)
        print(f"   fake-adapter -> {frame.decode()}", file=sys.stderr, flush=True)

    t0 = time.time()
    said_hello = False
    last_beat = None
    beat = args.heartbeat / args.speed
    buf = b""
    frames_in = 0
    end = t0 + args.duration
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try:
                chunk = os.read(master, 512)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while CR in buf:
                line, buf = buf.split(CR, 1)
                line = line.strip(b"\n")
                if not line:
                    continue
                frames_in += 1
                print(f"   fake-adapter <- {line.decode('ascii', 'replace')}",
                      file=sys.stderr, flush=True)
                if line.startswith(b"REQINFO"):
                    up = int(time.time() - t0)
                    send(b'WIFIINFO 1 0 "" 0 HR_000000000000 0 0 %d' % up)
                if not said_hello:
                    said_hello = True
                    for h in HELLO:
                        send(h)
                    last_beat = time.time()
        if said_hello and time.time() - last_beat >= beat:
            last_beat = time.time()
            send(b"STATE 1 0")
    print(f"   fake-adapter: {frames_in} frames received, exiting",
          file=sys.stderr, flush=True)
    os.close(master)
    os.close(slave)
    return 0


if __name__ == "__main__":
    sys.exit(main())
