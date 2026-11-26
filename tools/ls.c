/* ==========================================================================
 * ls.c — our own ls (§3.9): a program, not a built-in
 *
 * opendir() and readdir() read the directory entries. For -l, lstat() on
 * each entry gives its type, size, owner, permissions and time — lstat, not
 * stat, so a symbolic link is described as the link itself.
 *
 * msh finds this program through $PATH and runs it in a forked child, so
 * > and | work on it unchanged: like any program, it just writes to
 * descriptor 1 and never knows where that descriptor leads.
 *
 *   usage: ls [-a] [-l] [-1] [file ...]
 *     -a   also show names that start with '.'
 *     -l   long format: mode, links, owner, group, size, time, name
 *     -1   one name per line (automatic when descriptor 1 is not a terminal)
 * ========================================================================== */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <locale.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

typedef struct {
    char       *name;             /* what is printed */
    char       *path;             /* what lstat() and readlink() are given */
    struct stat st;
    bool        ok;               /* st is valid */
} entry;

static bool opt_all, opt_long, opt_one, color;
static int  status;               /* 0 ok · 1 minor trouble · 2 an operand was unusable */

static void usage(FILE *f)
{
    fputs("usage: ls [-a] [-l] [-1] [file ...]\n", f);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { perror("ls"); exit(2); }
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p) { perror("ls"); exit(2); }
    return p;
}

static char *join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name), k = a;
    char  *p = xmalloc(a + b + 2);
    memcpy(p, dir, a);
    if (a && dir[a - 1] != '/') p[k++] = '/';
    memcpy(p + k, name, b + 1);
    return p;
}

/* screen columns, not bytes: a Thai or accented name is narrower than its UTF-8 */
static int width_of(const char *s)
{
    wchar_t w[512];
    size_t  n = mbstowcs(w, s, 512);
    if (n == (size_t)-1 || n >= 512) return (int)strlen(s);
    int cols = wcswidth(w, n);
    return cols < 0 ? (int)strlen(s) : cols;
}

static const char *color_of(const entry *e)
{
    if (!color || !e->ok) return NULL;
    mode_t m = e->st.st_mode;
    if (S_ISDIR(m))               return "\033[1;34m";
    if (S_ISLNK(m))               return "\033[1;36m";
    if (S_ISFIFO(m))              return "\033[33m";
    if (S_ISSOCK(m))              return "\033[1;35m";
    if (S_ISCHR(m) || S_ISBLK(m)) return "\033[1;33m";
    if (m & (S_IXUSR | S_IXGRP | S_IXOTH)) return "\033[1;32m";
    return NULL;
}

static void put_name(const entry *e)
{
    const char *c = color_of(e);
    if (c) printf("%s%s\033[0m", c, e->name);
    else   fputs(e->name, stdout);
}

static void mode_string(mode_t m, char out[11])
{
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b'
           : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (1u << (8 - i))) ? rwx[i] : '-';
    if (m & S_ISUID) out[3] = (m & S_IXUSR) ? 's' : 'S';
    if (m & S_ISGID) out[6] = (m & S_IXGRP) ? 's' : 'S';
    if (m & S_ISVTX) out[9] = (m & S_IXOTH) ? 't' : 'T';
    out[10] = '\0';
}

/* -l: every column is sized to its widest value first, then printed */
static void print_long(entry *v, size_t n, bool show_total)
{
    char (*user)[64] = xmalloc(n * sizeof *user);
    char (*group)[64] = xmalloc(n * sizeof *group);
    int  wl = 1, wu = 1, wg = 1, ws = 1;
    long long blocks = 0;

    for (size_t i = 0; i < n; i++) {
        if (!v[i].ok) continue;
        struct passwd *pw = getpwuid(v[i].st.st_uid);
        struct group  *gr = getgrgid(v[i].st.st_gid);
        if (pw) snprintf(user[i], 64, "%s", pw->pw_name);
        else    snprintf(user[i], 64, "%u", (unsigned)v[i].st.st_uid);
        if (gr) snprintf(group[i], 64, "%s", gr->gr_name);
        else    snprintf(group[i], 64, "%u", (unsigned)v[i].st.st_gid);

        char tmp[32];
        int  k = snprintf(tmp, sizeof tmp, "%lu", (unsigned long)v[i].st.st_nlink);
        if (k > wl) wl = k;
        if ((k = (int)strlen(user[i])) > wu) wu = k;
        if ((k = (int)strlen(group[i])) > wg) wg = k;
        if ((k = snprintf(tmp, sizeof tmp, "%lld", (long long)v[i].st.st_size)) > ws) ws = k;
        blocks += v[i].st.st_blocks;                      /* counted in 512-byte units */
    }
    if (show_total) printf("total %lld\n", (blocks + 1) / 2);   /* shown in KiB, like GNU ls */

    time_t now = time(NULL);
    for (size_t i = 0; i < n; i++) {
        if (!v[i].ok) continue;
        char mode[11], when[64];
        mode_string(v[i].st.st_mode, mode);
        struct tm tm;
        localtime_r(&v[i].st.st_mtime, &tm);
        bool recent = v[i].st.st_mtime > now - 15778476 && v[i].st.st_mtime < now + 3600;
        strftime(when, sizeof when, recent ? "%b %e %H:%M" : "%b %e  %Y", &tm);   /* 6 months */

        printf("%s %*lu %-*s %-*s %*lld %s ", mode, wl, (unsigned long)v[i].st.st_nlink,
               wu, user[i], wg, group[i], ws, (long long)v[i].st.st_size, when);
        put_name(&v[i]);
        if (S_ISLNK(v[i].st.st_mode)) {
            char    target[4096];
            ssize_t k = readlink(v[i].path, target, sizeof target - 1);
            if (k >= 0) {
                target[k] = '\0';
                printf(" -> %s", target);
            }
        }
        putchar('\n');
    }
    free(user);
    free(group);
}

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    const char *c = getenv("COLUMNS");
    if (c && atoi(c) > 0) return atoi(c);
    return 80;
}

