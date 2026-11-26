#!/usr/bin/env python3
"""
tests/run_tests.py — the msh test plan (§7.2), run for real

Every case starts msh inside a pseudo-terminal and drives it the way a
person at the keyboard would: it types a line, presses Ctrl-C, Ctrl-Z or
Ctrl-D, and then judges the result only by what can be observed from
outside the shell — the text on the screen, the files on disk, /proc and
ps. Nothing inside msh is asked whether it worked.

Cases 01-12 are the rows of the test-plan slide. Cases 13-29 are extra
checks for the rest of the design (tokenizer, redirection errors, the two
bug modes, the x-ray, our own ls and cat).

    python3 tests/run_tests.py          run every case
    python3 tests/run_tests.py 06 10    run only these cases
    python3 tests/run_tests.py -v       also show what msh printed
"""
import os
import pty
import re
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MSH = os.path.join(ROOT, "msh")
BIN = os.path.join(ROOT, "bin")
VERBOSE = False

# msh [127] user@host:~/dir$
PROMPT = re.compile(rb"msh (?:\[(\d+)\] )?[^\s@]+@[^\s:]+:([^\n]*)\$ \Z")

TTY = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
GREEN, RED, DIM, BOLD, AMBER, RESET = (
    ("\033[32m", "\033[31m", "\033[2m", "\033[1m", "\033[38;5;214m", "\033[0m") if TTY else ("",) * 6)


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def clean(raw):
    return raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "")


def program_lines(out):
    """what the programs printed: everything except x-ray lines and msh's own notes"""
    return [l for l in out.split("\n")
            if l and not l.startswith(("│", "╭", "╰", "msh:", "[done]", "[bg]", "^C", "^Z"))]


