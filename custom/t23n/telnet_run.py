#!/usr/bin/env python3
"""Run one command on the telnet device, print stdout+stderr, exit with its rc.

Usage: telnet_run.py [-t SECONDS] cmd args...
Handles telnet IAC (refuses all options), auto-login as root, sentinel wait.
Exit 124 on timeout.
"""
import re
import socket
import sys
import time

HOST, PORT = "192.168.66.56", 23


class Telnet:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.buf = b""
        self.closed = False

    def _clean(self, data):
        """Strip IAC sequences, queuing replies (always WONT)."""
        out = bytearray()
        replies = bytearray()
        i = 0
        while i < len(data):
            if data[i] == 0xFF and i + 1 < len(data):
                cmd = data[i + 1]
                opt = data[i + 2] if i + 2 < len(data) else 0
                if cmd in (0xFB, 0xFD):          # WILL/DO -> WONT/DONT
                    replies += bytes([0xFF, 0xFC if cmd == 0xFB else 0xFC, opt])
                    i += 3
                    continue
                if cmd in (0xFE, 0xF0):          # WONT/DONT (2-3 bytes)
                    i += 3 if cmd == 0xFE else 2
                    continue
                if cmd == 0xFF:                  # escaped 0xFF
                    out.append(0xFF)
                    i += 2
                    continue
                i += 3
                continue
            out.append(data[i])
            i += 1
        if replies:
            self.s.sendall(bytes(replies))
        return bytes(out)

    def pump(self, tmo, pattern):
        """Read until pattern matches buffer; return match or None."""
        end = time.time() + tmo
        pat = re.compile(pattern if isinstance(pattern, bytes) else pattern.encode())
        while True:
            m = pat.search(self.buf)
            if m:
                return m
            if time.time() > end:
                return None
            try:
                data = self.s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                self.closed = True
                return None
            self.buf = (self.buf + self._clean(data))[-200000:]

    def login(self):
        deadline = time.time() + 40
        while time.time() < deadline:
            m = self.pump(4, r"login:|Password:|[#\$]\s*$")
            if self.closed:
                raise RuntimeError("telnet connection closed")
            if not m:
                self.s.sendall(b"\n")  # nudge
                continue
            s = m.group(0)
            self.buf = b""
            if s == b"login:":
                self.s.sendall(b"root\n")
            elif s == b"Password:":
                self.s.sendall(b"\n")
            else:
                return
        raise RuntimeError("login: prompt not found")


def main():
    argv = sys.argv[1:]
    tmo = 60.0
    if argv and argv[0] == "-t":
        tmo = float(argv[1])
        argv = argv[2:]
    cmd = " ".join(argv)

    t = Telnet()
    t.login()
    sent = "__RCD_%d" % (int(time.time() * 137) % 1000000)
    t.s.sendall(b"\n")
    t.pump(3, r"[#\$]\s*$")
    t.buf = b""
    t.s.sendall(("%s; echo %s_$?__END\n" % (cmd, sent)).encode())
    m = t.pump(tmo, r"%s_[0-9]+__END" % sent)
    out = t.buf.decode("latin1", "replace")
    rc = 124
    if m:
        rc = int(re.search(rb"_(\d+)__END", m.group(0)).group(1))
        out = out[: m.start()]
    elif "-v" in (argv[0] if False else ""):
        pass
    if rc == 124:
        sys.stderr.write("[TIMEOUT closed=%s tail=%r]\n" % (t.closed, t.buf[-160:]))
    try:
        t.s.sendall(b"exit\n")
        t.s.close()
    except OSError:
        pass
    lines = out.split("\n")
    if lines and cmd.split()[0][:15] in lines[0]:
        lines = lines[1:]
    print("\n".join(lines).replace("\r", "").rstrip())
    sys.exit(rc)


if __name__ == "__main__":
    main()
