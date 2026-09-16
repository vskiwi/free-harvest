#!/usr/bin/env python3
"""
Replay a Free Harvest capture log (format v2) - on the bench or on the host.

A v2 capture (GET /api/capture) records both halves of the conversation:

    <ms>\t<epoch|->\t<dir>\t<payload>
      dir  >  frame from the dryer        <  frame the adapter sent
           !  adapter event               ?  bytes the parser rejected
           ~  "repeat <dir> <body> xN first..last" run summary
           e  "enc <len> <frame>": a complete encoded frame from a
              6.0.644170 dryer (")S" + length transport), no terminator

This tool reads such a file and can

  * VALIDATE / SUMMARISE it (default, no hardware): column structure, counts
    per direction, verbs seen, STAT screen types, unanswered REQINFO, gaps in
    the millisecond clock (reboots), rejected lines.

  * EXPORT the dryer's side as a plain frame script (--to-frames FILE), one
    frame per line prefixed with its relative time in ms, for host harnesses
    such as tools/host_replay.c.

  * REPLAY the dryer's side over a serial port to a Free Harvest adapter
    (--port /dev/cu.usbmodemXXXX). The Mac plays the dryer: '>' lines are sent
    with their original spacing (scaled by --speed), everything the adapter
    answers is recorded, and each '<' line in the capture is checked against
    what the adapter actually sent at that point. Differences are printed as
    they happen and summarised at the end. Only frames the DRYER sent are ever
    written to the port; the adapter's own frames are compared, never replayed.

Format v1 files ("<ms>\t<body>", inbound only) are accepted too; every line is
treated as '>'.

Examples

    python3 tools/replay_capture.py hr_capture_1757845200.txt
    python3 tools/replay_capture.py cap.txt --to-frames /tmp/frames.txt
    python3 tools/replay_capture.py cap.txt --port /dev/cu.usbmodem1101 --speed 10
"""

import argparse
import collections
import sys
import time

CR = b"\r"


class Line:
    __slots__ = ("ms", "epoch", "dir", "payload", "lineno")

    def __init__(self, ms, epoch, d, payload, lineno):
        self.ms = ms
        self.epoch = epoch
        self.dir = d
        self.payload = payload
        self.lineno = lineno


def parse_capture(path):
    """Yield Line objects; raise ValueError with the line number on bad input."""
    lines = []
    header = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for n, raw in enumerate(f, 1):
            raw = raw.rstrip("\n")
            if not raw:
                continue
            if raw.startswith("#"):
                if raw.startswith("# hr-capture"):
                    header = raw
                continue
            cols = raw.split("\t")
            if len(cols) == 4:
                ms_s, epoch_s, d, payload = cols
                if d not in "><!?~e" or len(d) != 1:
                    raise ValueError(f"line {n}: bad dir column {d!r}")
            elif len(cols) == 2:
                # v1: "<ms>\t<body>"; "~repeat"/"~boot" bodies are summaries
                ms_s, payload = cols
                epoch_s = "-"
                if payload.startswith("~repeat"):
                    d = "~"
                elif payload.startswith("~boot"):
                    d = "!"
                else:
                    d = ">"
            elif len(cols) == 3:
                # the RAM-ring fallback of older builds: "<seq>\t<ms>\t<body>"
                _, ms_s, payload = cols
                epoch_s, d = "-", ">"
            else:
                raise ValueError(f"line {n}: expected 2 or 4 tab columns, got "
                                 f"{len(cols)}")
            try:
                ms = int(ms_s)
            except ValueError:
                raise ValueError(f"line {n}: bad ms {ms_s!r}") from None
            epoch = None if epoch_s == "-" else int(epoch_s)
            lines.append(Line(ms, epoch, d, payload, n))
    return header, lines


def verb_of(payload, d):
    """Verb of a frame: comma-split for the dryer's side, space for ours."""
    if d == ">":
        return payload.split(",", 1)[0]
    return payload.split(" ", 1)[0]


