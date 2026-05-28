// Pre-Phase-C foundation hygiene (A6) — virtual-source name collision.
//
// workspace_admit_virtual admits an in-memory source under a synthetic
// name. db_admit_virtual_source does NOT register into source_by_path
// (that map holds DISK paths only), so before the fix a repeated
// synthetic name was not collision-detected — the second admit silently
// minted a duplicate SourceId + NamespaceId. The fix adds a
// virtual_by_name index keyed by interned synthetic-name StrId.
//
// This test admits the same name twice and asserts the second admit is
// rejected (SOURCE_ID_NONE), while a distinct name still succeeds.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static SourceId admit(struct db *s, const char *name, const char *text) {
    return workspace_admit_virtual(s, name, strlen(name), text, strlen(text));
}

int main(void) {
    struct db s;
    db_init(&s);

    const char *name = "virtual://generated_a.ore";
    const char *text = "x := 1\n";

    // First admit under this name succeeds.
    SourceId a = admit(&s, name, text);
    assert(a.idx != SOURCE_ID_NONE.idx && "first virtual admit should succeed");

    // Second admit under the SAME name is rejected (A6 guard).
    SourceId dup = admit(&s, name, text);
    assert(dup.idx == SOURCE_ID_NONE.idx &&
           "duplicate synthetic name must be rejected, not silently "
           "minting a second source/namespace");

    // A DISTINCT name still admits fine.
    SourceId b = admit(&s, "virtual://generated_b.ore", text);
    assert(b.idx != SOURCE_ID_NONE.idx &&
           "distinct synthetic name should still succeed");
    assert(b.idx != a.idx && "distinct names must yield distinct sources");

    db_free(&s);

    printf("PASS virtual_collision: duplicate synthetic name rejected, "
           "distinct names admit independently\n");
    return 0;
}
