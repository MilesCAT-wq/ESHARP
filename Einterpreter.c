#define _POSIX_C_SOURCE 200809L

/*
 * E# (E-Sharp) Interpreter
 * ------------------------
 * A tree-walking interpreter for the E# language as described in the
 * E# Language Reference Manual (v1.2).
 *
 * Build:   gcc -std=c11 -O2 -o esharp esharp.c -lm
 * Run:     ./esharp path/to/script.esh
 *
 * Implementation notes / disambiguation rules
 * --------------------------------------------
 * E# uses square brackets `[...]` both as "containers" (function bodies,
 * array literals, module imports) AND as the syntax for function
 * execution / array indexing (`double[5]`, `left[i]`). It also uses `[`
 * to open the body of an `if`/`loop`/`Func` after a condition or
 * parameter list that itself may end in an indexing expression
 * (e.g. `if left[i] < right[j] [ ... ]`).
 *
 * Looking at every example in the manual, call/index brackets are always
 * written *tight* against the preceding token (`double[5]`,
 * `Studio.print["Hello"]`, `left[i]`) while block-opening brackets always
 * have a space before them (`Func main() [`, `if x > 1 [`,
 * `loop (energy > 0) [`, `] ELSE [`). This interpreter uses that
 * whitespace as the disambiguating signal: the lexer records whether a
 * `[` token was preceded by whitespace, and the parser only treats a
 * "tight" `[` as a postfix call/index operator. A "spaced" `[` is never
 * consumed by postfix parsing, so it is free to be treated as a block
 * opener (or an array literal, in primary position).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===================================================================== */
/* Utility                                                                */
/* ===================================================================== */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "esharp: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    memcpy(p, s, n);
    return p;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p) die("out of memory");
    return p;
}

/* ===================================================================== */
/* Lexer                                                                  */
/* ===================================================================== */

typedef enum {
    TK_EOF, TK_IDENT, TK_NUMBER, TK_STRING,
    TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_DOT,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_AND, TK_OR, TK_NOT, TK_ARROW,
    /* keywords */
    TK_IMPORT, TK_FUNC, TK_ASYNC, TK_VAR, TK_IF, TK_ELSE, TK_LOOP,
    TK_RETURN, TK_LAMBDA, TK_FN, TK_CALL, TK_PRESS, TK_TRUE, TK_FALSE
} TokType;

typedef struct {
    TokType type;
    char *text;      /* for IDENT/STRING (owned) */
    double number;   /* for NUMBER */
    int line;
    bool space_before; /* only meaningful for TK_LBRACKET / TK_LPAREN */
} Token;

typedef struct {
    const char *src;
    size_t pos, len;
    int line;
} Lexer;

static void lex_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->len = strlen(src);
    lx->line = 1;
}

static int lx_peek(Lexer *lx) { return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1; }
static int lx_peek2(Lexer *lx) { return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : -1; }
static int lx_advance(Lexer *lx) {
    int c = lx_peek(lx);
    if (c == -1) return -1;
    lx->pos++;
    if (c == '\n') lx->line++;
    return c;
}

/* Skip whitespace and comments. Returns true if anything was skipped. */
static bool lx_skip_trivia(Lexer *lx) {
    bool skipped = false;
    for (;;) {
        int c = lx_peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx_advance(lx);
            skipped = true;
            continue;
        }
        if (c == '/' && lx_peek2(lx) == '<') {
            /* single-line annotation /< ... >/ */
            lx_advance(lx); lx_advance(lx);
            while (lx_peek(lx) != -1 && !(lx_peek(lx) == '>' && lx_peek2(lx) == '/')) lx_advance(lx);
            if (lx_peek(lx) != -1) { lx_advance(lx); lx_advance(lx); }
            skipped = true;
            continue;
        }
        if (c == '/' && lx_peek2(lx) == '[') {
            /* multi-line block documentation /[ ... ]/ */
            lx_advance(lx); lx_advance(lx);
            while (lx_peek(lx) != -1 && !(lx_peek(lx) == ']' && lx_peek2(lx) == '/')) lx_advance(lx);
            if (lx_peek(lx) != -1) { lx_advance(lx); lx_advance(lx); }
            skipped = true;
            continue;
        }
        break;
    }
    return skipped;
}

static Token lx_make(Lexer *lx, TokType t, int line) {
    Token tok = {0};
    tok.type = t;
    tok.line = line;
    return tok;
}

static Token lex_next(Lexer *lx) {
    bool had_space = lx_skip_trivia(lx);
    int line = lx->line;
    int c = lx_peek(lx);
    if (c == -1) return lx_make(lx, TK_EOF, line);

    /* identifiers / keywords */
    if (isalpha(c) || c == '_') {
        size_t start = lx->pos;
        while (isalnum(lx_peek(lx)) || lx_peek(lx) == '_') lx_advance(lx);
        size_t n = lx->pos - start;
        char *word = xmalloc(n + 1);
        memcpy(word, lx->src + start, n);
        word[n] = 0;

        struct { const char *kw; TokType t; } kws[] = {
            {"import", TK_IMPORT}, {"Func", TK_FUNC}, {"Async", TK_ASYNC},
            {"Var", TK_VAR}, {"if", TK_IF}, {"ELSE", TK_ELSE}, {"loop", TK_LOOP},
            {"return", TK_RETURN}, {"LAMBDA", TK_LAMBDA}, {"fn", TK_FN},
            {"call", TK_CALL}, {"press", TK_PRESS}, {"true", TK_TRUE}, {"false", TK_FALSE},
        };
        for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
            if (strcmp(word, kws[i].kw) == 0) {
                Token tok = lx_make(lx, kws[i].t, line);
                tok.text = word;
                return tok;
            }
        }
        Token tok = lx_make(lx, TK_IDENT, line);
        tok.text = word;
        return tok;
    }

    /* numbers */
    if (isdigit(c)) {
        size_t start = lx->pos;
        while (isdigit(lx_peek(lx))) lx_advance(lx);
        if (lx_peek(lx) == '.' && isdigit(lx_peek2(lx))) {
            lx_advance(lx);
            while (isdigit(lx_peek(lx))) lx_advance(lx);
        }
        size_t n = lx->pos - start;
        char buf[128];
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, lx->src + start, n);
        buf[n] = 0;
        Token tok = lx_make(lx, TK_NUMBER, line);
        tok.number = strtod(buf, NULL);
        return tok;
    }

    /* strings */
    if (c == '"') {
        lx_advance(lx);
        size_t cap = 32, n = 0;
        char *buf = xmalloc(cap);
        while (lx_peek(lx) != -1 && lx_peek(lx) != '"') {
            int ch = lx_advance(lx);
            if (ch == '\\') {
                int esc = lx_advance(lx);
                switch (esc) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case '"': ch = '"'; break;
                    case '\\': ch = '\\'; break;
                    default: ch = esc; break;
                }
            }
            if (n + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); }
            buf[n++] = (char)ch;
        }
        if (lx_peek(lx) == '"') lx_advance(lx);
        buf[n] = 0;
        Token tok = lx_make(lx, TK_STRING, line);
        tok.text = buf;
        return tok;
    }

    /* punctuation / operators */
    switch (c) {
        case '(': lx_advance(lx); { Token t = lx_make(lx, TK_LPAREN, line); t.space_before = had_space; return t; }
        case ')': lx_advance(lx); return lx_make(lx, TK_RPAREN, line);
        case '[': lx_advance(lx); { Token t = lx_make(lx, TK_LBRACKET, line); t.space_before = had_space; return t; }
        case ']': lx_advance(lx); return lx_make(lx, TK_RBRACKET, line);
        case ',': lx_advance(lx); return lx_make(lx, TK_COMMA, line);
        case '.': lx_advance(lx); return lx_make(lx, TK_DOT, line);
        case '+': lx_advance(lx); return lx_make(lx, TK_PLUS, line);
        case '-': lx_advance(lx); return lx_make(lx, TK_MINUS, line);
        case '*': lx_advance(lx); return lx_make(lx, TK_STAR, line);
        case '/': lx_advance(lx); return lx_make(lx, TK_SLASH, line);
        case '%': lx_advance(lx); return lx_make(lx, TK_PERCENT, line);
        case '=':
            lx_advance(lx);
            if (lx_peek(lx) == '=') { lx_advance(lx); return lx_make(lx, TK_EQ, line); }
            if (lx_peek(lx) == '>') { lx_advance(lx); return lx_make(lx, TK_ARROW, line); }
            return lx_make(lx, TK_ASSIGN, line);
        case '!':
            lx_advance(lx);
            if (lx_peek(lx) == '=') { lx_advance(lx); return lx_make(lx, TK_NEQ, line); }
            return lx_make(lx, TK_NOT, line);
        case '<':
            lx_advance(lx);
            if (lx_peek(lx) == '=') { lx_advance(lx); return lx_make(lx, TK_LE, line); }
            return lx_make(lx, TK_LT, line);
        case '>':
            lx_advance(lx);
            if (lx_peek(lx) == '=') { lx_advance(lx); return lx_make(lx, TK_GE, line); }
            return lx_make(lx, TK_GT, line);
        case '&':
            lx_advance(lx);
            if (lx_peek(lx) == '&') { lx_advance(lx); return lx_make(lx, TK_AND, line); }
            die("line %d: unexpected character '&'", line);
        case '|':
            lx_advance(lx);
            if (lx_peek(lx) == '|') { lx_advance(lx); return lx_make(lx, TK_OR, line); }
            die("line %d: unexpected character '|'", line);
        default:
            die("line %d: unexpected character '%c'", line, c);
    }
    return lx_make(lx, TK_EOF, line); /* unreachable */
}

