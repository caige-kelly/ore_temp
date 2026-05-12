#include "../../support/common/vec.h"
#include "../../support/diag/diag.h"
#include "../../db/db.h"

// #include "../sema/eval/dump.h"
// #include "../sema/ids/ids.h"
// #include "../sema/modules/inputs.h"
// #include "../sema/modules/modules.h"
// #include "../sema/query/query.h"
// #include "../sema/resolve/dump.h"
// #include "../sema/resolve/scope_index.h"
// #include "../sema/sema.h"
// #include "../sema/type/checker.h"
// #include "../sema/type/dump.h"
// #include "build.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>

static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "could not open %s\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    free(buf);
    return NULL;
  }
  buf[sz] = '\0';
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}

int driver_build_run(const struct CompilerOptions *opts) {
  size_t src_len = 0;
  char *src = slurp_file(opts->input_path, &src_len);
  if (!src) return EXIT_FAILURE;

  struct db db;
  db_init(&db)

  Input iid = sema_register_input(&sema, opts->input_path);
  sema_set_input_source(&sema, iid, src, scr_len);

  ModuleId mid = module_create(&sema, iid, /*is_primitives*/false);

  // Errors are pushed directly to the query slots
  sema_check_module(&sema, mid);

  struct DiagBag collected = diag_bag_new(&sema.pass_arena);
  diag_collect_all(&sema, &collected, /*file_id_filter=*/-1);

  bool has_errors = diag_has_errors(&collected);

  if (has_errors) {
    diag_render(stderr, &collected, &sema.source_map);
  }

  int rc = has_errors ? EXIT_FAILURE : EXIT_SUCCEDD;
  sema_free(&sema);
  return rc;
}
