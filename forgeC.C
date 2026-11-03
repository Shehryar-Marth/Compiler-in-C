/*
 * ForgeC: a tiny educational compiler.
 *
 * Pipeline:  source -> parser -> AST -> semantic check -> C code -> system C compiler
 *
 * Grammar:
 *   program := statement+
 *   statement := "print" expr ";"
 *   expr    := term   (("+" | "-") term)*
 *   term    := unary  (("*" | "/" | "%") unary)*
 *   unary   := ("-" | "+") unary | primary
 *   primary := integer | "(" expr ")"
 *   integer := digit+
 *
 * - All numbers are 64-bit signed integers. Overflow and division by zero are
 *   compile-time errors, reported at the offending operator.
 * - "/" and "%" truncate toward zero, like C.
 * - Whitespace is insignificant. "//" starts a comment that runs to end of line.
 *
 * The compiler pipes the generated C to the system C compiler (gcc by default,
 * or the program named by $CC) to build a native executable.
 *
 * Usage: forgec [--emit-c] <input.fc> <output>
 *   --emit-c   write the generated C to <output> instead of compiling it
 *
 * Requires a POSIX system (Linux, macOS, WSL) and GCC or Clang
 * (uses __builtin_*_overflow).
 */
#define _POSIX_C_SOURCE 200809L
 
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
 
#define MAX_INPUT_BYTES (1024 * 1024)
#define MAX_NESTING 200      /* parentheses / chained unary operators */
#define MAX_TREE_DEPTH 5000  /* longest chain like 1+1+1+... */
 
/* ---------- diagnostics ---------- */
 
static _Noreturn void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "forgec: error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
 
/* ---------- reading the source file ---------- */
 
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open '%s': %s", path, strerror(errno));
 
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) fail("out of memory");
 
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len > MAX_INPUT_BYTES) fail("'%s' is too large (limit %d bytes)", path, MAX_INPUT_BYTES);
        if (len == cap) {
            cap *= 2;
            char *grown = realloc(buf, cap + 1);
            if (!grown) fail("out of memory");
            buf = grown;
        }
    }
    if (ferror(f)) fail("error reading '%s'", path);
    fclose(f);
    buf[len] = '\0';
    if (strlen(buf) != len) fail("'%s' contains a NUL byte", path);
    return buf;
}
 
/* ---------- growable output buffer ---------- */
 
typedef struct { char *data; size_t len, cap; } Buf;
 
static void buf_addf(Buf *b, const char *fmt, ...) {
    va_list ap, cp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    int need = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (need < 0) fail("internal formatting error");
    size_t total = b->len + (size_t)need + 1;
    if (total > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        while (ncap < total) ncap *= 2;
        char *grown = realloc(b->data, ncap);
        if (!grown) fail("out of memory");
        b->data = grown;
        b->cap = ncap;
    }
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}
 
/* ---------- AST ---------- */
 
typedef enum { N_NUM, N_NEG, N_BIN } NodeKind;
 
typedef struct Node {
    NodeKind kind;
    char op;                    /* N_BIN: one of + - * / % */
    long long value;            /* N_NUM */
    struct Node *left, *right;  /* N_NEG uses left; N_BIN uses both */
    struct Node *next_alloc;    /* intrusive list so all nodes can be freed together */
    int line, col, depth;
} Node;
 
typedef struct {
    const char *file;
    const char *src;
    size_t pos;
    int line, col;
    int nesting;
    Node *allocs;
} Parser;
 
static int cur(const Parser *p) { return (unsigned char)p->src[p->pos]; }
 
static void advance(Parser *p) {
    if (!p->src[p->pos]) return;
    if (p->src[p->pos] == '\n') { p->line++; p->col = 1; }
    else p->col++;
    p->pos++;
}
 
