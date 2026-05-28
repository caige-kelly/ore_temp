// Pre-Phase-C foundation hygiene (A8) — db_format_type recursion bound.
//
// db_format_type recurses through compound types, allocating stack
// buffers (inner / inner_p / inner_r) at each level. Before the fix it
// had no depth limit, so a pathologically nested type (?^?^...i32, or a
// generated deeply-nested generic) would overflow the stack during the
// most visible operation: error-diagnostic rendering. The fix mirrors
// ip_format's IP_FORMAT_MAX_DEPTH=16 — past the bound, render "...".
//
// This test builds a 2000-deep optional chain via ip_get and formats it.
// With the cap: formatting recurses only ~16 levels regardless of input
// depth, so it returns without a stack overflow AND the output carries
// the "..." truncation marker. Remove the cap and the marker vanishes
// (and deep-enough input crashes) — so the strstr assertion is a real
// regression guard.

#include "../src/db/db.h"
#include "../src/db/intern_pool/intern_pool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NEST_DEPTH 2000

int main(void) {
    struct db s;
    db_init(&s);

    // Build ?(?(?(...i32))) — NEST_DEPTH optionals deep. Each level wraps
    // a distinct elem, so no structural dedup collapses the chain.
    IpIndex t = IP_I32_TYPE;
    for (int i = 0; i < NEST_DEPTH; i++) {
        IpKey k = {.kind = IPK_OPTIONAL_TYPE, .optional_type = {.elem = t}};
        t = ip_get(&s.intern, k);
        assert(t.v != IP_NONE.v && "ip_get returned IP_NONE building nest");
    }

    char buf[512];
    size_t n = db_format_type(&s, t, buf, sizeof buf);

    // Returned (no stack overflow) and truncated at the depth bound.
    assert(n > 0 && "db_format_type produced empty output");
    assert(strstr(buf, "...") != NULL &&
           "depth cap did not fire — '...' truncation marker missing "
           "(regression: db_format_type recursion is unbounded again)");

    db_free(&s);

    printf("PASS format_type_depth: %d-deep nested type rendered without "
           "overflow, capped with '...' marker\n", NEST_DEPTH);
    return 0;
}
