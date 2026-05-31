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
#include <stdbool.h>
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
// M2: advertises textDocument.diagnostic so the server gates push.
static const char *MSG_INITIALIZE_WITH_PULL =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
    "\"params\":{\"rootUri\":null,\"capabilities\":{"
    "\"textDocument\":{\"diagnostic\":{\"dynamicRegistration\":false,"
    "\"relatedDocumentSupport\":false}}}}}";
// N1: pull + refreshSupport. Server sends workspace/diagnostic/refresh
// after any input mutation so the client knows to re-poll.
static const char *MSG_INITIALIZE_WITH_PULL_AND_REFRESH =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
    "\"params\":{\"rootUri\":null,\"capabilities\":{"
    "\"textDocument\":{\"diagnostic\":{\"dynamicRegistration\":false,"
    "\"relatedDocumentSupport\":false}},"
    "\"workspace\":{\"diagnostics\":{\"refreshSupport\":true}}}}}";
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

// M2: initialize with the pull-diagnostics client capability. Server
// suppresses push and waits for textDocument/diagnostic + workspace/
// diagnostic polls instead.
static void init_session_with_pull(LspClient *c) {
    lsp_send(c, MSG_INITIALIZE_WITH_PULL);
    free(lsp_recv_message(c, 5000));
    lsp_send(c, MSG_INITIALIZED);
}

// N1: pull + refreshSupport.
static void init_session_with_pull_and_refresh(LspClient *c) {
    lsp_send(c, MSG_INITIALIZE_WITH_PULL_AND_REFRESH);
    free(lsp_recv_message(c, 5000));
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

// Send a hover request and pump messages until we see the matching
// `"id":req_id` response, then extract the `result.contents.value`
// string into `out` (caller-sized). Returns 1 on hit, 0 on miss/null.
//
// Parser is permissive — finds `"value":"..."` anywhere in the body.
// Sufficient because the hover response is the only place we emit
// that key; cJSON would be overkill in a regression harness.
static int hover_at(LspClient *c, const char *uri, int req_id,
                    int line0, int char0, char *out, size_t outcap) {
    out[0] = '\0';
    char msg[512];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/hover\","
             "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
             "\"position\":{\"line\":%d,\"character\":%d}}}",
             req_id, uri, line0, char0);
    lsp_send(c, msg);

    char id_needle[32];
    snprintf(id_needle, sizeof id_needle, "\"id\":%d", req_id);

    for (int i = 0; i < 20; i++) {
        char *body = lsp_recv_message(c, 5000);
        if (!body) return 0;
        if (!contains(body, id_needle)) { free(body); continue; }
        // Null result (hover returned 0 — no content for this position).
        if (contains(body, "\"result\":null")) { free(body); return 0; }
        const char *v = strstr(body, "\"value\":\"");
        if (!v) { free(body); return 0; }
        v += strlen("\"value\":\"");
        size_t j = 0;
        while (*v && *v != '"' && j + 1 < outcap) {
            if (*v == '\\' && v[1]) { out[j++] = v[1]; v += 2; continue; }
            out[j++] = *v++;
        }
        out[j] = '\0';
        free(body);
        return 1;
    }
    return 0;
}

