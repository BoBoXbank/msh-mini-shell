#!/usr/bin/env python3
"""
present.py — presenter mode for the live demo (started by ./demo.sh)

Runs msh in its own pseudo-terminal and passes the keyboard through to it,
the way the `script` command does. One key is special:

    Enter at an empty msh prompt   types the next command from demo-steps.txt,
                                   one character at a time, as if typed
    Enter again                    runs it (msh receives an ordinary Enter)

Every other key goes to msh unchanged, so Ctrl-C, Ctrl-Z and Ctrl-D work as
usual and you can still type your own commands at any time. An empty Enter
pressed while a command is still running is ignored, so an impatient double
press never shows up as a blank line or a doubled prompt.

msh never reads demo-steps.txt itself: it only ever receives keystrokes on
its terminal (running a file of commands is still future work, §8.2).

    usage: present.py MSH_PATH [msh arguments...]
    MSH_DEMO_FROM=N in the environment starts at step N (./demo.sh N sets it)
"""
import fcntl
import os
import pty
import random
import re
import select
import signal
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS_FILE = os.path.join(os.path.dirname(HERE), "demo-steps.txt")

PROMPT = re.compile(r"msh (?:\[\d+\] )?\S+@\S+:[^\n]*\$ $")      # msh [130] user@host:~/dir$
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def load_steps(path):
    """demo-steps.txt: one command per line; '#' starts a note; '@wait N' pauses"""
    steps, wait = [], 0.0
    try:
        with open(path, encoding="utf-8") as f:
            for raw in f:
                s = raw.strip()
                if not s or s.startswith("#"):
                    continue
                if s.startswith("@wait"):
                    wait = float(s.split()[1])
                    continue
                steps.append((wait, s))
                wait = 0.0
    except OSError:
        pass
    return steps


class Presenter:
    def __init__(self, fd, steps):
        self.fd = fd
        self.steps = steps
        self.next = 0
        self.tail = ""            # recent output without colors, to spot the prompt
        self.line = ""            # what has been typed on the current line
        self.last_enter = time.time()
        self.busy = True          # msh is starting or running a command: no prompt yet

    # ---- output: msh → screen -------------------------------------------------
    def show(self, data):
        os.write(1, data)
        text = ANSI.sub("", data.decode("utf-8", "replace")).replace("\r", "")
        self.tail = (self.tail + text)[-500:]
        if self.at_prompt():
            self.busy = False

    def relay_for(self, seconds):
        """keep showing msh's output for a while (used while 'typing')"""
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                data = os.read(self.fd, 65536)
                if not data:
                    raise EOFError
                self.show(data)

    def at_prompt(self):
        return PROMPT.search(self.tail) is not None

    # ---- input: keyboard → msh ------------------------------------------------
    def type_next(self):
        wait, cmd = self.steps[self.next]
        self.next += 1
        remaining = wait - (time.time() - self.last_enter)
        if remaining > 0:
            self.relay_for(remaining)
        for ch in cmd:
            os.write(self.fd, ch.encode())
            self.relay_for(random.uniform(0.02, 0.05))
        self.line = cmd

    def key(self, b):
        ch = bytes([b])
        if ch in (b"\r", b"\n"):
            if not self.line and self.next < len(self.steps) and self.at_prompt():
                self.type_next()
                return
            if not self.line and self.busy:
                # an extra Enter while a command runs would only turn into an
                # empty command later (a blank line, then a doubled prompt): drop it
                return
            os.write(self.fd, ch)
            self.line = ""
            self.tail = ""                            # the prompt is used up
            self.busy = True
            self.last_enter = time.time()
            return
        os.write(self.fd, ch)
        if ch in (b"\x7f", b"\x08"):                  # Backspace
            self.line = self.line[:-1]
        elif ch in (b"\x03", b"\x04", b"\x15"):       # Ctrl-C, Ctrl-D, Ctrl-U
            self.line = ""
        elif b >= 32:
            self.line += ch.decode("latin-1")


def copy_winsize(fd):
    try:
        size = fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, size)
    except OSError:
        pass


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    steps = load_steps(STEPS_FILE)
    if not (os.isatty(0) and os.isatty(1)):           # no terminal: plain msh
        os.execv(argv[1], argv[1:])
    first = int(os.environ.pop("MSH_DEMO_FROM", "1") or 1)   # ./demo.sh 6 → start at step 6
    first = min(max(first, 1), len(steps) + 1)

    print(f"\033[2mpresenter mode · step {first}/{len(steps)} · Enter: type the next command"
          f" · Enter again: run it · Ctrl-C: stop it\033[0m")
    pid, fd = pty.fork()
    if pid == 0:                                      # the child becomes msh
        try:
            os.execv(argv[1], argv[1:])
        finally:
            os._exit(127)

    copy_winsize(fd)
    signal.signal(signal.SIGWINCH, lambda *_: copy_winsize(fd))
    saved = termios.tcgetattr(0)
    tty.setraw(0)
    p = Presenter(fd, steps)
    p.next = first - 1
    try:
        while True:
            try:
                r, _, _ = select.select([0, fd], [], [])
            except InterruptedError:
                continue
            if fd in r:
                data = os.read(fd, 65536)
                if not data:
                    break
                p.show(data)
            if 0 in r:
                data = os.read(0, 1024)
                if not data:
                    break
                for b in data:
                    p.key(b)
    except (OSError, EOFError):                       # msh has exited
        pass
    finally:
        termios.tcsetattr(0, termios.TCSAFLUSH, saved)
        try:
            os.close(fd)                              # a hang-up for msh if it is still running
        except OSError:
            pass

    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
