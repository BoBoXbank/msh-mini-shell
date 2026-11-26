/* ==========================================================================
 * main.c — §2.1 the loop:  READ → TOKENIZE → PARSE → EXECUTE → WAIT → again
 *
 * Also here: §5.4 taking control of the terminal at start-up, §8.1 the
 * prompt, and the demo switches. The switches are ordinary environment
 * variables, changed at the msh prompt with the export built-in (§3.7) and
 * read again before every prompt:
 *
 *   MSH_TRACE=1|2   x-ray: print every step of the loop and its system calls
 *   MSH_LEAK=1      the §4.5 bug on purpose: the parent keeps pipe write ends
 *   MSH_REAPER=0    the §5.1 bug on purpose: nobody reaps background jobs
 * ========================================================================== */
#include "msh.h"
#include <pwd.h>
#include <strings.h>

shell_t sh = { .trace_fd = -1 };

static pid_t orig_fg = -1;       /* the group that owned the terminal before msh */

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%smsh:%s ", OP.amb, OP.rst);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

/* ---------- the demo switches ------------------------------------------------ */
static int env_level(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false"))
        return 0;
    if (!strcmp(v, "2") || !strcasecmp(v, "full")) return 2;
    return 1;
}

static void read_toggles(void)
{
    int  trace = env_level("MSH_TRACE", 0);
    bool leak  = env_level("MSH_LEAK", 0) > 0;
    bool reap  = env_level("MSH_REAPER", 1) > 0;

    if (trace != sh.trace) {
        if (trace && sh.trace_fd < 0) {
            sh.trace_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 10);   /* §4.6 no child inherits it */
        } else if (!trace && sh.trace_fd >= 0) {
            close(sh.trace_fd);
            sh.trace_fd = -1;
        }
        sh.trace = trace;
        if (trace == 0)      note("x-ray off");
        else if (trace == 1) note("x-ray ON — every step of the loop and the system calls behind it");
        else                 note("x-ray ON, full detail — plus fd tables, the $PATH walk and the signal mask");
    }

    if (leak != sh.leak) {
        sh.leak = leak;
        if (leak) {
            note("leak mode ON — the parent will forget to close pipe write ends (§4.5)");
        } else {
            int k = leaked_count();
            leaked_close_all();
            note("leak mode off — closed %d leaked descriptor%s", k, k == 1 ? "" : "s");
        }
    }

    if (reap != (g_reaper_on != 0)) {
        g_reaper_on = reap;
        if (reap) {
            note("reaper on — the SIGCHLD handler collects finished children again (§5.2)");
            reap_now();                                   /* the zombies that piled up */
        } else {
            note("reaper OFF — finished background jobs will stay zombies (§5.1)");
        }
    }
}

/* ---------- §8.1 the prompt, rebuilt on every iteration ------------------------ */
static void print_prompt(void)
{
    char       *cwd  = getcwd(NULL, 0);                  /* true even after a failed cd */
    const char *home = getenv("HOME");
    const char *user = getenv("USER");
    if (!user || !*user) {
        struct passwd *pw = getpwuid(getuid());
        user = pw ? pw->pw_name : "?";
    }
    char host[256];
    if (gethostname(host, sizeof host) != 0) snprintf(host, sizeof host, "localhost");
    host[sizeof host - 1] = '\0';
    host[strcspn(host, ".")] = '\0';

    const char *dir   = cwd ? cwd : "?";
    const char *tilde = "";
    size_t      hl    = home ? strlen(home) : 0;
    if (cwd && hl > 1 && !strncmp(cwd, home, hl) && (cwd[hl] == '/' || cwd[hl] == '\0')) {
        tilde = "~";
        dir   = cwd + hl;
    }

    printf("%s%smsh%s ", OP.bld, OP.amb, OP.rst);
    if (sh.last_status) printf("%s[%d]%s ", OP.red, sh.last_status, OP.rst);
    printf("%s%s%s@%s%s:%s%s%s%s%s$ ", OP.bld, OP.grn, user, host, OP.rst,
           OP.bld, OP.blu, tilde, dir, OP.rst);
    fflush(stdout);
    free(cwd);
}

static void banner(void)
{
    static const char *logo[] = {
        "███╗   ███╗███████╗██╗  ██╗",
        "████╗ ████║██╔════╝██║  ██║",
        "██╔████╔██║███████╗███████║",
        "██║╚██╔╝██║╚════██║██╔══██║",
        "██║ ╚═╝ ██║███████║██║  ██║",
        "╚═╝     ╚═╝╚══════╝╚═╝  ╚═╝",
    };
    static const int shade[] = { 220, 220, 214, 214, 208, 208 };   /* amber, top to bottom */

    const char *tty = ttyname(STDIN_FILENO);
    char info[6][256];
    snprintf(info[0], sizeof info[0], "%smsh%s · a mini Unix shell, written in C", OP.bld, OP.rst);
    snprintf(info[1], sizeof info[1], "%sGroup 4 · 03603332 Operating Systems%s", OP.dim, OP.rst);
    snprintf(info[2], sizeof info[2], "pid %d · process group %d · %s",
             (int)sh.pid, (int)sh.pgid, tty ? tty : "no terminal");
    snprintf(info[3], sizeof info[3], "built-ins  %scd  pwd  export  exit%s", OP.amb, OP.rst);
    snprintf(info[4], sizeof info[4], "x-ray      %sexport MSH_TRACE=1%s  (or 2)", OP.amb, OP.rst);
    snprintf(info[5], sizeof info[5], "bug modes  %sMSH_LEAK=1%s · %sMSH_REAPER=0%s",
             OP.amb, OP.rst, OP.amb, OP.rst);

    printf("\n");
    for (int i = 0; i < 6; i++) {
        if (sh.color) printf("  \033[1;38;5;%dm%s\033[0m   %s\n", shade[i], logo[i], info[i]);
        else          printf("  %s   %s\n", logo[i], info[i]);
    }
    printf("\n  %stry%s  ls -l | wc -l   ·   export MSH_TRACE=1   ·   Ctrl-D or exit to leave\n\n",
           OP.dim, OP.rst);
    fflush(stdout);
}

