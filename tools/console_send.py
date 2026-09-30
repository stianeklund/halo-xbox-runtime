#!/usr/bin/env python3
"""Send console input to a running standalone build over XBDM.

  python3 tools/console_send.py "rasterizer_stats 0"
  python3 tools/console_send.py --type '`cheat_all_powerups\\n`'

The default evaluates the command directly, like a line in init.txt. --type
presses keys through the debug keyboard queue instead; backslash escapes are
decoded, '`' or '~' toggles the console, '\\n' presses Enter and {NAME}
presses a named key: {F1}..{F12}, {ESC}, {TAB}, {BACKSPACE}, {ENTER},
{UP}, {DOWN}, {LEFT}, {RIGHT}, {HOME}, {END}, {PGUP}, {PGDN}, {INSERT},
{DELETE}, {PRINTSCREEN}, {PAUSE}; {{ types a '{'. Both work without a
keyboard attached. Writes compat/xapi/keyboard.c's
halo_remote_console mailbox, located through the map of the deployed build.
"""
import argparse
import codecs
import os
import re
import struct
import sys
import time

from symbolize import MAP_RE, rdcp

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"HALOCON1"
TEXT_SIZE = 256
# HID usages; the mailbox carries a named key as the byte 0x80 + usage.
KEYS = {f"F{n}": 0x39 + n for n in range(1, 13)}
KEYS.update(ENTER=0x28, ESC=0x29, BACKSPACE=0x2a, TAB=0x2b, PRINTSCREEN=0x46,
            PAUSE=0x48, INSERT=0x49, HOME=0x4a, PGUP=0x4b, DELETE=0x4c, END=0x4d,
            PGDN=0x4e, RIGHT=0x4f, LEFT=0x50, DOWN=0x51, UP=0x52)


def encode_keys(text):
    def key(match):
        if match[0] == "{{":
            return "{"
        name = match[1].upper()
        if name not in KEYS:
            sys.exit(f"unknown key {{{match[1]}}}; known: {', '.join(KEYS)}")
        return chr(0x80 + KEYS[name])
    return re.sub(r"\{\{|\{(\w+)\}", key, text)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("text")
    parser.add_argument("--type", action="store_true", help="press keys instead of evaluating")
    parser.add_argument("--map", default=os.path.join(HERE, "..", "build-seed", "halo.map"))
    parser.add_argument("--host", default=os.environ.get("XBOX_HOST", "127.0.0.1"))
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    text = encode_keys(codecs.decode(args.text, "unicode_escape")) if args.type else args.text
    data = text.encode("latin-1" if args.type else "ascii") + b"\0"
    if len(data) > TEXT_SIZE:
        sys.exit(f"text is longer than {TEXT_SIZE - 1} characters")

    address = None
    with open(args.map) as source:
        for line in source:
            match = MAP_RE.match(line)
            if match and match[1] == "_halo_remote_console":
                address = int(match[2], 16)
    if address is None:
        sys.exit(f"{args.map} has no _halo_remote_console; rebuild and redeploy")

    def read(offset, size):
        response = rdcp(args.host, f"getmem addr=0x{address + offset:x} length=0x{size:x}")
        hexes = "".join(line.strip() for line in response.splitlines()[1:]
                        if re.fullmatch(r"[0-9A-Fa-f?]+", line.strip()))
        if "?" in hexes or len(hexes) != size * 2:
            sys.exit(f"could not read the mailbox at 0x{address + offset:x}: {response.strip()}")
        return bytes.fromhex(hexes)

    def write(offset, payload):
        for start in range(0, len(payload), 64):
            chunk = payload[start:start + 64]
            response = rdcp(args.host, f"setmem addr=0x{address + offset + start:x} "
                                       f"data={chunk.hex()}")
            if not response.startswith("200"):
                sys.exit(f"setmem failed: {response.strip()}")

    if read(0, 8) != MAGIC:
        sys.exit(f"no mailbox at 0x{address:x}; is {args.map} the deployed build?")
    request, done = struct.unpack("<II", read(8, 8))
    if request != done:
        sys.exit(f"a previous request ({request}) is still pending; is the game running?")
    write(24, data)
    write(16, struct.pack("<I", 1 if args.type else 0))
    write(8, struct.pack("<I", done + 1))

    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        done, _, result = struct.unpack("<IIi", read(12, 12))
        if done == request + 1:
            if not args.type and not result:
                sys.exit(f"console rejected: {text}")
            print("typed" if args.type else "ok")
            return
        time.sleep(0.1)
    sys.exit("timed out: the game polls the mailbox from its input update, once per frame")


if __name__ == "__main__":
    main()
