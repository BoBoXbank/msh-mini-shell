/* ==========================================================================
 * trace.c — the x-ray view (export MSH_TRACE=1, or 2 for full detail)
 *
 * The trace is written to its own descriptor: a copy of stderr made with
 * F_DUPFD_CLOEXEC, so it is closed automatically at exec (§4.6) and no
 * program msh starts ever inherits it.
 * ========================================================================== */
#include "msh.h"

static const palette PAL_ON  = { "\033[38;5;214m", "\033[2m", "\033[32m", "\033[31m",
                                 "\033[36m", "\033[34m", "\033[1m", "\033[0m" };
static const palette PAL_OFF = { "", "", "", "", "", "", "", "" };
palette OP, TP;

void palette_init(bool out_on, bool trace_on)
{
    OP = out_on ? PAL_ON : PAL_OFF;
    TP = trace_on ? PAL_ON : PAL_OFF;
}

/* ---------- small growable string ------------------------------------------ */
void sb_vprintf(strbuf *b, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    if (need < 0) { va_end(ap2); return; }
    if (b->n + (size_t)need + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 128;
        while (nc < b->n + (size_t)need + 1) nc *= 2;
        char *p = realloc(b->s, nc);
        if (!p) { va_end(ap2); return; }
        b->s = p;
        b->cap = nc;
    }
    vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap2);
    va_end(ap2);
    b->n += (size_t)need;
}

void sb_printf(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

void sb_free(strbuf *b)
{
    free(b->s);
    b->s = NULL;
    b->n = b->cap = 0;
}

void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void emit(const strbuf *b)
{
    if (sh.trace_fd >= 0 && b->n) write_all(sh.trace_fd, b->s, b->n);
}

/* ---------- the box ---------------------------------------------------------- */
static const char *pending_label;

void trace_header(const char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    strbuf b = {0};
    sb_printf(&b, "%s╭─ x-ray ─%s %s%.*s%s\n%s│%s\n",
              TP.dim, TP.rst, TP.bld, (int)n, line, TP.rst, TP.dim, TP.rst);
    emit(&b);
    sb_free(&b);
}

void trace_section(const char *label)
{
    pending_label = label;
}

static void trace_linev(const char *fmt, va_list ap)
{
    strbuf b = {0};
    const char *lab = pending_label ? pending_label : "";
    pending_label = NULL;
    sb_printf(&b, "%s│  %-8s%s  ", TP.dim, lab, TP.rst);
    sb_vprintf(&b, fmt, ap);
    sb_printf(&b, "\n");
    emit(&b);
    sb_free(&b);
}

void trace_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    trace_linev(fmt, ap);
    va_end(ap);
}

void trace_gap(void)
{
    strbuf b = {0};
    sb_printf(&b, "%s│%s\n", TP.dim, TP.rst);
    emit(&b);
    sb_free(&b);
}

void trace_footer(int status)
{
    strbuf b = {0};
    sb_printf(&b, "%s╰─%s status %s%d%s\n", TP.dim, TP.rst, status ? TP.red : TP.grn, status, TP.rst);
    emit(&b);
    sb_free(&b);
}

/* Lines written by a child are collected in its own buffer and handed to the
   parent through a pipe, so the parent can print them in order. */
void trace_child_line(strbuf *b, pid_t pid, const char *fmt, ...)
{
    sb_printf(b, "%s│              ┊%s %s%d%s  ", TP.dim, TP.rst, TP.cyn, (int)pid, TP.rst);
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
    sb_printf(b, "\n");
}

/* ---------- §2.2 TOKENIZE: text on top, type underneath -------------------- */
void trace_tokens(const token_list *t)
{
    strbuf top = {0}, bot = {0};
    for (size_t i = 0; i < t->n; i++) {
        const char *tx = t->v[i].text, *ty = tok_type_name(t->v[i].type);
        int  w  = (int)(strlen(tx) > strlen(ty) ? strlen(tx) : strlen(ty));
        bool op = t->v[i].type != T_WORD;
        const char *gap = i ? "  " : "";
        if (i + 1 < t->n) {                               /* pad to the column width … */
            sb_printf(&top, "%s%s%-*s%s", gap, op ? TP.amb : "", w, tx, op ? TP.rst : "");
            sb_printf(&bot, "%s%s%-*s%s", gap, TP.dim, w, ty, TP.rst);
        } else {                                          /* … but no trailing blanks */
            sb_printf(&top, "%s%s%s%s", gap, op ? TP.amb : "", tx, op ? TP.rst : "");
            sb_printf(&bot, "%s%s%s%s", gap, TP.dim, ty, TP.rst);
        }
    }
    trace_section("TOKENIZE");
    trace_line("%s", top.s ? top.s : "");
    trace_line("%s", bot.s ? bot.s : "");
    trace_gap();
    sb_free(&top);
    sb_free(&bot);
}

/* ---------- §2.3 PARSE: the tree ---------------------------------------------- */
const char *redir_name(redir_kind k)
{
    switch (k) {
    case R_IN:     return "REDIR_IN";
    case R_OUT:    return "REDIR_OUT";
    case R_APPEND: return "REDIR_APPEND";
    case R_ERR:    return "REDIR_ERR";
    }
    return "REDIR";
}