/* ===================================================================== */
/* AST                                                                    */
/* ===================================================================== */

typedef enum {
    ND_PROGRAM, ND_IMPORT, ND_FUNC_DECL, ND_VAR_DECL, ND_RETURN,
    ND_IF, ND_LOOP, ND_BLOCK, ND_EXPR_STMT, ND_ASSIGN,
    ND_NUMBER, ND_STRING, ND_BOOL, ND_ARRAY_LIT, ND_IDENT,
    ND_BINARY, ND_UNARY, ND_MEMBER, ND_SUBSCRIPT, ND_LAMBDA, ND_ASYNC_CALL
} NodeType;

typedef struct Node Node;

struct Node {
    NodeType type;
    int line;

    /* generic children */
    Node *a, *b;

    /* generic list (statements in a block/program, elements of an array
     * literal, arguments of a subscript/call) */
    Node **list;
    int list_count;

    /* literal payloads */
    double number;
    int boolean;
    char *str; /* IDENT name / STRING value / property name / module name / op symbol */

    /* function-like payloads (FUNC_DECL, LAMBDA) */
    char **params;
    int param_count;
    bool is_async;

    /* if-chain payloads: parallel arrays of conditions/blocks, plus a
     * trailing else block (nullable) */
    Node **conds;
    Node **blocks;
    int branch_count;
    Node *else_block;
};

static Node *node_new(NodeType t) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->type = t;
    return n;
}

static void list_push(Node ***list, int *count, Node *item) {
    *list = xrealloc(*list, sizeof(Node *) * (*count + 1));
    (*list)[*count] = item;
    (*count)++;
}

/* ===================================================================== */
/* Parser                                                                 */
/* ===================================================================== */

typedef struct {
    Lexer lx;
    Token cur;
    Token ahead;
    bool has_ahead;
} Parser;

static void p_advance(Parser *p) {
    if (p->has_ahead) {
        p->cur = p->ahead;
        p->has_ahead = false;
    } else {
        p->cur = lex_next(&p->lx);
    }
}

static Token p_peek_ahead(Parser *p) {
    if (!p->has_ahead) {
        p->ahead = lex_next(&p->lx);
        p->has_ahead = true;
    }
    return p->ahead;
}

static void p_init(Parser *p, const char *src) {
    lex_init(&p->lx, src);
    p->has_ahead = false;
    p_advance(p);
}

static const char *tok_name(TokType t) {
    switch (t) {
        case TK_EOF: return "end of file";
        case TK_IDENT: return "identifier";
        case TK_NUMBER: return "number";
        case TK_STRING: return "string";
        case TK_LPAREN: return "'('";
        case TK_RPAREN: return "')'";
        case TK_LBRACKET: return "'['";
        case TK_RBRACKET: return "']'";
        case TK_COMMA: return "','";
        case TK_DOT: return "'.'";
        case TK_ASSIGN: return "'='";
        case TK_ARROW: return "'=>'";
        case TK_FUNC: return "'Func'";
        case TK_VAR: return "'Var'";
        case TK_IF: return "'if'";
        case TK_ELSE: return "'ELSE'";
        case TK_LOOP: return "'loop'";
        case TK_RETURN: return "'return'";
        default: return "token";
    }
}

static void p_error(Parser *p, const char *msg) {
    die("line %d: %s", p->cur.line, msg);
}

static Token p_expect(Parser *p, TokType t) {
    if (p->cur.type != t) {
        if (t == TK_LBRACKET && p->cur.type == TK_LPAREN) {
            p_error(p, "syntax error: parentheses cannot be used for function "
                       "execution or block bodies. Use square brackets instead, "
                       "e.g. 'name[args]' not 'name(args)'.");
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "expected %s but got %s", tok_name(t), tok_name(p->cur.type));
        p_error(p, buf);
    }
    Token tok = p->cur;
    p_advance(p);
    return tok;
}

static bool p_check(Parser *p, TokType t) { return p->cur.type == t; }

static bool p_match(Parser *p, TokType t) {
    if (p_check(p, t)) { p_advance(p); return true; }
    return false;
}

/* forward decls */
static Node *parse_expr(Parser *p);
static Node *parse_block(Parser *p); /* consumes '[' ... ']' and returns ND_BLOCK */
static Node *parse_statement(Parser *p);

/* ---- expressions ---- */