// Test 4 — hover renders correct types for the bug-class fixed by the
// nd->types IP_NONE init + the type-expr coverage gaps. Each assertion
// here corresponds to a user-reported hover bug from allocator.ore:
//
//   - struct field hovered as `bool`                 → "x: i32"
//   - array-size literal in [N]T hovered as `bool`   → "comptime_int"
//   - primitive type-name `u8` hovered as `?`        → contains "u8"
//   - fn-decl name hovered as `?`                    → contains "fn(" / "->"
//
// We test for inclusion of expected substrings, not exact equality —
// the surface format (e.g., "x: i32" vs "x : i32") is a polish layer
// that may evolve; the load-bearing assertion is the type itself.
static void test_hover_correctness(const char *bin) {
    // 2-space indent for struct fields, 4-space for fn bodies (matches
    // the project's existing examples + test 3).
    //
    // `buf : [100]u8` is the field-annotated form of `BINS := [100]u8`
    // from allocator.ore. The latter parses as AST_EXPR_PRODUCT and
    // sema_type_of_expr doesn't recurse into it (separate coverage
    // gap, tracked separately). The field-annotation slot routes
    // through sema_resolve_type_expr → AST_TYPE_ARRAY → Fix-2, which
    // is what we want to regression-test here.
    //
    // Column accounting (0-indexed):
    //   L1 "  x : i32"             col 2='x'
    //   L2 "  buf : [100]u8"       col 2='b', col 10='1', col 14='u'
    //   L3 "add :: fn(a: i32..."   col 0='a'
    const char *src =
        "foo :: struct\n"                  // L0
        "  x : i32\n"                      // L1
        "  buf : [100]u8\n"                // L2
        "add :: fn(a: i32, b: i32) i32\n"  // L3
        "    a + b\n"                      // L4
        "main :: pub fn() i32\n"           // L5
        "    return 0\n";                  // L6
    file_write("/tmp/lsp_test_t4.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t4.ore", src);

    // Drain initial diags so the hover responses aren't interleaved
    // behind them in the read buffer.
    free(wait_for_diags_for(c, "lsp_test_t4.ore", 5000));

    char hov[512];
    const char *uri = "file:///tmp/lsp_test_t4.ore";

    // (1) Struct field `x` — was `bool` pre-Fix-1+Fix-4. Now reads via
    // the AST_DECL_FIELD case → sema_resolve_type_expr on the annotated
    // type-expr.
    if (!hover_at(c, uri, 100, 1, 2, hov, sizeof hov))
        die("test 4: no hover for struct field `x`");
    if (!contains(hov, "i32") || contains(hov, "bool"))
        die("test 4: struct field `x` hover wrong: %s (want i32, not bool)", hov);

    // (2) Array-size literal `100` — was `bool` (uninit IP_NONE→0=bool
    // sentinel). Fix-1 makes unvisited slots `?`; Fix-2 then makes
    // sema_type_of_expr visit the literal during type-expr resolution.
    if (!hover_at(c, uri, 101, 2, 10, hov, sizeof hov))
        die("test 4: no hover for array-size literal `100`");
    if (!contains(hov, "comptime_int") || contains(hov, "bool"))
        die("test 4: literal `100` hover wrong: %s (want comptime_int)", hov);

    // (3) Primitive `u8` in type-slot — was `?`. Fix-3 routes the
    // namespace-miss through sema_lookup_primitive_name.
    if (!hover_at(c, uri, 102, 2, 14, hov, sizeof hov))
        die("test 4: no hover for primitive `u8`");
    if (!contains(hov, "u8") || contains(hov, "bool"))
        die("test 4: primitive `u8` hover wrong: %s (want u8)", hov);

    // (4) Fn-decl name `add` — was `?` for the simple case
    // pre-Fix-1+Fix-3 (the more complex `validate_heap` w/ anytype is a
    // separate sema gap, tracked outside hover). Should render as
    // `add: fn(i32, i32) -> i32`.
    if (!hover_at(c, uri, 103, 3, 0, hov, sizeof hov))
        die("test 4: no hover for fn-decl name `add`");
    if (!contains(hov, "fn(") || !contains(hov, "->") || !contains(hov, "i32"))
        die("test 4: fn-decl `add` hover wrong: %s (want fn(i32, i32) -> i32)",
            hov);

    // (5) Param name `a` at SIGNATURE position (line 3, col 10). Before
    // L2.5 (signature scope) this returned the param's type only via
    // the AST_DECL_PARAM fallback that re-ran sema_resolve_type_expr.
    // After L2.5, body_scope_lookup itself resolves it because the
    // signature subtree's nodes are mapped into the root scope.
    if (!hover_at(c, uri, 104, 3, 10, hov, sizeof hov))
        die("test 4: no hover for signature-position param `a`");
    if (!contains(hov, "a") || !contains(hov, "i32"))
        die("test 4: signature param `a` hover wrong: %s (want a: i32)", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_correctness: OK\n");
}

// Test 5 — sibling-decl edit must not stale the per-node type cache.
//
// Background: salsa early-cuts per-decl type queries (type_of_def,
// build_struct_type, sema_build_fn_type) when a decl's AST fingerprint
// is unchanged. Those impls used to be the ONLY writers of
// FileNodeData.types[] for struct-field / fn-param nodes. Reparse
// zeroes types[] each time. So before L2.4's post-typecheck walker,
// editing struct B leaves struct A's field cache at IP_NONE → hover on
// A's field returns `?`. The post-typecheck walker re-stamps types[]
// from salsa-cached results on every typecheck, so this can't happen.
static void test_hover_resilient_to_sibling_edits(const char *bin) {
    const char *src1 =
        "alpha :: struct\n"   // L0
        "  ax : i32\n"        // L1
        "  ay : u8\n"         // L2
        "beta :: struct\n"    // L3
        "  bx : i64\n"        // L4
        "main :: pub fn() i32\n"
        "    return 0\n";
    file_write("/tmp/lsp_test_t5.ore", src1);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t5.ore", src1);
    free(wait_for_diags_for(c, "lsp_test_t5.ore", 5000));

    const char *uri = "file:///tmp/lsp_test_t5.ore";
    char hov[512];

    // Pre-edit: hover on alpha.ax → i32.
    if (!hover_at(c, uri, 200, 1, 2, hov, sizeof hov))
        die("test 5: pre-edit hover on `ax` missing");
    if (!contains(hov, "i32"))
        die("test 5: pre-edit `ax` wrong: %s", hov);

    // Edit ONLY beta — change bx's type to f64. alpha's fingerprint is
    // unchanged so type_of_def for alpha should early-cut on next type-
    // check. Without the post-typecheck walker, alpha's field types[]
    // entries stay at IP_NONE after the reparse-zero.
    const char *src2 =
        "alpha :: struct\n"
        "  ax : i32\n"
        "  ay : u8\n"
        "beta :: struct\n"
        "  bx : f64\n"        // ← changed from i64
        "main :: pub fn() i32\n"
        "    return 0\n";
    send_did_change(c, uri, 2, src2);
    free(wait_for_diags_for(c, "lsp_test_t5.ore", 5000));

    // Post-edit: hover on alpha.ax must STILL be i32, not `?`.
    if (!hover_at(c, uri, 201, 1, 2, hov, sizeof hov))
        die("test 5: post-edit hover on `ax` missing (stale cache?)");
    if (!contains(hov, "i32") || contains(hov, "?"))
        die("test 5: post-edit `ax` wrong: %s (sibling-edit staled the cache)",
            hov);

    // Beta's edit took effect.
    if (!hover_at(c, uri, 202, 4, 2, hov, sizeof hov))
        die("test 5: post-edit hover on `bx` missing");
    if (!contains(hov, "f64"))
        die("test 5: post-edit `bx` wrong: %s (want f64)", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_resilient_to_sibling_edits: OK\n");
}

// Test 6 — self-referential struct types resolve via the ip_wip_struct
// cycle trampoline. Regression for the long-standing sema gap where
// `next : ?^Self` bottomed out at IP_NONE because the recursive
// type_of_def call returned the cycle-default instead of the wip's
// IpIndex. Surfaced when allocator.ore's Header struct hovered every
// pointer field as `?`. Closed by publishing wip.index into the def's
// type cell before the field loop, and reading the cell from the
// salsa cycle-return path in db_query_type_of_def.
static void test_hover_self_referential_struct(const char *bin) {
    const char *src =
        "Node :: struct\n"     // L0
        "  next  : ?^Node\n"   // L1   col 2='n'
        "  value : i32\n"      // L2   col 2='v'
        "main :: pub fn() i32\n"
        "    return 0\n";
    file_write("/tmp/lsp_test_t6.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t6.ore", src);
    free(wait_for_diags_for(c, "lsp_test_t6.ore", 5000));

    const char *uri = "file:///tmp/lsp_test_t6.ore";
    char hov[512];

    // `next : ?^Node` — pointer self-reference. Pre-fix this hovered as `?`.
    if (!hover_at(c, uri, 300, 1, 2, hov, sizeof hov))
        die("test 6: no hover for self-ref field `next`");
    // The exact rendering uses db_format_type (`?^struct Node` or similar).
    // Require: contains `Node` AND not bare `?` AND not `bool`.
    if (!contains(hov, "Node") || contains(hov, ": ?\""))
        die("test 6: self-ref field `next` hover wrong: %s (want ?^Node)", hov);
    if (contains(hov, "bool"))
        die("test 6: self-ref field `next` rendered as bool: %s", hov);

    // Plain field still works.
    if (!hover_at(c, uri, 301, 2, 2, hov, sizeof hov))
        die("test 6: no hover for plain field `value`");
    if (!contains(hov, "i32"))
        die("test 6: plain field `value` hover wrong: %s (want i32)", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_self_referential_struct: OK\n");
}

// Test 7 — mutually-referential struct types. A field of A references B,
// a field of B references A. Same cycle mechanism as test 6 covers
// this — the order of declaration matters (Ore processes top-level
// decls in source order), so the second declaration to reach the
// build-loop is the one that triggers the cycle trampoline.
static void test_hover_mutual_struct(const char *bin) {
    const char *src =
        "A :: struct\n"        // L0
        "  b : ^B\n"           // L1   col 2='b'
        "B :: struct\n"        // L2
        "  a : ^A\n"           // L3   col 2='a'
        "  v : i32\n"          // L4   col 2='v'
        "main :: pub fn() i32\n"
        "    return 0\n";
    file_write("/tmp/lsp_test_t7.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t7.ore", src);
    free(wait_for_diags_for(c, "lsp_test_t7.ore", 5000));

    const char *uri = "file:///tmp/lsp_test_t7.ore";
    char hov[512];

    // A.b → ^B
    if (!hover_at(c, uri, 400, 1, 2, hov, sizeof hov))
        die("test 7: no hover for A.b");
    if (!contains(hov, "B") || contains(hov, "bool"))
        die("test 7: A.b hover wrong: %s (want ^B)", hov);

    // B.a → ^A
    if (!hover_at(c, uri, 401, 3, 2, hov, sizeof hov))
        die("test 7: no hover for B.a");
    if (!contains(hov, "A") || contains(hov, "bool"))
        die("test 7: B.a hover wrong: %s (want ^A)", hov);

    // B.v → i32
    if (!hover_at(c, uri, 402, 4, 2, hov, sizeof hov))
        die("test 7: no hover for B.v");
    if (!contains(hov, "i32"))
        die("test 7: B.v hover wrong: %s (want i32)", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_mutual_struct: OK\n");
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

// Test 8 — long-edit session stress test for pool compaction.
//
// Open a file, do many didChange cycles, hover after each, verify the
// type still resolves correctly. The point isn't to exercise every
// hover position — it's to drive ENOUGH cumulative pool growth that
// at least one pool crosses ORE_COMPACT_MIN_THRESHOLD (4096 entries)
// and compaction fires at db_request_end. If compaction breaks
// anything (missed offset rewrite, sentinel relocation, etc.), the
// subsequent hover would return the wrong type — or crash the server.
//
// The threshold (4096) means a small fn re-typed ~30 times would just
// barely cross it for body_scope pools. We do 60 cycles to be safe.
static void test_hover_compaction_stress(const char *bin) {
    // Same shape as test 5 but with edits that toggle a field's type
    // back and forth — every cycle re-runs build_struct_type for foo
    // AND infer_body for main, growing all three pool families.
    const char *src_a =
        "foo :: struct\n"
        "  x : i32\n"
        "  y : u8\n"
        "add :: fn(a: i32, b: i32) i32\n"
        "    a + b\n"
        "main :: pub fn() i32\n"
        "    return 0\n";
    const char *src_b =
        "foo :: struct\n"
        "  x : i64\n"   // ← x's type toggles each cycle
        "  y : u8\n"
        "add :: fn(a: i32, b: i32) i32\n"
        "    a + b\n"
        "main :: pub fn() i32\n"
        "    return 0\n";

    file_write("/tmp/lsp_test_t8.ore", src_a);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t8.ore", src_a);
    free(wait_for_diags_for(c, "lsp_test_t8.ore", 5000));

    const char *uri = "file:///tmp/lsp_test_t8.ore";
    char hov[512];

    // Sanity check on cycle 0 — confirms the test setup before churn.
    if (!hover_at(c, uri, 8000, 1, 2, hov, sizeof hov))
        die("test 8: initial hover on `x` missing");
    if (!contains(hov, "i32"))
        die("test 8: initial hover wrong: %s", hov);

    // Stress: 60 cycles. Each cycle = one didChange + one hover. The
    // didChange forces a re-typecheck (struct fields toggle); the
    // hover at the end exercises the router after the request_end
    // potentially fired a compaction.
    for (int cycle = 1; cycle <= 60; cycle++) {
        const char *cur_src = (cycle % 2) ? src_b : src_a;
        const char *expected = (cycle % 2) ? "i64" : "i32";

        send_did_change(c, uri, cycle + 1, cur_src);
        free(wait_for_diags_for(c, "lsp_test_t8.ore", 5000));

        // Hover at line 1, col 2 — the `x` field name token. Type
        // toggles between i32 and i64 per cycle.
        if (!hover_at(c, uri, 8000 + cycle, 1, 2, hov, sizeof hov))
            die("test 8: cycle %d hover on `x` missing (compaction broke "
                "the cache?)", cycle);
        if (!contains(hov, expected))
            die("test 8: cycle %d hover wrong: %s (want %s)",
                cycle, hov, expected);
    }

    // Also verify a hover OUTSIDE the churning struct — `add`'s
    // signature — stays correct. Tests that compaction didn't corrupt
    // ranges that didn't re-run.
    if (!hover_at(c, uri, 8999, 3, 0, hov, sizeof hov))
        die("test 8: final hover on `add` missing");
    if (!contains(hov, "fn(") || !contains(hov, "->"))
        die("test 8: final hover on `add` wrong: %s", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_compaction_stress: OK\n");
}

// Test 9 — adding a new top-level decl mid-session must be visible to
// hover. Pins down the architectural fix for the latent staleness bug
// in db_query_namespace_scopes: before the (decl_lo, decl_len) rewrite,
// internal scope's range was written only on first allocation and
// re-runs grew the pool but never updated internal's hi. A decl added
// post-open landed at the pool tail, OUTSIDE internal scope's stale
// slice, and resolve_ref couldn't find it.
//
// This test would have failed before the rewrite; passing it confirms
// internal scope's range is refreshed on every re-run.
static void test_hover_add_decl_mid_session(const char *bin) {
    const char *src1 =
        "first :: fn(a: i32) i32\n"
        "    a\n";
    // Add a SECOND decl. If internal scope's range isn't refreshed,
    // resolve_ref(internal_scope, \"second\") returns DEF_ID_NONE and
    // the hover misses.
    const char *src2 =
        "first :: fn(a: i32) i32\n"
        "    a\n"
        "second :: pub fn(b: i64) i64\n"
        "    b\n";

    file_write("/tmp/lsp_test_t9.ore", src1);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t9.ore", src1);
    free(wait_for_diags_for(c, "lsp_test_t9.ore", 5000));

    const char *uri = "file:///tmp/lsp_test_t9.ore";
    char hov[512];

    // Sanity: `first` is hoverable on the initial open.
    if (!hover_at(c, uri, 9000, 0, 0, hov, sizeof hov))
        die("test 9: initial hover on `first` missing");
    if (!contains(hov, "fn("))
        die("test 9: initial hover on `first` wrong: %s", hov);

    // didChange to add `second`. This re-runs db_query_namespace_scopes,
    // appending `second` to the pool tail. Pre-fix, internal scope's
    // stale slice would not include it.
    send_did_change(c, uri, 2, src2);
    free(wait_for_diags_for(c, "lsp_test_t9.ore", 5000));

    // Hover on `second` at line 2, col 0. If internal scope's range
    // isn't refreshed, this returns no info.
    if (!hover_at(c, uri, 9001, 2, 0, hov, sizeof hov))
        die("test 9: hover on newly-added `second` missing — internal "
            "scope staleness regression?");
    if (!contains(hov, "i64"))
        die("test 9: hover on `second` wrong: %s (want i64)", hov);

    // And `first` still resolves — internal scope's old entries didn't
    // get lost in the refresh.
    if (!hover_at(c, uri, 9002, 0, 0, hov, sizeof hov))
        die("test 9: hover on `first` missing after adding `second`");
    if (!contains(hov, "i32"))
        die("test 9: hover on `first` wrong post-add: %s", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_add_decl_mid_session: OK\n");
}

// Test 10 — hover on the value-expression subtree of a top-level
// const/var bind. Two layered behaviors are pinned down:
//
//   (a) Unannotated const (`EXIT_SUCCESS :: 0`): the RHS literal `0`
//       is comptime_int — its truthful, polymorphic identity. Hover
//       reports comptime_int.
//
//   (b) Annotated const/var (`FOO : i32 : 42`): the RHS is checked
//       bidirectionally against the annotation. The literal coerces
//       to i32; hover reports i32, not comptime_int.
//
//   (c) Multi-context: `BASE :: 5` referenced in both an i32 expression
//       and an i64 expression. Hover on BASE at the definition still
//       returns comptime_int (the def's truthful identity, which is
//       polymorphic). Hover on BASE at each use site returns the
//       contextual concrete type (i32 in one, i64 in the other).
//
// The just-shipped value_node_types per-decl range is the cache
// bidirectional coercion writes into; sema_check_expr's leaf cases
// (LIT_INT, PATH) overwrite the cache entry with the expected concrete
// type when the natural type is comptime numeric.
static void test_hover_const_value_literal(const char *bin) {
    const char *src =
        "EXIT_SUCCESS :: 0\n"
        "FOO : i32 : 42\n"
        "BASE :: 5\n"
        "foo : i32 : BASE + 10\n"
        "bar : i64 : BASE + 20\n";

    file_write("/tmp/lsp_test_t10.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t10.ore", src);
    free(wait_for_diags_for(c, "lsp_test_t10.ore", 5000));

    const char *uri = "file:///tmp/lsp_test_t10.ore";
    char hov[512];

    // (a) Unannotated: `0` at line 0, col 16. Stays comptime_int.
    if (!hover_at(c, uri, 10000, 0, 16, hov, sizeof hov))
        die("test 10a: hover on `0` in EXIT_SUCCESS :: 0 missing");
    if (!contains(hov, "comptime_int"))
        die("test 10a: hover on `0` wrong: %s (want comptime_int)", hov);

    // (b) Annotated: `42` at line 1, col 13. Coerces to i32.
    if (!hover_at(c, uri, 10001, 1, 13, hov, sizeof hov))
        die("test 10b: hover on `42` in FOO : i32 : 42 missing");
    if (!contains(hov, "i32") || contains(hov, "comptime_int"))
        die("test 10b: hover on `42` wrong: %s (want i32, not comptime_int)",
            hov);

    // (c) Multi-context: BASE used as i32 in foo, as i64 in bar.
    // Hover on BASE at its DEF (line 2, col 0) — stays comptime_int
    // (the def's polymorphic identity).
    if (!hover_at(c, uri, 10002, 2, 0, hov, sizeof hov))
        die("test 10c1: hover on BASE def missing");
    if (!contains(hov, "comptime_int"))
        die("test 10c1: hover on BASE def wrong: %s (want comptime_int)", hov);

    // Hover on BASE INSIDE foo's expression (line 3, col 12) — i32.
    if (!hover_at(c, uri, 10003, 3, 12, hov, sizeof hov))
        die("test 10c2: hover on BASE in foo (i32 ctx) missing");
    if (!contains(hov, "i32") || contains(hov, "comptime_int"))
        die("test 10c2: hover on BASE in foo wrong: %s (want i32)", hov);

    // Hover on BASE INSIDE bar's expression (line 4, col 12) — i64.
    if (!hover_at(c, uri, 10004, 4, 12, hov, sizeof hov))
        die("test 10c3: hover on BASE in bar (i64 ctx) missing");
    if (!contains(hov, "i64") || contains(hov, "comptime_int"))
        die("test 10c3: hover on BASE in bar wrong: %s (want i64)", hov);

    close_session(c);
    fprintf(stderr, "  test_hover_const_value_literal: OK\n");
}

// Test 11 — unused-decl diagnostic emission. A private top-level decl
// with zero references gets a DIAG_WARNING (LSP severity 2). A used
// decl, a `pub` exported decl, and `main` are all exempt. Tests both
// the polymorphism case (an unannotated comptime literal with no uses
// → warn) and the concrete-but-unused case (annotated, still no refs).
static void test_unused_decl_warning(const char *bin) {
    const char *src =
        "UNUSED_PRIVATE :: 0\n"               // line 0: warn
        "UNUSED_TYPED   : i32 : 0\n"          // line 1: warn (concrete but unused)
        "USED_BASE      :: 7\n"               // line 2: NO warn (used below)
        "USED_RESULT    : i32 : USED_BASE\n"  // line 3: NO warn (pub? no — but the use of USED_BASE means USED_BASE is used; USED_RESULT itself has no refs so it WILL warn — see assertion below)
        "EXPORTED       :: pub 42\n";         // line 4: NO warn (pub)

    file_write("/tmp/lsp_test_t11.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t11.ore", src);
    char *body = wait_for_diags_for(c, "lsp_test_t11.ore", 5000);
    if (!body)
        die("test 11: timeout waiting for publishDiagnostics");

    // Spot-check: expected warnings for UNUSED_PRIVATE, UNUSED_TYPED,
    // USED_RESULT (3). Suppressed for USED_BASE (referenced) and
    // EXPORTED (pub).
    if (!contains(body, "UNUSED_PRIVATE is declared but never used"))
        die("test 11: missing warning for UNUSED_PRIVATE: %s", body);
    if (!contains(body, "UNUSED_TYPED is declared but never used"))
        die("test 11: missing warning for UNUSED_TYPED: %s", body);
    if (!contains(body, "USED_RESULT is declared but never used"))
        die("test 11: missing warning for USED_RESULT: %s", body);
    if (contains(body, "USED_BASE is declared but never used"))
        die("test 11: false unused warning for USED_BASE: %s", body);
    if (contains(body, "EXPORTED is declared but never used"))
        die("test 11: false unused warning for pub EXPORTED: %s", body);
    // All "unused" diags should be severity 2 (warning, yellow).
    if (!contains(body, "\"severity\":2"))
        die("test 11: no warning-severity diag in payload: %s", body);
    // N2 — unused-decl diags carry DiagnosticTag.Unnecessary (=1) so
    // editors render the identifier faded.
    if (!contains(body, "\"tags\":[1]"))
        die("test 11: unused-decl diags missing tags=[1] (Unnecessary): %s",
            body);

    free(body);
    close_session(c);
    fprintf(stderr, "  test_unused_decl_warning: OK\n");
}

// Test 12 — sticky-squiggle regression. Pins down the architectural fix
// for Bug A: diagnostics anchor to (file, AstNodeId) instead of frozen
// byte offsets, and the byte range is looked up at LSP publish time via
// db_get_node_span. Pre-fix, inserting blank lines above an error would
// leave the squiggle at its OLD byte position. Post-fix, the squiggle
// moves with the offending code.
static void test_sticky_squiggle_resilient_to_text_shift(const char *bin) {
    // Single error: `foo :: bar` on line 0 — `bar` is undefined.
    const char *src1 = "foo :: bar\n";
    // After insert: same error, but now on line 3.
    const char *src2 = "\n\n\nfoo :: bar\n";

    file_write("/tmp/lsp_test_t12.ore", src1);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t12.ore", src1);
    char *body1 = wait_for_diags_for(c, "lsp_test_t12.ore", 5000);
    if (!body1)
        die("test 12: timeout waiting for initial publishDiagnostics");

    // Pre-edit: the diag's range should be at line 0.
    if (!contains(body1, "\"line\":0"))
        die("test 12: initial diag not at line 0: %s", body1);
    free(body1);

    // Insert 3 blank lines at the top. Same identifier `bar` is now on
    // line 3. Pre-fix, the cached diag would still point at line 0
    // (stale byte offset). Post-fix, it shifts to line 3.
    send_did_change(c, "file:///tmp/lsp_test_t12.ore", 2, src2);
    char *body2 = wait_for_diags_for(c, "lsp_test_t12.ore", 5000);
    if (!body2)
        die("test 12: timeout waiting for post-edit publishDiagnostics");

    if (!contains(body2, "\"line\":3"))
        die("test 12: diag did not shift to line 3 after insert: %s", body2);
    if (contains(body2, "\"line\":0,\"character\":0"))
        die("test 12: diag still at (line 0, col 0) — sticky squiggle "
            "regression: %s", body2);
    free(body2);

    close_session(c);
    fprintf(stderr, "  test_sticky_squiggle_resilient_to_text_shift: OK\n");
}

// Test 13 — unused-warning persistence across whitespace edits. Pins
// down the architectural fix for Bug B: the unused walk is no longer a
// synthetic salsa query, so it doesn't suffer the cache-staleness
// pathology (zero-dep query at DUR_LOW returning CACHED). Each
// sema_check_module call clears the prior unused-diags and re-emits
// fresh ones with stable AstSpan anchors.
static void test_unused_warning_survives_whitespace_edit(const char *bin) {
    // Single unused decl, plus a used one to make sure the test setup
    // is producing the warning to begin with.
    const char *src1 =
        "UNUSED_DECL :: 0\n"
        "USED_BASE   :: 7\n"
        "result      : i32 : USED_BASE\n";
    // Same decls; inserted a blank line in the middle. UNUSED_DECL is
    // still unused; warning should persist.
    const char *src2 =
        "UNUSED_DECL :: 0\n"
        "\n"
        "USED_BASE   :: 7\n"
        "result      : i32 : USED_BASE\n";

    file_write("/tmp/lsp_test_t13.ore", src1);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_test_t13.ore", src1);
    char *body1 = wait_for_diags_for(c, "lsp_test_t13.ore", 5000);
    if (!body1)
        die("test 13: timeout waiting for initial publishDiagnostics");
    if (!contains(body1, "UNUSED_DECL is declared but never used"))
        die("test 13: initial unused warning missing: %s", body1);
    free(body1);

    // Insert a blank line. Pre-fix: QUERY_UNUSED_WARNINGS slot stays
    // CACHED (zero deps), body short-circuits, no re-emit. Combined
    // with stale byte offsets, the client effectively loses the
    // warning. Post-fix: the walk runs every sema_check_module,
    // clears its diag unit, and re-emits with stable anchors.
    send_did_change(c, "file:///tmp/lsp_test_t13.ore", 2, src2);
    char *body2 = wait_for_diags_for(c, "lsp_test_t13.ore", 5000);
    if (!body2)
        die("test 13: timeout waiting for post-edit publishDiagnostics");
    if (!contains(body2, "UNUSED_DECL is declared but never used"))
        die("test 13: unused warning vanished after whitespace edit "
            "(Bug B regression): %s", body2);
    free(body2);

    close_session(c);
    fprintf(stderr, "  test_unused_warning_survives_whitespace_edit: OK\n");
}

// Request textDocument/definition at (line0, char0) for `uri` and parse
// the response. On success, fills *out_uri with response.result.uri and
// *out_line with response.result.range.start.line. Returns 1 on hit, 0
// on null/miss. Mirrors hover_at's permissive string-scanning style so
// we don't need cJSON in the test harness.
static int definition_at(LspClient *c, const char *uri, int req_id,
                         int line0, int char0,
                         char *out_uri, size_t uri_cap, int *out_line) {
    out_uri[0] = '\0';
    *out_line = -1;
    char msg[512];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/definition\","
             "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
             "\"position\":{\"line\":%d,\"character\":%d}}}",
             req_id, uri, line0, char0);
    lsp_send(c, msg);

    char id_needle[32];
    snprintf(id_needle, sizeof id_needle, "\"id\":%d", req_id);

    for (int i = 0; i < 20; i++) {
        char *body = lsp_recv_message(c, 5000);
        if (!body) return 0;
        if (!contains(body, id_needle)) { free(body); continue; }
        if (contains(body, "\"result\":null")) { free(body); return 0; }
        // Extract uri.
        const char *u = strstr(body, "\"uri\":\"");
        if (!u) { free(body); return 0; }
        u += strlen("\"uri\":\"");
        size_t j = 0;
        while (*u && *u != '"' && j + 1 < uri_cap) {
            if (*u == '\\' && u[1]) { out_uri[j++] = u[1]; u += 2; continue; }
            out_uri[j++] = *u++;
        }
        out_uri[j] = '\0';
        // Extract range.start.line.
        const char *r = strstr(body, "\"range\":");
        const char *s = r ? strstr(r, "\"start\":") : NULL;
        const char *l = s ? strstr(s, "\"line\":") : NULL;
        if (l) *out_line = atoi(l + strlen("\"line\":"));
        free(body);
        return 1;
    }
    return 0;
}

// Test — go-to-definition for a TOP-LEVEL reference within the same file.
static void test_definition_top_level(const char *bin) {
    // L0: x :: 7
    // L1: use :: pub fn() i32
    // L2:     x          <- cursor on this `x`, col 4
    const char *src =
        "x :: 7\n"
        "use :: pub fn() i32\n"
        "    x\n";
    file_write("/tmp/lsp_def_top.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_def_top.ore", src);

    char uri[256]; int line = -1;
    if (!definition_at(c, "file:///tmp/lsp_def_top.ore", 200, 2, 4,
                       uri, sizeof uri, &line))
        die("test definition top-level: no result for `x` use-site");
    if (!contains(uri, "lsp_def_top.ore"))
        die("test definition top-level: wrong file: %s", uri);
    if (line != 0)
        die("test definition top-level: expected line 0 (decl), got %d", line);

    close_session(c);
    fprintf(stderr, "  test_definition_top_level: OK\n");
}

// Test — go-to-definition for a BODY-LOCAL reference (a fn param).
// Also covers shadowing: a top-level `p :: 99` exists above the fn,
// but the local must win since it's in body scope.
static void test_definition_local(const char *bin) {
    // L0: p :: 99
    // L1: use :: pub fn(p: i32) i32
    // L2:     p              <- cursor on this `p`, col 4
    const char *src =
        "p :: 99\n"
        "use :: pub fn(p: i32) i32\n"
        "    p\n";
    file_write("/tmp/lsp_def_loc.ore", src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_def_loc.ore", src);

    char uri[256]; int line = -1;
    if (!definition_at(c, "file:///tmp/lsp_def_loc.ore", 201, 2, 4,
                       uri, sizeof uri, &line))
        die("test definition local: no result for `p` use-site");
    if (!contains(uri, "lsp_def_loc.ore"))
        die("test definition local: wrong file: %s", uri);
    // Local `p` is the param on line 1; top-level `p :: 99` is line 0.
    // The body-first lookup must pick line 1, not line 0.
    if (line != 1)
        die("test definition local: expected line 1 (param, local wins over "
            "top-level), got %d", line);

    close_session(c);
    fprintf(stderr, "  test_definition_local: OK\n");
}

// Test — go-to-definition for CROSS-NAMESPACE member access through
// @import. Also exercises the D3.3a lazy-load fix: only def_a.ore is
// opened explicitly; def_dep.ore is admitted by the resolver inside
// the typecheck request. Without the fix the server would abort here.
static void test_definition_cross_namespace(const char *bin) {
    mkdir("/tmp/lsp_def_xn", 0755);
    const char *dep_src = "x :: pub 7\n";
    // L0: B :: pub @import("./def_dep.ore")
    // L1: use :: pub fn() i32
    // L2:     B.x         <- cursor on `x` at col 6
    const char *a_src =
        "B :: pub @import(\"./def_dep.ore\")\n"
        "use :: pub fn() i32\n"
        "    B.x\n";
    file_write("/tmp/lsp_def_xn/def_dep.ore", dep_src);
    file_write("/tmp/lsp_def_xn/def_a.ore", a_src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    // Open ONLY a — b is lazy-loaded by @import resolution.
    send_did_open(c, "file:///tmp/lsp_def_xn/def_a.ore", a_src);

    char uri[256]; int line = -1;
    if (!definition_at(c, "file:///tmp/lsp_def_xn/def_a.ore", 202, 2, 6,
                       uri, sizeof uri, &line))
        die("test definition cross-namespace: no result for `B.x`");
    if (!contains(uri, "def_dep.ore"))
        die("test definition cross-namespace: expected def_dep.ore, got %s",
            uri);
    if (line != 0)
        die("test definition cross-namespace: expected line 0, got %d", line);

    close_session(c);
    fprintf(stderr, "  test_definition_cross_namespace: OK\n");
}

// Extract a JSON string value for `"key":"..."` from `body`. Returns
// malloc'd copy on success, NULL if absent. Tolerant of escaped chars
// only by skipping them — adequate for the resultId hex strings.
static char *find_json_string(const char *body, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(body, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *end = p;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) end++;
        end++;
    }
    if (!*end) return NULL;
    size_t n = (size_t)(end - p);
    char *s = (char *)malloc(n + 1);
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

// M2.2 — pull diagnostics initial fetch. Initialize with pull capability
// so no publishDiagnostics fires; then poll textDocument/diagnostic and
// expect a "full" report with the file's diagnostics.
static void test_pull_diagnostic_initial(const char *bin) {
    const char *src =
        "main :: pub fn() i32\n    badname.foo\n    return 0\n";
    file_write("/tmp/lsp_pull_t1.ore", src);
    LspClient *c = lsp_start(bin);
    init_session_with_pull(c);
    send_did_open(c, "file:///tmp/lsp_pull_t1.ore", src);

    // Server should NOT push diagnostics in pull mode. Drain any
    // unexpected messages with a short timeout; bail if we see one.
    char *body = lsp_recv_message(c, 300);
    if (body) {
        if (contains(body, "publishDiagnostics"))
            die("test pull initial: server pushed publishDiagnostics in "
                "pull mode (push suppression broken)");
        free(body);
    }

    // Send textDocument/diagnostic request.
    char msg[1024];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"textDocument/diagnostic\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/lsp_pull_t1.ore\"}}}");
    lsp_send(c, msg);

    body = lsp_recv_message(c, 5000);
    if (!body) die("test pull initial: timeout waiting for response");
    if (!contains(body, "\"kind\":\"full\""))
        die("test pull initial: expected kind=full, got: %s", body);
    if (!contains(body, "\"resultId\":\""))
        die("test pull initial: missing resultId");
    if (count_diag_entries(body) < 1)
        die("test pull initial: expected diag items, got 0");
    free(body);

    close_session(c);
    fprintf(stderr, "  test_pull_diagnostic_initial: OK\n");
}

// M2.2 — same file, second pull with previousResultId echoed back.
// Server returns "unchanged" without sending the items array.
static void test_pull_diagnostic_unchanged(const char *bin) {
    const char *src =
        "main :: pub fn() i32\n    badname.foo\n    return 0\n";
    file_write("/tmp/lsp_pull_t2.ore", src);
    LspClient *c = lsp_start(bin);
    init_session_with_pull(c);
    send_did_open(c, "file:///tmp/lsp_pull_t2.ore", src);
    (void)lsp_recv_message(c, 200); // drain anything stray

    // First pull → grab the resultId.
    char msg[1024];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/diagnostic\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/lsp_pull_t2.ore\"}}}");
    lsp_send(c, msg);
    char *body = lsp_recv_message(c, 5000);
    if (!body) die("test pull unchanged: no first response");
    char *rid = find_json_string(body, "resultId");
    if (!rid) die("test pull unchanged: no resultId in first response");
    free(body);

    // Second pull with previousResultId = rid → "unchanged".
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/diagnostic\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/lsp_pull_t2.ore\"},"
             "\"previousResultId\":\"%s\"}}", rid);
    lsp_send(c, msg);
    body = lsp_recv_message(c, 5000);
    if (!body) die("test pull unchanged: no second response");
    if (!contains(body, "\"kind\":\"unchanged\""))
        die("test pull unchanged: expected kind=unchanged, got: %s", body);
    free(body);
    free(rid);

    close_session(c);
    fprintf(stderr, "  test_pull_diagnostic_unchanged: OK\n");
}

// M2.3 — workspace/diagnostic returns reports for every open file.
static void test_workspace_diagnostic(const char *bin) {
    mkdir("/tmp/lsp_pull_ws", 0755);
    const char *a_src = "x :: pub 1\n";
    const char *b_src =
        "main :: pub fn() i32\n    badname.foo\n    return 0\n";
    file_write("/tmp/lsp_pull_ws/a.ore", a_src);
    file_write("/tmp/lsp_pull_ws/b.ore", b_src);

    LspClient *c = lsp_start(bin);
    init_session_with_pull(c);
    send_did_open(c, "file:///tmp/lsp_pull_ws/a.ore", a_src);
    send_did_open(c, "file:///tmp/lsp_pull_ws/b.ore", b_src);
    (void)lsp_recv_message(c, 200);

    char msg[1024];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"workspace/diagnostic\","
             "\"params\":{\"previousResultIds\":[]}}");
    lsp_send(c, msg);
    char *body = lsp_recv_message(c, 5000);
    if (!body) die("test workspace pull: no response");
    if (!contains(body, "a.ore") || !contains(body, "b.ore"))
        die("test workspace pull: expected both files in report, got: %s",
            body);
    if (!contains(body, "\"kind\":\"full\""))
        die("test workspace pull: expected at least one full report");
    free(body);

    close_session(c);
    fprintf(stderr, "  test_workspace_diagnostic: OK\n");
}

// N1 — server-initiated workspace/diagnostic/refresh nudges pull
// clients after input mutations. Open a file, edit it, expect the
// server to send a workspace/diagnostic/refresh request within a
// short window. Without N1 the client would have to poll on its
// own schedule.
static void test_pull_refresh_after_edit(const char *bin) {
    const char *src = "x :: pub 1\n";
    file_write("/tmp/lsp_pull_refresh.ore", src);
    LspClient *c = lsp_start(bin);
    init_session_with_pull_and_refresh(c);
    send_did_open(c, "file:///tmp/lsp_pull_refresh.ore", src);

    // Drain refresh from didOpen (republish_all_open fires it).
    for (int i = 0; i < 3; i++) {
        char *body = lsp_recv_message(c, 300);
        if (!body) break;
        free(body);
    }

    // Edit triggers a fresh refresh.
    const char *src2 = "x :: pub 2\n";
    send_did_change(c, "file:///tmp/lsp_pull_refresh.ore", 2, src2);

    bool got_refresh = false;
    for (int i = 0; i < 5 && !got_refresh; i++) {
        char *body = lsp_recv_message(c, 1000);
        if (!body) break;
        if (contains(body, "workspace/diagnostic/refresh"))
            got_refresh = true;
        if (contains(body, "publishDiagnostics"))
            die("test pull refresh: server pushed publishDiagnostics in "
                "pull mode (M2.4 suppression broken)");
        free(body);
    }
    if (!got_refresh)
        die("test pull refresh: never received workspace/diagnostic/refresh "
            "after didChange");

    close_session(c);
    fprintf(stderr, "  test_pull_refresh_after_edit: OK\n");
}

// N3 — workspace/diagnostic with partialResultToken streams per-file
// reports via $/progress notifications; the final response is empty.
static void test_workspace_diagnostic_streaming(const char *bin) {
    mkdir("/tmp/lsp_pull_ws_stream", 0755);
    const char *a_src = "x :: pub 1\n";
    const char *b_src =
        "main :: pub fn() i32\n    badname.foo\n    return 0\n";
    file_write("/tmp/lsp_pull_ws_stream/a.ore", a_src);
    file_write("/tmp/lsp_pull_ws_stream/b.ore", b_src);

    LspClient *c = lsp_start(bin);
    init_session_with_pull(c);
    send_did_open(c, "file:///tmp/lsp_pull_ws_stream/a.ore", a_src);
    send_did_open(c, "file:///tmp/lsp_pull_ws_stream/b.ore", b_src);
    (void)lsp_recv_message(c, 200);

    // Workspace pull WITH a progress token. Server streams each per-file
    // report via $/progress; final response has empty items.
    char msg[1024];
    snprintf(msg, sizeof msg,
             "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"workspace/diagnostic\","
             "\"params\":{\"previousResultIds\":[],"
             "\"partialResultToken\":\"ws-tok-1\"}}");
    lsp_send(c, msg);

    int progress_count = 0;
    bool got_a = false, got_b = false;
    char *final_resp = NULL;
    for (int i = 0; i < 10 && !final_resp; i++) {
        char *body = lsp_recv_message(c, 2000);
        if (!body) break;
        if (contains(body, "\"method\":\"$/progress\"")) {
            progress_count++;
            if (!contains(body, "ws-tok-1"))
                die("test workspace streaming: progress missing token: %s",
                    body);
            if (contains(body, "a.ore")) got_a = true;
            if (contains(body, "b.ore")) got_b = true;
            free(body);
            continue;
        }
        // The numeric id 11 marks our response.
        if (contains(body, "\"id\":11"))
            final_resp = body;
        else
            free(body);
    }
    if (!final_resp)
        die("test workspace streaming: no final response");
    if (progress_count < 2)
        die("test workspace streaming: expected >=2 $/progress, got %d",
            progress_count);
    if (!got_a || !got_b)
        die("test workspace streaming: missing per-file report (a=%d b=%d)",
            got_a, got_b);
    // Final response items array should be empty in streaming mode.
    if (!contains(final_resp, "\"items\":[]"))
        die("test workspace streaming: final items not empty: %s", final_resp);
    free(final_resp);

    close_session(c);
    fprintf(stderr, "  test_workspace_diagnostic_streaming: OK\n");
}

// O — sticky-squiggle suppression. While the user types and the
// current parse is broken, the server should NOT republish
// diagnostics (the diags' anchors carry stale byte ranges from cached
// slots; publishing them would overwrite the editor's locally-tracked
// squiggle positions with wrong ones). When the parse recovers, the
// server publishes again with fresh positions.
static void test_sticky_squiggle_during_typing(const char *bin) {
    // Stable unused-decl warning at line 0. The decl will shift down
    // as we prepend partial-parse junk above it; the editor handles
    // the shift locally if we don't republish wrong positions.
    const char *clean_src = "UNUSED :: 0\n";
    file_write("/tmp/lsp_sticky.ore", clean_src);

    LspClient *c = lsp_start(bin);
    init_session(c);
    send_did_open(c, "file:///tmp/lsp_sticky.ore", clean_src);

    // Drain the initial publishDiagnostics for the unused-decl warning.
    char *body = wait_for_diags_for(c, "lsp_sticky.ore", 5000);
    if (!body) die("test sticky: no initial publish");
    if (count_diag_entries(body) < 1)
        die("test sticky: initial publish has no diag, got: %s", body);
    free(body);

    // Inject partial-parse text. Each successive didChange prepends a
    // longer prefix that's unambiguously syntactically broken
    // (unbalanced `(`, dangling `::`, etc.). The unused-decl on the
    // next line shifts visually but the server should suppress
    // republish because QUERY_FILE_AST emits parse-error diags.
    const char *partials[] = {
        "(\nUNUSED :: 0\n",     // unbalanced open-paren
        "((\nUNUSED :: 0\n",    // two unbalanced
        "(((\nUNUSED :: 0\n",   // three
        "((((\nUNUSED :: 0\n",  // four
    };
    int n_partials = (int)(sizeof(partials) / sizeof(partials[0]));

    int unwanted_publishes = 0;
    for (int i = 0; i < n_partials; i++) {
        send_did_change(c, "file:///tmp/lsp_sticky.ore", 2 + i, partials[i]);
        // Drain anything that arrives in a short window — should be
        // empty per O3's gate. (Non-publish messages — e.g. progress
        // tokens — would also count, but we don't expect any in push
        // mode on a simple session.)
        for (;;) {
            char *m = lsp_recv_message(c, 200);
            if (!m) break;
            if (contains(m, "publishDiagnostics"))
                unwanted_publishes++;
            free(m);
        }
    }
    if (unwanted_publishes > 0)
        die("test sticky: server published %d times during partial parse "
            "(O3 gate failed)", unwanted_publishes);

    // Final didChange completes the syntax. Parse recovers; the server
    // should now publish — possibly with a shifted line number for the
    // unused-decl.
    const char *recovered = "Z :: 42\nUNUSED :: 0\n";
    send_did_change(c, "file:///tmp/lsp_sticky.ore", 2 + n_partials,
                    recovered);

    body = wait_for_diags_for(c, "lsp_sticky.ore", 5000);
    if (!body)
        die("test sticky: no publish after parse recovery");
    // Z :: 42 introduces ANOTHER unused decl. Either way ≥1 diag.
    if (count_diag_entries(body) < 1)
        die("test sticky: post-recovery publish has no diags: %s", body);
    free(body);

    close_session(c);
    fprintf(stderr, "  test_sticky_squiggle_during_typing: OK\n");
}

int main(int argc, char **argv) {
    const char *bin = argc > 1 ? argv[1] : "./ore";
    fprintf(stderr, "lsp_test: using binary %s\n", bin);
    test_did_open_publishes_diag(bin);
    test_cross_file_invalidation(bin);
    test_hover_no_sigabrt(bin);
    test_hover_correctness(bin);
    test_hover_resilient_to_sibling_edits(bin);
    test_hover_self_referential_struct(bin);
    test_hover_mutual_struct(bin);
    test_hover_compaction_stress(bin);
    test_hover_add_decl_mid_session(bin);
    test_hover_const_value_literal(bin);
    test_unused_decl_warning(bin);
    test_sticky_squiggle_resilient_to_text_shift(bin);
    test_unused_warning_survives_whitespace_edit(bin);
    test_definition_top_level(bin);
    test_definition_local(bin);
    test_definition_cross_namespace(bin);
    test_pull_diagnostic_initial(bin);
    test_pull_diagnostic_unchanged(bin);
    test_workspace_diagnostic(bin);
    test_pull_refresh_after_edit(bin);
    test_workspace_diagnostic_streaming(bin);
    test_sticky_squiggle_during_typing(bin);
    fprintf(stderr, "lsp_test: all PASS\n");
    return 0;
}
