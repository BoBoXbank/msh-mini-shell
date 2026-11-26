#!/bin/sh
# ==========================================================================
#  demo.sh — start msh for the live demo
#
#      ./demo.sh            presenter mode: Enter types the next command from
#                           demo-steps.txt, Enter again runs it
#      ./demo.sh 6          presenter mode, starting at step 6
#      ./demo.sh --plain    plain msh, you type everything yourself
#      ./demo.sh -q         (either mode) without the banner
#
#  1. builds msh, bin/ls and bin/cat if they are missing or out of date
#  2. creates ~/msh-playground the first time (scripts/playground.sh)
#  3. starts msh inside it, with our own ls and cat first on $PATH
# ==========================================================================
here=$(cd "$(dirname "$0")" && pwd)
play=${PLAY:-$HOME/msh-playground}
plain=no
case "${1:-}" in
    --plain) plain=yes; shift ;;
    [0-9]|[0-9][0-9]) MSH_DEMO_FROM=$1; export MSH_DEMO_FROM; shift ;;
esac

# a build made by another Linux (say Kali, then Ubuntu, on the same folder) may
# not run here (GLIBC_x.y not found): if it cannot even print its help, drop it
for prog in "$here/msh" "$here/bin/ls"; do
    if [ -e "$prog" ] && ! "$prog" --help >/dev/null 2>&1; then
        rm -rf "$here/build" "$here/bin" "$here/msh"
        break
    fi
done

if [ -f "$here/Makefile" ] && command -v make >/dev/null 2>&1; then
    make -s -C "$here" >/dev/null || exit 1
else                                         # no make: the same build, one gcc per program
    command -v gcc >/dev/null 2>&1 || { echo "gcc is missing: sudo apt install -y build-essential"; exit 1; }
    cd "$here" || exit 1
    mkdir -p bin
    gcc -std=gnu11 -Wall -Wextra -O2 -g -o msh msh.c &&
    gcc -std=gnu11 -Wall -Wextra -O2 -g -o bin/ls tools/ls.c &&
    gcc -std=gnu11 -Wall -Wextra -O2 -g -o bin/cat tools/cat.c &&
    cp msh bin/msh || exit 1
fi
[ -d "$play" ] || sh "$here/scripts/playground.sh" "$play" || exit 1
cd "$play" || exit 1
PATH="$here/bin:$PATH"
export PATH

if [ "$plain" = no ] && [ -t 0 ] && [ -t 1 ] && command -v python3 >/dev/null 2>&1; then
    exec python3 "$here/scripts/present.py" "$here/msh" "$@"
fi
exec "$here/msh" "$@"                        # exec: nothing sits between your shell and msh
