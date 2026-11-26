/* ==========================================================================
 * cat.c — our own cat (§3.9): a copy loop and nothing else
 *
 * read() from the file, write() to descriptor 1, stop when read() returns 0
 * at end of file. With no argument (or "-") it copies descriptor 0, so
 * "cat < notes.txt" and "ls | cat" work through whatever msh wired to 0.
 *
 * It is a program, not a built-in: msh finds it through $PATH and runs it
 * in a forked child, exactly like /usr/bin/cat.
 *
 *   usage: cat [file ...]
 * ========================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 0 = copied, 1 = could not read this input, -1 = cannot write: stop */
static int copy(int fd, const char *name)
{
    char    buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
            return 1;
        }
        for (ssize_t off = 0; off < n;) {                /* write() may take less than asked */
            ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "cat: write error: %s\n", strerror(errno));
                return -1;
            }
            off += w;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return copy(STDIN_FILENO, "standard input") ? 1 : 0;

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc;
        if (!strcmp(argv[i], "-")) {
            rc = copy(STDIN_FILENO, "standard input");
        } else {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
            rc = copy(fd, argv[i]);
            close(fd);
        }
        if (rc < 0) return 1;
        if (rc) status = 1;
    }
    return status;
}
