/* ==========================================================================
 * signals.c — §5.2 reaping asynchronously, and the race inside the fix
 *
 * The SIGCHLD handler reaps every finished child without blocking. It may
 * only call async-signal-safe functions, so it never prints: it records
 * (pid, status) pairs, and the main loop prints "[done]" before the prompt.
 * ========================================================================== */
#include "msh.h"

volatile sig_atomic_t g_reaper_on = 1;   /* MSH_REAPER=0 turns reaping off (§5.1 demo) */

#define MAXDONE 128
static pid_t done_pid[MAXDONE];
static int   done_status[MAXDONE];
static volatile sig_atomic_t done_n, done_lost;

static void record(pid_t pid, int st)
{
    if (done_n < MAXDONE) {
        done_pid[done_n]    = pid;
        done_status[done_n] = st;
        done_n = done_n + 1;
    } else {
        done_lost = done_lost + 1;
    }
}

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;                                   /* keep main code safe */
    if (g_reaper_on) {
        pid_t pid;
        int   st;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0)   /* while, not if: signals do not queue */
            record(pid, st);                             /* WNOHANG: never block in a handler */
    }
    errno = saved;
}

void signals_install_sigchld(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;                            /* resume getline() after the handler */
    sigaction(SIGCHLD, &sa, NULL);
}

/* §5.4 the shell itself must survive Ctrl-C and Ctrl-Z, and must be able to
   take the terminal back with tcsetpgrp() (hence SIGTTOU as well). */
void signals_ignore_job_control(void)
{
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU };
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &ign, NULL);
}

/* In the child, before exec: ignored signals stay ignored across exec (caught
   ones are reset), so every job-control signal goes back to SIG_DFL here.
   SIGPIPE too — a parent process may have started msh with it ignored. */
void signals_reset_child(void)
{
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE };
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &dfl, NULL);

    sigset_t m;                                          /* signal masks survive fork and exec */
    sigemptyset(&m);
    sigaddset(&m, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
}

void sigchld_block(void)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGCHLD);
    sigprocmask(SIG_BLOCK, &m, NULL);
}

void sigchld_unblock(void)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
}

/* Used when MSH_REAPER goes back to 1: reap the zombies that piled up. */
int reap_now(void)
{
    int   k = 0;
    pid_t pid;
    int   st;
    sigchld_block();
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        record(pid, st);
        k++;
    }
    sigchld_unblock();
    return k;
}

void print_done_notices(void)
{
    pid_t p[MAXDONE];
    int   s[MAXDONE];
    int   n, lost;

    sigchld_block();                                     /* the handler shares these arrays */
    n = done_n;
    for (int i = 0; i < n; i++) { p[i] = done_pid[i]; s[i] = done_status[i]; }
    lost = done_lost;
    done_n = 0;
    done_lost = 0;
    sigchld_unblock();

    for (int i = 0; i < n; i++) {
        char d[64];
        describe_status(s[i], d, sizeof d);
        printf("%s[done]%s %d  %s\n", OP.dim, OP.rst, (int)p[i], d);
    }
    if (lost) printf("%s[done]%s ... and %d more\n", OP.dim, OP.rst, lost);
    if (n || lost) fflush(stdout);
}
