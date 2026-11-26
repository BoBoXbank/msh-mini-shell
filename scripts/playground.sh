#!/bin/sh
# ==========================================================================
#  playground.sh — the folder msh is demonstrated in
#
#  make demo creates it the first time; make playground rebuilds it.
#  It lives in the Linux home directory (not on /mnt/d), so file permissions
#  behave exactly as on a real Linux machine.
#
#  What each file is for:
#    main.c parser.c util.c   ls | grep .c | wc -l   → 3
#    notes.txt                cat < notes.txt, wc -l < notes.txt
#    data.txt                 cat data.txt | sort | uniq -c | sort -rn | head -3
#    readonly.txt             ls > readonly.txt      → EACCES, the shell survives
#    trap/ls                  the §3.6 warning: an empty entry in $PATH
# ==========================================================================
set -e

dir=${1:?usage: sh scripts/playground.sh DIR}
case "$dir" in
    *msh-playground*) ;;
    *) echo "playground.sh: refusing to rebuild '$dir' (the name must contain msh-playground)" >&2
       exit 1 ;;
esac

if [ -d "$dir" ]; then
    chmod -R u+w "$dir"
    rm -rf "$dir"
fi
umask 022
mkdir -p "$dir/lib" "$dir/trap"
cd "$dir"

cat > main.c <<'EOF'
#include <stdio.h>
#include "util.h"

/* TODO: read the configuration file */
int main(void)
{
    greet("msh");
    return 0;
}
EOF

cat > parser.c <<'EOF'
#include <string.h>
#include "util.h"

/* TODO: handle quotes */
int count_words(const char *line)
{
    int n = 0, in_word = 0;
    for (; *line; line++) {
        if (*line == ' ') in_word = 0;
        else if (!in_word) { in_word = 1; n++; }
    }
    return n;
}
EOF

cat > util.c <<'EOF'
#include <stdio.h>
#include "util.h"

void greet(const char *name)
{
    printf("hello from %s\n", name);
}
EOF

cat > notes.txt <<'EOF'
fork creates the child
exec replaces the program
wait collects the exit status
pipe joins two processes
dup2 rewires a descriptor
signals reach the foreground group
EOF

cat > data.txt <<'EOF'
fork
pipe
exec
fork
wait
pipe
fork
dup2
exec
fork
signal
pipe
EOF

cat > todo.md <<'EOF'
# todo
- [x] pipes of any length
- [x] background jobs and the SIGCHLD reaper
- [ ] fg and bg
- [ ] globbing and quotes
EOF

cat > lib/helpers.txt <<'EOF'
helper notes live here
EOF

printf 'this file is read-only: redirecting into it fails with EACCES\n' > readonly.txt
chmod 444 readonly.txt

cat > trap/ls <<'EOF'
#!/bin/sh
echo "!! You just ran $(pwd)/ls, a script sitting in this folder, not the real ls."
echo "!! It ran because \$PATH begins with an empty entry, and an empty entry means the"
echo "!! current directory. A PATH should never contain one (the security note, §3.6)."
EOF
chmod 755 trap/ls

echo "playground ready: $dir"
