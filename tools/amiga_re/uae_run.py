#!/usr/bin/env python3
"""Run FS-UAE under a PTY (controlling terminal) so the console debugger works.

The emulator's stdin/stdout/stderr are attached to a pty so it "has a
terminal" (needed by the UAE console debugger, entered with Alt+D). A command
FIFO lets you send lines to the emulator's console (debugger commands). All
emulator output is captured to a log file.

Usage:
  uae_run.py <config.fs-uae> <fifo> <logfile> [pidfile]
"""
import os
import pty
import select
import subprocess
import sys
import time

BINARY = "/opt/FS-UAE/Linux/x86-64/fs-uae"
LIBDIR = "/opt/FS-UAE/Linux/x86-64"


def main():
    config = sys.argv[1]
    fifo = sys.argv[2]
    logfile = sys.argv[3]
    pidfile = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(fifo), "uae.pid")

    with open(pidfile, "w") as f:
        f.write(str(os.getpid()))
    os.chmod(pidfile, 0o644)

    if not os.path.exists(fifo):
        os.mkfifo(fifo)
    cmd_fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)

    master, slave = pty.openpty()
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIBDIR
    env["DISPLAY"] = os.environ.get("DISPLAY", ":0")

    proc = subprocess.Popen(
        [BINARY, config],
        stdin=slave, stdout=slave, stderr=slave,
        close_fds=True, env=env,
    )
    os.close(slave)

    log = open(logfile, "ab", buffering=0)
    buf = b""

    def flush_buf():
        nonlocal buf
        if not buf:
            return
        sys.stdout.buffer.write(buf)
        sys.stdout.buffer.flush()
        log.write(buf)
        buf = b""

    while True:
        if proc.poll() is not None:
            break
        r, _, _ = select.select([master, cmd_fd], [], [], 0.4)
        if master in r:
            try:
                data = os.read(master, 65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                sys.stdout.buffer.write(line + b"\n")
                sys.stdout.buffer.flush()
                log.write(line + b"\n")
                log.flush()
        if cmd_fd in r:
            while True:
                try:
                    data = os.read(cmd_fd, 65536)
                except OSError:
                    data = b""
                if not data:
                    break
                os.write(master, data)
                # drain nothing; continue loop
    flush_buf()
    try:
        os.close(master)
    except OSError:
        pass


main()