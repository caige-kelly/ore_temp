#include "compiler/compiler.h"
#include "common/vec.h"
#include "hir/dump.h"
#include "parser/parser.h"
#include "project/module_loader.h"
#include "sema/ids/ids.h"
#include "sema/modules/def_map.h"
#include "sema/modules/inputs.h"
#include "sema/modules/modules.h"
#include "sema/query/query.h"
#include "sema/resolve/resolve.h"
#include "sema/resolve/scope_index.h"
#include "sema/scope/scope.h"
#include "sema/sema.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s [options] <filename>\n"
          "\n"
          "Options:\n"
          "  --dump-ast      print parsed AST\n"
          "  --dump-resolve  print top-level def map (new lazy layer)\n"
          "  --dump-lex      print normalized lexer output\n"
          "  --quiet         suppress non-diagnostic status lines\n"
          "  --no-color      disable ANSI color in diagnostics\n"
          "  --help          show this help\n",
          program);
}

static bool parse_options(int argc, char **argv, struct CompilerOptions *opts) {
  *opts = (struct CompilerOptions){.use_color = true};

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--dump-ast") == 0) {
      opts->dump_ast = true;
    } else if (strcmp(arg, "--dump-resolve") == 0) {
      opts->dump_resolve = true;
    } else if (strcmp(arg, "--quiet") == 0) {
      opts->quiet = true;
    } else if (strcmp(arg, "--dump-lex") == 0) {
      opts->dump_lex = true;
    } else if (strcmp(arg, "--dump-raw") == 0) {
      opts->dump_raw = true;
    } else if (strcmp(arg, "--no-color") == 0) {
      opts->use_color = false;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout, argv[0]);
      opts->help = true;
      return true;
    } else if (arg[0] == '-') {
      fprintf(stderr, "unknown option: %s\n", arg);
      print_usage(stderr, argv[0]);
      return false;
    } else if (!opts->input_path) {
      opts->input_path = arg;
    } else {
      fprintf(stderr, "unexpected extra input: %s\n", arg);
      print_usage(stderr, argv[0]);
      return false;
    }
  }

  if (!opts->input_path) {
    print_usage(stderr, argv[0]);
    return false;
  }
  return true;
}

// Read a file in full into a malloc'd buffer (caller frees). Returns
// NULL and prints an error on failure.
static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "could not open %s\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) { free(buf); return NULL; }
  buf[sz] = '\0';
  if (out_len) *out_len = (size_t)sz;
  return buf;
}

// Minimal Sema bring-up on top of a Compiler. Mirrors the OreDb shell
// in src/sema/lsp/lsp_abi.c, but without the FFI wrapper. Once both
// drivers stabilize, lift this into a shared helper.
static void sema_driver_init(struct Sema *s, struct Compiler *c) {
  *s = (struct Sema){
      .compiler = c,
      .arena = &c->arena,
      .pool = &c->pool,
      .diags = &c->diags,
      .slot_budget = 50000,
  };
  s->query_stack = vec_new_in(&c->arena, sizeof(struct QueryFrame));
  hashmap_init_in(&s->module_by_path, &c->arena);
  sema_ids_init(s);
  sema_inputs_init(s);
  prelude_init(s);
}

// Recursive Ident walker — for every expr_Ident in the tree, runs
// query_resolve_ref and prints the outcome. Type-position children get
// NS_TYPE; everything else NS_VALUE. Effect-position resolution is left
// for a follow-up — the layer doesn't yet know which Idents sit there.
static void walk_resolve(struct Sema *s, struct Expr *e, Namespace ns,
                         int depth);

static void walk_resolve_vec(struct Sema *s, Vec *v, Namespace ns, int depth) {
  if (!v) return;
  for (size_t i = 0; i < v->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(v, i);
    if (slot) walk_resolve(s, *slot, ns, depth);
  }
}

static const char *ns_name(Namespace ns) {
  switch (ns) {
  case NS_VALUE: return "val";
  case NS_TYPE:  return "type";
  case NS_EFFECT: return "eff";
  case NS_OP:    return "op";
  }
  return "?";
}

