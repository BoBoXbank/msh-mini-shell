/* ==========================================================================
 * msh.c — the whole shell as one translation unit
 *
 *     gcc -Wall -Wextra -o msh msh.c        the build command on slide §1.4
 *     ./msh
 *
 * The code lives in src/, one file per stage of the loop. This file only
 * pulls every one of them in, so that a single gcc command builds the
 * shell. `make` compiles the very same files separately, and also builds
 * our own ls and cat (tools/ → bin/).
 * ========================================================================== */
#include "src/msh.h"

#include "src/main.c"        /* §2.1  the loop, §5.4 start-up, §8.1 the prompt  */
#include "src/tokenizer.c"   /* §2.2  one string becomes a stream              */
#include "src/parser.c"      /* §2.3  the stream becomes a tree                */
#include "src/execute.c"     /* §3–§5 fork, exec, pipes, redirection, waitpid   */
#include "src/builtins.c"    /* §3.8  cd, pwd, export, exit                    */
#include "src/signals.c"     /* §5.2  SIGCHLD and the race inside the fix       */
#include "src/trace.c"       /*       the x-ray view (MSH_TRACE)                */