/* ---------- §5.4 take the terminal (GNU libc manual, "Initializing the Shell") -- */
static void job_control_init(void)
{
    sh.pid = getpid();
    sh.pgid = getpgrp();
    sh.interactive = isatty(STDIN_FILENO);
    if (!sh.interactive) return;                          /* input from a pipe: no job control */

    pid_t fg;
    while ((fg = tcgetpgrp(STDIN_FILENO)) != (sh.pgid = getpgrp())) {
        if (fg < 0) {                                     /* a terminal, but not ours */
            sh.interactive = false;
            return;
        }
        kill(-sh.pgid, SIGTTIN);                          /* started in the background: wait */
    }

    signals_ignore_job_control();                         /* Ctrl-C / Ctrl-Z cannot touch msh */
    orig_fg = sh.pgid;
    if (setpgid(sh.pid, sh.pid) < 0 && errno != EPERM) {  /* EPERM: already a session leader */
        perror("msh: setpgid");
        exit(1);
    }
    sh.pgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, sh.pgid);                     /* the terminal belongs to msh */
    tcgetattr(STDIN_FILENO, &sh.tmodes);                  /* restored after every job */
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: msh [-q]\n"
        "\n"
        "  -q, --quiet    start without the banner\n"
        "  -h, --help     show this text\n"
        "\n"
        "switches, changed at the msh prompt with export and read before every prompt:\n"
        "  MSH_TRACE=1    x-ray: show every step of the loop and the system calls behind it\n"
        "  MSH_TRACE=2    x-ray, full detail: fd tables, the $PATH walk, the signal mask\n"
        "  MSH_LEAK=1     bug mode: the parent forgets to close pipe write ends (pipelines hang)\n"
        "  MSH_REAPER=0   bug mode: finished background jobs are never reaped (zombies)\n"
        "  NO_COLOR=1     plain output, no escape codes\n");
}

int main(int argc, char **argv)
{
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            quiet = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "msh: %s: unknown argument (running a file of commands is future work, §8.2)\n",
                    argv[i]);
            usage(stderr);
            return 2;
        }
    }

    job_control_init();                                   /* §5.4 */
    signals_install_sigchld();                            /* §5.2 */
    const char *nc = getenv("NO_COLOR");
    bool no_color = nc && *nc;
    sh.color = !no_color && isatty(STDOUT_FILENO);        /* §8.1 escape codes only for a terminal */
    palette_init(sh.color, !no_color && isatty(STDERR_FILENO));
    if (sh.interactive && !quiet) banner();

    char  *line = NULL;                                   /* §6.3 getline() sizes the buffer */
    size_t cap  = 0;
    for (;;) {
        read_toggles();
        print_done_notices();                             /* "[done]" lines from the reaper */
        if (sh.interactive) print_prompt();

        /* READ */
        errno = 0;
        ssize_t len = getline(&line, &cap, stdin);
        if (len < 0) {
            if (errno == EINTR) {                         /* §6.2 a signal, not the end */
                clearerr(stdin);
                continue;
            }
            if (sh.interactive) printf("exit\n");         /* Ctrl-D: a clean exit */
            break;
        }
        if (len > MSH_MAX_LINE) {                         /* §6.3 reject, do not process */
            fprintf(stderr, "msh: line too long (%zd bytes, the limit is %d) — ignored\n",
                    len, MSH_MAX_LINE);
            free(line);                                   /* do not keep the huge buffer */
            line = NULL;
            cap  = 0;
            sh.last_status = 2;
            continue;
        }

        /* TOKENIZE */
        token_list toks;
        if (tokenize(line, &toks) < 0) {
            fprintf(stderr, "msh: out of memory\n");
            sh.last_status = 1;
            continue;
        }
        if (toks.n == 0) {                                /* §6.2 blank line: never fork */
            tokens_free(&toks);
            continue;
        }

        /* PARSE */
        ast  tree;
        char err[160];
        int  rc = parse(&toks, &tree, err, sizeof err);
        if (sh.trace) {
            trace_header(line);
            trace_section("READ");
            trace_line("getline() = %zd bytes · buffer of %zu bytes, sized by getline itself", len, cap);
            trace_gap();
            trace_tokens(&toks);
            if (rc == 0) {
                trace_ast(&tree);
            } else {
                trace_section("PARSE");
                trace_line("%s%s%s", TP.red, err, TP.rst);
            }
        }
        tokens_free(&toks);                               /* §6.1 freed on the error path too */
        if (rc < 0) {
            fprintf(stderr, "msh: %s\n", err);
            sh.last_status = 2;
            if (sh.trace) trace_footer(2);
            continue;
        }

        /* EXECUTE and WAIT */
        sh.last_status = execute(&tree);
        if (sh.trace) trace_footer(sh.last_status);
        ast_free(tree.root);                              /* §6.1 clean before the next prompt */
        if (sh.exit_requested) break;
    }

    free(line);
    leaked_close_all();
    if (sh.trace_fd >= 0) close(sh.trace_fd);
    if (sh.interactive && orig_fg > 0) tcsetpgrp(STDIN_FILENO, orig_fg);   /* hand it back */
    return sh.exit_requested ? sh.exit_code : sh.last_status;
}
