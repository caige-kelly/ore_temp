// LSP integration test harness — spawns `./ore lsp` as a subprocess,
// pipes JSON-RPC over stdin/stdout, parses responses by Content-Length
// framing, and asserts on the outputs.
//
// Failure mode: any assertion failure aborts (exit code 1). Makefile
// target reports failure to the user.
//
// This codifies the /tmp/lsp_*.py scripts we wrote ad-hoc during the
// LSP sessions (cross-file invalidation, hover, completion). Every
// future LSP feature gets a regression net here.

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// Harness
// ============================================================================

typedef struct {
    pid_t pid;
    int   to_server;   // write end of subprocess stdin
    int   from_server; // read end of subprocess stdout
    char *buf;
    size_t buf_len;
    size_t buf_cap;
} LspClient;

__attribute__((format(printf, 1, 2)))
__attribute__((noreturn))
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "lsp_test: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static LspClient *lsp_start(const char *binary_path) {
    int to_pipe[2], from_pipe[2];
    if (pipe(to_pipe) < 0 || pipe(from_pipe) < 0)
        die("pipe() failed: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork() failed: %s", strerror(errno));

    if (pid == 0) {
        dup2(to_pipe[0], 0);
        dup2(from_pipe[1], 1);
        // stderr inherited — surfaces server crash messages.
        close(to_pipe[0]); close(to_pipe[1]);
        close(from_pipe[0]); close(from_pipe[1]);
        execl(binary_path, binary_path, "lsp", (char *)NULL);
        fprintf(stderr, "lsp_test: execl(%s) failed: %s\n",
                binary_path, strerror(errno));
        _exit(127);
    }

    close(to_pipe[0]);
    close(from_pipe[1]);

    LspClient *c = (LspClient *)calloc(1, sizeof(LspClient));
    c->pid = pid;
    c->to_server = to_pipe[1];
    c->from_server = from_pipe[0];
    c->buf_cap = 16384;
    c->buf = (char *)malloc(c->buf_cap);
    return c;
}

static void lsp_stop(LspClient *c) {
    if (c->to_server >= 0) { close(c->to_server); c->to_server = -1; }
    int status = 0;
    for (int i = 0; i < 50; i++) {
        pid_t r = waitpid(c->pid, &status, WNOHANG);
        if (r == c->pid) goto done;
        if (r < 0) break;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(c->pid, SIGKILL);
    waitpid(c->pid, &status, 0);
done:
    if (c->from_server >= 0) close(c->from_server);
    free(c->buf);
    free(c);
}

static void lsp_send(LspClient *c, const char *json_body) {
    size_t body_len = strlen(json_body);
    char header[128];
    int n = snprintf(header, sizeof header,
                     "Content-Length: %zu\r\n\r\n", body_len);
    if (write(c->to_server, header, (size_t)n) != n ||
        write(c->to_server, json_body, body_len) != (ssize_t)body_len)
        die("write to server failed: %s", strerror(errno));
}

static int read_at_least(LspClient *c, size_t need, int timeout_ms) {
    while (c->buf_len < need) {
        struct pollfd pf = {.fd = c->from_server, .events = POLLIN};
        int r = poll(&pf, 1, timeout_ms);
        if (r <= 0) return 0;
        if (c->buf_len == c->buf_cap) {
            c->buf_cap *= 2;
            c->buf = (char *)realloc(c->buf, c->buf_cap);
        }
        ssize_t got = read(c->from_server, c->buf + c->buf_len,
                           c->buf_cap - c->buf_len);
        if (got <= 0) return 0;
        c->buf_len += (size_t)got;
    }
    return 1;
}

static char *lsp_recv_message(LspClient *c, int timeout_ms) {
    for (;;) {
        for (size_t i = 0; i + 3 < c->buf_len; i++) {
            if (c->buf[i]   == '\r' && c->buf[i+1] == '\n' &&
                c->buf[i+2] == '\r' && c->buf[i+3] == '\n') {
                size_t cl = 0;
                static const char k[] = "content-length:";
                for (size_t s = 0; s + sizeof(k) - 1 <= i; s++) {
                    if (strncasecmp(c->buf + s, k, sizeof(k) - 1) == 0) {
                        s += sizeof(k) - 1;
                        while (s < i && (c->buf[s] == ' ' || c->buf[s] == '\t')) s++;
                        cl = (size_t)atoll(c->buf + s);
                        break;
                    }
                }
                size_t total = i + 4 + cl;
                if (!read_at_least(c, total, timeout_ms)) return NULL;
                char *body = (char *)malloc(cl + 1);
                memcpy(body, c->buf + i + 4, cl);
                body[cl] = '\0';
                memmove(c->buf, c->buf + total, c->buf_len - total);
                c->buf_len -= total;
                return body;
            }
        }
        if (!read_at_least(c, c->buf_len + 1, timeout_ms)) return NULL;
    }
}

static int contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static int count_diag_entries(const char *body) {
    // Each diag entry has exactly one "severity": field. Reliable
    // because our diag messages never embed the literal string.
    int n = 0;
    const char *p = body;
    while ((p = strstr(p, "\"severity\":"))) { n++; p++; }
    return n;
}

static void file_write(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) die("fopen(%s): %s", path, strerror(errno));
    fputs(contents, f);
    fclose(f);
}