static Node *parse_primary(Parser *p) {
    int line = p->cur.line;

    if (p_check(p, TK_NUMBER)) {
        Node *n = node_new(ND_NUMBER); n->line = line;
        n->number = p->cur.number;
        p_advance(p);
        return n;
    }
    if (p_check(p, TK_STRING)) {
        Node *n = node_new(ND_STRING); n->line = line;
        n->str = p->cur.text;
        p_advance(p);
        return n;
    }
    if (p_check(p, TK_TRUE) || p_check(p, TK_FALSE)) {
        Node *n = node_new(ND_BOOL); n->line = line;
        n->boolean = p_check(p, TK_TRUE) ? 1 : 0;
        p_advance(p);
        return n;
    }
    if (p_check(p, TK_IDENT)) {
        Node *n = node_new(ND_IDENT); n->line = line;
        n->str = p->cur.text;
        p_advance(p);
        return n;
    }
    if (p_check(p, TK_LBRACKET)) {
        /* array literal: [ expr, expr, ... ] */
        p_advance(p);
        Node *n = node_new(ND_ARRAY_LIT); n->line = line;
        if (!p_check(p, TK_RBRACKET)) {
            list_push(&n->list, &n->list_count, parse_expr(p));
            while (p_match(p, TK_COMMA)) {
                list_push(&n->list, &n->list_count, parse_expr(p));
            }
        }
        p_expect(p, TK_RBRACKET);
        return n;
    }
    if (p_check(p, TK_LAMBDA)) {
        /* LAMBDA(name) [ fn a, b => [ expr ] ] */
        p_advance(p);
        p_expect(p, TK_LPAREN);
        char *label = NULL;
        if (p_check(p, TK_IDENT)) { label = p->cur.text; p_advance(p); }
        p_expect(p, TK_RPAREN);
        p_expect(p, TK_LBRACKET);
        p_expect(p, TK_FN);
        Node *n = node_new(ND_LAMBDA); n->line = line;
        n->str = label;
        n->params = xmalloc(sizeof(char *) * 4);
        int cap = 4;
        if (p_check(p, TK_IDENT)) {
            n->params[n->param_count++] = p->cur.text;
            p_advance(p);
            while (p_match(p, TK_COMMA)) {
                if (n->param_count >= cap) { cap *= 2; n->params = xrealloc(n->params, sizeof(char *) * cap); }
                n->params[n->param_count++] = p_expect(p, TK_IDENT).text;
            }
        }
        p_expect(p, TK_ARROW);
        p_expect(p, TK_LBRACKET);
        n->a = parse_expr(p);
        p_expect(p, TK_RBRACKET);
        p_expect(p, TK_RBRACKET);
        return n;
    }
    if (p_check(p, TK_ASYNC)) {
        p_advance(p);
        p_expect(p, TK_CALL);
        Node *n = node_new(ND_ASYNC_CALL); n->line = line;
        n->a = parse_expr(p);
        return n;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "unexpected %s in expression", tok_name(p->cur.type));
    p_error(p, buf);
    return NULL;
}

/* postfix: '.' member access (always) and *tight* '[' subscript/call
 * (only when not preceded by whitespace -- see file header comment). */
static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    for (;;) {
        if (p_check(p, TK_DOT)) {
            p_advance(p);
            Node *m = node_new(ND_MEMBER); m->line = p->cur.line;
            m->a = n;
            m->str = p_expect(p, TK_IDENT).text;
            n = m;
            continue;
        }
        if (p_check(p, TK_LBRACKET) && !p->cur.space_before) {
            p_advance(p);
            Node *s = node_new(ND_SUBSCRIPT); s->line = p->cur.line;
            s->a = n;
            if (!p_check(p, TK_RBRACKET)) {
                list_push(&s->list, &s->list_count, parse_expr(p));
                while (p_match(p, TK_COMMA)) {
                    list_push(&s->list, &s->list_count, parse_expr(p));
                }
            }
            p_expect(p, TK_RBRACKET);
            n = s;
            continue;
        }
        /* A tight '(' right after an expression means someone tried to
         * "call" it C#-style, which E# explicitly forbids. */
        if (p_check(p, TK_LPAREN) && !p->cur.space_before) {
            p_error(p, "syntax error: parentheses are banned for function "
                       "execution. Use square brackets, e.g. 'name[args]'.");
        }
        break;
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    if (p_check(p, TK_MINUS) || p_check(p, TK_NOT)) {
        char *op = (p_check(p, TK_MINUS)) ? "-" : "!";
        int line = p->cur.line;
        p_advance(p);
        Node *n = node_new(ND_UNARY); n->line = line;
        n->str = xstrdup(op);
        n->a = parse_unary(p);
        return n;
    }
    return parse_postfix(p);
}

/* precedence climbing implemented directly, level by level */
static Node *parse_mul(Parser *p) {
    Node *n = parse_unary(p);
    for (;;) {
        const char *op = NULL;
        if (p_check(p, TK_STAR)) op = "*";
        else if (p_check(p, TK_SLASH)) op = "/";
        else if (p_check(p, TK_PERCENT)) op = "%";
        else break;
        int line = p->cur.line;
        p_advance(p);
        Node *b = node_new(ND_BINARY); b->line = line;
        b->str = xstrdup(op); b->a = n; b->b = parse_unary(p);
        n = b;
    }
    return n;
}

static Node *parse_add(Parser *p) {
    Node *n = parse_mul(p);
    for (;;) {
        const char *op = NULL;
        if (p_check(p, TK_PLUS)) op = "+";
        else if (p_check(p, TK_MINUS)) op = "-";
        else break;
        int line = p->cur.line;
        p_advance(p);
        Node *b = node_new(ND_BINARY); b->line = line;
        b->str = xstrdup(op); b->a = n; b->b = parse_mul(p);
        n = b;
    }
    return n;
}

static Node *parse_rel(Parser *p) {
    Node *n = parse_add(p);
    for (;;) {
        const char *op = NULL;
        if (p_check(p, TK_LT)) op = "<";
        else if (p_check(p, TK_LE)) op = "<=";
        else if (p_check(p, TK_GT)) op = ">";
        else if (p_check(p, TK_GE)) op = ">=";
        else break;
        int line = p->cur.line;
        p_advance(p);
        Node *b = node_new(ND_BINARY); b->line = line;
        b->str = xstrdup(op); b->a = n; b->b = parse_add(p);
        n = b;
    }
    return n;
}

static Node *parse_eq(Parser *p) {
    Node *n = parse_rel(p);
    for (;;) {
        const char *op = NULL;
        if (p_check(p, TK_EQ)) op = "==";
        else if (p_check(p, TK_NEQ)) op = "!=";
        else break;
        int line = p->cur.line;
        p_advance(p);
        Node *b = node_new(ND_BINARY); b->line = line;
        b->str = xstrdup(op); b->a = n; b->b = parse_rel(p);
        n = b;
    }
    return n;
}

static Node *parse_and(Parser *p) {
    Node *n = parse_eq(p);
    while (p_check(p, TK_AND)) {
        int line = p->cur.line;
        p_advance(p);
        Node *b = node_new(ND_BINARY); b->line = line;
        b->str = xstrdup("&&"); b->a = n; b->b = parse_eq(p);
        n = b;
    }
    return n;
}

static Node *parse_or(Parser *p) {
    Node *n = parse_and(p);
    while (p_check(p, TK_OR)) {
        int line = p->cur.line;
        p_advance(p);
        Node *b = node_new(ND_BINARY); b->line = line;
        b->str = xstrdup("||"); b->a = n; b->b = parse_and(p);
        n = b;
    }
    return n;
}

static Node *parse_expr(Parser *p) {
    return parse_or(p);
}

/* ---- statements ---- */

static char **parse_param_list(Parser *p, int *out_count) {
    char **params = xmalloc(sizeof(char *) * 8);
    int cap = 8, count = 0;
    p_expect(p, TK_LPAREN);
    if (!p_check(p, TK_RPAREN)) {
        params[count++] = p_expect(p, TK_IDENT).text;
        while (p_match(p, TK_COMMA)) {
            if (count >= cap) { cap *= 2; params = xrealloc(params, sizeof(char *) * cap); }
            params[count++] = p_expect(p, TK_IDENT).text;
        }
    }
    p_expect(p, TK_RPAREN);
    *out_count = count;
    return params;
}

static Node *parse_func_decl(Parser *p, bool is_async) {
    int line = p->cur.line;
    p_expect(p, TK_FUNC);
    Node *n = node_new(ND_FUNC_DECL); n->line = line;
    n->is_async = is_async;
    n->str = p_expect(p, TK_IDENT).text;
    n->params = parse_param_list(p, &n->param_count);
    n->a = parse_block(p);
    return n;
}

