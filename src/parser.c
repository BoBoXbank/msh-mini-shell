/* ==========================================================================
 * parser.c — §2.3 the token stream becomes a tree
 *
 *   pipeline := command ( '|' command )* [ '&' ]
 *   command  := ( WORD | redirection )+          -- at least one WORD
 *   redirection := ( '<' | '>' | '>>' | '2>' ) WORD
 *
 * A pipeline of n commands is n-1 nested PIPE nodes (right-nested).
 * Each redirection wraps the command it belongs to, so ownership is explicit:
 * in  ls -l | wc -l > out.txt  the > belongs to wc, not to the pipeline.
 * '&' is not a node: it is a flag on the root.
 * ========================================================================== */
#include "msh.h"

static node *new_node(node_type type)
{
    node *n = calloc(1, sizeof *n);
    if (n) n->type = type;
    return n;
}

static const char *near(const token_list *t, size_t i)
{
    return i < t->n ? t->v[i].text : "newline";
}

static bool is_redir(tok_type ty)
{
    return ty == T_IN || ty == T_OUT || ty == T_APPEND || ty == T_ERR;
}

static redir_kind kind_of(tok_type ty)
{
    switch (ty) {
    case T_IN:     return R_IN;
    case T_APPEND: return R_APPEND;
    case T_ERR:    return R_ERR;
    default:       return R_OUT;
    }
}

static node *parse_command(const token_list *t, size_t *i, char *err, size_t errlen)
{
    size_t start = *i, j = *i, words = 0;

    /* first pass: validate the shape and count the words */
    while (j < t->n && (t->v[j].type == T_WORD || is_redir(t->v[j].type))) {
        if (is_redir(t->v[j].type)) {
            if (j + 1 >= t->n || t->v[j + 1].type != T_WORD) {
                snprintf(err, errlen, "syntax error near unexpected token '%s'", near(t, j + 1));
                return NULL;
            }
            j += 2;
        } else {
            words++;
            j++;
        }
    }
    if (words == 0) {
        snprintf(err, errlen, "syntax error near unexpected token '%s'", near(t, j));
        return NULL;
    }

    /* second pass: build — the tree owns its own copy of every string */
    node *cmd = new_node(N_COMMAND);
    if (!cmd) goto oom;
    cmd->argv = calloc(words + 1, sizeof *cmd->argv);   /* NULL-terminated for execvp() */
    if (!cmd->argv) { free(cmd); goto oom; }

    node *top = cmd;
    for (size_t q = start; q < j;) {
        if (t->v[q].type == T_WORD) {
            cmd->argv[cmd->argc] = strdup(t->v[q].text);
            if (!cmd->argv[cmd->argc]) { ast_free(top); goto oom; }
            cmd->argc++;
            q++;
        } else {
            node *r = new_node(N_REDIR);
            if (!r) { ast_free(top); goto oom; }
            r->kind   = kind_of(t->v[q].type);
            r->target = strdup(t->v[q + 1].text);
            r->child  = top;                       /* each redirection wraps what came before */
            top = r;
            if (!r->target) { ast_free(top); goto oom; }
            q += 2;
        }
    }
    *i = j;
    return top;

oom:
    snprintf(err, errlen, "out of memory");
    return NULL;
}

static node *parse_pipeline(const token_list *t, size_t *i, char *err, size_t errlen)
{
    node *left = parse_command(t, i, err, errlen);
    if (!left) return NULL;

    if (*i < t->n && t->v[*i].type == T_PIPE) {
        (*i)++;
        node *right = parse_pipeline(t, i, err, errlen);   /* a | b | c  =  PIPE(a, PIPE(b, c)) */
        if (!right) { ast_free(left); return NULL; }
        node *p = new_node(N_PIPE);
        if (!p) {
            ast_free(left);
            ast_free(right);
            snprintf(err, errlen, "out of memory");
            return NULL;
        }
        p->left  = left;
        p->right = right;
        return p;
    }
    return left;
}

#define LISTS_MSG "command lists such as a ; b, a && b and a || b are not supported yet (§8.2)"

int parse(const token_list *t, ast *out, char *err, size_t errlen)
{
    out->root = NULL;
    out->background = false;

    for (size_t k = 0; k + 1 < t->n; k++) {              /* && and || arrive as two tokens */
        tok_type a = t->v[k].type, b = t->v[k + 1].type;
        if ((a == T_PIPE && b == T_PIPE) || (a == T_AMP && b == T_AMP)) {
            snprintf(err, errlen, LISTS_MSG);
            return -1;
        }
    }

    size_t i = 0;
    node *root = parse_pipeline(t, &i, err, errlen);
    if (!root) return -1;

    bool bg = false;
    if (i < t->n && t->v[i].type == T_AMP) {
        bg = true;
        i++;
    }
    if (i < t->n) {
        tok_type ty = t->v[i].type;
        if (ty == T_SEMI || ty == T_AMP || bg)
            snprintf(err, errlen, LISTS_MSG);
        else
            snprintf(err, errlen, "syntax error near unexpected token '%s'", t->v[i].text);
        ast_free(root);
        return -1;
    }
    out->root = root;
    out->background = bg;
    return 0;
}

/* §6.1 post-order walk: free the children before their parent */
void ast_free(node *n)
{
    if (!n) return;
    switch (n->type) {
    case N_PIPE:
        ast_free(n->left);
        ast_free(n->right);
        break;
    case N_REDIR:
        ast_free(n->child);
        free(n->target);
        break;
    case N_COMMAND:
        for (int k = 0; k < n->argc; k++) free(n->argv[k]);
        free(n->argv);
        break;
    }
    free(n);
}