// JSON-escape `src` into `dst` (newlines, quotes, backslashes). Caller
// sizes dst conservatively (2x src + 1).
static void json_escape(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < cap; i++) {
        char c = src[i];
        if (c == '\n')      { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (c == '"')  { dst[j++] = '\\'; dst[j++] = '"'; }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else                  dst[j++] = c;
    }
    dst[j] = '\0';
}

// Pump until a publishDiagnostics for `uri_fragment` arrives. NULL on timeout.
static char *wait_for_diags_for(LspClient *c, const char *uri_fragment,
                                int timeout_ms) {
    for (;;) {
        char *body = lsp_recv_message(c, timeout_ms);
        if (!body) return NULL;
        if (contains(body, "publishDiagnostics") &&
            contains(body, uri_fragment))
            return body;
        free(body);
    }
}

// ============================================================================
// JSON-RPC message constants
// ============================================================================

static const char *MSG_INITIALIZE =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
    "\"params\":{\"rootUri\":null,\"capabilities\":{}}}";
static const char *MSG_INITIALIZED =
    "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}";
static const char *MSG_SHUTDOWN =
    "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"shutdown\"}";
static const char *MSG_EXIT =
    "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";

static void send_did_open(LspClient *c, const char *uri,
                          const char *text /* raw, will be escaped */) {
    char esc[8192];
    json_escape(esc, sizeof esc, text);
    char msg[16384];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
             "\"params\":{\"textDocument\":{\"uri\":\"%s\","
             "\"languageId\":\"ore\",\"version\":1,\"text\":\"%s\"}}}",
             uri, esc);
    lsp_send(c, msg);
}

static void send_did_change(LspClient *c, const char *uri, int version,
                            const char *text) {
    char esc[8192];
    json_escape(esc, sizeof esc, text);
    char msg[16384];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
             "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%d},"
             "\"contentChanges\":[{\"text\":\"%s\"}]}}",
             uri, version, esc);
    lsp_send(c, msg);
}

static void init_session(LspClient *c) {
    lsp_send(c, MSG_INITIALIZE);
    free(lsp_recv_message(c, 5000)); // initialize result
    lsp_send(c, MSG_INITIALIZED);
}

static void close_session(LspClient *c) {
    lsp_send(c, MSG_SHUTDOWN);
    lsp_send(c, MSG_EXIT);
    lsp_stop(c);
}

// ============================================================================
// Tests
// ============================================================================