static Node *parse_if(Parser *p) {
    int line = p->cur.line;
    p_expect(p, TK_IF);
    Node *n = node_new(ND_IF); n->line = line;

    int cond_count = 0, block_count = 0;

    Node *cond = parse_expr(p);
    if (p_check(p, TK_PRESS)) p_advance(p);
    Node *block = parse_block(p);
    list_push(&n->conds, &cond_count, cond);
    list_push(&n->blocks, &block_count, block);

    while (p_check(p, TK_ELSE)) {
        p_advance(p);
        if (p_check(p, TK_LBRACKET)) {
            /* terminal ELSE [ ... ] */
            n->else_block = parse_block(p);
            break;
        }
        Node *c = parse_expr(p);
        if (p_check(p, TK_PRESS)) p_advance(p);
        Node *b = parse_block(p);
        list_push(&n->conds, &cond_count, c);
        list_push(&n->blocks, &block_count, b);
    }
    n->branch_count = cond_count; /* == block_count */
    return n;
}

static Node *parse_loop(Parser *p) {
    int line = p->cur.line;
    p_expect(p, TK_LOOP);
    p_expect(p, TK_LPAREN);
    Node *n = node_new(ND_LOOP); n->line = line;
    n->a = parse_expr(p);
    p_expect(p, TK_RPAREN);
    n->b = parse_block(p);
    return n;
}

static Node *parse_statement(Parser *p) {
    int line = p->cur.line;

    if (p_check(p, TK_IMPORT)) {
        p_advance(p);
        p_expect(p, TK_LBRACKET);
        Node *n = node_new(ND_IMPORT); n->line = line;
        n->str = p_expect(p, TK_IDENT).text;
        p_expect(p, TK_RBRACKET);
        return n;
    }
    if (p_check(p, TK_ASYNC) && p_peek_ahead(p).type == TK_FUNC) {
        p_advance(p); /* Async */
        return parse_func_decl(p, true);
    }
    if (p_check(p, TK_FUNC)) {
        return parse_func_decl(p, false);
    }
    if (p_check(p, TK_VAR)) {
        p_advance(p);
        Node *n = node_new(ND_VAR_DECL); n->line = line;
        n->str = p_expect(p, TK_IDENT).text;
        p_expect(p, TK_ASSIGN);
        n->a = parse_expr(p);
        return n;
    }
    if (p_check(p, TK_RETURN)) {
        p_advance(p);
        Node *n = node_new(ND_RETURN); n->line = line;
        /* return with no value: next token closes the block */
        if (!p_check(p, TK_RBRACKET)) n->a = parse_expr(p);
        return n;
    }
    if (p_check(p, TK_IF)) {
        return parse_if(p);
    }
    if (p_check(p, TK_LOOP)) {
        return parse_loop(p);
    }

    /* expression statement or assignment */
    Node *expr = parse_expr(p);
    if (p_check(p, TK_ASSIGN)) {
        p_advance(p);
        Node *n = node_new(ND_ASSIGN); n->line = line;
        n->a = expr;
        n->b = parse_expr(p);
        return n;
    }
    Node *n = node_new(ND_EXPR_STMT); n->line = line;
    n->a = expr;
    return n;
}

/* consumes '[' statement* ']' */
static Node *parse_block(Parser *p) {
    p_expect(p, TK_LBRACKET);
    Node *n = node_new(ND_BLOCK); n->line = p->cur.line;
    while (!p_check(p, TK_RBRACKET) && !p_check(p, TK_EOF)) {
        list_push(&n->list, &n->list_count, parse_statement(p));
    }
    p_expect(p, TK_RBRACKET);
    return n;
}

static Node *parse_program(Parser *p) {
    Node *n = node_new(ND_PROGRAM);
    while (!p_check(p, TK_EOF)) {
        list_push(&n->list, &n->list_count, parse_statement(p));
    }
    return n;
}

/* ===================================================================== */
/* Values                                                                 */
/* ===================================================================== */

typedef struct Value Value;
typedef struct Env Env;

typedef enum {
    VAL_NULL, VAL_NUMBER, VAL_BOOL, VAL_STRING, VAL_ARRAY,
    VAL_FUNCTION, VAL_NATIVE, VAL_MODULE
} ValueType;

typedef struct EArray {
    Value *items;
    int count, capacity;
} EArray;

typedef struct EFunction {
    char *name;
    char **params;
    int param_count;
    Node *body;      /* ND_BLOCK for normal functions */
    Node *lambda_expr; /* for lambdas: single expression body (body==NULL) */
    Env *closure;
    bool is_async;
} EFunction;

typedef Value (*NativeFn)(Value *args, int argc);
typedef struct ENative ENative;

struct Value {
    ValueType type;
    union {
        double number;
        int boolean;
        char *string;
        EArray *array;
        EFunction *function;
        ENative *native;
        char *module_name;
    } as;
};

struct ENative {
    NativeFn fn;
    const char *name;
    char *external_command;
    bool has_receiver;
    Value receiver; /* used for bound array methods */
};

static Value V_NULL(void) { Value v; v.type = VAL_NULL; return v; }
static Value V_NUM(double d) { Value v; v.type = VAL_NUMBER; v.as.number = d; return v; }
static Value V_BOOL(int b) { Value v; v.type = VAL_BOOL; v.as.boolean = b; return v; }
static Value V_STR(const char *s) { Value v; v.type = VAL_STRING; v.as.string = xstrdup(s); return v; }
static Value V_STR_OWN(char *s) { Value v; v.type = VAL_STRING; v.as.string = s; return v; }
static Value V_MODULE(const char *name) { Value v; v.type = VAL_MODULE; v.as.module_name = (char *)name; return v; }

static Value V_ARRAY_NEW(void) {
    Value v; v.type = VAL_ARRAY;
    EArray *a = xmalloc(sizeof(EArray));
    a->items = NULL; a->count = 0; a->capacity = 0;
    v.as.array = a;
    return v;
}

static void array_push(EArray *a, Value item) {
    if (a->count >= a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 8;
        a->items = xrealloc(a->items, sizeof(Value) * a->capacity);
    }
    a->items[a->count++] = item;
}

static Value V_NATIVE(NativeFn fn, const char *name) {
    Value v; v.type = VAL_NATIVE;
    ENative *nat = xmalloc(sizeof(ENative));
    nat->fn = fn; nat->name = name; nat->external_command = NULL;
    nat->has_receiver = false; nat->receiver = V_NULL();
    v.as.native = nat;
    return v;
}

static Value V_NATIVE_BOUND(NativeFn fn, const char *name, Value receiver) {
    Value v; v.type = VAL_NATIVE;
    ENative *nat = xmalloc(sizeof(ENative));
    nat->fn = fn; nat->name = name; nat->external_command = NULL;
    nat->has_receiver = true; nat->receiver = receiver;
    v.as.native = nat;
    return v;
}

static Value V_EXTERNAL(const char *command, const char *name) {
    Value v = V_NATIVE(NULL, name);
    v.as.native->name = xstrdup(name);
    v.as.native->external_command = xstrdup(command);
    return v;
}

static bool is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL: return v.as.boolean != 0;
        case VAL_NUMBER: return v.as.number != 0;
        case VAL_NULL: return false;
        case VAL_STRING: return v.as.string[0] != 0;
        default: return true;
    }
}

