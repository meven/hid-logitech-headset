#!/usr/bin/env python3
"""Ask a Logitech G PRO X 2 LIGHTSPEED receiver for its headset battery, over hidraw.

The packet layout is HeadsetControl's, from lib/devices/logitech_gpro_x2_lightspeed.hpp: the
vendor collection on usage page 0xffa0, report 0x51, 64 byte frames. This is the legacy request,
which is a single write, rather than the Centurion feature dance that needs discovery first.

The driver sends the same request; running this while it is bound also refreshes its reading.
"""

import argparse
import os
import select
import sys

REPORT_ID = 0x51
PACKET_SIZE = 64


def legacy_battery_request() -> bytes:
    request = bytearray(PACKET_SIZE)
    request[0] = REPORT_ID
    request[1] = 0x08
    request[3] = 0x03
    request[4] = 0x1A
    request[6] = 0x03
    request[8] = 0x04
    request[9] = 0x0A
    return bytes(request)


def classify(packet: bytes) -> str:
    if len(packet) < 2 or packet[0] != REPORT_ID:
        return "not ours"
    if packet[1] == 0x03:
        return "ack"
    if packet[1] == 0x05:
        return "powered off" if len(packet) >= 7 and packet[6] == 0x00 else "power event"
    if packet[1] == 0x0B and len(packet) >= 13 and packet[8] == 0x04:
        level = packet[10]
        charging = packet[12] == 0x02
        return f"BATTERY {level}%{' charging' if charging else ''}"
    return "other"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("node")
    parser.add_argument("--reads", type=int, default=8, help="packets to read before giving up")
    parser.add_argument("--timeout", type=float, default=1.0, help="seconds to wait per packet")
    parser.add_argument("-q", "--quiet", action="store_true", help="print the percentage alone, for feeding another program")
    args = parser.parse_args()

    try:
        fd = os.open(args.node, os.O_RDWR | os.O_NONBLOCK)
    except OSError as e:
        print(f"{args.node}: {e}", file=sys.stderr)
        print("run as root, or give yourself the node with a udev rule", file=sys.stderr)
        return 1

    try:
        request = legacy_battery_request()
        os.write(fd, request)
        if not args.quiet:
            print(f"sent: {request[:16].hex()} …")

        for _ in range(args.reads):
            ready, _, _ = select.select([fd], [], [], args.timeout)
            if not ready:
                if not args.quiet:
                    print("(silence)")
                break
            packet = os.read(fd, PACKET_SIZE)
            verdict = classify(packet)
            if not args.quiet:
                print(f"recv: {packet[:16].hex()} …  {verdict}")
            if verdict.startswith("BATTERY"):
                if args.quiet:
                    print(packet[10])
                return 0
    finally:
        os.close(fd)

    if not args.quiet:
        print("\nNo battery packet. Either this firmware only speaks the Centurion feature protocol,")
        print("or the request has to go to another interface of the receiver.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