const char *redir_op(redir_kind k)
{
    switch (k) {
    case R_IN:     return "<";
    case R_OUT:    return ">";
    case R_APPEND: return ">>";
    case R_ERR:    return "2>";
    }
    return "?";
}

static void node_desc(strbuf *b, const node *n)
{
    switch (n->type) {
    case N_PIPE:
        sb_printf(b, "%s%sPIPE%s", TP.bld, TP.amb, TP.rst);
        break;
    case N_REDIR:
        sb_printf(b, "%s%s%s%s  %s \"%s\"", TP.bld, TP.amb, redir_name(n->kind), TP.rst,
                  redir_op(n->kind), n->target);
        break;
    case N_COMMAND:
        sb_printf(b, "%sCOMMAND%s  argv = {", TP.bld, TP.rst);
        for (int k = 0; k < n->argc; k++) sb_printf(b, "%s\"%s\"", k ? ", " : "", n->argv[k]);
        sb_printf(b, "}");
        break;
    }
}

static void ast_rec(const node *n, const char *prefix, bool last, bool root, bool bg)
{
    strbuf line = {0};
    if (!root) sb_printf(&line, "%s%s%s%s", TP.dim, prefix, last ? "└── " : "├── ", TP.rst);
    node_desc(&line, n);
    if (root && bg) sb_printf(&line, "   %s[background = true]%s", TP.amb, TP.rst);
    trace_line("%s", line.s);
    sb_free(&line);

    char next[512];
    snprintf(next, sizeof next, "%s%s", prefix, root ? "" : (last ? "    " : "│   "));
    if (n->type == N_PIPE) {
        ast_rec(n->left, next, false, false, false);
        ast_rec(n->right, next, true, false, false);
    } else if (n->type == N_REDIR) {
        ast_rec(n->child, next, true, false, false);
    }
}

void trace_ast(const ast *a)
{
    trace_section("PARSE");
    ast_rec(a->root, "", true, true, a->background);
    trace_gap();
}

/* ---------- name tables (portable: no glibc-only helpers) ------------------- */
const char *sig_name(int sig)
{
    static const struct { int n; const char *s; } t[] = {
        { SIGHUP, "SIGHUP" },   { SIGINT, "SIGINT" },   { SIGQUIT, "SIGQUIT" }, { SIGILL, "SIGILL" },
        { SIGABRT, "SIGABRT" }, { SIGBUS, "SIGBUS" },   { SIGFPE, "SIGFPE" },   { SIGKILL, "SIGKILL" },
        { SIGUSR1, "SIGUSR1" }, { SIGSEGV, "SIGSEGV" }, { SIGUSR2, "SIGUSR2" }, { SIGPIPE, "SIGPIPE" },
        { SIGALRM, "SIGALRM" }, { SIGTERM, "SIGTERM" }, { SIGCHLD, "SIGCHLD" }, { SIGCONT, "SIGCONT" },
        { SIGSTOP, "SIGSTOP" }, { SIGTSTP, "SIGTSTP" }, { SIGTTIN, "SIGTTIN" }, { SIGTTOU, "SIGTTOU" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i].n == sig) return t[i].s;
    static char buf[24];
    snprintf(buf, sizeof buf, "signal %d", sig);
    return buf;
}

const char *errno_name(int e)
{
    static const struct { int n; const char *s; } t[] = {
        { ENOENT, "ENOENT" }, { EACCES, "EACCES" }, { ENOTDIR, "ENOTDIR" }, { EISDIR, "EISDIR" },
        { EEXIST, "EEXIST" }, { EMFILE, "EMFILE" }, { EAGAIN, "EAGAIN" },   { ENOMEM, "ENOMEM" },
        { EPERM, "EPERM" },   { ENOEXEC, "ENOEXEC" }, { ELOOP, "ELOOP" },   { EINTR, "EINTR" },
        { ECHILD, "ECHILD" }, { EBADF, "EBADF" },   { EROFS, "EROFS" },     { ENAMETOOLONG, "ENAMETOOLONG" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i].n == e) return t[i].s;
    static char buf[24];
    snprintf(buf, sizeof buf, "errno %d", e);
    return buf;
}

void describe_status(int ws, char *buf, size_t len)
{
    if (WIFEXITED(ws))        snprintf(buf, len, "exited %d", WEXITSTATUS(ws));
    else if (WIFSIGNALED(ws)) snprintf(buf, len, "killed by %s%s", sig_name(WTERMSIG(ws)),
                                       WCOREDUMP(ws) ? " (core dumped)" : "");
    else if (WIFSTOPPED(ws))  snprintf(buf, len, "stopped by %s", sig_name(WSTOPSIG(ws)));
    else                      snprintf(buf, len, "status 0x%x", (unsigned)ws);
}

int status_code(int ws)
{
    if (WIFEXITED(ws))   return WEXITSTATUS(ws);
    if (WIFSIGNALED(ws)) return 128 + WTERMSIG(ws);
    if (WIFSTOPPED(ws))  return 128 + WSTOPSIG(ws);
    return 1;
}