static char *value_to_string(Value v) {
    char buf[512];
    switch (v.type) {
        case VAL_NULL: return xstrdup("null");
        case VAL_BOOL: return xstrdup(v.as.boolean ? "true" : "false");
        case VAL_NUMBER: {
            double d = v.as.number;
            if (d == (long long)d) snprintf(buf, sizeof(buf), "%lld", (long long)d);
            else snprintf(buf, sizeof(buf), "%g", d);
            return xstrdup(buf);
        }
        case VAL_STRING: return xstrdup(v.as.string);
        case VAL_ARRAY: {
            size_t cap = 64, len = 0;
            char *out = xmalloc(cap);
            out[0] = '['; out[1] = 0; len = 1;
            for (int i = 0; i < v.as.array->count; i++) {
                char *elem = value_to_string(v.as.array->items[i]);
                size_t elen = strlen(elem);
                size_t need = len + elen + 4;
                if (need > cap) { cap = need * 2; out = xrealloc(out, cap); }
                if (i > 0) { strcpy(out + len, ", "); len += 2; }
                strcpy(out + len, elem);
                len += elen;
                free(elem);
            }
            if (len + 2 > cap) { cap = len + 2; out = xrealloc(out, cap); }
            out[len++] = ']'; out[len] = 0;
            return out;
        }
        case VAL_FUNCTION: {
            snprintf(buf, sizeof(buf), "<function %s>", v.as.function->name ? v.as.function->name : "anonymous");
            return xstrdup(buf);
        }
        case VAL_NATIVE: {
            snprintf(buf, sizeof(buf), "<native %s>", v.as.native->name);
            return xstrdup(buf);
        }
        case VAL_MODULE: {
            snprintf(buf, sizeof(buf), "<module %s>", v.as.module_name);
            return xstrdup(buf);
        }
    }
    return xstrdup("");
}

static const char *type_name(ValueType t) {
    switch (t) {
        case VAL_NULL: return "null";
        case VAL_NUMBER: return "number";
        case VAL_BOOL: return "bool";
        case VAL_STRING: return "string";
        case VAL_ARRAY: return "array";
        case VAL_FUNCTION: return "function";
        case VAL_NATIVE: return "function";
        case VAL_MODULE: return "module";
    }
    return "?";
}

/* ===================================================================== */
/* Environment                                                            */
/* ===================================================================== */

struct Env {
    char **names;
    Value *values;
    int count, capacity;
    Env *parent;
};

static Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(Env));
    e->names = NULL; e->values = NULL; e->count = 0; e->capacity = 0;
    e->parent = parent;
    return e;
}

static void env_define(Env *e, const char *name, Value v) {
    for (int i = 0; i < e->count; i++) {
        if (strcmp(e->names[i], name) == 0) { e->values[i] = v; return; }
    }
    if (e->count >= e->capacity) {
        e->capacity = e->capacity ? e->capacity * 2 : 8;
        e->names = xrealloc(e->names, sizeof(char *) * e->capacity);
        e->values = xrealloc(e->values, sizeof(Value) * e->capacity);
    }
    e->names[e->count] = xstrdup(name);
    e->values[e->count] = v;
    e->count++;
}

static bool env_set_existing(Env *e, const char *name, Value v) {
    for (Env *cur = e; cur; cur = cur->parent) {
        for (int i = 0; i < cur->count; i++) {
            if (strcmp(cur->names[i], name) == 0) { cur->values[i] = v; return true; }
        }
    }
    return false;
}

static bool env_get(Env *e, const char *name, Value *out) {
    for (Env *cur = e; cur; cur = cur->parent) {
        for (int i = 0; i < cur->count; i++) {
            if (strcmp(cur->names[i], name) == 0) { *out = cur->values[i]; return true; }
        }
    }
    return false;
}

/* ===================================================================== */
/* Import / namespace tracking                                           */
/* ===================================================================== */

static char *g_imports[64];
static int g_import_count = 0;

typedef struct {
    char *name;
    Env *env;
    char **exports;
    int export_count;
} UserModule;

static UserModule g_modules[64];
static int g_module_count = 0;

static UserModule *find_user_module(const char *name) {
    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].name, name) == 0) return &g_modules[i];
    }
    return NULL;
}

static bool module_exports(UserModule *module, const char *name) {
    for (int i = 0; i < module->export_count; i++) {
        if (strcmp(module->exports[i], name) == 0) return true;
    }
    return false;
}

static bool is_imported(const char *name) {
    for (int i = 0; i < g_import_count; i++) {
        if (strcmp(g_imports[i], name) == 0) return true;
    }
    return false;
}

static void mark_imported(const char *name) {
    if (is_imported(name)) return;
    g_imports[g_import_count++] = xstrdup(name);
}

static bool is_known_module(const char *name) {
    return strcmp(name, "Studio") == 0 || strcmp(name, "math") == 0 ||
           strcmp(name, "lambda") == 0 || find_user_module(name) != NULL;
}

/* ===================================================================== */
/* Native functions                                                      */
/* ===================================================================== */

static void require_argc(const char *fn, int argc, int expected) {
    if (argc != expected) die("runtime error: %s expects %d argument(s), got %d", fn, expected, argc);
}

static double require_num(const char *fn, Value v, int idx) {
    if (v.type != VAL_NUMBER) die("runtime error: %s argument %d must be a number, got %s", fn, idx, type_name(v.type));
    return v.as.number;
}

static Value native_studio_print(Value *args, int argc) {
    for (int i = 0; i < argc; i++) {
        char *s = value_to_string(args[i]);
        if (i > 0) fputc(' ', stdout);
        fputs(s, stdout);
        free(s);
    }
    fputc('\n', stdout);
    return V_NULL();
}

static Value native_input(Value *args, int argc) {
    char buffer[4096];
    (void)args;
    require_argc("INPUT", argc, 0);
    if (!fgets(buffer, sizeof(buffer), stdin)) return V_STR("");
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return V_STR(buffer);
}

static Value native_key(Value *args, int argc) {
    int pressed;
    const char *expected;
    (void)args;
    require_argc("KEY", argc, 1);
    if (args[0].type != VAL_STRING)
        die("runtime error: KEY expects a string key name");
    expected = args[0].as.string;
    pressed = getchar();
    if (pressed == EOF) return V_BOOL(0);
    if (pressed != '\n') {
        int discarded;
        while ((discarded = getchar()) != '\n' && discarded != EOF) { }
    }
    if (strlen(expected) == 1) return V_BOOL(pressed == (unsigned char)expected[0]);
    if (strcmp(expected, "enter") == 0) return V_BOOL(pressed == '\n' || pressed == '\r');
    if (strcmp(expected, "space") == 0) return V_BOOL(pressed == ' ');
    return V_BOOL(0);
}

static Value native_num(Value *args, int argc) {
    char *end;
    double value;
    require_argc("NUM", argc, 1);
    if (args[0].type != VAL_STRING)
        die("runtime error: NUM expects a string");
    value = strtod(args[0].as.string, &end);
    if (end == args[0].as.string || *end != '\0')
        die("runtime error: NUM could not convert '%s' to a number", args[0].as.string);
    return V_NUM(value);
}