def enc_len_of(payload):
    """Declared length of an 'e' line: 'enc <len> <frame>'."""
    p = payload.split(" ", 2)
    try:
        return int(p[1]) if p[0] == "enc" else 0
    except (IndexError, ValueError):
        return 0


def enc_frame_of(payload):
    """The raw frame of an 'e' line, header included, without the prefix."""
    p = payload.split(" ", 2)
    return p[2] if len(p) == 3 and p[0] == "enc" else ""


def expand_repeats(lines):
    """
    Turn '~ repeat <dir> <body> xN first..last' summaries back into N frames
    spread evenly over [first, last], so the replay keeps the cadence.
    """
    out = []
    for ln in lines:
        if ln.dir != "~":
            out.append(ln)
            continue
        # "repeat > REQINFO, x30 45231..105240"  (v2)
        # "~repeat REQINFO, x30 45231..105240"    (v1)
        p = ln.payload.split(" ")
        try:
            if p[0] == "repeat" and len(p[1]) == 1:
                d, body = p[1], " ".join(p[2:-2])
            else:
                d, body = ">", " ".join(p[1:-2])
            count = int(p[-2].lstrip("x"))
            first_s, last_s = p[-1].split("..")
            first, last = int(first_s), int(last_s)
        except (IndexError, ValueError):
            out.append(ln)
            continue
        if count <= 0:
            continue
        step = (last - first) / count if count > 1 else 0
        for i in range(count):
            out.append(Line(int(first + i * step), ln.epoch, d, body, ln.lineno))
    out.sort(key=lambda l: l.ms)
    return out


def summarise(header, lines, verbose=False):
    counts = collections.Counter(l.dir for l in lines)
    rx_verbs = collections.Counter(verb_of(l.payload, ">") for l in lines
                                   if l.dir == ">")
    tx_verbs = collections.Counter(verb_of(l.payload, "<") for l in lines
                                   if l.dir == "<")
    stat_types = collections.Counter(
        l.payload.split(",")[1] for l in lines
        if l.dir == ">" and l.payload.startswith("STAT,") and
        len(l.payload.split(",")) > 1)
    events = [l for l in lines if l.dir == "!"]
    rejects = [l for l in lines if l.dir == "?"]
    enc_lens = collections.Counter(enc_len_of(l.payload) for l in lines
                                   if l.dir == "e")

    # A clock that goes backwards means a reboot (or several files pasted).
    reboots = 0
    prev = None
    for l in lines:
        if prev is not None and l.ms < prev:
            reboots += 1
        prev = l.ms

    # REQINFO the adapter did not answer within 2 s.
    unanswered = 0
    pending = None
    for l in lines:
        if l.dir == ">" and l.payload.startswith("REQINFO"):
            if pending is not None:
                unanswered += 1
            pending = l.ms
        elif l.dir == "<" and l.payload.startswith("WIFIINFO"):
            pending = None
        elif pending is not None and l.ms - pending > 2000:
            unanswered += 1
            pending = None
    if pending is not None:
        unanswered += 1

    span_ms = (lines[-1].ms - lines[0].ms) if lines else 0
    print(header or "# (no v2 header - v1 or ram-ring capture)")
    print(f"lines: {len(lines)}   span: {span_ms/1000:.1f} s   "
          f"clock resets: {reboots}")
    print(f"  > dryer->adapter : {counts['>']}")
    print(f"  < adapter->dryer : {counts['<']}"
          + ("   (none: v1 capture, TX not recorded)" if counts['<'] == 0 else ""))
    print(f"  ! events         : {counts['!']}")
    print(f"  ? rejected       : {counts['?']}")
    print(f"  ~ repeat runs    : {counts['~']}")
    if counts['e']:
        print(f"  e encoded frames : {counts['e']}   by declared length: "
              + ", ".join(f"{n}x{c}" for n, c in sorted(enc_lens.items())))
    print("dryer verbs : " + ", ".join(f"{v}x{n}" for v, n in rx_verbs.most_common()))
    if tx_verbs:
        print("our verbs   : " + ", ".join(f"{v}x{n}" for v, n in tx_verbs.most_common()))
    if stat_types:
        print("STAT screens: " + ", ".join(f"type {t}x{n}" for t, n in
                                           sorted(stat_types.items(),
                                                  key=lambda kv: int(kv[0]))))
    if counts['<']:
        print(f"REQINFO without a WIFIINFO within 2 s: {unanswered}")
    if events:
        print("events:")
        for e in events[: (None if verbose else 12)]:
            print(f"  {e.ms:>10} ms  {e.payload}")
        if not verbose and len(events) > 12:
            print(f"  ... {len(events) - 12} more (use -v)")
    if rejects:
        print("rejected lines (the undecoded part of the protocol):")
        for r in rejects[: (None if verbose else 12)]:
            print(f"  {r.ms:>10} ms  {r.payload}")
    return 0