/* Skips whitespace and // comments. */
static void skip_ws(Parser *p) {
    for (;;) {
        if (cur(p) && isspace(cur(p))) {
            advance(p);
        } else if (cur(p) == '/' && p->src[p->pos + 1] == '/') {
            while (cur(p) && cur(p) != '\n') advance(p);
        } else {
            break;
        }
    }
}
 
static _Noreturn void fail_at(const Parser *p, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", p->file, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
 
/* Describes what is at the current position, for "expected X but found Y" messages. */
static const char *found(const Parser *p, char *buf, size_t size) {
    int c = cur(p);
    if (!c) return "end of input";
    if (!isprint(c)) {
        snprintf(buf, size, "byte 0x%02X", c);
        return buf;
    }
    size_t n = 0;
    buf[n++] = '\'';
    if (isalpha(c) || c == '_') {
        for (size_t j = p->pos; n + 2 < size && (isalnum((unsigned char)p->src[j]) || p->src[j] == '_'); j++)
            buf[n++] = p->src[j];
    } else {
        buf[n++] = (char)c;
    }
    buf[n++] = '\'';
    buf[n] = '\0';
    return buf;
}
 
static Node *new_node(Parser *p, NodeKind kind, int line, int col) {
    Node *n = calloc(1, sizeof *n);
    if (!n) fail("out of memory");
    n->kind = kind;
    n->line = line;
    n->col = col;
    n->depth = 1;
    n->next_alloc = p->allocs;
    p->allocs = n;
    return n;
}
 
static void free_nodes(Parser *p) {
    while (p->allocs) {
        Node *next = p->allocs->next_alloc;
        free(p->allocs);
        p->allocs = next;
    }
}
 
static void set_depth(Parser *p, Node *n, int child_depth) {
    n->depth = child_depth + 1;
    if (n->depth > MAX_TREE_DEPTH) fail_at(p, n->line, n->col, "expression is too complex");
}
 
static Node *make_neg(Parser *p, Node *operand, int line, int col) {
    Node *n = new_node(p, N_NEG, line, col);
    n->left = operand;
    set_depth(p, n, operand->depth);
    return n;
}
 
static Node *make_bin(Parser *p, char op, Node *l, Node *r, int line, int col) {
    Node *n = new_node(p, N_BIN, line, col);
    n->op = op;
    n->left = l;
    n->right = r;
    set_depth(p, n, l->depth > r->depth ? l->depth : r->depth);
    return n;
}
 
static void enter(Parser *p, int line, int col) {
    if (++p->nesting > MAX_NESTING) fail_at(p, line, col, "expression is nested too deeply");
}
 
static void leave(Parser *p) { p->nesting--; }
 
/* ---------- parser ---------- */
 
static Node *parse_expr(Parser *p);
 
/* Parses digits at the current position. If 'negative', the value is negated
 * (this is how -9223372036854775808 becomes representable). line/col locate
 * the start of the literal, including any sign. */
static Node *parse_number(Parser *p, int negative, int line, int col) {
    unsigned long long limit = negative ? (1ULL << 63) : (1ULL << 63) - 1;
    unsigned long long mag = 0;
    int overflow = 0;
 
    while (isdigit(cur(p))) {
        unsigned d = (unsigned)(cur(p) - '0');
        if (!overflow) {
            if (mag > (limit - d) / 10) overflow = 1;
            else mag = mag * 10 + d;
        }
        advance(p);
    }
    if (isalnum(cur(p)) || cur(p) == '_') fail_at(p, line, col, "invalid integer literal");
    if (overflow) fail_at(p, line, col, "integer literal is out of range for a 64-bit integer");
 
    Node *n = new_node(p, N_NUM, line, col);
    if (negative) n->value = (mag == (1ULL << 63)) ? LLONG_MIN : -(long long)mag;
    else n->value = (long long)mag;
    return n;
}
 
static Node *parse_primary(Parser *p) {
    char desc[40];
    skip_ws(p);
    int line = p->line, col = p->col;
 
    if (isdigit(cur(p))) return parse_number(p, 0, line, col);
 
    if (cur(p) == '(') {
        advance(p);
        enter(p, line, col);
        Node *e = parse_expr(p);
        skip_ws(p);
        if (cur(p) != ')')
            fail_at(p, p->line, p->col, "expected ')' but found %s", found(p, desc, sizeof desc));
        advance(p);
        leave(p);
        return e;
    }
    fail_at(p, line, col, "expected an expression but found %s", found(p, desc, sizeof desc));
}
 
static Node *parse_unary(Parser *p) {
    skip_ws(p);
    if (cur(p) == '+' || cur(p) == '-') {
        int line = p->line, col = p->col;
        int negative = cur(p) == '-';
        advance(p);
        enter(p, line, col);
        skip_ws(p);
        Node *n;
        if (negative && isdigit(cur(p))) {
            n = parse_number(p, 1, line, col);   /* fold the sign into the literal */
        } else {
            Node *operand = parse_unary(p);
            n = negative ? make_neg(p, operand, line, col) : operand;
        }
        leave(p);
        return n;
    }
    return parse_primary(p);
}
 
static Node *parse_term(Parser *p) {
    Node *left = parse_unary(p);
    for (;;) {
        skip_ws(p);
        int c = cur(p);
        if (c != '*' && c != '/' && c != '%') return left;
        int line = p->line, col = p->col;
        advance(p);
        Node *right = parse_unary(p);
        left = make_bin(p, (char)c, left, right, line, col);
    }
}
 
static Node *parse_expr(Parser *p) {
    Node *left = parse_term(p);
    for (;;) {
        skip_ws(p);
        int c = cur(p);
        if (c != '+' && c != '-') return left;
        int line = p->line, col = p->col;
        advance(p);
        Node *right = parse_term(p);
        left = make_bin(p, (char)c, left, right, line, col);
    }
}
 
/* ---------- semantic check: checked constant evaluation ---------- */
 
static long long eval(const Parser *p, const Node *n) {
    switch (n->kind) {
    case N_NUM:
        return n->value;
    case N_NEG: {
        long long v = eval(p, n->left);
        if (v == LLONG_MIN) fail_at(p, n->line, n->col, "integer overflow in unary '-'");
        return -v;
    }
    case N_BIN: {
        long long a = eval(p, n->left);
        long long b = eval(p, n->right);
        long long r;
        switch (n->op) {
        case '+':
            if (__builtin_add_overflow(a, b, &r)) fail_at(p, n->line, n->col, "integer overflow in '+'");
            return r;
        case '-':
            if (__builtin_sub_overflow(a, b, &r)) fail_at(p, n->line, n->col, "integer overflow in '-'");
            return r;
        case '*':
            if (__builtin_mul_overflow(a, b, &r)) fail_at(p, n->line, n->col, "integer overflow in '*'");
            return r;
        default: /* '/' or '%' */
            if (b == 0) fail_at(p, n->line, n->col, "division by zero");
            if (a == LLONG_MIN && b == -1) fail_at(p, n->line, n->col, "integer overflow in '%c'", n->op);
            return n->op == '/' ? a / b : a % b;
        }
    }
    }
    fail("internal error: bad node kind");
}
 
/* ---------- code generation ---------- */
 
static void emit_expr(Buf *b, const Node *n) {
    switch (n->kind) {
    case N_NUM:
        if (n->value == LLONG_MIN) buf_addf(b, "(-9223372036854775807LL - 1)");
        else if (n->value < 0) buf_addf(b, "(%lldLL)", n->value);
        else buf_addf(b, "%lldLL", n->value);
        break;
    case N_NEG:
        buf_addf(b, "(-");
        emit_expr(b, n->left);
        buf_addf(b, ")");
        break;
    case N_BIN:
        buf_addf(b, "(");
        emit_expr(b, n->left);
        buf_addf(b, " %c ", n->op);
        emit_expr(b, n->right);
        buf_addf(b, ")");
        break;
    }
}
 
/* Parses the whole program, appending generated C to 'out'. */
static void compile_program(Parser *p, Buf *out) {
    char desc[40];
    int statements = 0;
 
    buf_addf(out, "#include <stdio.h>\nint main(void) {\n");
    for (;;) {
        skip_ws(p);
        if (!cur(p)) break;
 
        /* "print" */
        int line = p->line, col = p->col;
        size_t start = p->pos;
        while (isalnum(cur(p)) || cur(p) == '_') advance(p);
        size_t len = p->pos - start;
        if (len == 0)
            fail_at(p, line, col, "expected 'print' but found %s", found(p, desc, sizeof desc));
        if (len != 5 || strncmp(p->src + start, "print", 5) != 0)
            fail_at(p, line, col, "expected 'print' but found '%.*s'", (int)len, p->src + start);
 
        Node *expr = parse_expr(p);
 
        /* ";" */
        skip_ws(p);
        if (cur(p) != ';')
            fail_at(p, p->line, p->col, "expected ';' but found %s", found(p, desc, sizeof desc));
        advance(p);
 
        eval(p, expr);   /* reports overflow / division by zero; the C below is then safe */
 
        buf_addf(out, "    printf(\"%%lld\\n\", ");
        emit_expr(out, expr);
        buf_addf(out, ");\n");
        free_nodes(p);
        statements++;
    }
    if (statements == 0)
        fail_at(p, p->line, p->col, "empty program: expected at least one 'print' statement");
    buf_addf(out, "    return 0;\n}\n");
}
 
/* ---------- invoking the C compiler (no shell, no temp files) ---------- */
 
static int run_c_compiler(const char *cc, const char *exe_path, const char *csrc) {
    int fds[2];
    if (pipe(fds) != 0) fail("pipe failed: %s", strerror(errno));
 
    pid_t pid = fork();
    if (pid < 0) fail("fork failed: %s", strerror(errno));
    if (pid == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp(cc, cc, "-x", "c", "-", "-o", exe_path, (char *)NULL);
        fprintf(stderr, "forgec: error: cannot run '%s': %s\n", cc, strerror(errno));
        _exit(127);
    }
 
    close(fds[0]);
    signal(SIGPIPE, SIG_IGN); /* if the compiler dies early, report that instead of crashing */
    size_t left = strlen(csrc);
    const char *cursor = csrc;
    while (left > 0) {
        ssize_t n = write(fds[1], cursor, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        cursor += n;
        left -= (size_t)n;
    }
    close(fds[1]);
 
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) fail("waitpid failed: %s", strerror(errno));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
 
/* ---------- main ---------- */
 
static void usage(FILE *to) {
    fprintf(to,
            "Usage: forgec [--emit-c] <input.fc> <output>\n"
            "  --emit-c   write the generated C to <output> instead of compiling it\n"
            "Environment: CC selects the C compiler (default: gcc)\n");
}
 
int main(int argc, char **argv) {
    int emit_c = 0;
    int i = 1;
    if (i < argc && (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (i < argc && strcmp(argv[i], "--emit-c") == 0) { emit_c = 1; i++; }
    if (argc - i != 2) {
        usage(stderr);
        return 2;
    }
    const char *input = argv[i], *output = argv[i + 1];
 
    char *source = read_file(input);
    Parser parser = { input, source, 0, 1, 1, 0, NULL };
    Buf csrc = { NULL, 0, 0 };
    compile_program(&parser, &csrc);
    free(source);
 
    if (emit_c) {
        FILE *out = fopen(output, "w");
        if (!out) fail("cannot create '%s': %s", output, strerror(errno));
        if (fputs(csrc.data, out) == EOF || fclose(out) == EOF) fail("error writing '%s'", output);
        free(csrc.data);
        return 0;
    }
 
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "gcc";
    if (run_c_compiler(cc, output, csrc.data) != 0) fail("C compiler '%s' failed", cc);
    free(csrc.data);
    puts("Compilation successful.");
    return 0;
}
 