static Value native_math_sqrt(Value *args, int argc) { require_argc("math.sqrt", argc, 1); return V_NUM(sqrt(require_num("math.sqrt", args[0], 1))); }
static Value native_math_pow(Value *args, int argc)  { require_argc("math.pow", argc, 2);  return V_NUM(pow(require_num("math.pow", args[0], 1), require_num("math.pow", args[1], 2))); }
static Value native_math_abs(Value *args, int argc)  { require_argc("math.abs", argc, 1);  return V_NUM(fabs(require_num("math.abs", args[0], 1))); }
static Value native_math_floor(Value *args, int argc){ require_argc("math.floor", argc, 1);return V_NUM(floor(require_num("math.floor", args[0], 1))); }
static Value native_math_ceil(Value *args, int argc) { require_argc("math.ceil", argc, 1); return V_NUM(ceil(require_num("math.ceil", args[0], 1))); }
static Value native_math_max(Value *args, int argc)  { require_argc("math.max", argc, 2);  double a=require_num("math.max",args[0],1),b=require_num("math.max",args[1],2); return V_NUM(a>b?a:b); }
static Value native_math_min(Value *args, int argc)  { require_argc("math.min", argc, 2);  double a=require_num("math.min",args[0],1),b=require_num("math.min",args[1],2); return V_NUM(a<b?a:b); }

static Value native_array_append(Value *args, int argc) {
    /* args[0] is the bound receiver injected by the caller */
    require_argc("array.append", argc, 2);
    EArray *arr = args[0].as.array;
    array_push(arr, args[1]);
    return V_NULL();
}

static Value native_array_slice(Value *args, int argc) {
    require_argc("array.slice", argc, 3);
    EArray *arr = args[0].as.array;
    int start = (int)require_num("array.slice", args[1], 1);
    int end = (int)require_num("array.slice", args[2], 2);
    if (start < 0) start = 0;
    if (end > arr->count) end = arr->count;
    Value out = V_ARRAY_NEW();
    for (int i = start; i < end; i++) array_push(out.as.array, arr->items[i]);
    return out;
}

/* Lookup table for module.property -> native function. Array methods are
 * resolved separately since they need a bound receiver. */
static Value lookup_module_member(const char *module, const char *prop) {
    if (strcmp(module, "Studio") == 0) {
        if (strcmp(prop, "print") == 0) return V_NATIVE(native_studio_print, "Studio.print");
    } else if (strcmp(module, "math") == 0) {
        if (strcmp(prop, "sqrt") == 0)  return V_NATIVE(native_math_sqrt, "math.sqrt");
        if (strcmp(prop, "pow") == 0)   return V_NATIVE(native_math_pow, "math.pow");
        if (strcmp(prop, "abs") == 0)   return V_NATIVE(native_math_abs, "math.abs");
        if (strcmp(prop, "floor") == 0) return V_NATIVE(native_math_floor, "math.floor");
        if (strcmp(prop, "ceil") == 0)  return V_NATIVE(native_math_ceil, "math.ceil");
        if (strcmp(prop, "max") == 0)   return V_NATIVE(native_math_max, "math.max");
        if (strcmp(prop, "min") == 0)   return V_NATIVE(native_math_min, "math.min");
        if (strcmp(prop, "PI") == 0)    return V_NUM(M_PI);
    }
    UserModule *user_module = find_user_module(module);
    if (user_module != NULL) {
        if (!module_exports(user_module, prop))
            die("runtime error: module '%s' does not export '%s'", module, prop);
        Value value;
        if (env_get(user_module->env, prop, &value)) return value;
        die("runtime error: module '%s' exports '%s' but does not define it", module, prop);
    }
    die("runtime error: module '%s' has no member '%s'", module, prop);
    return V_NULL();
}

/* ===================================================================== */
/* Evaluator                                                              */
/* ===================================================================== */

typedef enum { EXEC_NORMAL, EXEC_RETURN } ExecKind;
typedef struct { ExecKind kind; Value value; } ExecResult;

static Value eval_expr(Node *n, Env *env);
static ExecResult exec_block(Node *block, Env *env);
static Value call_value(Value callee, Value *args, int argc, int line);
static void load_user_module(const char *name, Env *parent);
static char *read_file(const char *path);

static Value eval_binary(Node *n, Env *env) {
    /* short-circuit logical ops */
    if (strcmp(n->str, "&&") == 0) {
        Value l = eval_expr(n->a, env);
        if (!is_truthy(l)) return V_BOOL(0);
        return V_BOOL(is_truthy(eval_expr(n->b, env)));
    }
    if (strcmp(n->str, "||") == 0) {
        Value l = eval_expr(n->a, env);
        if (is_truthy(l)) return V_BOOL(1);
        return V_BOOL(is_truthy(eval_expr(n->b, env)));
    }

    Value l = eval_expr(n->a, env);
    Value r = eval_expr(n->b, env);
    const char *op = n->str;

    if (strcmp(op, "+") == 0) {
        if (l.type == VAL_STRING || r.type == VAL_STRING) {
            char *ls = value_to_string(l), *rs = value_to_string(r);
            char *out = xmalloc(strlen(ls) + strlen(rs) + 1);
            strcpy(out, ls); strcat(out, rs);
            free(ls); free(rs);
            return V_STR_OWN(out);
        }
        if (l.type != VAL_NUMBER || r.type != VAL_NUMBER)
            die("line %d: runtime error: '+' requires numbers or strings", n->line);
        return V_NUM(l.as.number + r.as.number);
    }
    if (strcmp(op, "-") == 0) return V_NUM(require_num("-", l, 1) - require_num("-", r, 2));
    if (strcmp(op, "*") == 0) return V_NUM(require_num("*", l, 1) * require_num("*", r, 2));
    if (strcmp(op, "/") == 0) return V_NUM(require_num("/", l, 1) / require_num("/", r, 2));
    if (strcmp(op, "%") == 0) return V_NUM(fmod(require_num("%", l, 1), require_num("%", r, 2)));
    if (strcmp(op, "<") == 0)  return V_BOOL(require_num("<", l, 1)  < require_num("<", r, 2));
    if (strcmp(op, "<=") == 0) return V_BOOL(require_num("<=", l, 1) <= require_num("<=", r, 2));
    if (strcmp(op, ">") == 0)  return V_BOOL(require_num(">", l, 1)  > require_num(">", r, 2));
    if (strcmp(op, ">=") == 0) return V_BOOL(require_num(">=", l, 1) >= require_num(">=", r, 2));
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
        bool eq;
        if (l.type != r.type) eq = false;
        else switch (l.type) {
            case VAL_NUMBER: eq = l.as.number == r.as.number; break;
            case VAL_BOOL: eq = l.as.boolean == r.as.boolean; break;
            case VAL_STRING: eq = strcmp(l.as.string, r.as.string) == 0; break;
            case VAL_NULL: eq = true; break;
            case VAL_ARRAY: eq = l.as.array == r.as.array; break;
            default: eq = false; break;
        }
        return V_BOOL(strcmp(op, "==") == 0 ? eq : !eq);
    }
    die("line %d: runtime error: unknown operator '%s'", n->line, op);
    return V_NULL();
}

static Value eval_member(Node *n, Env *env) {
    Value obj = eval_expr(n->a, env);
    if (obj.type == VAL_MODULE) {
        return lookup_module_member(obj.as.module_name, n->str);
    }
    if (obj.type == VAL_ARRAY) {
        if (strcmp(n->str, "length") == 0) return V_NUM(obj.as.array->count);
        if (strcmp(n->str, "append") == 0) return V_NATIVE_BOUND(native_array_append, "array.append", obj);
        if (strcmp(n->str, "slice") == 0)  return V_NATIVE_BOUND(native_array_slice, "array.slice", obj);
        die("line %d: runtime error: array has no member '%s'", n->line, n->str);
    }
    die("line %d: runtime error: cannot access member '%s' on a %s", n->line, n->str, type_name(obj.type));
    return V_NULL();
}

