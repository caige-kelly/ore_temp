// QUERY_FILE_IMPORTS — walk a file's green tree for @import refs. Pure.

#include "file_imports.h"

#include "../../parser/syntax_kind.h"
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax.h"
#include "../db.h"
#include "ast.h"
#include "query.h"
#include "query_engine.h"

#include <string.h>

// Visitor state for the green-tree walk.
typedef struct {
  struct db   *s;
  StrId        import_name;     // pre-interned "import" StrId
  ImportRef   *out;             // NULL on pass 1 (count only)
  uint32_t     n_out;
  uint32_t     out_cap;         // pass-2 capacity
  Fingerprint  fp;
} ImportWalk;

// Returns true if `node` is an SK_BUILTIN_EXPR whose name is "@import"
// AND whose first ARG_LIST entry is SK_LITERAL_EXPR with a string
// literal token. On match, *out_path receives the StrId of the literal
// text and *out_site_ptr receives the SyntaxNodePtr of the BUILTIN.
static bool match_import_builtin(struct db *s, SyntaxNode *node,
                                 StrId import_name,
                                 StrId *out_path,
                                 SyntaxNodePtr *out_site_ptr) {
  if (syntax_node_kind(node) != SK_BUILTIN_EXPR)
    return false;

  // Find the name IDENT token (skips trivia + the leading "@").
  uint32_t count = syntax_node_num_children(node);
  StrId nm = (StrId){0};
  uint32_t arg_list_idx = UINT32_MAX;
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(syntax_node_green(node), i);
    if (g.kind == GREEN_ELEM_TOKEN) {
      SyntaxKind tk = green_token_kind(g.token);
      if (tk == SK_IDENT && nm.idx == 0) {
        const char *txt = green_token_text(g.token);
        uint32_t len = green_token_text_len(g.token);
        nm = pool_lookup(&s->strings, txt, len);
      }
    } else if (g.kind == GREEN_ELEM_NODE &&
               green_node_kind(g.node) == SK_ARG_LIST) {
      arg_list_idx = i;
    }
  }
  if (nm.idx != import_name.idx || arg_list_idx == UINT32_MAX)
    return false;

  // Look into ARG_LIST for the first SK_LITERAL_EXPR with a string
  // literal token child.
  SyntaxNode *arg_list = syntax_node_child(node, arg_list_idx);
  if (!arg_list)
    return false;
  bool found = false;
  uint32_t arg_count = syntax_node_num_children(arg_list);
  for (uint32_t i = 0; i < arg_count && !found; i++) {
    GreenElement g = green_node_child(syntax_node_green(arg_list), i);
    if (g.kind != GREEN_ELEM_NODE)
      continue;
    if (green_node_kind(g.node) != SK_LITERAL_EXPR)
      break; // first arg isn't a literal — bail
    // Look for the string-literal token inside.
    uint32_t lit_count = green_node_num_children(g.node);
    for (uint32_t j = 0; j < lit_count; j++) {
      GreenElement gg = green_node_child(g.node, j);
      if (gg.kind != GREEN_ELEM_TOKEN)
        continue;
      if (green_token_kind(gg.token) == SK_STRING_LIT) {
        const char *txt = green_token_text(gg.token);
        uint32_t len = green_token_text_len(gg.token);
        // Strip surrounding quotes if present.
        if (len >= 2 && txt[0] == '"' && txt[len - 1] == '"') {
          *out_path = pool_intern(&s->strings, txt + 1, len - 2);
        } else {
          *out_path = pool_intern(&s->strings, txt, len);
        }
        *out_site_ptr = syntax_node_ptr_new(node);
        found = true;
        break;
      }
    }
    break; // only consider first arg
  }
  syntax_node_release(arg_list);
  return found;
}

// Recursive walk. Each node visited; on match, either count or emit.
static void walk(ImportWalk *w, SyntaxNode *node) {
  StrId path = {0};
  SyntaxNodePtr site = {0};
  if (match_import_builtin(w->s, node, w->import_name, &path, &site)) {
    if (w->out && w->n_out < w->out_cap) {
      w->out[w->n_out].path = path;
      w->out[w->n_out].site_ptr = site;
      w->fp = db_fp_combine(w->fp, db_fp_u64((uint64_t)path.idx));
    }
    w->n_out++;
  }

  uint32_t count = syntax_node_num_children(node);
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(syntax_node_green(node), i);
    if (g.kind != GREEN_ELEM_NODE)
      continue;
    SyntaxNode *child = syntax_node_child(node, i);
    if (child) {
      walk(w, child);
      syntax_node_release(child);
    }
  }
}

FileArray *db_query_file_imports(struct db *s, FileId fid) {
  if (fid.idx == FILE_ID_NONE.idx)
    return NULL;
  uint32_t f = file_id_local(fid);
  if (f >= s->files.imports.count)
    return NULL;
  FileArray *out = (FileArray *)vec_get(&s->files.imports, f);

  DB_QUERY_GUARD(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx, out, NULL, NULL);

  // Dep on the whole-file parse (auto-recorded). File text change →
  // file_ast recomputes → file_imports invalidates → re-walks.
  (void)db_query_file_ast(s, fid);

  if (f >= s->files.green_roots.count) {
    out->data = NULL;
    out->count = 0;
    db_query_succeed(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx,
                     FINGERPRINT_NONE);
    return out;
  }
  GreenNode *groot = *(GreenNode **)vec_get(&s->files.green_roots, f);
  if (!groot) {
    out->data = NULL;
    out->count = 0;
    db_query_succeed(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx,
                     FINGERPRINT_NONE);
    return out;
  }

  SyntaxTree *tree = syntax_tree_new(groot);
  SyntaxNode *root_red = syntax_tree_root(tree);

  ImportWalk w = {
      .s = s, .import_name = s->names.IMPORT,
      .out = NULL, .n_out = 0, .out_cap = 0,
      .fp = FINGERPRINT_NONE,
  };

  // Pass 1: count.
  walk(&w, root_red);
  uint32_t count = w.n_out;

  // Allocate in the file's arena; pointer is valid until the next
  // QUERY_FILE_AST reparse (which resets this arena).
  Arena *fa = (Arena *)vec_get(&s->files.arenas, f);
  ImportRef *refs = NULL;
  if (count > 0)
    refs = (ImportRef *)arena_alloc_raw(fa, (size_t)count * sizeof(ImportRef));

  // Pass 2: collect.
  w.out = refs;
  w.n_out = 0;
  w.out_cap = count;
  w.fp = (Fingerprint)count;
  walk(&w, root_red);

  syntax_node_release(root_red);
  syntax_tree_free(tree);

  out->data = refs;
  out->count = count;
  db_query_succeed(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx, w.fp);
  return out;
}
