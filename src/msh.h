/* ==========================================================================
 * msh.h — shared declarations for msh, a mini Unix shell
 *
 * Group 4 · 03603332 Operating Systems
 * Every "§x.y" marker in this code points at the slide that explains it.
 * ========================================================================== */
#ifndef MSH_H
#define MSH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE               /* pipe2(), strndup(), F_DUPFD_CLOEXEC with a plain gcc command */
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MSH_MAX_LINE 65536        /* §6.3 a pathological line is rejected */

/* ---------- §2.2 tokens ---------------------------------------------------- */
typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_ERR, T_AMP, T_SEMI } tok_type;

typedef struct {
    tok_type type;
    char    *text;                /* strdup()'d: every token is copied, not pointed at */
} token;

typedef struct {
    token  *v;
    size_t  n, cap;
} token_list;

int         tokenize(const char *line, token_list *out);
void        tokens_free(token_list *t);
const char *tok_type_name(tok_type t);

/* ---------- §2.3 abstract syntax tree -------------------------------------- */
typedef enum { N_COMMAND, N_PIPE, N_REDIR } node_type;
typedef enum { R_IN, R_OUT, R_APPEND, R_ERR } redir_kind;

typedef struct node {
    node_type    type;
    char       **argv;            /* N_COMMAND: NULL-terminated argument vector */
    int          argc;
    struct node *left, *right;    /* N_PIPE:   left | right                      */
    redir_kind   kind;            /* N_REDIR:  kind, target file, wrapped child  */
    char        *target;
    struct node *child;
} node;

typedef struct {
    node *root;
    bool  background;             /* '&' is a flag on the root, not a node */
} ast;

int  parse(const token_list *t, ast *out, char *err, size_t errlen);
void ast_free(node *n);           /* §6.1 post-order: children before their parent */

/* ---------- shell state ---------------------------------------------------- */
typedef struct {
    bool           interactive;   /* stdin is a terminal: job control is on      */
    bool           color;         /* §8.1 escape codes only when stdout is a tty */
    pid_t          pid, pgid;
    struct termios tmodes;        /* terminal modes, restored after every job    */
    int            trace_fd;      /* x-ray channel: a stderr copy with FD_CLOEXEC */
    int            trace;         /* MSH_TRACE  0 off · 1 summary · 2 full detail */
    bool           leak;          /* MSH_LEAK=1 reproduces the §4.5 bug on purpose */
    bool           in_child;      /* true after fork(), before exec()            */
    int            last_status;
    bool           exit_requested;
    int            exit_code;
} shell_t;

extern shell_t sh;

/* ---------- executor: §3 process creation · §4 I/O plumbing · §5.4 jobs ---- */
int  execute(ast *a);
int  leaked_count(void);
void leaked_close_all(void);

/* ---------- §3.8 built-ins -------------------------------------------------- */
typedef int (*builtin_fn)(char **argv);
builtin_fn builtin_lookup(const char *name);

/* ---------- §5.2 signals ---------------------------------------------------- */
extern volatile sig_atomic_t g_reaper_on;
void signals_install_sigchld(void);
void signals_ignore_job_control(void);
void signals_reset_child(void);
void sigchld_block(void);
void sigchld_unblock(void);
int  reap_now(void);
void print_done_notices(void);

/* ---------- x-ray trace and small helpers ---------------------------------- */
typedef struct { const char *amb, *dim, *grn, *red, *cyn, *blu, *bld, *rst; } palette;
extern palette OP;                /* colors for normal output (stdout)  */
extern palette TP;                /* colors for the x-ray channel       */
void palette_init(bool out_on, bool trace_on);

typedef struct { char *s; size_t n, cap; } strbuf;
void sb_printf(strbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_vprintf(strbuf *b, const char *fmt, va_list ap);
void sb_free(strbuf *b);
void write_all(int fd, const char *p, size_t n);

void trace_header(const char *line);
void trace_section(const char *label);
void trace_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void trace_gap(void);
void trace_footer(int status);
void trace_tokens(const token_list *t);
void trace_ast(const ast *a);
void trace_child_line(strbuf *b, pid_t pid, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

const char *redir_name(redir_kind k);
const char *redir_op(redir_kind k);
const char *sig_name(int sig);
const char *errno_name(int e);
void        describe_status(int ws, char *buf, size_t len);
int         status_code(int ws);

#endif
