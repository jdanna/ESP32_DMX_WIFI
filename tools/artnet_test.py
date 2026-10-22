#!/usr/bin/env python3
"""Send Art-Net test data to the DMX Relay Node. No dependencies beyond Python 3.

Examples:
  python3 tools/artnet_test.py 10.0.0.179 --universe 10            # poll, then chase relays 1-8
  python3 tools/artnet_test.py 10.0.0.179 --universe 10 poll       # only check the node answers
  python3 tools/artnet_test.py 10.0.0.179 --universe 10 on         # hold all 8 channels at 255
  python3 tools/artnet_test.py 10.0.0.179 --universe 10 off
  python3 tools/artnet_test.py 10.0.0.179 --universe 10 set 1=255 3=255
  python3 tools/artnet_test.py 10.0.0.179 --universe 10 --start 101 chase

Keep it running while you watch the device: Art-Net counts as lost a few seconds
after packets stop. Ctrl+C sends all-zero and exits.
"""
import argparse
import socket
import struct
import sys
import time

PORT = 6454
HEADER = b"Art-Net\x00"
OP_POLL, OP_POLL_REPLY, OP_DMX = 0x2000, 0x2100, 0x5000
RATE_HZ = 30


def art_poll():
    # ID, OpCode (LE), ProtVer 14 (BE), Flags, DiagPriority
    return HEADER + struct.pack("<H", OP_POLL) + bytes([0, 14, 0, 0])


def art_dmx(universe, seq, data):
    data = bytes(data) + b"\x00" * (len(data) % 2)  # length must be even
    return (HEADER + struct.pack("<H", OP_DMX) + bytes([0, 14, seq, 0])
            + bytes([universe & 0xFF, (universe >> 8) & 0x7F])  # SubUni, Net
            + struct.pack(">H", len(data)) + data)


def open_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        s.bind(("", PORT))  # the node replies to port 6454, not our source port
        return s, True
    except OSError as e:
        print(f"! Can't listen on UDP {PORT} ({e}); another Art-Net app may be running.")
        print("  Sending will still work, but poll replies can't be shown.")
        s.close()
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        return s, False


def poll(sock, host, listening):
    print(f"> ArtPoll -> {host}:{PORT}")
    sock.sendto(art_poll(), (host, PORT))
    if not listening:
        return False
    sock.settimeout(0.25)
    deadline = time.time() + 2
    while time.time() < deadline:
        try:
            pkt, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue
        if len(pkt) < 200 or not pkt.startswith(HEADER) or struct.unpack_from("<H", pkt, 8)[0] != OP_POLL_REPLY:
            continue
        ip = ".".join(str(b) for b in pkt[10:14])
        short = pkt[26:44].split(b"\x00")[0].decode(errors="replace")
        report = pkt[108:172].split(b"\x00")[0].decode(errors="replace")
        port_addr = (pkt[18] << 8) | (pkt[19] << 4) | (pkt[190] & 0x0F)
        print(f"< ArtPollReply from {addr[0]}: name={short!r} ip={ip} universe={port_addr}")
        print(f"  report: {report}")
        return True
    print("< No ArtPollReply within 2 s. Check the IP, that you're on the same network,")
    print("  and that no firewall blocks UDP 6454 inbound on this computer.")
    return False


class Sender:
    def __init__(self, sock, host, universe, start):
        self.sock, self.host, self.universe, self.start = sock, host, universe, start
        self.frame = bytearray(512)
        self.seq = 1

    def set_relays(self, values):
        """values: 8 channel values for the node's range (relay slots 1-8)."""
        for i, v in enumerate(values):
            self.frame[self.start - 1 + i] = v

    def send(self):
        self.sock.sendto(art_dmx(self.universe, self.seq, self.frame), (self.host, PORT))
        self.seq = self.seq % 255 + 1  # 0 means "sequencing disabled"

    def hold(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.send()
            time.sleep(1 / RATE_HZ)


def show(start, values):
    cells = " ".join(f"{start + i:03d}:{v:3d}" for i, v in enumerate(values))
    print(f"  {cells}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="device IP, e.g. 10.0.0.179")
    ap.add_argument("--universe", type=int, default=0, help="Art-Net port-address 0-32767 (default 0)")
    ap.add_argument("--start", type=int, default=1, help="start channel set on the device (default 1)")
    ap.add_argument("--step", type=float, default=1.0, help="seconds per chase step (default 1)")
    ap.add_argument("mode", nargs="?", default="test", choices=["test", "poll", "chase", "on", "off", "set"])
    ap.add_argument("values", nargs="*", help="for 'set': CH=VAL pairs, CH is 1-8 within the range")
    args = ap.parse_intermixed_args()

    if not 0 <= args.universe <= 32767 or not 1 <= args.start <= 505:
        sys.exit("universe must be 0-32767 and start 1-505")

    sock, listening = open_socket()
    u = args.universe
    print(f"Target {args.host}  universe {u} (Net {u >> 8} / Sub-Net {(u >> 4) & 15} / Universe {u & 15})"
          f"  channels {args.start:03d}-{args.start + 7:03d}")

    if args.mode in ("test", "poll"):
        poll(sock, args.host, listening)
        if args.mode == "poll":
            return

    tx = Sender(sock, args.host, u, args.start)
    try:
        if args.mode in ("test", "chase"):
            print(f"> Chasing relays 1-8 at {RATE_HZ} packets/s (Ctrl+C to stop)")
            while True:
                for i in range(8):
                    values = [255 if j == i else 0 for j in range(8)]
                    tx.set_relays(values)
                    show(args.start, values)
                    tx.hold(args.step)
                if args.mode == "test":
                    tx.set_relays([255] * 8)
                    print("  all on")
                    tx.hold(args.step * 2)
                    tx.set_relays([0] * 8)
                    print("  all off")
                    tx.hold(args.step * 2)
                    break
        else:
            values = [0] * 8
            if args.mode == "on":
                values = [255] * 8
            elif args.mode == "set":
                for pair in args.values:
                    ch, _, val = pair.partition("=")
                    if not (ch.isdigit() and val.isdigit() and 1 <= int(ch) <= 8 and 0 <= int(val) <= 255):
                        sys.exit(f"bad value {pair!r}: use CH=VAL with CH 1-8, VAL 0-255")
                    values[int(ch) - 1] = int(val)
            tx.set_relays(values)
            show(args.start, values)
            print(f"> Sending at {RATE_HZ} packets/s (Ctrl+C to stop)")
            while True:
                tx.hold(1)
    except KeyboardInterrupt:
        pass
    finally:
        tx.set_relays([0] * 8)
        for _ in range(3):
            tx.send()
        print("\n> Sent zeros, done.")


if __name__ == "__main__":
    main()