static void walk_resolve(struct Sema *s, struct Expr *e, Namespace ns,
                         int depth) {
  if (!e) return;

  switch (e->kind) {
  case expr_Ident: {
    DefId def = query_resolve_ref(s, e, ns);
    const char *name = pool_get(s->pool, e->ident.string_id, 0);
    printf("  %*s%s/%s -> def=%u\n", depth * 2, "",
           name ? name : "?", ns_name(ns), def.idx);
    return;
  }
  case expr_Bind:
    if (e->bind.type_ann) walk_resolve(s, e->bind.type_ann, NS_TYPE, depth);
    if (e->bind.value)    walk_resolve(s, e->bind.value, NS_VALUE, depth);
    return;
  case expr_Bin:
    walk_resolve(s, e->bin.Left, ns, depth);
    walk_resolve(s, e->bin.Right, ns, depth);
    return;
  case expr_Assign:
    walk_resolve(s, e->assign.target, NS_VALUE, depth);
    walk_resolve(s, e->assign.value, NS_VALUE, depth);
    return;
  case expr_Unary:
    walk_resolve(s, e->unary.operand, ns, depth);
    return;
  case expr_Call:
    walk_resolve(s, e->call.callee, NS_VALUE, depth);
    walk_resolve_vec(s, e->call.args, NS_VALUE, depth);
    return;
  case expr_Builtin:
    walk_resolve_vec(s, e->builtin.args, NS_VALUE, depth);
    return;
  case expr_If:
    walk_resolve(s, e->if_expr.condition, NS_VALUE, depth);
    walk_resolve(s, e->if_expr.then_branch, NS_VALUE, depth);
    walk_resolve(s, e->if_expr.else_branch, NS_VALUE, depth);
    return;
  case expr_Block:
    walk_resolve_vec(s, e->block.stmts, NS_VALUE, depth);
    return;
  case expr_Field:
    // Only the root object is resolved; the field name is a member
    // selector, not a free identifier — that's a path-resolve concern.
    walk_resolve(s, e->field.object, ns, depth);
    return;
  case expr_Index:
    walk_resolve(s, e->index.object, NS_VALUE, depth);
    walk_resolve(s, e->index.index, NS_VALUE, depth);
    return;
  case expr_Lambda: {
    if (e->lambda.params) {
      for (size_t i = 0; i < e->lambda.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
        if (p && p->type_ann) walk_resolve(s, p->type_ann, NS_TYPE, depth);
      }
    }
    if (e->lambda.ret_type) walk_resolve(s, e->lambda.ret_type, NS_TYPE, depth);
    if (e->lambda.body)     walk_resolve(s, e->lambda.body, NS_VALUE, depth);
    return;
  }
  case expr_Switch:
    walk_resolve(s, e->switch_expr.scrutinee, NS_VALUE, depth);
    if (e->switch_expr.arms) {
      for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
        struct SwitchArm *a = (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
        if (!a) continue;
        walk_resolve_vec(s, a->patterns, NS_VALUE, depth);
        walk_resolve(s, a->body, NS_VALUE, depth);
      }
    }
    return;
  case expr_Loop:
    walk_resolve(s, e->loop_expr.init, NS_VALUE, depth);
    walk_resolve(s, e->loop_expr.condition, NS_VALUE, depth);
    walk_resolve(s, e->loop_expr.step, NS_VALUE, depth);
    walk_resolve(s, e->loop_expr.body, NS_VALUE, depth);
    return;
  case expr_ArrayType:
    walk_resolve(s, e->array_type.size, NS_VALUE, depth);
    walk_resolve(s, e->array_type.elem, NS_TYPE, depth);
    return;
  case expr_SliceType:
    walk_resolve(s, e->slice_type.elem, NS_TYPE, depth);
    return;
  case expr_ManyPtrType:
    walk_resolve(s, e->many_ptr_type.elem, NS_TYPE, depth);
    return;
  case expr_ArrayLit:
    walk_resolve(s, e->array_lit.size, NS_VALUE, depth);
    walk_resolve(s, e->array_lit.elem_type, NS_TYPE, depth);
    walk_resolve(s, e->array_lit.initializer, NS_VALUE, depth);
    return;
  case expr_Struct:
    if (e->struct_expr.members) {
      for (size_t i = 0; i < e->struct_expr.members->count; i++) {
        struct StructMember *m =
            (struct StructMember *)vec_get(e->struct_expr.members, i);
        if (!m) continue;
        if (m->kind == member_Field && m->field.type)
          walk_resolve(s, m->field.type, NS_TYPE, depth);
      }
    }
    return;
  case expr_Product:
    if (e->product.type_expr)
      walk_resolve(s, e->product.type_expr, NS_TYPE, depth);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f && f->value) walk_resolve(s, f->value, NS_VALUE, depth);
      }
    }
    return;
  default:
    return;
  }
}

static void dump_resolve(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m) {
    printf("=== resolve === <invalid module>\n");
    return;
  }
  Vec *idx = m->top_level_index;
  printf("=== resolve === module=%u top_level=%zu\n", mid.idx,
         idx ? idx->count : 0);
  if (!idx)
    return;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e)
      continue;
    DefId def = query_def_for_name(s, mid, e->name_id);
    const char *name = pool_get(s->pool, e->name_id, 0);
    printf("[%zu] %-20s vis=%d def=%u\n", i, name ? name : "?",
           (int)e->vis, def.idx);
    if (e->node)
      walk_resolve(s, e->node, NS_VALUE, 1);
  }
}

int main(int argc, char *argv[]) {
  struct CompilerOptions opts;
  if (!parse_options(argc, argv, &opts))
    return EXIT_FAILURE;
  if (opts.help)
    return EXIT_SUCCESS;

  struct Compiler compiler;
  if (!compiler_init(&compiler, opts))
    return EXIT_FAILURE;

  size_t src_len = 0;
  char *src = slurp_file(opts.input_path, &src_len);
  if (!src) {
    compiler_free(&compiler);
    return EXIT_FAILURE;
  }

  struct Sema sema;
  sema_driver_init(&sema, &compiler);

  InputId iid = sema_register_input(&sema, opts.input_path);
  sema_set_input_source(&sema, iid, src, src_len);
  free(src);
  src = NULL;

  ModuleId mid = module_create(&sema, iid, /*is_prelude=*/false);

  if (opts.dump_ast) {
    Vec *ast = query_module_ast(&sema, mid);
    if (ast) {
      printf("=== ast (%zu top-level expressions) ===\n", ast->count);
      for (size_t i = 0; i < ast->count; i++) {
        struct Expr **e = (struct Expr **)vec_get(ast, i);
        if (e)
          print_ast(*e, &compiler.pool, 0);
      }
    }
  }

  bool ok = query_module_def_map(&sema, mid);
  if (ok)
    scope_index_build_module(&sema, mid);

  if (opts.dump_resolve)
    dump_resolve(&sema, mid);

  if (diag_has_errors(&compiler.diags))
    compiler_render_diags(&compiler, stderr);

  int rc = (ok && !diag_has_errors(&compiler.diags)) ? EXIT_SUCCESS : EXIT_FAILURE;
  compiler_free(&compiler);
  return rc;
}
