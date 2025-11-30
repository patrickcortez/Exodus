#!/usr/bin/env python3
"""
ckg_server.py - simple threaded package server for the ckg client

Protocol (line-based over TCP):
 - Client connects and sends a single command line terminated by '\n'.
 - Commands supported:
     UPDATE\n         -> server responds: OK\n<content-length>\n<manifest-bytes>
     INSTALL <pkg>\n
         -> server responds: OK\n<content-length>\n<payload>
            where <payload> is a sequence of file entries:
                <relative_path>\n<size>\n<raw-bytes>
            Files are sent in arbitrary order. Paths use '/' as separator.
     LIST\n  (optional: server sends manifest too)
 - On error, server responds: ERROR <message>\n
Usage:
    python3 ckg_server.py --root /path/to/ckg --host 0.0.0.0 --port 9000

Directory layout (root):
    <root>/manifest.txt      # text file listing packages and descriptions
    <root>/<package>/...     # directories for each package; files under these directories

Security notes:
 - Package names are sanitized to prevent directory traversal.
 - This server is intentionally tiny and not hardened for untrusted networks.
 - When exposing over the internet, run behind a firewall / authentication.

"""

import argparse
import io
import os
import socketserver
import threading
import sys
from typing import Tuple


def read_line(sock) -> str:
    """Read a line ending with '\n' from a connected socket and return it without the newline.
    Returns empty string on EOF.
    """
    data = bytearray()
    while True:
        ch = sock.recv(1)
        if not ch:
            return ''
        if ch == b'\n':
            break
        data.extend(ch)
    return data.decode('utf-8', errors='replace')


def send_all(sock, data: bytes):
    totalsent = 0
    while totalsent < len(data):
        sent = sock.send(data[totalsent:])
        if sent == 0:
            raise ConnectionError("socket connection broken")
        totalsent += sent


class CkgTCPHandler(socketserver.BaseRequestHandler):
    """Handle incoming ckg commands.
    self.server.root is the root ckg directory (set on the TCPServer instance)
    """
    def handle(self):
        peer = self.client_address
        try:
            line = read_line(self.request)
            if not line:
                return
            line = line.strip()
            if not line:
                return
            parts = line.split(None, 1)
            cmd = parts[0].upper()
            arg = parts[1] if len(parts) > 1 else ''
            if cmd == 'UPDATE' or cmd == 'LIST':
                self.handle_update()
            elif cmd == 'INSTALL':
                if not arg:
                    self.send_error('missing package name')
                    return
                self.handle_install(arg)
            else:
                self.send_error(f'unrecognized command: {cmd}')
        except ConnectionError:
            # client disconnected
            return
        except Exception as e:
            try:
                self.send_error('internal server error')
            except Exception:
                pass
            print(f'Error handling request from {peer}: {e}', file=sys.stderr)

    def send_error(self, msg: str):
        s = f'ERROR {msg}\n'.encode('utf-8')
        try:
            send_all(self.request, s)
        except Exception:
            pass

    def handle_update(self):
        manifest_path = os.path.join(self.server.root, 'manifest.txt')
        if not os.path.isfile(manifest_path):
            self.send_error('manifest not found')
            return
        # read manifest bytes
        with open(manifest_path, 'rb') as mf:
            data = mf.read()
        header = f'OK\n{len(data)}\n'.encode('ascii')
        try:
            send_all(self.request, header)
            send_all(self.request, data)
        except Exception as e:
            print('Send error during UPDATE:', e, file=sys.stderr)

    def handle_install(self, pkg_name: str):
        # sanitize package name - disallow path separators or traversal
        if '/' in pkg_name or '\\' in pkg_name or pkg_name.find('..') != -1:
            self.send_error('invalid package name')
            return
        pkg_dir = os.path.join(self.server.root, pkg_name)
        if not os.path.isdir(pkg_dir):
            self.send_error('package not found')
            return
        # Walk the package directory and collect files
        entries = []  # list of tuples (relpath_unix, filesize, fullpath)
        for root, dirs, files in os.walk(pkg_dir):
            for fname in files:
                full = os.path.join(root, fname)
                rel = os.path.relpath(full, pkg_dir)
                # normalize to forward slashes for client
                rel_unix = rel.replace(os.path.sep, '/')
                if rel_unix.startswith('..'):
                    # unexpected; skip
                    continue
                try:
                    size = os.path.getsize(full)
                except OSError:
                    continue
                entries.append((rel_unix, size, full))
        # compute total length of payload
        total = 0
        for rel, size, full in entries:
            total += len(rel.encode('utf-8')) + 1  # path + '\n'
            total += len(str(size).encode('ascii')) + 1  # size + '\n'
            total += size
        header = f'OK\n{total}\n'.encode('ascii')
        try:
            send_all(self.request, header)
            # now stream files
            for rel, size, full in entries:
                send_all(self.request, (rel + '\n').encode('utf-8'))
                send_all(self.request, (str(size) + '\n').encode('ascii'))
                # stream file contents in chunks
                with open(full, 'rb') as f:
                    while True:
                        chunk = f.read(8192)
                        if not chunk:
                            break
                        send_all(self.request, chunk)
        except Exception as e:
            print('Send error during INSTALL:', e, file=sys.stderr)
            # if we can't send, abort
            return


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser(description='ckg server')
    parser.add_argument('--root', '-r', default='./ckg', help='ckg root directory (manifest + packages)')
    parser.add_argument('--host', default='0.0.0.0', help='bind address')
    parser.add_argument('--port', '-p', type=int, default=9000, help='port to listen on')
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f'root directory {root} does not exist, creating', file=sys.stderr)
        os.makedirs(root, exist_ok=True)

    server = ThreadedTCPServer((args.host, args.port), CkgTCPHandler)
    server.root = root
    print(f'ckg server listening on {args.host}:{args.port} (root={root})')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nshutting down')
        server.shutdown()


if __name__ == '__main__':
    main()
