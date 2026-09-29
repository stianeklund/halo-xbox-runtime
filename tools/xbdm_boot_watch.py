#!/usr/bin/env python3
"""Launch an XBE over XBDM and stream its debug output / exceptions.

Used for standalone-build bring-up: the generated stubs report every missing
symbol through DbgPrint ("[halo-standalone] missing: ..."), which XBDM forwards
as `debugstr` notifications.

  python3 tools/xbdm_boot_watch.py --host 10.0.0.21 \
      --title 'E:\\GAMES\\halo-patched\\standalone_seed.xbe' --seconds 40
"""
import argparse
import socket
import time


def command(host, port, cmd, timeout=10):
    s = socket.create_connection((host, port), timeout=timeout)
    s.recv(1024)  # 201 banner
    s.sendall(cmd.encode() + b'\r\n')
    data = b''
    try:
        while not data.endswith(b'\r\n'):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data.decode(errors='replace').strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, default=731)
    ap.add_argument('--title', help='XBE to launch (omit to only listen)')
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--stop-on-exception', action='store_true',
                    help='halt on first-chance exceptions (before SEH filters run)')
    ap.add_argument('--log', help='also append the stream to this file')
    args = ap.parse_args()

    def open_notify():
        while time.time() < deadline:
            try:
                n = socket.create_connection((args.host, args.port), timeout=5)
                n.recv(1024)
                n.sendall(b'notify\r\n')
                n.recv(1024)  # 205: this socket is now a notification channel
                n.settimeout(1.0)
                return n
            except OSError:
                time.sleep(0.5)
        return None

    deadline = time.time() + args.seconds
    n = open_notify()
    if args.title:
        print(command(args.host, args.port, 'magicboot title=%s debug' % args.title))
    buf = b''
    log = open(args.log, 'a') if args.log else None
    while n and time.time() < deadline:
        try:
            chunk = n.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            chunk = b''
        if not chunk:
            # The title reboot drops the channel; reattach to the new title.
            n.close()
            n = open_notify()
            print('-- notify channel reattached', flush=True)
            if n and args.title:
                time.sleep(1.0)
                print(command(args.host, args.port, 'xbeinfo running'))
                if args.stop_on_exception:
                    print(command(args.host, args.port, 'stopon fce'))
                print(command(args.host, args.port, 'go'))  # `debug` boot halts at entry
            continue
        buf += chunk
        while b'\r\n' in buf:
            line, buf = buf.split(b'\r\n', 1)
            text = line.decode(errors='replace')
            print(text, flush=True)
            if log:
                log.write(text + '\n')
    if n:
        n.close()


if __name__ == '__main__':
    main()
