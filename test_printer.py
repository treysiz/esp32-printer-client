#!/usr/bin/env python3
"""
Mock ESC/POS thermal printer on TCP 9100, for testing the firmware's paper
detection without a real printer (and without pulling a real paper roll).

Pair it with test_server.py (mock backend) and you have the whole loop on one
laptop: real ESP32 + fake backend + fake printer.

===========================================================================
 !!  THE SENSOR BIT LAYOUT IN THIS FILE IS AN ASSUMPTION, NOT THE TRUTH  !!
===========================================================================
  本文件的位定义是假设，用于验证固件的逻辑分支，不能替代真机核对。
  上板前必须做一次：取纸读一个字节、装纸读一个字节，看实际哪一位翻转。

  This mock encodes paper-out as bits 5,6 and near-end as bits 2,3 (the EPSON
  standard) -- the SAME assumption main/printer.c makes. So if the firmware's
  masks are wrong for YOUR printer, these tests still go green. That green is
  worthless for the bit layout question.

  What this mock DOES prove is that the firmware's logic branches are correct:
  that paper-out reports /failed, that a refill triggers a reprint, and above
  all that an unresponsive printer still gets printed to (fail-open). Those are
  layout-independent, and fail-open is the one you cannot test any other way --
  you would have to go buy a printer that doesn't support DLE EOT.

  Before flashing for real, do this once against the actual machine:
      1. take the roll out, read one status byte
      2. put the roll back, read one status byte
      3. see which bit actually flipped
  Enable debug logging on the firmware and watch for:
      PRINTER: paper query: status byte 0xXX -> yyy
===========================================================================

Modes (--mode):

    paper       has paper, answers DLE EOT normally           -> normal path
    no-paper    out of paper from the start                   -> /failed, then
                                                                 refill (Enter)
                                                                 and it reprints
    runs-out    has paper, runs out mid-payload (--after N)   -> short receipt,
                                                                 then a full one
    silent      never answers DLE EOT at all                  -> FAIL-OPEN
    garbage     answers a byte that violates the fixed bits   -> FAIL-OPEN

`silent` and `garbage` are the two halves of acceptance criterion 5: both must
end with the payload printed anyway. If either one stops printing, the
fail-open path is broken -- which is worse than the bug this feature fixes,
because it takes out every printer that doesn't implement DLE EOT.

Usage:
    python3 test_printer.py                          # mode=paper, port 9100
    python3 test_printer.py --mode no-paper
    python3 test_printer.py --mode runs-out --after 4096
    python3 test_printer.py --mode silent
    python3 test_printer.py --mode garbage
    python3 test_printer.py --dump last.bin          # save received payload

Press Enter in the console to toggle paper in/out at runtime -- that is how you
test "put the roll back and the receipt prints itself" without restarting.
"""
import argparse
import socket
import sys
import threading

# ── The assumed status-byte layout (see the warning above) ──────────────
STATUS_FIXED_BITS = 0x12   # bit 1 and bit 4 are always set...
STATUS_FIXED_MASK = 0x93   # ...and bit 0 and bit 7 always clear
PAPER_END_BITS = 0x60      # bits 5,6 -> out of paper
NEAR_END_BITS = 0x0C       # bits 2,3 -> roll running low

DLE = 0x10
EOT = 0x04

STATE = {
    "mode": "paper",
    "paper_out": False,
    "near_end": False,
    "runs_out_after": 4096,
    "runs_out_fired": False,   # a roll runs out ONCE, not on every connection
    "dump_path": None,
}
LOCK = threading.Lock()


def status_byte():
    """Build the DLE EOT 4 reply for the current paper state."""
    b = STATUS_FIXED_BITS
    if STATE["paper_out"]:
        b |= PAPER_END_BITS
    if STATE["near_end"]:
        b |= NEAR_END_BITS
    return b


def describe(b):
    bits = []
    if (b & PAPER_END_BITS) == PAPER_END_BITS:
        bits.append("PAPER-OUT")
    if (b & NEAR_END_BITS) == NEAR_END_BITS:
        bits.append("near-end")
    if (b & STATUS_FIXED_MASK) != STATUS_FIXED_BITS:
        bits.append("MALFORMED(fixed bits wrong)")
    return "0x%02X %s" % (b, "[" + ",".join(bits) + "]" if bits else "[paper ok]")


