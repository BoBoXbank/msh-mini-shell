/* ==========================================================================
 * tokenizer.c — §2.2 one string becomes a stream
 *
 * The raw line is a single char array. strtok_r() only splits on the
 * delimiters it is given, so metacharacters are found first and padded with
 * spaces; after that, splitting on whitespace yields the token stream.
 * ========================================================================== */
#include "msh.h"
#include <ctype.h>

static const char *DELIMS = " \t\r\n\v\f";

static bool at_word_start(const char *s, size_t i)
{
    if (i == 0) return true;
    char p = s[i - 1];
    return isspace((unsigned char)p) || p == '|' || p == '<' || p == '>' || p == '&' || p == ';';
}

/* |  <  >  >>  2>  &  ;  must split even without surrounding spaces,
   so  ls>out.txt  yields three tokens, not one. */
static char *pad_metachars(const char *s)
{
    size_t len = strlen(s);
    char  *out = malloc(len * 3 + 1);          /* worst case: every char becomes " c " */
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '2' && s[i + 1] == '>' && s[i + 2] != '>' && at_word_start(s, i)) {
            memcpy(out + j, " 2> ", 4); j += 4; i++;          /* stderr redirection */
        } else if (c == '>' && s[i + 1] == '>') {
            memcpy(out + j, " >> ", 4); j += 4; i++;          /* append redirection */
        } else if (c == '|' || c == '<' || c == '>' || c == '&' || c == ';') {
            out[j++] = ' '; out[j++] = c; out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

static tok_type classify(const char *w)
{
    if (!strcmp(w, "|"))  return T_PIPE;
    if (!strcmp(w, "<"))  return T_IN;
    if (!strcmp(w, ">"))  return T_OUT;
    if (!strcmp(w, ">>")) return T_APPEND;
    if (!strcmp(w, "2>")) return T_ERR;
    if (!strcmp(w, "&"))  return T_AMP;
    if (!strcmp(w, ";"))  return T_SEMI;
    return T_WORD;
}

static int push(token_list *t, token tok)
{
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 16;
        token *p = realloc(t->v, nc * sizeof *p);
        if (!p) return -1;
        t->v = p;
        t->cap = nc;
    }
    t->v[t->n++] = tok;
    return 0;
}

int tokenize(const char *line, token_list *out)
{
    out->v = NULL;
    out->n = out->cap = 0;

    char *buf = pad_metachars(line);
    if (!buf) return -1;

    char *save = NULL;                          /* strtok_r keeps its state here: re-entrant */
    for (char *w = strtok_r(buf, DELIMS, &save); w; w = strtok_r(NULL, DELIMS, &save)) {
        token tok = { classify(w), strdup(w) };  /* copied, not pointed at: buf dies below */
        if (!tok.text || push(out, tok) < 0) {
            free(tok.text);
            free(buf);
            tokens_free(out);
            return -1;
        }
    }
    free(buf);
    return 0;
}

void tokens_free(token_list *t)
{
    for (size_t i = 0; i < t->n; i++) free(t->v[i].text);
    free(t->v);
    t->v = NULL;
    t->n = t->cap = 0;
}

const char *tok_type_name(tok_type t)
{
    switch (t) {
    case T_WORD:   return "WORD";
    case T_PIPE:   return "PIPE";
    case T_IN:     return "REDIR_IN";
    case T_OUT:    return "REDIR_OUT";
    case T_APPEND: return "REDIR_APPEND";
    case T_ERR:    return "REDIR_ERR";
    case T_AMP:    return "AMP";
    case T_SEMI:   return "SEMI";
    }
    return "?";
}
