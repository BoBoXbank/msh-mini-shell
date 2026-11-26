/* ==========================================================================
 * execute.c — §3 process creation · §4 I/O plumbing · §5.4 job control
 *
 * For a pipeline of n commands: n-1 pipes and n forks (§4.5). Each child
 * dup2()s the ends it needs, closes every other pipe end, applies its
 * redirections, resets its signals and execs. The parent closes all 2(n-1)
 * pipe ends, gives the terminal to the job, and waits — or, for '&', does
 * not wait at all (§5.1).
 *
 * In x-ray mode each child also sends its trace lines to the parent over a
 * close-on-exec pipe and pauses until the parent releases it, so the trace
 * appears in a stable order. Without x-ray the children run immediately.
 * ========================================================================== */
#include "msh.h"

extern char **environ;

/* ---------- descriptors deliberately leaked by MSH_LEAK=1 (§4.5 demo) ---------- */
static int *leaked;
static int  n_leaked, cap_leaked;

static void leaked_add(int fd)
{
    if (n_leaked == cap_leaked) {
        int  nc = cap_leaked ? cap_leaked * 2 : 8;
        int *p  = realloc(leaked, (size_t)nc * sizeof *p);
        if (!p) { close(fd); return; }
        leaked = p;
        cap_leaked = nc;
    }
    leaked[n_leaked++] = fd;
}

int leaked_count(void) { return n_leaked; }

void leaked_close_all(void)
{
    for (int i = 0; i < n_leaked; i++) close(leaked[i]);
    free(leaked);
    leaked = NULL;
    n_leaked = cap_leaked = 0;
}

/* ---------- walking the tree: n commands = n-1 nested PIPE nodes (§2.3) --------- */
static int count_stages(const node *n)
{
    return n->type == N_PIPE ? count_stages(n->left) + count_stages(n->right) : 1;
}

static void collect_stages(node *n, node **out, int *k)
{
    if (n->type == N_PIPE) {
        collect_stages(n->left, out, k);
        collect_stages(n->right, out, k);
    } else {
        out[(*k)++] = n;
    }
}

static node *stage_command(node *st)
{
    while (st->type == N_REDIR) st = st->child;
    return st;
}

/* The outermost REDIR node is the last redirection written; return them in
   the order the user typed them, which is the order they are applied. */
static int stage_redirs(node *st, node ***out)
{
    int k = 0;
    for (node *c = st; c->type == N_REDIR; c = c->child) k++;
    *out = NULL;
    if (!k) return 0;
    node **r = malloc((size_t)k * sizeof *r);
    if (!r) return 0;
    int i = k;
    for (node *c = st; c->type == N_REDIR; c = c->child) r[--i] = c;
    *out = r;
    return k;
}

/* ---------- §4.1 / §4.2 redirection: open → dup2 → close ------------------------ */
static int redir_target_fd(redir_kind k) { return k == R_IN ? 0 : k == R_ERR ? 2 : 1; }

static int redir_flags(redir_kind k)
{
    switch (k) {
    case R_IN:     return O_RDONLY;
    case R_APPEND: return O_WRONLY | O_CREAT | O_APPEND;
    default:       return O_WRONLY | O_CREAT | O_TRUNC;
    }
}

static const char *redir_flags_str(redir_kind k)
{
    switch (k) {
    case R_IN:     return "O_RDONLY";
    case R_APPEND: return "O_WRONLY|O_CREAT|O_APPEND";
    default:       return "O_WRONLY|O_CREAT|O_TRUNC";
    }
}

static const char *fd_role(int fd) { return fd == 0 ? "stdin" : fd == 1 ? "stdout" : "stderr"; }