static Value eval_subscript(Node *n, Env *env) {
    Value base = eval_expr(n->a, env);

    if (base.type == VAL_ARRAY) {
        if (n->list_count != 1)
            die("line %d: runtime error: array indexing takes exactly one index", n->line);
        Value idx = eval_expr(n->list[0], env);
        if (idx.type != VAL_NUMBER) die("line %d: runtime error: array index must be a number", n->line);
        int i = (int)idx.as.number;
        if (i < 0 || i >= base.as.array->count)
            die("line %d: runtime error: array index %d out of bounds (length %d)", n->line, i, base.as.array->count);
        return base.as.array->items[i];
    }

    if (base.type == VAL_FUNCTION || base.type == VAL_NATIVE) {
        Value *argv = n->list_count ? xmalloc(sizeof(Value) * n->list_count) : NULL;
        for (int i = 0; i < n->list_count; i++) argv[i] = eval_expr(n->list[i], env);
        Value result = call_value(base, argv, n->list_count, n->line);
        free(argv);
        return result;
    }

    die("line %d: runtime error: cannot index or call a %s", n->line, type_name(base.type));
    return V_NULL();
}

static char *shell_quote(const char *text) {
    size_t size = strlen(text) * 4 + 3;
    char *quoted = xmalloc(size);
    size_t out = 0;
    quoted[out++] = '\'';
    for (size_t i = 0; text[i]; i++) {
        if (text[i] == '\'') {
            quoted[out++] = '\''; quoted[out++] = '\\'; quoted[out++] = '\''; quoted[out++] = '\'';
        } else {
            quoted[out++] = text[i];
        }
    }
    quoted[out++] = '\''; quoted[out] = '\0';
    return quoted;
}

static Value call_external(const ENative *native, Value *args, int argc, int line) {
    size_t command_size = strlen(native->external_command) + 1;
    char *command = xmalloc(command_size);
    strcpy(command, native->external_command);
    for (int i = 0; i < argc; i++) {
        char *text = value_to_string(args[i]);
        char *quoted = shell_quote(text);
        command_size += strlen(quoted) + 1;
        command = xrealloc(command, command_size);
        strcat(command, " ");
        strcat(command, quoted);
        free(text);
        free(quoted);
    }
    FILE *pipe = popen(command, "r");
    free(command);
    if (!pipe) die("line %d: could not launch external library function '%s'", line, native->name);
    char output[4096] = {0};
    size_t length = fread(output, 1, sizeof(output) - 1, pipe);
    int status = pclose(pipe);
    if (status != 0) die("line %d: external library function '%s' failed", line, native->name);
    output[length] = '\0';
    output[strcspn(output, "\r\n")] = '\0';
    return V_STR(output);
}

static Value call_value(Value callee, Value *args, int argc, int line) {
    if (callee.type == VAL_NATIVE) {
        ENative *nat = callee.as.native;
        if (nat->external_command != NULL)
            return call_external(nat, args, argc, line);
        if (nat->has_receiver) {
            Value *full = xmalloc(sizeof(Value) * (argc + 1));
            full[0] = nat->receiver;
            for (int i = 0; i < argc; i++) full[i + 1] = args[i];
            Value r = nat->fn(full, argc + 1);
            free(full);
            return r;
        }
        return nat->fn(args, argc);
    }
    if (callee.type == VAL_FUNCTION) {
        EFunction *f = callee.as.function;
        Env *call_env = env_new(f->closure);
        for (int i = 0; i < f->param_count; i++) {
            Value v = (i < argc) ? args[i] : V_NULL();
            env_define(call_env, f->params[i], v);
        }
        if (f->body == NULL && f->lambda_expr != NULL) {
            return eval_expr(f->lambda_expr, call_env);
        }
        ExecResult res = exec_block(f->body, call_env);
        return (res.kind == EXEC_RETURN) ? res.value : V_NULL();
    }
    die("line %d: runtime error: value of type %s is not callable", line, type_name(callee.type));
    return V_NULL();
}

static Value eval_expr(Node *n, Env *env) {
    switch (n->type) {
        case ND_NUMBER: return V_NUM(n->number);
        case ND_STRING: return V_STR(n->str);
        case ND_BOOL: return V_BOOL(n->boolean);
        case ND_ARRAY_LIT: {
            Value v = V_ARRAY_NEW();
            for (int i = 0; i < n->list_count; i++) array_push(v.as.array, eval_expr(n->list[i], env));
            return v;
        }
        case ND_IDENT: {
            Value v;
            if (env_get(env, n->str, &v)) return v;
            if (is_known_module(n->str)) {
                if (!is_imported(n->str))
                    die("line %d: runtime error: '%s' is undefined! (did you forget 'import [%s]'?)",
                        n->line, n->str, n->str);
                return V_MODULE(n->str);
            }
            die("line %d: runtime error: '%s' is undefined", n->line, n->str);
        }
        case ND_UNARY: {
            Value v = eval_expr(n->a, env);
            if (strcmp(n->str, "-") == 0) return V_NUM(-require_num("-", v, 1));
            if (strcmp(n->str, "!") == 0) return V_BOOL(!is_truthy(v));
            die("line %d: runtime error: unknown unary operator", n->line);
        }
        case ND_BINARY: return eval_binary(n, env);
        case ND_MEMBER: return eval_member(n, env);
        case ND_SUBSCRIPT: return eval_subscript(n, env);
        case ND_ASYNC_CALL: return eval_expr(n->a, env); /* executed synchronously */
        case ND_LAMBDA: {
            if (!is_imported("lambda"))
                die("line %d: runtime error: LAMBDA requires 'import [lambda]'", n->line);
            Value v; v.type = VAL_FUNCTION;
            EFunction *f = xmalloc(sizeof(EFunction));
            f->name = n->str ? n->str : NULL;
            f->params = n->params;
            f->param_count = n->param_count;
            f->body = NULL;
            f->lambda_expr = n->a;
            f->closure = env;
            f->is_async = false;
            v.as.function = f;
            return v;
        }
        default:
            die("line %d: internal error: node type %d is not an expression", n->line, n->type);
    }
    return V_NULL();
}

static ExecResult exec_stmt(Node *n, Env *env);

static ExecResult exec_block(Node *block, Env *env) {
    Env *scope = env_new(env);
    for (int i = 0; i < block->list_count; i++) {
        ExecResult r = exec_stmt(block->list[i], scope);
        if (r.kind == EXEC_RETURN) return r;
    }
    ExecResult r = { EXEC_NORMAL, V_NULL() };
    return r;
}