class Session(threading.Thread):
    """One TCP connection. Scans the byte stream for real-time DLE EOT n
    queries and answers them; everything else counts as print payload.

    Scanning the whole stream is faithful, not lazy: DLE EOT is a REAL-TIME
    command, handled by the printer the moment the bytes arrive rather than
    queued behind the data being printed. It also means a raster payload that
    happens to contain 0x10 0x04 0x04 will draw an extra status reply out of a
    real printer, exactly as it does here.
    """

    def __init__(self, conn, addr, n):
        threading.Thread.__init__(self, daemon=True)
        self.conn = conn
        self.addr = addr
        self.n = n
        self.payload = bytearray()
        self.queries = 0
        self.replies = 0

    def reply_to_query(self, arg):
        self.queries += 1
        mode = STATE["mode"]

        if mode == "silent":
            print("  [#%d] DLE EOT %d  -> (silent: no reply, printer does not "
                  "support it)" % (self.n, arg))
            return

        if mode == "garbage":
            # Violates the fixed bits on purpose: bit 0 set, bit 4 clear.
            bad = 0xA1
            self.conn.sendall(bytes([bad]))
            self.replies += 1
            print("  [#%d] DLE EOT %d  -> %s  (garbage on purpose)"
                  % (self.n, arg, describe(bad)))
            return

        if arg != 4:
            # Other real-time queries: answer something well-formed but dull.
            self.conn.sendall(bytes([STATUS_FIXED_BITS]))
            self.replies += 1
            print("  [#%d] DLE EOT %d  -> 0x%02X (not the paper query)"
                  % (self.n, arg, STATUS_FIXED_BITS))
            return

        with LOCK:
            b = status_byte()
        self.conn.sendall(bytes([b]))
        self.replies += 1
        print("  [#%d] DLE EOT 4  -> %s" % (self.n, describe(b)))

    def note_payload_byte(self):
        """Called per payload byte; fires the runs-out trip point.

        Fires ONCE for the whole run, not once per connection: a roll runs out
        a single time, and after you refill it the reprint has to succeed --
        which is the whole point of the test.
        """
        if STATE["mode"] != "runs-out":
            return
        with LOCK:
            if STATE["runs_out_fired"]:
                return
            if len(self.payload) < STATE["runs_out_after"]:
                return
            STATE["runs_out_fired"] = True
            STATE["paper_out"] = True
        print("  [#%d] *** paper ran out after %d payload bytes ***"
              % (self.n, len(self.payload)))
        print("      (expect a SHORT receipt; press Enter to refill, then "
              "the backend should re-send a full one)")

    def run(self):
        print("[conn #%d] from %s:%d" % (self.n, self.addr[0], self.addr[1]))
        # 0 = normal, 1 = saw DLE, 2 = saw DLE EOT (next byte is the argument)
        state = 0
        try:
            while True:
                chunk = self.conn.recv(4096)
                if not chunk:
                    break
                for byte in chunk:
                    if state == 0:
                        if byte == DLE:
                            state = 1
                        else:
                            self.payload.append(byte)
                            self.note_payload_byte()
                    elif state == 1:
                        if byte == EOT:
                            state = 2
                        elif byte == DLE:
                            # previous DLE was just data; stay armed on this one
                            self.payload.append(DLE)
                            self.note_payload_byte()
                        else:
                            self.payload.append(DLE)
                            self.payload.append(byte)
                            self.note_payload_byte()
                            state = 0
                    else:  # state == 2
                        self.reply_to_query(byte)
                        state = 0
        except OSError as e:
            print("[conn #%d] socket error: %s" % (self.n, e))
        finally:
            if state != 0:   # trailing partial sequence was really data
                self.payload.extend(b"\x10" if state == 1 else b"\x10\x04")
            try:
                self.conn.close()
            except OSError:
                pass
            self.report()

    def report(self):
        p = self.payload
        kind = "raster(GS v 0)" if b"\x1d\x76\x30" in p else "text/other"
        print("[conn #%d] closed: %d payload bytes, %s, %d DLE EOT quer%s, "
              "%d repl%s"
              % (self.n, len(p), kind,
                 self.queries, "y" if self.queries == 1 else "ies",
                 self.replies, "y" if self.replies == 1 else "ies"))
        if self.queries and not self.replies:
            if p:
                print("      -> firmware got NO status and printed anyway "
                      "(%d bytes): FAIL-OPEN WORKS." % len(p))
            else:
                print("      -> firmware got NO status and sent NOTHING: "
                      "FAIL-OPEN IS BROKEN.")
        if STATE["dump_path"] and p:
            with open(STATE["dump_path"], "wb") as f:
                f.write(bytes(p))
            print("      -> payload written to %s (compare against the "
                  "backend's bytes to prove byte-losslessness end to end)"
                  % STATE["dump_path"])


def console():
    while True:
        try:
            cmd = input("Enter=toggle paper, n=toggle near-end, s=status, "
                        "q=quit: ").strip().lower()
        except EOFError:
            return
        if cmd == "q":
            import os
            os._exit(0)
        with LOCK:
            if cmd == "n":
                STATE["near_end"] = not STATE["near_end"]
            elif cmd != "s":
                STATE["paper_out"] = not STATE["paper_out"]
            b = status_byte()
        print("  paper_out=%s near_end=%s -> next DLE EOT 4 answers %s"
              % (STATE["paper_out"], STATE["near_end"], describe(b)))
        if STATE["mode"] in ("silent", "garbage"):
            print("  (mode=%s, so the query is not answered honestly anyway)"
                  % STATE["mode"])


def main():
    ap = argparse.ArgumentParser(
        description="Mock ESC/POS printer for paper-detection testing.")
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--mode", default="paper",
                    choices=["paper", "no-paper", "runs-out", "silent", "garbage"])
    ap.add_argument("--after", type=int, default=4096,
                    help="runs-out mode: trip after this many payload bytes")
    ap.add_argument("--near-end", action="store_true",
                    help="start with the near-end sensor tripped")
    ap.add_argument("--dump", metavar="FILE",
                    help="write each connection's payload to FILE")
    args = ap.parse_args()

    STATE["mode"] = args.mode
    STATE["paper_out"] = (args.mode == "no-paper")
    STATE["near_end"] = args.near_end
    STATE["runs_out_after"] = args.after
    STATE["dump_path"] = args.dump

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(5)

    print("Mock printer on 0.0.0.0:%d  mode=%s" % (args.port, args.mode))
    if args.mode == "runs-out":
        print("  will run out of paper after %d payload bytes" % args.after)
    if args.mode in ("silent", "garbage"):
        print("  *** ACCEPTANCE CRITERION 5: the firmware must print ANYWAY. ***")
        print("  *** If nothing arrives, fail-open is broken.               ***")
    print("  reminder: the bit layout here is an ASSUMPTION -- verify against "
          "the real printer before flashing (see the header of this file)")

    threading.Thread(target=console, daemon=True).start()

    n = 0
    try:
        while True:
            conn, addr = srv.accept()
            n += 1
            Session(conn, addr, n).start()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