// Test 1 — didOpen on a file with a known sema error publishes a
// diagnostic. Regression for the post-Phase-A hover SIGABRT class
// (server crashed before publishDiagnostics could fire).
static void test_did_open_publishes_diag(const char *bin) {
    const char *src = "main :: pub fn() i32\n    badname.foo\n    return 0\n";
    file_write("/tmp/lsp_test_t1.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t1.ore", src);

    char *body = wait_for_diags_for(c, "lsp_test_t1.ore", 5000);
    if (!body) die("test 1: timeout waiting for publishDiagnostics");
    int n = count_diag_entries(body);
    if (n < 1) die("test 1: expected >=1 diag, got %d", n);
    free(body);

    close_session(c);
    fprintf(stderr, "  test_did_open_publishes_diag: OK\n");
}

// Test 2 — cross-file invalidation. Open A + B (B @imports A), edit A
// to drop the `pub`, expect B's publishDiagnostics without typing in B.
// Direct regression for the Phase B cross-file invalidation work.
static void test_cross_file_invalidation(const char *bin) {
    mkdir("/tmp/lsp_test_t2", 0755);
    const char *a_pub  = "Foo :: pub effect\n  op :: fn() void\n";
    const char *a_priv = "Foo :: effect\n  op :: fn() void\n";
    const char *b_src  = "a :: @import(\"./a.ore\")\nx :: a.Foo\n";

    // Files must exist on disk for workspace_resolve_import's realpath.
    file_write("/tmp/lsp_test_t2/a.ore", a_pub);
    file_write("/tmp/lsp_test_t2/b.ore", b_src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t2/a.ore", a_pub);
    send_did_open(c, "file:///tmp/lsp_test_t2/b.ore", b_src);

    // Drain the initial publishes (one or two per file via republish_all_open).
    for (int i = 0; i < 4; i++) {
        char *body = lsp_recv_message(c, 1500);
        if (!body) break;
        free(body);
    }

    // Drop `pub` from Foo. B's `a.Foo` should error after republish.
    send_did_change(c, "file:///tmp/lsp_test_t2/a.ore", 2, a_priv);

    char *body_b = wait_for_diags_for(c, "b.ore", 5000);
    if (!body_b) die("test 2: timeout — b.ore did not get republished");
    int n = count_diag_entries(body_b);
    if (n < 1) die("test 2: expected >=1 diag on b.ore after a.ore lost "
                   "`pub`, got %d", n);
    free(body_b);

    close_session(c);
    fprintf(stderr, "  test_cross_file_invalidation: OK\n");
}

// Test 3 — hover on a file doesn't crash. Regression for the SIGABRT
// in oredb_hover (db_emit_* called outside any query frame).
static void test_hover_no_sigabrt(const char *bin) {
    const char *src =
        "foo :: struct\n  x : i32\n\nmain :: pub fn() i32\n"
        "    bad.field\n    return 0\n";
    file_write("/tmp/lsp_test_t3.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t3.ore", src);

    // Drain initial diags before sending hover.
    free(wait_for_diags_for(c, "lsp_test_t3.ore", 5000));

    char hover_msg[512];
    snprintf(hover_msg, sizeof hover_msg,
             "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"textDocument/hover\","
             "\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///tmp/lsp_test_t3.ore\"},"
             "\"position\":{\"line\":0,\"character\":0}}}");
    lsp_send(c, hover_msg);

    int got_response = 0;
    for (int i = 0; i < 10; i++) {
        char *body = lsp_recv_message(c, 5000);
        if (!body) break;
        if (contains(body, "\"id\":42")) {
            got_response = 1;
            free(body);
            break;
        }
        free(body);
    }
    if (!got_response)
        die("test 3: hover request never got a response (server crashed?)");

    close_session(c);
    fprintf(stderr, "  test_hover_no_sigabrt: OK\n");
}

int main(int argc, char **argv) {
    const char *bin = argc > 1 ? argv[1] : "./ore";
    fprintf(stderr, "lsp_test: using binary %s\n", bin);
    test_did_open_publishes_diag(bin);
    test_cross_file_invalidation(bin);
    test_hover_no_sigabrt(bin);
    fprintf(stderr, "lsp_test: all PASS\n");
    return 0;
}