static int apply_redir(const node *r, strbuf *log)
{
    int want = redir_target_fd(r->kind);
    int fd = open(r->target, redir_flags(r->kind), 0644);   /* 0644 applies only when O_CREAT creates */
    if (fd < 0) return -1;
    if (log) sb_printf(log, "%sopen%s(\"%s\", %s) = %d", TP.amb, TP.rst, r->target, redir_flags_str(r->kind), fd);
    if (fd != want) {
        if (dup2(fd, want) < 0) {
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        close(fd);                                           /* drop the extra reference */
        if (log) sb_printf(log, " · %sdup2%s(%d, %d) · %sclose%s(%d)", TP.amb, TP.rst, fd, want, TP.amb, TP.rst, fd);
    }
    return 0;
}

/* ---------- x-ray helpers for the child ---------------------------------------- */
static void describe_fds(strbuf *b, int skip1, int skip2)
{
    for (int fd = 0; fd < 64; fd++) {
        if (fd == skip1 || fd == skip2) continue;
        int fl = fcntl(fd, F_GETFD);
        if (fl < 0) continue;
        char path[48], target[256];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t k = readlink(path, target, sizeof target - 1);
        if (k < 0) strcpy(target, "?");
        else target[k] = '\0';
        const char *arrow = fd == 0 ? "←" : "→";
        if (fl & FD_CLOEXEC)
            sb_printf(b, "   %d %s %s %s(close-on-exec%s)%s", fd, arrow, target, TP.dim,
                      fd == sh.trace_fd ? ": the x-ray channel" : "", TP.rst);
        else if (fd > 2)
            sb_printf(b, "   %s%d %s %s (inherited: a leak)%s", TP.red, fd, arrow, target, TP.rst);
        else
            sb_printf(b, "   %d %s %s", fd, arrow, target);
    }
}

/* §3.6 walk $PATH the way execvp() will: an empty entry means "." */
static void describe_path(strbuf *b, const char *cmd)
{
    if (strchr(cmd, '/')) {
        sb_printf(b, "\"%s\" contains '/': no $PATH search", cmd);
        return;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    sb_printf(b, "%s$PATH%s ", TP.bld, TP.rst);
    const char *p = path;
    int misses = 0, folded = 0;
    for (;;) {
        const char *e   = strchr(p, ':');
        size_t      len = e ? (size_t)(e - p) : strlen(p);
        char dir[PATH_MAX], full[PATH_MAX + 256];
        if (len == 0) strcpy(dir, ".");
        else snprintf(dir, sizeof dir, "%.*s", (int)len, p);
        snprintf(full, sizeof full, "%s/%s", dir, cmd);
        const char *shown = len ? dir : ". (empty entry)";
        if (access(full, X_OK) == 0) {                    /* exists and may be executed */
            if (folded) sb_printf(b, "  %s… %d more ✗%s", TP.dim, folded, TP.rst);
            sb_printf(b, "  %s%s ✓%s", TP.grn, shown, TP.rst);
            return;
        }
        if (++misses <= 6) sb_printf(b, "  %s%s ✗%s", TP.dim, shown, TP.rst);
        else folded++;
        if (!e) break;
        p = e + 1;
    }
    if (folded) sb_printf(b, "  %s… %d more ✗%s", TP.dim, folded, TP.rst);
    sb_printf(b, "  %s→ not found%s", TP.red, TP.rst);
}

/* ---------- the child: everything between fork() and exec() (§3.5 "the gap") ---- */
static void run_child(node *stage, int idx, int n, const int *pfd, pid_t pgid, bool fg,
                      int up_w, int down_r, const int *older, int n_older)
{
    sh.in_child = true;
    pid_t  me   = getpid();
    node  *cmd  = stage_command(stage);
    bool   tr   = up_w >= 0, full = tr && sh.trace > 1;
    strbuf tb   = {0}, sum = {0};

    for (int k = 0; k < n_older; k++) close(older[k]);   /* release pipes of earlier siblings */

    if (full) trace_child_line(&tb, me, "fork() returned 0 — \"I am the child\", still running msh's code");

    pid_t group = pgid ? pgid : me;
    if (sh.interactive) {
        setpgid(0, group);                                /* §5.4 ① own process group (child side) */
        if (fg) tcsetpgrp(STDIN_FILENO, group);           /* §5.4 ② take the terminal (child side) */
    }
    signals_reset_child();                                /* SIG_DFL again; SIGCHLD unblocked (§5.2) */
    if (full) {
        if (sh.interactive)
            trace_child_line(&tb, me, "%ssetpgid%s(0, %d)%s · signals → SIG_DFL · SIGCHLD unblocked",
                             TP.amb, TP.rst, (int)group, fg ? " · tcsetpgrp(0, same)" : "");
        else
            trace_child_line(&tb, me, "no job control (stdin is not a terminal) · signals → SIG_DFL");
    }

    /* §4.3 wire the pipe ends, then §4.5 close every pipe end this child does not use */
    if (idx > 0) {
        dup2(pfd[2 * (idx - 1)], STDIN_FILENO);
        sb_printf(&sum, "stdin ← pipe (fd %d)", pfd[2 * (idx - 1)]);
    }
    if (idx < n - 1) {
        dup2(pfd[2 * idx + 1], STDOUT_FILENO);
        sb_printf(&sum, "%sstdout → pipe (fd %d)", sum.n ? " · " : "", pfd[2 * idx + 1]);
    }
    if (full && n > 1) {
        strbuf w = {0};
        if (idx > 0)     sb_printf(&w, "%sdup2%s(%d, 0) · ", TP.amb, TP.rst, pfd[2 * (idx - 1)]);
        if (idx < n - 1) sb_printf(&w, "%sdup2%s(%d, 1) · ", TP.amb, TP.rst, pfd[2 * idx + 1]);
        sb_printf(&w, "%sclose%s all %d pipe fds — keep only what was dup2()'d", TP.amb, TP.rst, 2 * (n - 1));
        trace_child_line(&tb, me, "%s", w.s);
        sb_free(&w);
    }
    for (int k = 0; k < 2 * (n - 1); k++) close(pfd[k]);

    /* §4.2 redirections come after the pipe wiring, so an explicit > wins */
    node **rs = NULL;
    int    nr = stage_redirs(stage, &rs);
    for (int k = 0; k < nr; k++) {
        strbuf rl = {0};
        if (apply_redir(rs[k], full ? &rl : NULL) < 0) {
            int e = errno;
            if (tr) {
                trace_child_line(&tb, me, "%sopen(\"%s\") failed: %s — the child reports it and exits%s",
                                 TP.red, rs[k]->target, errno_name(e), TP.rst);
                write_all(up_w, tb.s, tb.n);
                close(up_w);
            }
            fprintf(stderr, "msh: %s: %s\n", rs[k]->target, strerror(e));   /* §6.2 the shell survives */
            _exit(1);
        }
        if (full) trace_child_line(&tb, me, "%s", rl.s);
        sb_free(&rl);
        int t = redir_target_fd(rs[k]->kind);
        sb_printf(&sum, "%s%s %s \"%s\"", sum.n ? " · " : "", fd_role(t), t == 0 ? "←" : "→", rs[k]->target);
    }
    free(rs);

    builtin_fn fn = builtin_lookup(cmd->argv[0]);
    if (tr) {
        if (full) {
            strbuf f = {0};
            describe_fds(&f, up_w, down_r);
            trace_child_line(&tb, me, "fd table%s", f.s ? f.s : "");
            sb_free(&f);
        }
        const char *wiring = sum.n ? sum.s : "stdin, stdout, stderr → the terminal";
        if (fn) {
            trace_child_line(&tb, me, "%s · built-in '%s' runs in this child — whatever it changes dies with it",
                             wiring, cmd->argv[0]);
        } else {
            if (full) {
                strbuf pth = {0};
                describe_path(&pth, cmd->argv[0]);
                trace_child_line(&tb, me, "%s", pth.s);
                sb_free(&pth);
                int nenv = 0;
                while (environ[nenv]) nenv++;
                trace_child_line(&tb, me, "%s · %sexecvp%s(\"%s\") · envp: %d variables inherited",
                                 wiring, TP.amb, TP.rst, cmd->argv[0], nenv);
            } else {
                trace_child_line(&tb, me, "%s · %sexecvp%s(\"%s\")", wiring, TP.amb, TP.rst, cmd->argv[0]);
            }
        }
        write_all(up_w, tb.s, tb.n);                      /* hand the lines to the parent */
        close(up_w);
        char go;
        while (read(down_r, &go, 1) < 0 && errno == EINTR) {}   /* wait for the release */
        close(down_r);
    }
    sb_free(&tb);
    sb_free(&sum);

    if (fn) {
        int st = fn(cmd->argv);
        fflush(stdout);
        fflush(stderr);
        _exit(st & 0xff);
    }

    execvp(cmd->argv[0], cmd->argv);                      /* on success this never returns */
    int e = errno;
    if (e == ENOENT) {                                    /* §6.2 127, the same code bash uses */
        if (strchr(cmd->argv[0], '/'))                    /* a path: no $PATH search was made */
            fprintf(stderr, "msh: %s: %s\n", cmd->argv[0], strerror(e));
        else
            fprintf(stderr, "msh: %s: command not found\n", cmd->argv[0]);
        _exit(127);
    }
    fprintf(stderr, "msh: %s: %s\n", cmd->argv[0], strerror(e));
    _exit(126);                                           /* _exit, never exit: no double flush */
}

/* ---------- §3.8 a built-in in a single foreground command runs in the shell ------ */
static int run_builtin_here(node *stage, node *cmd, builtin_fn fn)
{
    bool tr = sh.trace > 0;
    if (tr) trace_line("built-in '%s' runs inside msh itself (pid %d) — no fork()", cmd->argv[0], (int)sh.pid);

    node **rs = NULL;
    int    nr = stage_redirs(stage, &rs);
    int    saved[3] = { -1, -1, -1 };
    int    status = 0;

    fflush(stdout);
    fflush(stderr);
    for (int k = 0; k < nr; k++) {
        int t = redir_target_fd(rs[k]->kind);
        if (saved[t] < 0) saved[t] = fcntl(t, F_DUPFD_CLOEXEC, 10);   /* keep the shell's own fd safe */
        strbuf rl = {0};
        if (apply_redir(rs[k], tr ? &rl : NULL) < 0) {
            int e = errno;
            sb_free(&rl);
            for (int f = 0; f < 3; f++)
                if (saved[f] >= 0) { dup2(saved[f], f); close(saved[f]); saved[f] = -1; }
            fprintf(stderr, "msh: %s: %s\n", rs[k]->target, strerror(e));
            free(rs);
            return 1;
        }
        if (tr) trace_line("saved fd %d as %d · %s", t, saved[t], rl.s);
        sb_free(&rl);
    }

    status = fn(cmd->argv);
    fflush(stdout);
    fflush(stderr);

    for (int f = 0; f < 3; f++) {
        if (saved[f] >= 0) {
            dup2(saved[f], f);
            close(saved[f]);
            if (tr) trace_line("restored fd %d — the shell's own descriptor is back", f);
        }
    }
    free(rs);
    return status;
}

/* the child's trace lines, relayed in order; EOF arrives when it closes its end */
static void relay(int fd)
{
    char buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (sh.trace_fd >= 0) write_all(sh.trace_fd, buf, (size_t)r);
    }
}

/* ---------- the parent ------------------------------------------------------------ */
int execute(ast *a)
{
    int    n  = count_stages(a->root);
    node **st = calloc((size_t)n, sizeof *st);
    if (!st) { perror("msh"); return 1; }
    int k = 0;
    collect_stages(a->root, st, &k);

    bool tr = sh.trace > 0, full = sh.trace > 1, fg = !a->background;
    if (tr) trace_section("EXECUTE");

    if (n == 1 && fg) {
        node      *cmd = stage_command(st[0]);
        builtin_fn fn  = builtin_lookup(cmd->argv[0]);
        if (fn) {
            int s = run_builtin_here(st[0], cmd, fn);
            free(st);
            return s;
        }
    }

    fflush(stdout);                   /* §6.2 leave nothing in stdio buffers for a child to copy */
    fflush(stderr);
    if (!sh.interactive) fflush(stdin);   /* script on a file: hand the unread bytes back */

    sigchld_block();                  /* §5.2 critical section opens: no reaping behind our back */
    if (full) trace_line("%ssigprocmask%s(SIG_BLOCK, {SIGCHLD}) — critical section opens", TP.amb, TP.rst);

    int    npfd    = 2 * (n - 1);
    int   *pfd     = npfd ? malloc((size_t)npfd * sizeof *pfd) : NULL;
    pid_t *pids    = calloc((size_t)n, sizeof *pids);
    int   *downs   = tr ? calloc((size_t)n, sizeof *downs) : NULL;
    int    status  = 1, started = 0;
    pid_t  pgid    = 0;

    if ((npfd && !pfd) || !pids || (tr && !downs)) {
        perror("msh");
        npfd = 0;
        goto out;
    }

    for (int i = 0; i < n - 1; i++) {                     /* §4.5 n commands → n-1 pipes */
        if (pipe(&pfd[2 * i]) < 0) {
            perror("msh: pipe");
            for (int j = 0; j < 2 * i; j++) close(pfd[j]);
            npfd = 0;
            goto out;
        }
        if (tr) trace_line("%spipe%s() = [%d, %d]   read end %d · write end %d", TP.amb, TP.rst,
                           pfd[2 * i], pfd[2 * i + 1], pfd[2 * i], pfd[2 * i + 1]);
    }

    for (int i = 0; i < n; i++) {                         /* … and n forks */
        int up[2] = { -1, -1 }, down[2] = { -1, -1 };
        if (tr) {
            if (pipe2(up, O_CLOEXEC) < 0) { perror("msh: pipe2"); break; }
            if (pipe2(down, O_CLOEXEC) < 0) { perror("msh: pipe2"); close(up[0]); close(up[1]); break; }
        }
        pid_t pid = fork();
        if (pid < 0) {                                    /* §6.2 report it, keep the loop running */
            perror("msh: fork");
            if (tr) { close(up[0]); close(up[1]); close(down[0]); close(down[1]); }
            break;
        }
        if (pid == 0) {
            if (tr) { close(up[0]); close(down[1]); }
            run_child(st[i], i, n, pfd, pgid, fg, up[1], down[0], downs, tr ? started : 0);
        }
        if (!pgid) pgid = pid;
        if (sh.interactive) setpgid(pid, pgid);          /* §5.4 ① parent side too: no race */
        pids[started++] = pid;
        if (tr) {
            close(up[1]);
            close(down[0]);
            node *cmd = stage_command(st[i]);
            trace_line("%sfork%s() = %s%d%s → child runs %s%s%s   (process group %d)", TP.amb, TP.rst,
                       TP.cyn, (int)pid, TP.rst, TP.bld, cmd->argv[0], TP.rst,
                       (int)(sh.interactive ? pgid : sh.pgid));
            relay(up[0]);
            close(up[0]);
            downs[started - 1] = down[1];
        }
    }

    if (npfd) {                                           /* §4.5 the parent keeps none … */
        strbuf cl = {0};
        for (int i = 0; i < npfd; i++) {
            bool write_end = (i % 2) == 1;
            if (sh.leak && write_end) {                   /* … unless MSH_LEAK=1 makes it "forget" */
                leaked_add(pfd[i]);
                sb_printf(&cl, "%s%s[fd %d left open!]%s", cl.n ? " · " : "", TP.red, pfd[i], TP.rst);
            } else {
                close(pfd[i]);
                sb_printf(&cl, "%s%sclose%s(%d)", cl.n ? " · " : "", TP.amb, TP.rst, pfd[i]);
            }
        }
        if (tr) trace_line("%s   %s", cl.s,
                           sh.leak ? "MSH_LEAK=1: the parent forgets its write ends (the §4.5 bug)"
                                   : "— the parent keeps none");
        sb_free(&cl);
        if (tr && sh.leak)
            for (int i = 0; i + 1 < n; i++)
                trace_line("%s↳ %s never sees end-of-file on this pipe: msh itself still holds write end %d%s",
                           TP.red, stage_command(st[i + 1])->argv[0], pfd[2 * i + 1], TP.rst);
    }

    if (fg && started && sh.interactive) {
        tcsetpgrp(STDIN_FILENO, pgid);                    /* §5.4 ② the terminal belongs to the job */
        if (tr) trace_line("%stcsetpgrp%s(0, %d) — the terminal now belongs to the job", TP.amb, TP.rst, (int)pgid);
    }

    if (tr && started) {                                  /* release every paused child at once */
        for (int j = 0; j < started; j++) close(downs[j]);
        trace_line("release → %s", started > 1 ? "the children continue into execvp()" : "the child continues into execvp()");
    }

    if (!started) {
        status = 1;
    } else if (fg) {
        bool newline = false;
        int  last_ws = 0, nstopped = 0;
        if (tr) trace_section("WAIT");
        for (int i = 0; i < started; i++) {
            int   ws = 0;
            pid_t r;
            do r = waitpid(pids[i], &ws, WUNTRACED);      /* §5.4 WUNTRACED: Ctrl-Z returns the prompt */
            while (r < 0 && errno == EINTR);
            if (r < 0) continue;
            bool from_keyboard = WIFSTOPPED(ws) ||
                                 (WIFSIGNALED(ws) && (WTERMSIG(ws) == SIGINT || WTERMSIG(ws) == SIGQUIT));
            if (from_keyboard && !newline) {              /* the terminal echoed ^C or ^Z */
                putchar('\n');
                fflush(stdout);
                newline = true;
            }
            char d[64];
            describe_status(ws, d, sizeof d);
            if (tr) trace_line("%swaitpid%s(%d) → %s", TP.amb, TP.rst, (int)pids[i], d);
            if (WIFSTOPPED(ws)) {
                pids[nstopped++] = pids[i];               /* i >= nstopped: safe to reuse */
            } else if (WIFSIGNALED(ws)) {
                int sg = WTERMSIG(ws);
                if (sg != SIGINT && sg != SIGPIPE) fprintf(stderr, "msh: %d %s\n", (int)pids[i], d);
            }
            if (i == started - 1) last_ws = ws;
        }
        status = status_code(last_ws);
        if (sh.interactive) {
            tcsetpgrp(STDIN_FILENO, sh.pgid);             /* §5.4 ④ the shell takes the terminal back */
            tcsetattr(STDIN_FILENO, TCSADRAIN, &sh.tmodes);
            if (tr) trace_line("%stcsetpgrp%s(0, %d) — the terminal is back with msh", TP.amb, TP.rst, (int)sh.pgid);
        }
        if (nstopped) {                                   /* Ctrl-Z: the prompt comes back */
            printf("%s[stopped]%s", OP.amb, OP.rst);
            for (int i = 0; i < nstopped; i++) printf(" %d", (int)pids[i]);
            printf("  · fg and bg are future work (§8.2) · end it with: kill -9");
            for (int i = 0; i < nstopped; i++) printf(" %d", (int)pids[i]);
            printf("\n");
            fflush(stdout);
        }
    } else {
        printf("%s[bg]%s", OP.amb, OP.rst);               /* §5.1 skip waitpid(): prompt returns now */
        for (int i = 0; i < started; i++) printf(" %d", (int)pids[i]);
        printf("\n");
        fflush(stdout);
        if (tr) {
            trace_section("WAIT");
            trace_line("skipped — '&' goes straight back to READ; the SIGCHLD handler reaps later");
        }
        status = 0;
    }

out:
    if (full) trace_line("%ssigprocmask%s(SIG_UNBLOCK, {SIGCHLD}) — critical section closes", TP.amb, TP.rst);
    sigchld_unblock();                /* §5.2 critical section closes: pending SIGCHLD runs now */
    free(downs);
    free(pids);
    free(pfd);
    free(st);
    return status;
}