def proc_state(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()[0]      # R S T Z ...
    except OSError:
        return None


def open_fds(pid):
    return sorted(int(x) for x in os.listdir(f"/proc/{pid}/fd"))


# ---------------------------------------------------------------------------
class Session:
    """one msh, running in its own pseudo-terminal"""

    def __init__(self, cwd, env=None, path_first=None):
        e = dict(os.environ)
        for k in ("MSH_TRACE", "MSH_LEAK", "MSH_REAPER"):
            e.pop(k, None)
        e["NO_COLOR"] = "1"                   # plain text is easier to judge
        e.setdefault("TERM", "xterm")
        if path_first:
            e["PATH"] = path_first + ":" + e.get("PATH", "/usr/bin:/bin")
        if env:
            e.update(env)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:                     # the child: become msh
            try:
                os.chdir(cwd)
                os.execve(MSH, [MSH, "-q"], e)
            finally:
                os._exit(127)
        self.buf = b""
        self.transcript = b""
        self.status = None
        self.banner = self.wait_prompt(5)[0]

    def _pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], max(timeout, 0))
        if not r:
            return False
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            data = b""
        if not data:
            raise EOFError("msh closed the terminal")
        self.buf += data
        self.transcript += data
        return True

    def pump(self, seconds):
        """collect whatever arrives during the next few seconds"""
        end = time.time() + seconds
        while time.time() < end:
            self._pump(end - time.time())

    def wait_prompt(self, timeout=5):
        end = time.time() + timeout
        while True:
            m = PROMPT.search(self.buf)
            if m:
                out = self.buf[:m.start()]
                self.buf = b""
                return clean(out), int(m.group(1) or 0), m.group(2).decode()
            left = end - time.time()
            if left <= 0:
                raise Failure(f"no prompt within {timeout}s (msh hung?) — last output: "
                              f"{clean(self.buf)[-200:]!r}")
            self._pump(left)

    def has_prompt(self):
        return PROMPT.search(self.buf) is not None

    def send(self, text):
        os.write(self.fd, text.encode())

    def key(self, ch):
        os.write(self.fd, bytes([ord(ch.upper()) - 64]))      # 'c' → Ctrl-C

    def run(self, line, timeout=5):
        """type a line; return (what it printed, exit status, directory in the prompt)"""
        self.send(line + "\n")
        out, status, cwd = self.wait_prompt(timeout)
        return out.split("\n", 1)[1] if "\n" in out else "", status, cwd

    def wait_exit(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = os.waitstatus_to_exitcode(st)
                try:
                    while self._pump(0.05):
                        pass
                except EOFError:
                    pass
                return self.status
            try:
                self._pump(0.05)
            except EOFError:
                pass
        raise Failure("msh did not exit")

    def close(self):
        if self.status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
            self.status = -9
        try:
            os.close(self.fd)
        except OSError:
            pass


def make_fixture():
    d = tempfile.mkdtemp(prefix="msh-test-")
    files = {
        "main.c": '#include "util.h"\nint main(void) { greet("msh"); return 0; }\n',
        "parser.c": '#include "util.h"\n/* TODO: quotes */\n',
        "util.c": '#include <stdio.h>\nvoid greet(const char *n) { printf("hi %s\\n", n); }\n',
        "notes.txt": "fork creates the child\nexec replaces the program\nwait collects the status\n",
        "data.txt": "fork\npipe\nfork\nexec\nfork\npipe\n",
    }
    for name, text in files.items():
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
    return d


# ---------------------------------------------------------------------------
CASES = []


def case(num, text, expect, section):
    def deco(fn):
        CASES.append((num, text, expect, section, fn))
        return fn
    return deco


@case("01", "ls -l", "child forked, execvp resolves via $PATH, exit status 0", "3.4")
def t01(d, s):
    s.run("export MSH_TRACE=2")
    out, st, _ = s.run("ls -l")
    fork = re.search(r"fork\(\) = (\d+)", out)
    found = re.search(r"\$PATH .*?(\S+) ✓", out)
    check(fork, "no fork() in the x-ray")
    check(found, "the $PATH walk found nothing")
    check("total" in out, "no listing was printed")
    check(st == 0, f"status {st}")
    return f"fork() = {fork.group(1)} · $PATH walk → {found.group(1)}/ls ✓ · status 0"


@case("02", "cd /tmp  then  pwd", "prompt directory changes and persists after the command", "3.8")
def t02(d, s):
    _, st, cwd = s.run("cd /tmp")
    check(st == 0 and cwd == "/tmp", f"after cd the prompt shows {cwd!r}")
    out, st, cwd = s.run("pwd")
    check(out.strip() == "/tmp", f"pwd printed {out.strip()!r}")
    check(cwd == "/tmp", "the directory did not persist")
    return "prompt shows /tmp · pwd prints /tmp"


@case("03", "echo hi > out.txt", "file created, contains one line, terminal stays silent", "4.1")
def t03(d, s):
    out, st, _ = s.run("echo hi > out.txt")
    check(out.strip() == "", f"the terminal showed {out!r}")
    data = open(os.path.join(d, "out.txt")).read()
    check(data == "hi\n", f"out.txt holds {data!r}")
    return "out.txt = 'hi\\n' · nothing on the terminal"


@case("04", "cat < out.txt", "file content on stdout, descriptor 0 rewired", "4.2")
def t04(d, s):
    with open(os.path.join(d, "out.txt"), "w") as f:
        f.write("hi\n")
    s.run("export MSH_TRACE=1")
    out, st, _ = s.run("cat < out.txt")
    check('stdin ← "out.txt"' in out, "the x-ray shows no rewired stdin")
    check("hi" in program_lines(out), f"cat printed {program_lines(out)!r}")
    return 'x-ray: stdin ← "out.txt" · cat printed hi'


@case("05", "echo more >> out.txt", "file grows; earlier content preserved", "4.2")
def t05(d, s):
    with open(os.path.join(d, "out.txt"), "w") as f:
        f.write("hi\n")
    s.run("echo more >> out.txt")
    data = open(os.path.join(d, "out.txt")).read()
    check(data == "hi\nmore\n", f"out.txt holds {data!r}")
    return "out.txt = 'hi\\nmore\\n'"


@case("06", "ls | wc -l", "two children, one pipe, single integer printed", "4.3")
def t06(d, s):
    s.run("export MSH_TRACE=1")
    out, st, _ = s.run("ls | wc -l")
    forks, pipes = out.count("fork() ="), out.count("pipe() =")
    printed = program_lines(out)
    want = len([n for n in os.listdir(d) if not n.startswith(".")])
    check(forks == 2 and pipes == 1, f"{forks} forks, {pipes} pipes")
    check(printed == [str(want)] or [p.strip() for p in printed] == [str(want)], f"printed {printed!r}")
    return f"2 forks · 1 pipe · printed {want}"


@case("07", "ls | grep .c | wc -l", "three children, two pipes, no hang", "4.5")
def t07(d, s):
    s.run("export MSH_TRACE=1")
    t0 = time.time()
    out, st, _ = s.run("ls | grep .c | wc -l", timeout=5)
    took = time.time() - t0
    forks, pipes = out.count("fork() ="), out.count("pipe() =")
    printed = [p.strip() for p in program_lines(out)]
    check(forks == 3 and pipes == 2, f"{forks} forks, {pipes} pipes")
    check(printed == ["3"], f"printed {printed!r}")
    return f"3 forks · 2 pipes · printed 3 · back in {took:.2f}s"


@case("08", "sleep 5 &", "prompt returns at once; ps shows the job running", "5.1")
def t08(d, s):
    t0 = time.time()
    out, st, _ = s.run("sleep 5 &")
    took = time.time() - t0
    m = re.search(r"\[bg\] (\d+)", out)
    check(m, f"no [bg] line in {out!r}")
    pid = int(m.group(1))
    check(took < 1.0, f"the prompt took {took:.2f}s")
    ps, _, _ = s.run("ps -o pid,stat,cmd")
    row = [l for l in ps.split("\n") if l.split() and l.split()[0] == str(pid)]
    check(row and row[0].split()[1].startswith("S") and "sleep 5" in row[0], f"ps shows {row!r}")
    os.kill(pid, signal.SIGKILL)
    return f"[bg] {pid} after {took:.2f}s · ps: {' '.join(row[0].split())}"


@case("09", "sleep 1 &  then  ps", "no <defunct> entry after one second", "5.2")
def t09(d, s):
    out, _, _ = s.run("sleep 1 &")
    pid = int(re.search(r"\[bg\] (\d+)", out).group(1))
    time.sleep(1.5)
    ps, _, _ = s.run("ps -o pid,stat,cmd")
    check("<defunct>" not in ps, f"a zombie is left: {ps!r}")
    check(proc_state(pid) is None, f"pid {pid} still exists")
    check(f"[done] {pid}" in ps, "no [done] notice from the reaper")
    return f"ps shows no <defunct> · reaper printed [done] {pid}"


@case("10", "Ctrl-C during sleep 30", "job dies, shell survives, background job untouched", "5.4")
def t10(d, s):
    out, _, _ = s.run("sleep 300 &")
    bg = int(re.search(r"\[bg\] (\d+)", out).group(1))
    s.send("sleep 30\n")
    s.pump(0.5)
    t0 = time.time()
    s.key("c")
    out, st, _ = s.wait_prompt(5)
    took = time.time() - t0
    check(st == 130, f"status {st} (expected 130 = 128 + SIGINT)")
    check(proc_state(s.pid) not in (None, "Z"), "msh died")
    check(proc_state(bg) in ("S", "R"), f"the background job is {proc_state(bg)}")
    out, st, _ = s.run("pwd")
    check(st == 0, "msh no longer answers")
    os.kill(bg, signal.SIGKILL)
    return f"prompt back in {took:.2f}s with [130] · msh alive · background {bg} still sleeping"


@case("11", "nosuchcmd", "error message, exit status 127, shell continues", "6.2")
def t11(d, s):
    out, st, _ = s.run("nosuchcmd")
    check("msh: nosuchcmd: command not found" in out, f"printed {out!r}")
    check(st == 127, f"status {st}")
    _, st, _ = s.run("pwd")
    check(st == 0, "the shell did not continue")
    return "'command not found' · status 127 · next command runs"


@case("12", "Ctrl-D at the prompt", "shell exits cleanly, no leaked descriptors", "6.2")
def t12(d, s):
    s.run("ls | wc -l")
    s.run("echo x > f.txt")
    s.run("cat < f.txt | wc -c")
    fds = open_fds(s.pid)
    check(fds == [0, 1, 2], f"msh holds descriptors {fds}")
    s.key("d")
    code = s.wait_exit(5)
    check(code == 0, f"exit status {code}")
    check(b"exit" in s.transcript[-40:], "no 'exit' on the terminal")
    return "only fds 0 1 2 open after pipes and redirects · 'exit' · status 0"


# ----- extra checks ----------------------------------------------------------
@case("13", "ls>listing.txt", "metacharacters split without spaces", "2.2")
def t13(d, s):
    out, st, _ = s.run("ls>listing.txt")
    names = open(os.path.join(d, "listing.txt")).read().split()
    check(st == 0 and "main.c" in names, f"listing.txt holds {names!r}")
    return f"listing.txt written ({len(names)} names)"


@case("14", "ls nope 2> err.txt", "only the error stream goes to the file", "4.2")
def t14(d, s):
    out, st, _ = s.run("ls nope 2> err.txt")
    err = open(os.path.join(d, "err.txt")).read()
    check(out.strip() == "", f"the terminal showed {out!r}")
    check("nope" in err, f"err.txt holds {err!r}")
    check(st != 0, "status 0 for a failing ls")
    return f"terminal silent · err.txt holds ls's error ({len(err)} bytes) · status {st}"


@case("15", "echo x > /", "open() fails in the child; the shell survives", "4.2")
def t15(d, s):
    out, st, _ = s.run("echo x > /")
    check("msh: /: Is a directory" in out and st == 1, f"printed {out!r}, status {st}")
    note = "EISDIR reported, status 1"
    if os.geteuid() != 0:
        ro = os.path.join(d, "ro.txt")
        open(ro, "w").close()
        os.chmod(ro, 0o444)
        out, st, _ = s.run("echo x > ro.txt")
        check("Permission denied" in out and st == 1, f"printed {out!r}")
        note += " · EACCES reported"
    _, st, _ = s.run("pwd")
    check(st == 0, "the shell did not survive")
    return note + " · shell alive"


@case("16", "cd /nonexistent  then  cd", "bad path reported; no argument means $HOME", "6.2")
def t16(d, s):
    _, _, before = s.run("pwd")
    out, st, cwd = s.run("cd /nonexistent")
    check("No such file or directory" in out and st == 1, f"printed {out!r}")
    check(cwd == before, "the directory changed anyway")
    _, st, cwd = s.run("cd")
    check(cwd == "~", f"cd with no argument went to {cwd!r}")
    return "error, directory unchanged · cd alone → ~"


@case("17", "cd /tmp | cat", "a built-in in a pipeline runs in a child", "3.8")
def t17(d, s):
    _, _, before = s.run("pwd")
    _, st, cwd = s.run("cd /tmp | cat")
    check(cwd == before and cwd != "/tmp", f"the prompt moved to {cwd!r}")
    return f"prompt still shows {before} — the change died with the child"


@case("18", "export GREETING=hi  then  printenv GREETING", "children inherit exported variables", "3.7")
def t18(d, s):
    s.run("export GREETING=hi")
    out, st, _ = s.run("printenv GREETING")
    check(out.strip() == "hi", f"printed {out!r}")
    return "printenv, a child forked afterwards, prints hi"


@case("19", "seq 1 100000 | sort -rn | head -1", "575 KB through 64 KiB pipes, no deadlock", "4.4")
def t19(d, s):
    out, st, _ = s.run("seq 1 100000 | sort -rn | head -1", timeout=10)
    check(out.strip() == "100000", f"printed {out!r}")
    return "printed 100000"


@case("20", "yes | head -3", "the writer is stopped by SIGPIPE", "4.6")
def t20(d, s):
    s.run("export MSH_TRACE=1")
    out, st, _ = s.run("yes | head -3", timeout=5)
    check(program_lines(out) == ["y", "y", "y"], f"printed {program_lines(out)!r}")
    check("killed by SIGPIPE" in out, "yes was not ended by SIGPIPE")
    return "3 lines · x-ray: yes killed by SIGPIPE"


@case("21", "| ls   ls >   ls ; pwd", "syntax errors reported, status 2, loop continues", "6.2")
def t21(d, s):
    for line, want in (("| ls", "unexpected token '|'"), ("ls >", "unexpected token 'newline'"),
                       ("ls ; pwd", "not supported yet")):
        out, st, _ = s.run(line)
        check(want in out and st == 2, f"{line!r} printed {out!r} status {st}")
    return "three errors, each with status 2"


@case("22", "Ctrl-Z during sleep 30", "WUNTRACED returns the prompt; the job is stopped", "5.4")
def t22(d, s):
    s.send("sleep 30\n")
    s.pump(0.5)
    s.key("z")
    out, st, _ = s.wait_prompt(5)
    m = re.search(r"\[stopped\] (\d+)", out)
    check(m and st == 148, f"printed {out!r}, status {st}")
    pid = int(m.group(1))
    check(proc_state(pid) == "T", f"state {proc_state(pid)}")
    out, _, _ = s.run(f"kill -9 {pid}")
    if f"[done] {pid}" not in out:                     # the SIGCHLD may land just after the prompt
        time.sleep(0.2)
        more, _, _ = s.run("true")
        out += more
    check(f"[done] {pid}  killed by SIGKILL" in out, f"no [done] notice: {out!r}")
    return f"[stopped] {pid} · state T · after kill -9: [done] killed by SIGKILL"


@case("23", "MSH_LEAK=1  then  ls | wc -l", "the §4.5 bug: the pipeline hangs until Ctrl-C", "4.5")
def t23(d, s):
    out, _, _ = s.run("export MSH_LEAK=1")
    check("leak mode ON" in out, "no leak-mode notice")
    s.send("ls | wc -l\n")
    s.pump(1.5)
    check(not s.has_prompt(), "the pipeline finished — the bug did not appear")
    fds = open_fds(s.pid)
    s.key("c")
    _, st, _ = s.wait_prompt(5)
    check(st == 130, f"status {st}")
    out, _, _ = s.run("export MSH_LEAK=0")
    check("closed 1 leaked descriptor" in out, f"printed {out!r}")
    after = open_fds(s.pid)
    check(len(fds) == 4 and after == [0, 1, 2], f"fds while hung {fds}, after {after}")
    return f"hung for 1.5s holding fds {fds} · Ctrl-C · leak off → fds {after}"


@case("24", "MSH_REAPER=0  then  sleep 0.2 & ×2", "zombies appear; reaper on removes them", "5.1")
def t24(d, s):
    s.run("export MSH_REAPER=0")
    pids = [int(re.search(r"\[bg\] (\d+)", s.run("sleep 0.2 &")[0]).group(1)) for _ in range(2)]
    time.sleep(0.8)
    ps, _, _ = s.run("ps -o pid,stat,cmd")
    zombies = [l for l in ps.split("\n") if "<defunct>" in l]
    check(len(zombies) == 2 and all(proc_state(p) == "Z" for p in pids), f"ps shows {zombies!r}")
    out, _, _ = s.run("export MSH_REAPER=1")
    check(all(f"[done] {p}" in out for p in pids), f"printed {out!r}")
    check(all(proc_state(p) is None for p in pids), "the zombies are still there")
    return f"ps: 2 × <defunct> · reaper on → [done] {pids[0]}, {pids[1]} · gone from /proc"


@case("25", "MSH_TRACE=1  ls -l | wc -l > out.txt &", "x-ray of the one line that touches every section", "2.3")
def t25(d, s):
    s.run("export MSH_TRACE=1")
    out, st, _ = s.run("ls -l | wc -l > out.txt &")
    for want in ("TOKENIZE", "REDIR_OUT", "AMP", "PARSE", "[background = true]",
                 'REDIR_OUT  > "out.txt"', "pipe() =", 'stdout → "out.txt"', "[bg]", "WAIT      skipped"):
        check(want in out, f"x-ray lacks {want!r}")
    check(out.count("fork() =") == 2, "expected 2 forks")
    time.sleep(0.5)
    data = open(os.path.join(d, "out.txt")).read().strip()
    check(data.isdigit(), f"out.txt holds {data!r}")
    return f"tokens, tree, 1 pipe, 2 forks, WAIT skipped · out.txt = {data}"


@case("26", "MSH_TRACE=2  ls | cat", "each child holds only 0, 1, 2 (+ close-on-exec)", "4.6")
def t26(d, s):
    s.run("export MSH_TRACE=2")
    out, st, _ = s.run("ls | cat")
    tables = [l.split("fd table", 1)[1] for l in out.split("\n") if "fd table" in l]
    check(len(tables) == 2, f"{len(tables)} fd tables")
    for t in tables:
        plain = [x.split()[0] for x in re.split(r"\s{3,}", t.strip()) if x and "close-on-exec" not in x]
        check(plain == ["0", "1", "2"] and "a leak" not in t, f"fd table: {t.strip()}")
    return "both children: 0 1 2 inherited, x-ray channel close-on-exec, no leaks"


@case("27", "a 70 000-byte line", "rejected with a message; the loop goes on", "6.3")
def t27(d, s):
    r = subprocess.run([MSH, "-q"], input="x" * 70000 + "\necho still-alive\n",
                       capture_output=True, text=True, timeout=10, cwd=d)
    check("line too long" in r.stderr, f"stderr: {r.stderr[:80]!r}")
    check("still-alive" in r.stdout, f"stdout: {r.stdout[:80]!r}")
    return "'line too long' · the next line still runs"


@case("28", "exit 3", "the built-in exit sets the shell's own status", "3.8")
def t28(d, s):
    s.send("exit 3\n")
    code = s.wait_exit(5)
    check(code == 3, f"exit status {code}")
    return "msh exited with status 3"


@case("29", "our own ls and cat:  ls | cat", "found on $PATH in bin/, run in a child", "3.9")
def t29(d, s):
    check(os.access(os.path.join(BIN, "ls"), os.X_OK), "bin/ls is missing — run make")
    s2 = Session(d, path_first=BIN)
    try:
        s2.run("export MSH_TRACE=2")
        out, st, _ = s2.run("ls | cat")
        check(out.count(f"{BIN} ✓") == 2, "the $PATH walk did not stop at bin/ for both")
        names = sorted(n for n in os.listdir(d) if not n.startswith("."))
        check(program_lines(out) == names, f"printed {program_lines(out)!r}")
        s2.run("export MSH_TRACE=0")
        out, st, _ = s2.run("ls -l notes.txt")
        info = os.stat(os.path.join(d, "notes.txt"))
        want = f"{stat.filemode(info.st_mode)} 1 "
        check(out.startswith(want) and f" {info.st_size} " in out and out.strip().endswith("notes.txt"),
              f"ls -l printed {out!r}")
        out, st, _ = s2.run("cat nope")
        check("cat: nope: No such file or directory" in out and st == 1, f"cat nope: {out!r}")
    finally:
        s2.close()
    return "both found in bin/ · ls lists, ls -l formats, cat reports errors"


# ---------------------------------------------------------------------------
def main(argv):
    global VERBOSE
    VERBOSE = "-v" in argv
    only = {a for a in argv if a.isdigit()}
    if not os.access(MSH, os.X_OK):
        print("msh is not built yet — run: make")
        return 2

    print(f"\n  {BOLD}msh test plan{RESET}  {DIM}every case typed into a real terminal, "
          f"judged from outside the shell{RESET}\n")
    passed = {"slide": 0, "extra": 0}
    total = {"slide": 0, "extra": 0}
    failed = []
    for num, text, expect, section, fn in CASES:
        if only and num not in only:
            continue
        kind = "slide" if int(num) <= 12 else "extra"
        total[kind] += 1
        if kind == "extra" and not total["extra"] - 1:
            print(f"\n  {DIM}extra checks — the rest of the design{RESET}\n")
        d = make_fixture()
        s = None
        t0 = time.time()
        try:
            s = Session(d)
            note = fn(d, s)
            ok = True
        except (Failure, EOFError, OSError, subprocess.TimeoutExpired) as e:
            ok, note = False, str(e)
        finally:
            if s:
                if VERBOSE:
                    print(DIM + clean(s.transcript) + RESET)
                s.close()
            shutil.rmtree(d, ignore_errors=True)
        took = time.time() - t0
        mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
        print(f"  {BOLD}{num}{RESET}  {mark}  {text:<46}{DIM}§{section:<5}{took:6.2f}s{RESET}")
        print(f"            {DIM if ok else RED}{'expected: ' + expect if not ok else note}{RESET}")
        if not ok:
            print(f"            {RED}observed: {note}{RESET}")
            failed.append(num)
        else:
            passed[kind] += 1

    print()
    parts = []
    if total["slide"]:
        parts.append(f"{passed['slide']}/{total['slide']} slide cases passed")
    if total["extra"]:
        parts.append(f"{passed['extra']}/{total['extra']} extra checks passed")
    line = "  " + " · ".join(parts)
    print(f"{GREEN if not failed else RED}{line}{RESET}"
          + (f"  ·  {RED}failed: {' '.join(failed)}{RESET}" if failed else ""))
    print()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