/* on a terminal: names in columns, filled top to bottom, like ls -C */
static void print_columns(entry *v, size_t n)
{
    if (!n) return;
    int maxw = 0;
    for (size_t i = 0; i < n; i++) {
        int w = width_of(v[i].name);
        if (w > maxw) maxw = w;
    }
    int    colw = maxw + 2;
    int    cols = term_width() / colw;
    if (cols < 1) cols = 1;
    size_t rows = (n + (size_t)cols - 1) / (size_t)cols;

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0;; c++) {
            size_t i = c * rows + r;
            if (i >= n) break;
            put_name(&v[i]);
            if ((c + 1) * rows + r < n)
                for (int pad = colw - width_of(v[i].name); pad > 0; pad--) putchar(' ');
        }
        putchar('\n');
    }
}

static void print_list(entry *v, size_t n, bool whole_directory)
{
    if (opt_long) {
        print_long(v, n, whole_directory);
    } else if (!opt_one && isatty(STDOUT_FILENO)) {
        print_columns(v, n);
    } else {
        for (size_t i = 0; i < n; i++) {                  /* into a pipe or a file: one per line */
            put_name(&v[i]);
            putchar('\n');
        }
    }
}

static int by_name(const void *a, const void *b)
{
    return strcoll(((const entry *)a)->name, ((const entry *)b)->name);
}

static int by_string(const void *a, const void *b)
{
    return strcoll(*(char *const *)a, *(char *const *)b);
}

static void free_entries(entry *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(v[i].name);
        free(v[i].path);
    }
    free(v);
}

static void list_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot open directory '%s': %s\n", path, strerror(errno));
        status = 2;
        return;
    }

    size_t n = 0, cap = 64;
    entry *v = xmalloc(cap * sizeof *v);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && !opt_all) continue;    /* hidden unless -a */
        if (n == cap) {
            cap *= 2;
            entry *p = realloc(v, cap * sizeof *v);
            if (!p) { perror("ls"); exit(2); }
            v = p;
        }
        v[n].name = xstrdup(e->d_name);
        v[n].path = join(path, e->d_name);
        v[n].ok   = false;
        if (opt_long || color) {
            if (lstat(v[n].path, &v[n].st) == 0) {
                v[n].ok = true;
            } else if (opt_long) {
                fprintf(stderr, "ls: cannot access '%s': %s\n", v[n].path, strerror(errno));
                status = status ? status : 1;
            }
        }
        n++;
    }
    closedir(d);

    qsort(v, n, sizeof *v, by_name);
    print_list(v, n, true);
    free_entries(v, n);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    char **ops  = xmalloc((size_t)argc * sizeof *ops);
    int    nops = 0;
    bool   only_operands = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!only_operands && !strcmp(a, "--"))     { only_operands = true; continue; }
        if (!only_operands && !strcmp(a, "--help")) { usage(stdout); free(ops); return 0; }
        if (!only_operands && a[0] == '-' && a[1]) {
            for (const char *c = a + 1; *c; c++) {
                if (*c == 'a')      opt_all  = true;
                else if (*c == 'l') opt_long = true;
                else if (*c == '1') opt_one  = true;
                else {
                    fprintf(stderr, "ls: invalid option -- '%c'\n", *c);
                    usage(stderr);
                    free(ops);
                    return 2;
                }
            }
            continue;
        }
        ops[nops++] = argv[i];
    }
    const char *nc = getenv("NO_COLOR");
    color = isatty(STDOUT_FILENO) && !(nc && *nc);        /* escape codes only for a terminal */

    if (nops == 0) {
        list_dir(".");
        free(ops);
        return status;
    }

    /* operands: plain files first, then each directory, as GNU ls orders them */
    entry  *files = xmalloc((size_t)nops * sizeof *files);
    char  **dirs  = xmalloc((size_t)nops * sizeof *dirs);
    size_t  nf = 0, nd = 0;
    for (int i = 0; i < nops; i++) {
        struct stat st;
        int r = opt_long ? lstat(ops[i], &st) : stat(ops[i], &st);
        if (r != 0) {
            fprintf(stderr, "ls: cannot access '%s': %s\n", ops[i], strerror(errno));
            status = 2;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            dirs[nd++] = ops[i];
        } else {
            files[nf].name = xstrdup(ops[i]);
            files[nf].path = xstrdup(ops[i]);
            files[nf].st   = st;
            files[nf].ok   = true;
            nf++;
        }
    }

    qsort(files, nf, sizeof *files, by_name);
    qsort(dirs, nd, sizeof *dirs, by_string);
    bool printed = false;
    if (nf) {
        print_list(files, nf, false);
        printed = true;
    }
    for (size_t i = 0; i < nd; i++) {
        if (printed) putchar('\n');
        if (nops > 1) printf("%s:\n", dirs[i]);
        list_dir(dirs[i]);
        printed = true;
    }

    free_entries(files, nf);
    free(dirs);
    free(ops);
    return status;
}