static ExecResult exec_stmt(Node *n, Env *env) {
    ExecResult normal = { EXEC_NORMAL, V_NULL() };
    switch (n->type) {
        case ND_IMPORT:
            if (!is_known_module(n->str) && !is_imported(n->str))
                load_user_module(n->str, env);
            mark_imported(n->str);
            return normal;
        case ND_FUNC_DECL: {
            Value v; v.type = VAL_FUNCTION;
            EFunction *f = xmalloc(sizeof(EFunction));
            f->name = n->str;
            f->params = n->params;
            f->param_count = n->param_count;
            f->body = n->a;
            f->lambda_expr = NULL;
            f->closure = env;
            f->is_async = n->is_async;
            v.as.function = f;
            env_define(env, n->str, v);
            return normal;
        }
        case ND_VAR_DECL:
            env_define(env, n->str, eval_expr(n->a, env));
            return normal;
        case ND_ASSIGN: {
            Value v = eval_expr(n->b, env);
            if (n->a->type == ND_IDENT) {
                if (!env_set_existing(env, n->a->str, v)) env_define(env, n->a->str, v);
            } else if (n->a->type == ND_SUBSCRIPT) {
                Value base = eval_expr(n->a->a, env);
                if (base.type != VAL_ARRAY || n->a->list_count != 1)
                    die("line %d: runtime error: invalid assignment target", n->line);
                int idx = (int)require_num("=", eval_expr(n->a->list[0], env), 1);
                if (idx < 0 || idx >= base.as.array->count)
                    die("line %d: runtime error: array index %d out of bounds", n->line, idx);
                base.as.array->items[idx] = v;
            } else {
                die("line %d: runtime error: invalid assignment target", n->line);
            }
            return normal;
        }
        case ND_RETURN: {
            ExecResult r = { EXEC_RETURN, n->a ? eval_expr(n->a, env) : V_NULL() };
            return r;
        }
        case ND_EXPR_STMT:
            eval_expr(n->a, env);
            return normal;
        case ND_IF: {
            for (int i = 0; i < n->branch_count; i++) {
                if (is_truthy(eval_expr(n->conds[i], env))) {
                    return exec_block(n->blocks[i], env);
                }
            }
            if (n->else_block) return exec_block(n->else_block, env);
            return normal;
        }
        case ND_LOOP: {
            Value first = eval_expr(n->a, env);
            if (first.type == VAL_NUMBER) {
                long count = (long)first.as.number;
                for (long i = 0; i < count; i++) {
                    ExecResult r = exec_block(n->b, env);
                    if (r.kind == EXEC_RETURN) return r;
                }
            } else {
                while (is_truthy(first)) {
                    ExecResult r = exec_block(n->b, env);
                    if (r.kind == EXEC_RETURN) return r;
                    first = eval_expr(n->a, env);
                }
            }
            return normal;
        }
        case ND_BLOCK:
            return exec_block(n, env);
        default:
            die("line %d: internal error: node type %d is not a statement", n->line, n->type);
    }
    return normal;
}

static char *trim_text(char *text) {
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static void add_module_export(UserModule *module, const char *name) {
    module->exports = xrealloc(module->exports,
                               sizeof(char *) * (module->export_count + 1));
    module->exports[module->export_count++] = xstrdup(name);
}

static void load_user_module(const char *name, Env *parent) {
    char header_path[512];
    char source_path[512];
    char header_line[512];
    char source_name[256];
    char header_dir[512] = {0};
    char candidate[512];
    FILE *header;
    UserModule *module;
    bool has_source = false;

    if (find_user_module(name) != NULL) return;
    snprintf(candidate, sizeof(candidate), "%s.hsh", name);
    header = fopen(candidate, "r");
    if (header != NULL) {
        snprintf(header_path, sizeof(header_path), "%s", candidate);
    } else {
        snprintf(candidate, sizeof(candidate), "%s/%s.hsh", name, name);
        header = fopen(candidate, "r");
        if (header != NULL) {
            snprintf(header_path, sizeof(header_path), "%s", candidate);
        } else {
            snprintf(candidate, sizeof(candidate), "%s-Esharp-Library/%s.hsh", name, name);
            header = fopen(candidate, "r");
            if (header != NULL) snprintf(header_path, sizeof(header_path), "%s", candidate);
        }
    }
    if (!header)
        die("cannot import '%s': expected header '%s' and implementation '%s.esh'",
            name, header_path, name);

    char *last_slash = strrchr(header_path, '/');
    if (last_slash != NULL) {
        size_t directory_length = (size_t)(last_slash - header_path);
        memcpy(header_dir, header_path, directory_length);
        header_dir[directory_length] = '\0';
    }

    module = &g_modules[g_module_count++];
    memset(module, 0, sizeof(*module));
    module->name = xstrdup(name);
    module->env = env_new(parent);
    snprintf(source_name, sizeof(source_name), "%s.esh", name);

    while (fgets(header_line, sizeof(header_line), header)) {
        char *line = trim_text(header_line);
        if (*line == '\0' || *line == '#') continue;
        char keyword[32];
        char value[256];
        char command[256] = {0};
        int fields = sscanf(line, "%31s %255s %255[^\n]", keyword, value, command);
        if (fields < 2)
            die("invalid HSH line in '%s': %s", header_path, line);
        if (strcmp(keyword, "module") == 0) {
            if (strcmp(value, name) != 0)
                die("HSH module name '%s' does not match import '%s'", value, name);
        } else if (strcmp(keyword, "export") == 0) {
            add_module_export(module, value);
        } else if (strcmp(keyword, "source") == 0) {
            snprintf(source_name, sizeof(source_name), "%s", value);
            has_source = true;
        } else if (strcmp(keyword, "external") == 0) {
            if (fields < 3 || command[0] == '\0')
                die("external directive needs a function name and command in '%s'", header_path);
            env_define(module->env, value, V_EXTERNAL(command, value));
        } else {
            die("unsupported HSH directive '%s' in '%s'", keyword, header_path);
        }
    }
    fclose(header);

    if (!has_source) {
        FILE *default_source = fopen(source_name, "r");
        if (default_source != NULL) {
            fclose(default_source);
            has_source = true;
        }
    }
    if (!has_source) return;
    if (header_dir[0] != '\0' && strchr(source_name, '/') == NULL)
        snprintf(source_path, sizeof(source_path), "%s/%s", header_dir, source_name);
    else
        snprintf(source_path, sizeof(source_path), "%s", source_name);
    char *source = read_file(source_path);
    Parser parser;
    p_init(&parser, source);
    Node *program = parse_program(&parser);

    for (int i = 0; i < program->list_count; i++) {
        Node *statement = program->list[i];
        if (statement->type == ND_IMPORT || statement->type == ND_FUNC_DECL)
            exec_stmt(statement, module->env);
    }
    for (int i = 0; i < program->list_count; i++) {
        Node *statement = program->list[i];
        if (statement->type != ND_IMPORT && statement->type != ND_FUNC_DECL)
            exec_stmt(statement, module->env);
    }
    free(source);
}

/* ===================================================================== */
/* Entry point                                                            */
/* ===================================================================== */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open file '%s'", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc(size + 1);
    size_t n = fread(buf, 1, size, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <script.esh>\n", argv[0]);
        return 1;
    }
    char *src = read_file(argv[1]);

    Parser p;
    p_init(&p, src);
    Node *program = parse_program(&p);

    Env *global = env_new(NULL);
    env_define(global, "INPUT", V_NATIVE(native_input, "INPUT"));
    env_define(global, "KEY", V_NATIVE(native_key, "KEY"));
    env_define(global, "NUM", V_NATIVE(native_num, "NUM"));

    /* Pass 1: run imports and register all top-level function declarations
     * first, so functions can call each other regardless of order (and so
     * main() can be called even if defined before its callees or vice
     * versa). */
    for (int i = 0; i < program->list_count; i++) {
        Node *stmt = program->list[i];
        if (stmt->type == ND_IMPORT || stmt->type == ND_FUNC_DECL) {
            exec_stmt(stmt, global);
        }
    }
    /* Pass 2: execute any remaining top-level statements (e.g. Var decls)
     * in order. */
    for (int i = 0; i < program->list_count; i++) {
        Node *stmt = program->list[i];
        if (stmt->type != ND_IMPORT && stmt->type != ND_FUNC_DECL) {
            exec_stmt(stmt, global);
        }
    }

    Value main_fn;
    if (env_get(global, "main", &main_fn) && main_fn.type == VAL_FUNCTION) {
        call_value(main_fn, NULL, 0, 0);
    } else {
        die("no 'Func main()' entry point found");
    }

    return 0;
}