def to_frames(lines, path):
    """Write '<rel_ms>\\t<frame>' for the dryer's side, expanding repeats."""
    lines = expand_repeats(lines)
    t0 = lines[0].ms if lines else 0
    n = 0
    with open(path, "w", encoding="utf-8") as f:
        for l in lines:
            if l.dir == ">":
                f.write(f"{l.ms - t0}\t{l.payload}\n")
                n += 1
    print(f"wrote {n} dryer frames to {path}")
    return 0


def replay(lines, port, speed, baud, timeout_s, max_stall_s=0.0):
    """
    Play the dryer's side of a capture into a real adapter.

    Behaves like the dryer, which never waits for anything: frames go out on
    their recorded schedule (scaled by --speed) whether or not the adapter
    has answered, and whatever the adapter sends is read by a separate thread
    so a silent adapter never stalls the sender. The one thing that CAN stall
    the sender is the adapter refusing bytes at USB level (its RX FIFO full
    because its TinyUSB task is not running) - the OS then blocks write().
    That is a firmware fault worth seeing, so writes carry a timeout and every
    stall is reported with its duration instead of hanging the tool.
    """
    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing:  python3 -m pip install pyserial")
    import threading

    lines = expand_repeats(lines)
    # Encoded frames ('e') are the dryer's side too; they go out verbatim
    # and WITHOUT a CR - that transport has no terminator.
    rx = [l for l in lines if l.dir in ">e"]
    expected_tx = [l for l in lines if l.dir == "<"]
    if not rx:
        sys.exit("nothing to replay: the capture has no '>' or 'e' lines")

    WRITE_TIMEOUT = 1.0
    ser = serial.serial_for_url(port, baudrate=baud, timeout=0.05,
                                write_timeout=WRITE_TIMEOUT)
    time.sleep(0.3)
    ser.reset_input_buffer()

    got_tx = []          # (wall_ms, text) what the adapter actually sent
    t_wall0 = time.time()
    t_cap0 = rx[0].ms
    stop = threading.Event()
    lock = threading.Lock()

    def wall_ms():
        return int((time.time() - t_wall0) * 1000)

    def reader():
        buf = b""
        while not stop.is_set():
            try:
                chunk = ser.read(512)
            except Exception as e:
                if not stop.is_set():
                    print(f"   (port read error: {e})")
                return
            if not chunk:
                continue
            buf += chunk
            while CR in buf:
                line, buf = buf.split(CR, 1)
                text = line.decode("ascii", "replace").strip()
                if text:
                    ms = wall_ms()
                    with lock:
                        got_tx.append((ms, text))
                        print(f"{ms:>9} <- {text}")

    rd = threading.Thread(target=reader, name="adapter-rx", daemon=True)
    rd.start()

    stalls = []          # (frame_index, seconds) writes the adapter refused
    print(f"replaying {len(rx)} dryer frames over {port} at {speed}x; "
          f"{len(expected_tx)} adapter frames to compare against")
    for i, l in enumerate(rx):
        due = (l.ms - t_cap0) / 1000.0 / speed
        wait = due - (time.time() - t_wall0)
        if wait > 0:
            time.sleep(wait)
        if l.dir == "e":
            raw = enc_frame_of(l.payload).encode("ascii", "replace")
        else:
            raw = l.payload.encode("ascii", "replace") + CR
        t_w = time.time()
        while True:
            try:
                ser.write(raw)
                ser.flush()
                break
            except serial.SerialTimeoutException:
                stalled = time.time() - t_w
                if stalled < 2 * WRITE_TIMEOUT:
                    with lock:
                        print(f"{wall_ms():>9} !! write stalled: the adapter is "
                              f"not accepting bytes (frame {i + 1}: {l.payload})")
                if max_stall_s and stalled >= max_stall_s:
                    with lock:
                        print(f"{wall_ms():>9} !! giving up after {stalled:.1f}s "
                              f"(--max-stall)")
                    stalls.append((i, stalled))
                    stop.set()
                    ser.close()
                    return 3
        stalled = time.time() - t_w
        if stalled >= WRITE_TIMEOUT:
            stalls.append((i, stalled))
            with lock:
                print(f"{wall_ms():>9} !! adapter accepted bytes again after "
                      f"{stalled:.1f}s")
        with lock:
            late = (time.time() - t_wall0) - due
            tag = f"  (late {late:.1f}s)" if late > 1.0 else ""
            print(f"{wall_ms():>9} -> {l.payload}{tag}")
    # let the adapter finish answering
    time.sleep(timeout_s)
    stop.set()
    ser.close()
    rd.join(timeout=1.0)

    if stalls:
        worst = max(s for _, s in stalls)
        print()
        print(f"=== USB write stalls: {len(stalls)} (longest {worst:.1f}s) - the "
              f"adapter stopped draining its RX FIFO; a real dryer would have "
              f"given up on the link ===")

    # Compare by verb sequence, ignoring fields that legitimately differ
    # (uptime, RSSI, counters) - a field-exact match would fail on every
    # WIFIINFO. Report missing and unexpected verbs.
    want = collections.Counter(verb_of(l.payload, "<") for l in expected_tx)
    have = collections.Counter(verb_of(t, "<") for _, t in got_tx)
    print()
    print("=== adapter-side comparison (by verb) ===")
    all_verbs = sorted(set(want) | set(have))
    mismatch = 0
    for v in all_verbs:
        flag = "" if want[v] == have[v] else "  <-- differs"
        if flag:
            mismatch += 1
        print(f"  {v:<12} capture {want[v]:>4}   live {have[v]:>4}{flag}")
    if not expected_tx:
        print("  (capture has no adapter frames - v1 file; live side listed only)")
    print(f"{'OK' if mismatch == 0 else 'DIFFERENCES: %d verbs' % mismatch}")
    if stalls:
        return 3
    return 0 if mismatch == 0 else 2


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="capture file from GET /api/capture")
    ap.add_argument("--to-frames", metavar="FILE",
                    help="write the dryer's frames as '<rel_ms>\\t<frame>'")
    ap.add_argument("--port", help="serial port of a Free Harvest adapter "
                                   "(the Mac plays the dryer)")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="time compression for --port (10 = ten times faster)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to keep listening after the last frame")
    ap.add_argument("--max-stall", type=float, default=0.0, metavar="SEC",
                    help="abort when the adapter refuses bytes for this long "
                         "(0 = keep waiting and report the stall)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    try:
        header, lines = parse_capture(args.capture)
    except ValueError as e:
        sys.exit(f"{args.capture}: {e}")
    if not lines:
        sys.exit(f"{args.capture}: no frames")

    if args.to_frames:
        return to_frames(lines, args.to_frames)
    if args.port:
        return replay(lines, args.port, args.speed, args.baud, args.settle,
                      args.max_stall)
    return summarise(header, lines, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
