/* ==========================================================================
 * builtins.c — §3.8 the commands that must run inside the shell itself
 *
 * The current working directory and the environment are per-process state,
 * and a child can never change its parent's state. So cd, exit and export
 * only work when they run in the shell process; pwd is here because it is
 * cheap and needs no program.
 * ========================================================================== */
#include "msh.h"
#include <ctype.h>

extern char **environ;

static int b_cd(char **argv)
{
    const char *dir = argv[1];
    char       *owned = NULL;
    if (dir && argv[2]) {
        fprintf(stderr, "msh: cd: too many arguments\n");
        return 1;
    }
    if (!dir || (dir[0] == '~' && (dir[1] == '\0' || dir[1] == '/'))) {
        const char *home = getenv("HOME");               /* §6.2 no argument: fall back to $HOME */
        if (!home || !*home) {
            fprintf(stderr, "msh: cd: HOME not set\n");
            return 1;
        }
        if (dir && dir[1]) {                             /* cd ~/dir */
            owned = malloc(strlen(home) + strlen(dir));
            if (!owned) {
                perror("msh: cd");
                return 1;
            }
            strcpy(owned, home);
            strcat(owned, dir + 1);
            dir = owned;
        } else {
            dir = home;
        }
    }
    int rc = 0;
    if (chdir(dir) != 0) {                               /* §6.2 bad path: report, keep going */
        int e = errno;
        if (sh.trace && !sh.in_child)
            trace_line("%schdir%s(\"%s\") = -1  %s", TP.amb, TP.rst, dir, errno_name(e));
        fprintf(stderr, "msh: cd: %s: %s\n", dir, strerror(e));
        rc = 1;
    } else {
        char *cwd = getcwd(NULL, 0);
        if (cwd) {
            setenv("PWD", cwd, 1);                       /* children see the right $PWD */
            free(cwd);
        }
        if (sh.trace && !sh.in_child)
            trace_line("%schdir%s(\"%s\") = 0   the shell's own directory changed", TP.amb, TP.rst, dir);
    }
    free(owned);
    return rc;
}

static int b_pwd(char **argv)
{
    (void)argv;
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        perror("msh: pwd");
        return 1;
    }
    puts(cwd);
    free(cwd);
    return 0;
}

static int b_exit(char **argv)
{
    int code = sh.last_status;
    if (argv[1] && argv[2]) {
        fprintf(stderr, "msh: exit: too many arguments\n");
        return 1;
    }
    if (argv[1]) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (!*argv[1] || *end) {
            fprintf(stderr, "msh: exit: %s: numeric argument required\n", argv[1]);
            code = 2;
        } else {
            code = (int)(v & 0xff);
        }
    }
    if (sh.interactive && !sh.in_child) printf("exit\n");
    sh.exit_requested = true;
    sh.exit_code = code;
    return code;
}

static bool valid_name(const char *s, size_t len)
{
    if (!len || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (size_t i = 1; i < len; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    return true;
}

/* §3.7 export writes into the shell's own environment, so every process
   forked afterwards inherits the new value through envp. */
static int b_export(char **argv)
{
    if (!argv[1]) {
        for (char **e = environ; *e; e++) printf("export %s\n", *e);
        return 0;
    }
    int rc = 0;
    for (int i = 1; argv[i]; i++) {
        const char *eq = strchr(argv[i], '=');
        size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!valid_name(argv[i], len)) {
            fprintf(stderr, "msh: export: '%s': not a valid identifier\n", argv[i]);
            rc = 1;
            continue;
        }
        if (!eq) continue;                               /* export NAME: already in the environment */
        char *name = strndup(argv[i], len);
        if (!name || setenv(name, eq + 1, 1) != 0) {
            perror("msh: export");
            rc = 1;
        } else if (sh.trace && !sh.in_child) {
            trace_line("%ssetenv%s(\"%s\", \"%s\") in the shell's own memory", TP.amb, TP.rst, name, eq + 1);
        }
        free(name);
    }
    return rc;
}

builtin_fn builtin_lookup(const char *name)
{
    static const struct { const char *name; builtin_fn fn; } table[] = {
        { "cd", b_cd }, { "exit", b_exit }, { "pwd", b_pwd }, { "export", b_export },
    };
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(name, table[i].name)) return table[i].fn;
    return NULL;
}
