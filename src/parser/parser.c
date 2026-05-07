#include "./parser.h"
#include "../lexer/token.h"
#include "ast.h"
#include "common/arena.h"
#include "common/vec.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



// -- Print --

static void print_indent(int indent) {
  for (int i = 0; i < indent; i++)
    printf("  ");
}

void print_ast(struct Expr *expr, StringPool *pool, int indent);

// Print a HandlerExpr's payload. Caller emits its own header line.
static void print_handler_payload(struct HandlerExpr *h, StringPool *pool,
                                  int indent) {
  if (!h) {
    print_indent(indent);
    printf("NULL\n");
    return;
  }
  print_indent(indent);
  printf("sort: %s\n", h->sort == HandlerSort_Instance ? "Instance" : "Normal");
  if (h->scope == HandlerScope_Scoped) {
    print_indent(indent);
    printf("scoped\n");
  }
  if (h->override == HandlerOverride_Override) {
    print_indent(indent);
    printf("override\n");
  }
  if (h->effect) {
    print_indent(indent);
    printf("effect:\n");
    print_ast(h->effect, pool, indent + 1);
  }
  if (h->branches && h->branches->count > 0) {
    print_indent(indent);
    printf("branches:\n");
    for (size_t i = 0; i < h->branches->count; i++) {
      struct HandlerBranch **bp =
          (struct HandlerBranch **)vec_get(h->branches, i);
      struct HandlerBranch *br = bp ? *bp : NULL;
      if (!br) continue;
      print_indent(indent + 1);
      const char *kind_str = (br->sort == OpControl)    ? "ctl"
                             : (br->sort == OpFn)       ? "fn"
                             : (br->sort == OpVal)      ? "val"
                             : (br->sort == OpExcept)   ? "final ctl"
                             : (br->sort == OpControlRaw) ? "raw ctl"
                                                          : "?";
      printf("%s %s:\n", kind_str,
             br->name.string_id ? pool_get(pool, br->name.string_id, 0) : "<null>");
      if (br->pars) {
        for (size_t j = 0; j < br->pars->count; j++) {
          struct Param *par = (struct Param *)vec_get(br->pars, j);
          if (!par) continue;
          print_indent(indent + 2);
          printf("param: %s\n", pool_get(pool, par->name.string_id, 0));
        }
      }
      if (br->expr) {
        print_indent(indent + 2);
        printf("body:\n");
        print_ast(br->expr, pool, indent + 3);
      }
    }
  }
  if (h->initially_clause) {
    print_indent(indent);
    printf("initially:\n");
    print_ast(h->initially_clause, pool, indent + 1);
  }
  if (h->finally_clause) {
    print_indent(indent);
    printf("finally:\n");
    print_ast(h->finally_clause, pool, indent + 1);
  }
  if (h->return_clause) {
    print_indent(indent);
    printf("return:\n");
    print_ast(h->return_clause, pool, indent + 1);
  }
}

void print_ast(struct Expr *expr, StringPool *pool, int indent) {
  if (!expr) {
    print_indent(indent);
    printf("NULL\n");
    return;
  }

  print_indent(indent);
  if (expr->is_comptime)
    printf("[comptime] ");
  switch (expr->kind) {
  case expr_Lit:
    printf("Lit: \"%s\"\n", pool_get(pool, expr->lit.string_id, 0));
    break;
  case expr_Ident:
    printf("Ident: \"%s\"\n", pool_get(pool, expr->ident.string_id, 0));
    break;
  case expr_Wildcard:
    printf("Wildcard\n");
    break;
  case expr_SliceType:
    printf("SliceType:\n");
    print_indent(indent + 1);
    printf("elem:\n");
    print_ast(expr->slice_type.elem, pool, indent + 2);
    break;
  case expr_ManyPtrType:
    printf("ManyPtrType:\n");
    print_indent(indent + 1);
    printf("elem:\n");
    print_ast(expr->many_ptr_type.elem, pool, indent + 2);
    break;
  case expr_Bin:
    printf("Bin: %s\n", token_kind_to_str(expr->bin.op));
    print_ast(expr->bin.Left, pool, indent + 1);
    print_ast(expr->bin.Right, pool, indent + 1);
    break;
  case expr_Assign:
    printf("Assign:\n");
    print_indent(indent + 1);
    printf("target:\n");
    print_ast(expr->assign.target, pool, indent + 2);
    print_indent(indent + 1);
    printf("value:\n");
    print_ast(expr->assign.value, pool, indent + 2);
    break;
  case expr_DestructureBind:
    printf("DestructureBind (%s):\n", expr->destructure.is_const ? "::" : ":=");
    print_indent(indent + 1);
    printf("pattern:\n");
    print_ast(expr->destructure.pattern, pool, indent + 2);
    print_indent(indent + 1);
    printf("value:\n");
    print_ast(expr->destructure.value, pool, indent + 2);
    break;
  case expr_Bind:
    printf("Bind (%s)%s: \"%s\"\n",
           expr->bind.kind == bind_Const ? "::" : ":=",
           expr->bind.visibility == Visibility_public ? " [pub]" : "",
           pool_get(pool, expr->bind.name.string_id, 0));
    if (expr->bind.type_ann) {
      print_indent(indent + 1);
      printf("type:\n");
      print_ast(expr->bind.type_ann, pool, indent + 2);
    }
    print_indent(indent + 1);
    printf("value:\n");
    print_ast(expr->bind.value, pool, indent + 2);
    break;
  case expr_Block:
    printf("Block:\n");
    for (size_t i = 0; i < expr->block.stmts->count; i++) {
      struct Expr **e = (struct Expr **)vec_get(expr->block.stmts, i);
      if (e)
        print_ast(*e, pool, indent + 1);
    }
    break;
  case expr_Product:
    printf("Product:\n");
    for (size_t i = 0; i < expr->product.Fields->count; i++) {
      struct ProductField *f =
          (struct ProductField *)vec_get(expr->product.Fields, i);
      if (f) {
        if (f->is_spread) {
          print_indent(indent + 1);
          printf("...\n");
          print_ast(f->value, pool, indent + 2);
        } else if (f->name.string_id) {
          print_indent(indent + 1);
          printf(".%s =\n", pool_get(pool, f->name.string_id, 0));
          print_ast(f->value, pool, indent + 2);
        } else {
          print_ast(f->value, pool, indent + 1);
        }
      }
    }
    break;
  case decl_Effect:
    printf("Effect:\n");
    for (int i = 0; i < indent + 2; i++) printf(" ");
    printf("is_named=%d, is_scoped=%d, is_linear=%d\n",
           expr->effect.is_named, expr->effect.is_scoped, expr->effect.is_linear);
    if (expr->effect.op_declaration) {
      for (size_t i = 0; i < expr->effect.op_declaration->count; i++) {
        struct OpDecl **opp = (struct OpDecl **)vec_get(expr->effect.op_declaration, i);
        if (!opp || !*opp) continue;
        struct OpDecl *op = *opp;
  
        for (int j = 0; j < indent + 2; j++) printf(" ");
        const char *nm = pool_get(pool, op->name.string_id, 0);
        printf("op %s%s (sort=%d)\n",
          nm ? nm : "?",
          op->visibility == Visibility_public ? " [pub]" : "",
          op->sort);
      
        // Params
        for (size_t j = 0; j < op->param_count; j++) {
          struct Param *param = &op->params[j];
          for (int k = 0; k < indent + 4; k++) printf(" ");
          const char *pnm = pool_get(pool, param->name.string_id, 0);
          printf("param %s%s:\n",
                 pnm ? pnm : "?",
                 param->kind == PARAM_COMPTIME ? " (comptime)" :
                 param->kind == PARAM_INFERRED_COMPTIME ? " (inferred)" : "");
          if (param->type_ann) {
            print_ast(param->type_ann, pool, indent + 6);
          }
        }
  
        // Result type
        if (op->result_type) {
          for (int k = 0; k < indent + 4; k++) printf(" ");
          printf("result_type:\n");
          print_ast(op->result_type, pool, indent + 6);
        }
      }
    }
  break;
  case expr_Loop:
    printf("Loop:\n");
    if (expr->loop_expr.init) {
      print_indent(indent + 1);
      printf("init:\n");
      print_ast(expr->loop_expr.init, pool, indent + 2);
    }
    if (expr->loop_expr.condition) {
      print_indent(indent + 1);
      printf("cond:\n");
      print_ast(expr->loop_expr.condition, pool, indent + 2);
    }
    if (expr->loop_expr.step) {
      print_indent(indent + 1);
      printf("step:\n");
      print_ast(expr->loop_expr.step, pool, indent + 2);
    }
    if (expr->loop_expr.capture.string_id != 0) {
      print_indent(indent + 1);
      printf("capture: %s\n",
             pool_get(pool, expr->loop_expr.capture.string_id, 0));
    }
    print_indent(indent + 1);
    printf("body:\n");
    print_ast(expr->loop_expr.body, pool, indent + 2);
    break;
  case expr_Builtin:
    printf("Builtin: @%s\n", pool_get(pool, expr->builtin.name_id, 0));
    if (expr->builtin.args) {
      for (size_t i = 0; i < expr->builtin.args->count; i++) {
        struct Expr **arg = (struct Expr **)vec_get(expr->builtin.args, i);
        if (arg)
          print_ast(*arg, pool, indent + 1);
      }
    }
    break;
  case expr_EffectRow:
    printf("EffectRow:\n");
    if (expr->effect_row.head) {
      print_indent(indent + 1);
      printf("head:\n");
      print_ast(expr->effect_row.head, pool, indent + 2);
    }
    print_indent(indent + 1);
    printf("row: %s\n", pool_get(pool, expr->effect_row.row.string_id, 0));
    break;
  case expr_Field:
    printf("Field: .%s\n", pool_get(pool, expr->field.field.string_id, 0));
    print_ast(expr->field.object, pool, indent + 1);
    break;
  case expr_Index:
    printf("Index:\n");
    print_indent(indent + 1);
    printf("object:\n");
    print_ast(expr->index.object, pool, indent + 2);
    print_indent(indent + 1);
    printf("index:\n");
    print_ast(expr->index.index, pool, indent + 2);
    break;
  case expr_Call:
    printf("Call:\n");
    print_indent(indent + 1);
    printf("callee:\n");
    print_ast(expr->call.callee, pool, indent + 2);
    print_indent(indent + 1);
    printf("args:\n");
    for (size_t i = 0; i < expr->call.args->count; i++) {
      struct Expr **arg = (struct Expr **)vec_get(expr->call.args, i);
      if (arg)
        print_ast(*arg, pool, indent + 2);
    }
    break;
  case expr_If:
    printf("If:\n");
    print_indent(indent + 1);
    printf("cond:\n");
    print_ast(expr->if_expr.condition, pool, indent + 2);
    print_indent(indent + 1);
    printf("then:\n");
    print_ast(expr->if_expr.then_branch, pool, indent + 2);
    if (expr->if_expr.else_branch) {
      print_indent(indent + 1);
      printf("else:\n");
      print_ast(expr->if_expr.else_branch, pool, indent + 2);
    }
    break;
  case expr_Unary: {
    const char *ops[] = {"&",     "*", "-",  "!", "~",
                         "const", "?", "++", "^", "[^]"};
    printf("Unary: %s%s\n", expr->unary.postfix ? "postfix " : "",
           ops[expr->unary.op]);
    print_ast(expr->unary.operand, pool, indent + 1);
    break;
  }
  case expr_Lambda:
    printf("Lambda:\n");
    print_indent(indent + 1);
    printf("params:\n");
    for (size_t i = 0; i < expr->lambda.params->count; i++) {
      struct Param *param = (struct Param *)vec_get(expr->lambda.params, i);
      if (param) {
        print_indent(indent + 2);
        printf("%s", pool_get(pool, param->name.string_id, 0));
        if (param->type_ann) {
          if (param->type_ann->kind == expr_Ident) {
            printf(": Ident: \"%s\"\n",
                   pool_get(pool, param->type_ann->ident.string_id, 0));
          } else {
            printf(":\n");
            print_ast(param->type_ann, pool, indent + 3);
          }
        } else {
          printf("\n");
        }
      }
    }
    if (expr->lambda.ret_type) {
      print_indent(indent + 1);
      printf("returns:\n");
      print_ast(expr->lambda.ret_type, pool, indent + 2);
    }
    if (expr->lambda.effect) {
      print_indent(indent + 1);
      printf("effect:\n");
      print_ast(expr->lambda.effect, pool, indent + 2);
    }
    print_indent(indent + 1);
    printf("body:\n");
    print_ast(expr->lambda.body, pool, indent + 2);
    break;
  case expr_Ctl:
    printf("Ctl:\n");
    print_indent(indent + 1);
    printf("params:\n");
    for (size_t i = 0; i < expr->ctl.params->count; i++) {
      struct Param *param = (struct Param *)vec_get(expr->ctl.params, i);
      if (param) {
        print_indent(indent + 2);
        printf("%s", pool_get(pool, param->name.string_id, 0));
        if (param->type_ann) {
          printf(": ");
          print_ast(param->type_ann, pool, 0);
        } else {
          printf("\n");
        }
      }
    }
    if (expr->ctl.ret_type) {
      print_indent(indent + 1);
      printf("returns:\n");
      print_ast(expr->ctl.ret_type, pool, indent + 2);
    }
    if (expr->ctl.body) {
      print_indent(indent + 1);
      printf("body:\n");
      print_ast(expr->ctl.body, pool, indent + 2);
    }
    break;

  case expr_Handler:
    printf("Handler:\n");
    print_handler_payload(&expr->handler, pool, indent + 1);
    break;

  case expr_Struct:
    printf("Struct:\n");
    print_indent(indent + 1);
    printf("members:\n");
    for (size_t i = 0; i < expr->struct_expr.members->count; i++) {
      struct StructMember *m =
          (struct StructMember *)vec_get(expr->struct_expr.members, i);
      if (!m)
        continue;

      if (m->kind == member_Field) {
        print_indent(indent + 2);
        printf("Field: %s\n", pool_get(pool, m->field.name.string_id, 0));
        print_indent(indent + 3);
        printf("type:\n");
        print_ast(m->field.type, pool, indent + 4);
      } else {
        print_indent(indent + 2);
        printf("Union:\n");
        print_indent(indent + 3);
        printf("variants:\n");
        for (size_t j = 0; j < m->union_def.variants->count; j++) {
          struct FieldDef *v =
              (struct FieldDef *)vec_get(m->union_def.variants, j);
          if (!v)
            continue;
          print_indent(indent + 4);
          printf("Variant: %s\n", pool_get(pool, v->name.string_id, 0));
          print_indent(indent + 5);
          printf("type:\n");
          print_ast(v->type, pool, indent + 6);
        }
      }
    }
    break;

  case expr_Enum:
    printf("Enum:\n");
    print_indent(indent + 1);
    printf("variants:\n");
    for (size_t i = 0; i < expr->enum_expr.variants->count; i++) {
      struct EnumVariant *v =
          (struct EnumVariant *)vec_get(expr->enum_expr.variants, i);
      if (!v)
        continue;
      print_indent(indent + 2);
      printf("Variant: %s", pool_get(pool, v->name.string_id, 0));
      if (v->explicit_value) {
        printf(" = \n");
        print_ast(v->explicit_value, pool, indent + 3);
      } else {
        printf("\n");
      }
    }
    break;

  case expr_Asm:
    printf("Asm: \"%s\"\n", pool_get(pool, expr->asm_expr.string_id, 0));
    break;
  case expr_EnumRef:
    printf("EnumRef: %s\n",
           pool_get(pool, expr->enum_ref_expr.name.string_id, 0));
    break;

  case expr_Switch:
    printf("Switch:\n");
    print_indent(indent + 1);
    printf("scrutinee:\n");
    print_ast(expr->switch_expr.scrutinee, pool, indent + 2);
    print_indent(indent + 1);
    printf("arms:\n");
    for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
      struct SwitchArm *arm =
          (struct SwitchArm *)vec_get(expr->switch_expr.arms, i);
      if (!arm)
        continue;
      print_indent(indent + 2);
      printf("arm:\n");
      print_indent(indent + 3);
      printf("patterns:\n");
      for (size_t j = 0; j < arm->patterns->count; j++) {
        struct Expr **pat = (struct Expr **)vec_get(arm->patterns, j);
        if (pat)
          print_ast(*pat, pool, indent + 4);
      }
      print_indent(indent + 3);
      printf("body:\n");
      print_ast(arm->body, pool, indent + 4);
    }
    break;

  case expr_Return:
    printf("Return:\n");
    if (expr->return_expr.value)
      print_ast(expr->return_expr.value, pool, indent + 1);
    break;
  case expr_Break:
    printf("Break\n");
    break;
  case expr_Continue:
    printf("Continue\n");
    break;
  case expr_Defer:
    printf("Defer:\n");
    print_ast(expr->defer_expr.value, pool, indent + 1);
    break;
  case expr_ArrayType:
    printf("ArrayType:\n");
    if (expr->array_type.size) {
      print_indent(indent + 1);
      printf("size:\n");
      print_ast(expr->array_type.size, pool, indent + 2);
    }
    print_indent(indent + 1);
    printf("elem:\n");
    print_ast(expr->array_type.elem, pool, indent + 2);
    break;
  case expr_ArrayLit:
    if (expr->array_lit.size_inferred) {
      printf("ArrayLit: [_]\n");
    } else {
      printf("ArrayLit:\n");
    }
    if (expr->array_lit.size) {
      print_indent(indent + 1);
      printf("size:\n");
      print_ast(expr->array_lit.size, pool, indent + 2);
    }
    print_indent(indent + 1);
    printf("elem_type:\n");
    print_ast(expr->array_lit.elem_type, pool, indent + 2);
    print_indent(indent + 1);
    printf("initializer:\n");
    print_ast(expr->array_lit.initializer, pool, indent + 2);
    break;
  default:
    // Intentional default: print_ast is a developer dump tool, so
    // a runtime trace ("<unhandled expr kind: N>") is the most
    // useful behavior when a new expr kind is added without a
    // dump handler. The default does suppress -Wswitch on this
    // function specifically; that's a deliberate trade for the
    // visible trace.
    printf("<unhandled expr kind: %d>\n", expr->kind);
    break;
  }
}

// -- Pratt section --

enum Precedence {
  PREC_NONE = 0,
  PREC_ASSIGN,
  PREC_OR,
  PREC_AND,
  PREC_EQUALITY,
  PREC_COMPARISON,
  PREC_RANGE,
  PREC_BITWISE,
  PREC_SHIFT,
  PREC_TERM,
  PREC_FACTOR,
  PREC_POWER,
  PREC_UNARY,
  PREC_POSTFIX,
};

static enum Precedence get_precedence(enum TokenKind kind) {
  switch (kind) {
  case OrElse:
    return PREC_OR;
  case Equal:
  case LeftArrow:
  case PlusEqual:
  case MinusEqual:
  case StarEqual:
  case ForwardSlashEqual:
  case PercentEqual:
  case PipeEqual:
  case AmpersandEqual:
    return PREC_ASSIGN;
  case PipePipe:
    return PREC_OR;
  case AmpersandAmpersand:
    return PREC_AND;
  case EqualEqual:
  case BangEqual:
    return PREC_EQUALITY;
  case Less:
  case Greater:
  case LessEqual:
  case GreaterEqual:
    return PREC_COMPARISON;
  case DotDot:
    return PREC_RANGE;
  case Pipe:
  case Ampersand:
  case Tilde:
    return PREC_BITWISE;
  case ShiftLeft:
  case ShiftRight:
    return PREC_SHIFT;
  case Plus:
  case Minus:
    return PREC_TERM;
  case Star:
  case ForwardSlash:
  case Percent:
    return PREC_FACTOR;
  case StarStar:
    return PREC_POWER;
  default:
    return PREC_NONE;
  }
}

// -- Init --

struct Parser parser_new_in_with_diags(Vec *tokens, StringPool *pool,
                                       Arena *arena, struct DiagBag *diags) {
  struct Parser p = {
      .tokens = tokens,
      .current = 0,
      .pool = pool,
      .arena = arena,
      .diags = diags,
      .had_error = false,
      .parsing_type = false,
      .in_handler_block_depth = 0,
  };

  return p;
}

static struct Token *peek(struct Parser *p) {
  return (struct Token *)vec_get(p->tokens, p->current);
}


static struct Token *previous(struct Parser *p) {
  return (struct Token *)vec_get(p->tokens, p->current - 1);
}

static struct Token *advance(struct Parser *p) {
  struct Token *t = peek(p);
  p->current++;
  return t;
}

static bool is_at_end(struct Parser *p) {
  return p->current >= p->tokens->count || peek(p)->kind == Eof;
}

static bool check(struct Parser *p, enum TokenKind kind) {
  struct Token *t = peek(p);
  return t && t->kind == kind;
}

static bool match(struct Parser *p, enum TokenKind kind) {
  if (check(p, kind)) {
    advance(p);
    return true;
  }
  return false;
}

static Visibility parse_optional_visibility(struct Parser *p) {
  if (peek(p)->kind == Pub)      { advance(p); return Visibility_public; }
  return Visibility_private;
}

static void parser_error(struct Parser *p, struct Span span, const char *fmt,
                         ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  if (p->diags) {
    diag_error(p->diags, span, "%s", msg);
  }
  p->had_error = true;
}

static struct Token *expect(struct Parser *p, enum TokenKind kind) {
  if (check(p, kind)) {
    return advance(p);
  }

  struct Token *t = peek(p);
  struct Span span = t ? t->span : (struct Span){0};
  parser_error(p, span, "expected %s, got %s", token_kind_to_str(kind),
               t ? token_kind_to_str(t->kind) : "<eof>");

  return NULL;
}

static struct Expr *alloc_expr(struct Parser *p, enum ExprKind kind,
                               struct Span span) {
  struct Expr *e = arena_alloc(p->arena, sizeof(struct Expr));
  e->kind = kind;
  e->span = span;
  return e;
}

// -- Error Recovery --

static void synchronize(struct Parser *p) {
  while (!check(p, Eof)) {
    // Stop at statement boundaries
    if (check(p, Semicolon)) {
      advance(p);
      return;
    }
    if (check(p, RBrace))
      return;

    // Stop at tokens that start new statements
    struct Token *t = peek(p);
    if (t) {
      switch (t->kind) {
      case If:
      case Loop:
      case Switch:
      case Handle:
      case Break:
      case Continue:
        return;
      default:
        break;
      }
    }

    advance(p);
  }
}

static bool is_valid_binding_pattern(struct Expr *e) {
  if (!e)
    return false;
  if (e->kind == expr_Ident)
    return true;
  if (e->kind == expr_Product) {
    Vec *fields = e->product.Fields;
    for (size_t i = 0; i < fields->count; i++) {
      struct ProductField *f = (struct ProductField *)vec_get(fields, i);
      if (!f || !f->value)
        return false;
      if (!is_valid_binding_pattern(f->value))
        return false;
    }
    return true;
  }
  return false;
}

// -- Parse Functions --

static struct Expr *parse_expr_prec(struct Parser *p, enum Precedence min_prec);

// Parse the contents of `( ... )` for a function-like signature into a
// fresh Vec<Param>. Caller has already consumed `(`; this consumes through
// the closing `)`. Handles three subtleties uniformly:
//   - leading `comptime` keyword on each param,
//   - optional `: type_ann` (parsed at PREC_BITWISE — tight enough that the
//     surrounding `,` and `)` don't get pulled in),
//   - the `comptime X: Scope` → PARAM_INFERRED_COMPTIME promotion that
//     replaced the old `forall<s>` syntax.
// `void` as the entire param list (`fn(void)`) parses as zero params.
static Vec *parse_param_list(struct Parser *p) {
  Vec *params = vec_new_in(p->arena, sizeof(struct Param));
  if (check(p, Void)) {
    advance(p);
    return params;
  }
  if (check(p, RParen))
    return params;
  for (;;) {
    bool param_is_comptime = match(p, Comptime);
    struct Token *name = expect(p, Identifier);
    if (!name)
      break;

    struct Param param = {
        .name = {.string_id = name->string_id, .span = name->span},
        .type_ann = NULL,
        .kind = param_is_comptime ? PARAM_COMPTIME : PARAM_RUNTIME,
    };
    if (match(p, Colon)) {
      param.type_ann = parse_expr_prec(p, PREC_BITWISE);
    }
    // `comptime X: Scope` is the syntax that replaced `forall<X>`. The
    // call site never supplies it; sema fills it from the active
    // evidence vector. See ParamKind in ast.h.
    if (param.kind == PARAM_COMPTIME && param.type_ann &&
        param.type_ann->kind == expr_Ident) {
      const char *tn = pool_get(p->pool, param.type_ann->ident.string_id, 0);
      if (tn && strcmp(tn, "Scope") == 0) {
        param.kind = PARAM_INFERRED_COMPTIME;
      }
    }
    vec_push(params, &param);
    if (!match(p, Comma))
      break;
  }
  return params;
}

static bool parse_op_params(struct Parser *p, struct OpDecl *op) {
  if (!expect(p, LParen)) return false;

  // Empty param list: `()`
  if (peek(p)->kind == RParen) {
    advance(p);
    op->params = NULL;
    op->param_count = 0;
    return true;
  }

  Vec *params = vec_new_in(p->arena, sizeof(struct Param));
  while (1) {
    struct Param param = {0};

    // name
    struct Token *name_tok = expect(p, Identifier);
    if (!name_tok) return false;
    param.name.string_id = name_tok->string_id;
    param.name.span = name_tok->span;

    // `:` type
    if (!expect(p, Colon)) return false;
    param.type_ann = parse_expr_prec(p, PREC_NONE);
    if (!param.type_ann) return false;
    param.kind = PARAM_RUNTIME;  // op params are runtime by default

    vec_push(params, &param);

    if (peek(p)->kind == Comma) {
      advance(p);
      continue;
    }
    break;
  }

  if (!expect(p, RParen)) return false;

  op->params = (struct Param *)params->data;
  op->param_count = params->count;
  return true;
}

static struct OpDecl *parse_op_decl(struct Parser *p, bool effect_is_linear) {
  // Op name (the "alloc" in `alloc :: fn(...)`)
  struct Token *name_tok = expect(p, Identifier);
  if (!name_tok) return NULL;

  // ::
  if (!expect(p, ColonColon)) return NULL;

  // Optional `pub` modifier (default is private — Rust-style opt-in)
  Visibility op_vis = Visibility_private;
  if (peek(p)->kind == Pub) {
    advance(p);
    op_vis = Visibility_public;
  }

  // Now look at what kind of op this is.
  struct Token *t = peek(p);
  OperationSort sort;
  bool op_is_linear = effect_is_linear;

  switch (t->kind) {
    case Val:
      advance(p);
      sort = OpVal;
      op_is_linear = true;
      break;

    case Fn:
      advance(p);
      sort = OpFn;
      op_is_linear = true;
      break;

    case Ctl:
      advance(p);
      if (effect_is_linear) {
        parser_error(p, t->span,
                     "'ctl' operations are invalid for a linear effect", NULL);
        return NULL;
      }
      sort = OpControl;
      break;

    case Final:
      advance(p);
      if (peek(p)->kind != Ctl) {
        parser_error(p, peek(p)->span, "'final' must be followed by 'ctl'", NULL);
        return NULL;
      }
      advance(p);
      if (effect_is_linear) {
        parser_error(p, t->span,
                     "'final ctl' operations are invalid for a linear effect", NULL);
        return NULL;
      }
      sort = OpExcept;
      break;

    default:
      parser_error(p, t->span,
                   "expected operation kind: 'fn', 'ctl', 'final ctl', or 'val'",
                   NULL);
      return NULL;
  }

  // Allocate.
  struct OpDecl *op = arena_alloc(p->arena, sizeof(struct OpDecl));
  *op = (struct OpDecl){0};
  op->visibility = op_vis;
  op->sort = sort;
  op->is_linear = op_is_linear;
  op->name.string_id = name_tok->string_id;
  op->name.span = name_tok->span;

  // Param list (skipped for val ops).
  if (sort != OpVal) {
    if (!parse_op_params(p, op)) return NULL;
  }

  // -> ResultType
  if (!expect(p, RightArrow)) return NULL;
  op->result_type = parse_expr_prec(p, PREC_NONE);
  if (!op->result_type) return NULL;

  return op;
}

// Parse block statements.
//
// A `with` mid-block consumes the **rest of the block** as its body,
// recursively:
//
//     with exn
//     with debug_allocator
//     r1 := alloc(...)
//
// parses (after with-desugaring at parse_expr_prec) as
//
//     Call(exn, [Lambda([], Block(
//         Call(debug_allocator, [Lambda([], Block(
//             r1 := alloc(...) ))]) ))])
//
// Detection: open_body_slot returns the slot to fill; we recurse to
// build the trailing block and assign it.
static struct Expr *parse_block_stmts(struct Parser *p, struct Span span) {
  struct Expr *e = alloc_expr(p, expr_Block, span);
  e->block.stmts = vec_new_in(p->arena, sizeof(struct Expr *));

  while (!check(p, RBrace) && !is_at_end(p)) {
    struct Expr *stmt = parse_expr_prec(p, PREC_NONE);
    if (!stmt) {
      match(p, Semicolon);
      continue;
    }

    vec_push(e->block.stmts, &stmt);
    match(p, Semicolon);
  }

  return e;
}

// -- Helpers --

static struct Expr *parse_array_literal(struct Parser *p, struct Expr *size,
                                        bool size_inferred,
                                        struct Expr *elem_type,
                                        struct Span start_span) {
  struct Token *brace_tok = advance(p); // consume {

  struct Expr *initializer = alloc_expr(p, expr_Product, brace_tok->span);
  initializer->product.Fields =
      vec_new_in(p->arena, sizeof(struct ProductField));

  while (!check(p, RBrace) && !check(p, Eof)) {
    struct ProductField field = {0};

    if (check(p, Dot) && peek(p)->kind == Identifier) {
      advance(p); // consume .
      struct Token *name_tok = advance(p);
      field.name = (struct Identifier){
          .string_id = name_tok->string_id,
          .span = name_tok->span,
      };
      if (!expect(p, Equal))
        return NULL;
    }

    field.value = parse_expr_prec(p, PREC_NONE);
    if (!field.value)
      return NULL;

    vec_push(initializer->product.Fields, &field);

    if (!match(p, Comma))
      break;
  }

  if (!expect(p, RBrace))
    return NULL;

  struct Expr *e = alloc_expr(p, expr_ArrayLit, start_span);
  e->array_lit.size = size;
  e->array_lit.size_inferred = size_inferred;
  e->array_lit.elem_type = elem_type;
  e->array_lit.initializer = initializer;
  return e;
}

// =====================================================================
// Handler parsing
//
// Mirrors Koka's `handlerExpr` / `handlerExprX` / `handlerClauses` in
// Syntax/Parse.hs. Two surface forms feed one HandlerExpr:
//
//   handle  [scoped] [override] [<eff>] (target)  { ops... }   → Call(H, [target])
//   handler [scoped] [override] [<eff>]           { ops... }   → bare H literal
//
// Optionally prefixed with `named` for instance handlers. Modifier order
// is fixed: scoped, override, <eff>; override is gated to non-named per
// Koka's `handlerOverride`.
// =====================================================================

// Parse a `<eff>` annotation if present. Returns NULL if no `<` follows.
// Form: `<E>` | `<E | row>` | `<| row>` — same shape as fn-signature
// effect rows. We commit on the leading `<`; failure to find `>` is a
// hard error.
static struct Expr *parse_angle_effect(struct Parser *p) {
  if (!match(p, Less)) return NULL;
  struct Span effect_span = previous(p)->span;
  struct Expr *head = NULL;
  struct Identifier row = {0};

  if (check(p, Pipe)) {
    advance(p);
    struct Token *row_tok = expect(p, Identifier);
    if (row_tok) {
      row = (struct Identifier){.string_id = row_tok->string_id,
                                .span = row_tok->span};
    }
  } else {
    head = parse_expr_prec(p, PREC_BITWISE);
    if (match(p, Pipe)) {
      struct Token *row_tok = expect(p, Identifier);
      if (row_tok) {
        row = (struct Identifier){.string_id = row_tok->string_id,
                                  .span = row_tok->span};
      }
    }
  }
  expect(p, Greater);

  if (row.string_id != 0) {
    struct Expr *eff = alloc_expr(p, expr_EffectRow, effect_span);
    eff->effect_row.head = head;
    eff->effect_row.row = row;
    return eff;
  }
  return head;
}

// Walk a parsed block of operation/lifecycle bindings and populate a
// HandlerExpr's branches and clause slots. Equivalent to Koka's
// `partitionClauses`, but operating on the Bind/Return statements that
// come out of Ore's generic block parser.
static struct Expr *parse_handler_clauses(struct Parser *p, struct Span span,
                                          struct Expr *body_block) {
  struct Expr *h = alloc_expr(p, expr_Handler, span);
  h->handler.sort        = HandlerSort_Normal;
  h->handler.scope       = HandlerScope_NoScope;
  h->handler.override    = HandlerOverride_None;
  h->handler.allow_mask  = HandlerMask_Unspecified;
  h->handler.effect      = NULL;
  h->handler.initially_clause = NULL;
  h->handler.return_clause    = NULL;
  h->handler.finally_clause   = NULL;
  h->handler.branches    = vec_new_in(p->arena, sizeof(struct HandlerBranch *));
  h->handler.effect_decl = NULL;
  h->handler.decl_range  = span;

  if (!body_block || body_block->kind != expr_Block) {
    parser_error(p, span,
                 "handler expects a block of operation/lifecycle definitions");
    return h;
  }

  uint32_t initially_id = pool_intern(p->pool, "initially", 9);
  uint32_t finally_id   = pool_intern(p->pool, "finally", 7);

  Vec *stmts = body_block->block.stmts;
  if (!stmts) return h;

  for (size_t i = 0; i < stmts->count; i++) {
    struct Expr **sp = (struct Expr **)vec_get(stmts, i);
    struct Expr *st = sp ? *sp : NULL;
    if (!st) continue;

    // `return(<expr>)` clause — unwrap the Return payload into the slot.
    if (st->kind == expr_Return) {
      if (h->handler.return_clause) {
        parser_error(p, st->span, "duplicate 'return' clause in handler");
      }
      h->handler.return_clause = st->return_expr.value;
      continue;
    }

    // Bind-shaped statements: lifecycle clauses by name, ops by value kind.
    if (st->kind == expr_Bind) {
      uint32_t nm = st->bind.name.string_id;

      if (nm == initially_id) {
        if (h->handler.initially_clause) {
          parser_error(p, st->span, "duplicate 'initially' clause in handler");
        }
        h->handler.initially_clause = st->bind.value;
        continue;
      }
      if (nm == finally_id) {
        if (h->handler.finally_clause) {
          parser_error(p, st->span, "duplicate 'finally' clause in handler");
        }
        h->handler.finally_clause = st->bind.value;
        continue;
      }

      // Operation: `name :: fn(...) body` (OpFn) or `name :: ctl(...) body`
      // (OpControl). Other value kinds are not legal in a handler block.
      struct Expr *val = st->bind.value;
      if (val && (val->kind == expr_Lambda || val->kind == expr_Ctl)) {
        struct HandlerBranch *br =
            arena_alloc(p->arena, sizeof(struct HandlerBranch));
        uint32_t name_copy = st->bind.name.string_id;
        br->name.string_id = name_copy;
        if (val->kind == expr_Lambda) {
          br->pars = val->lambda.params;
          br->expr = val->lambda.body;
          br->sort = OpFn;
        } else {
          br->pars = val->ctl.params;
          br->expr = val->ctl.body;
          br->sort = OpControl;
        }
        vec_push(h->handler.branches, &br);
        continue;
      }
    }

    parser_error(
        p, st->span,
        "only operation/lifecycle bindings allowed inside handler block");
  }

  return h;
}

// Parse the body block of a handler. Catches the Koka-deprecated
// `handler (params) { ... }` local-parameters form with a clear error.
static struct Expr *parse_handler_body(struct Parser *p) {
  if (check(p, LParen)) {
    parser_error(p, peek(p)->span,
                 "local parameters on handlers are not supported; "
                 "use a local 'var' instead");
    // Recover: skip the parenthesized group so we can keep parsing.
    advance(p);
    int depth = 1;
    while (depth > 0 && !check(p, Eof)) {
      if (check(p, LParen)) depth++;
      else if (check(p, RParen)) depth--;
      advance(p);
    }
  }
  return parse_expr_prec(p, PREC_NONE);
}

// Modifier matrix walker. Caller has consumed `named` (if any) and we
// are positioned at the `handle` or `handler` keyword.
static struct Expr *parse_handler_expr(struct Parser *p, struct Span start_span,
                                       bool is_named) {
  struct Token *kw = peek(p);
  bool has_target;
  if (kw->kind == Handle)        { advance(p); has_target = true;  }
  else if (kw->kind == Handler)  { advance(p); has_target = false; }
  else {
    parser_error(p, kw->span, "expected `handle` or `handler`");
    return NULL;
  }

  // Modifier order: scoped, override (gated to non-named), <eff>.
  HandlerScope    scope    = match(p, Scoped) ? HandlerScope_Scoped
                                              : HandlerScope_NoScope;
  HandlerOverride override = HandlerOverride_None;
  if (check(p, Override)) {
    if (is_named) {
      parser_error(p, peek(p)->span,
                   "`override` is not valid on a named handler");
      advance(p); // consume to recover
    } else {
      advance(p);
      override = HandlerOverride_Override;
    }
  }
  struct Expr *eff = parse_angle_effect(p);

  // For `handle` form, parse the target action expression before the body.
  struct Expr *target = has_target ? parse_expr_prec(p, PREC_NONE) : NULL;

  // Body block. in_handler_block_depth gates `initially`/`finally` keywords.
  p->in_handler_block_depth++;
  struct Expr *body = parse_handler_body(p);
  p->in_handler_block_depth--;

  struct Expr *handler = parse_handler_clauses(p, start_span, body);
  if (handler) {
    handler->handler.sort     = is_named ? HandlerSort_Instance
                                         : HandlerSort_Normal;
    handler->handler.scope    = scope;
    handler->handler.override = override;
    handler->handler.effect   = eff;
  }

  // `handle (target) { ops }` is `App handler [target]` — wrap.
  if (has_target) {
    struct Expr *call = alloc_expr(p, expr_Call, start_span);
    call->call.callee = handler;
    call->call.args = vec_new_in(p->arena, sizeof(struct Expr *));
    vec_push(call->call.args, &target);
    return call;
  }
  return handler;
}

static struct Expr *parse_primary(struct Parser *p) {
  struct Token *t = peek(p);

  switch (t->kind) {
  // Literals
  case IntLit: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Lit, t->span);
    e->lit.kind = lit_Int;
    e->lit.string_id = t->string_id;
    return e;
  }
  case FloatLit: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Lit, t->span);
    e->lit.kind = lit_Float;
    e->lit.string_id = t->string_id;
    return e;
  }
  case StringLit: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Lit, t->span);
    e->lit.kind = lit_String;
    e->lit.string_id = t->string_id;
    return e;
  }
  case ByteLit: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Lit, t->span);
    e->lit.kind = lit_Byte;
    e->lit.string_id = t->string_id;
    return e;
  }
  case True: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Lit, t->span);
    e->lit.kind = lit_True;
    e->lit.string_id = t->string_id;
    return e;
  }
  case False: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Lit, t->span);
    e->lit.kind = lit_False;
    e->lit.string_id = t->string_id;
    return e;
  }
  case AsmLit: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Asm, t->span);
    e->asm_expr.string_id = t->string_id;
    return e;
  }
  case Nil:
  case Void:
  case NoReturn:
  case AnyType:
  case Type: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Ident, t->span);
    e->ident.string_id = t->string_id;
    e->ident.span = t->span;
    return e;
  }
  case Underscore: {
    // `_` is a wildcard, not a name — used in switch arms,
    // destructure patterns, and explicit discard (`_ = expr`).
    // The resolver skips it (no binding, no lookup) and the
    // checker treats it as match-anything in pattern position.
    advance(p);
    return alloc_expr(p, expr_Wildcard, t->span);
  }
  case Identifier: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Ident, t->span);
    e->ident.string_id = t->string_id;
    e->ident.span = t->span;
    e->ident.resolved = NULL;
    return e;
  }
  case LParen: {
    advance(p);
    struct Expr *inner = parse_expr_prec(p, PREC_NONE);
    expect(p, RParen);
    return inner;
  }
  case LBrace: {
    advance(p);
    struct Expr *e = parse_block_stmts(p, t->span);
    expect(p, RBrace);
    return e;
  }
  // fn(params) <effects> return_type body
  case Fn: {
    advance(p); // consume fn
    expect(p, LParen);

    Vec *params = parse_param_list(p);

    expect(p, RParen);

    // Optional effect annotation: `<Exn>`, `<Exn | e>`, `<| e>`.
    //
    // Caveat for future contributors: `<H | e>` only parses as an
    // EffectRow because we're in this dedicated post-`fn(...)` slot.
    // In a generic expression context, `a < b | c > d` is a chain of
    // comparison + bitwise-or — and would be wrong here. Today there
    // is no `<...>` outside this slot and effect-op signatures, so
    // there's no actual ambiguity. If you ever want to allow effect
    // rows as first-class expression-level types (e.g. `comptime
    // <H|e>`), introduce a distinct delimiter (or a leading `~<`,
    // `effect<`, etc.) — don't try to disambiguate `<` by lookahead.
    struct Expr *effect = NULL;
    if (match(p, Less)) {
      struct Span effect_span = previous(p)->span;
      struct Expr *head = NULL;
      struct Identifier row = {0};

      // Parse effect list — handle | as row separator
      if (check(p, Pipe)) {
        // <|e> — open row only
        advance(p); // consume |
        struct Token *row_tok = expect(p, Identifier);
        if (row_tok) {
          row = (struct Identifier){.string_id = row_tok->string_id,
                                    .span = row_tok->span};
        }
      } else {
        head = parse_expr_prec(p, PREC_BITWISE);
        // Check for row variable: <allocator | e>
        if (match(p, Pipe)) {
          struct Token *row_tok = expect(p, Identifier);
          if (row_tok) {
            row = (struct Identifier){.string_id = row_tok->string_id,
                                      .span = row_tok->span};
          }
        }
      }

      if (row.string_id != 0) {
        effect = alloc_expr(p, expr_EffectRow, effect_span);
        effect->effect_row.head = head;
        effect->effect_row.row = row;
      } else {
        effect = head;
      }
      expect(p, Greater);
    }

    // Optional return type, gated by an explicit `->` arrow.
    //   fn(...) -> T          // function type or typed signature
    //   fn(...) -> T body     // typed lambda with body
    //   fn(...) body          // untyped lambda (ret inferred)
    // The arrow disambiguates value-position lambdas from type
    // expressions without a token-kind allowlist heuristic.
    struct Expr *ret_type = NULL;
    if (match(p, RightArrow)) {
      p->parsing_type = true;
      ret_type = parse_expr_prec(p, PREC_BITWISE);
      p->parsing_type = false;
    }

    // Parse body — unless next token suggests we're a type signature.
    struct Expr *body = NULL;
    struct Token *body_tok = peek(p);
    if (body_tok && body_tok->kind != RParen && body_tok->kind != Comma &&
        body_tok->kind != Greater && body_tok->kind != Semicolon &&
        body_tok->kind != RBrace && body_tok->kind != Pipe &&
        body_tok->kind != Eof) {
      body = parse_expr_prec(p, PREC_NONE);
    }

    struct Expr *e = alloc_expr(p, expr_Lambda, t->span);
    e->lambda.params = params;
    e->lambda.effect = effect;
    e->lambda.ret_type = ret_type;
    e->lambda.body = body;
    return e;
  }

  case Handle:
  case Handler:
    return parse_handler_expr(p, t->span, /*is_named=*/false);

  case With: {
    advance(p);  // consume `with`
    struct Expr *parsed = parse_expr_prec(p, PREC_NONE);
  
    // Layout injected `;` between the with-stmt and the rest. Skip it.
    if (peek(p)->kind == Semicolon) advance(p);
  
    // Consume the rest of the enclosing block as body.
    Vec *body_stmts = vec_new_in(p->arena, sizeof(struct Expr *));
    while (peek(p)->kind != RBrace && peek(p)->kind != Eof) {
      struct Expr *stmt = parse_expr_prec(p, PREC_NONE);
      if (stmt) vec_push(body_stmts, &stmt);
      if (peek(p)->kind == Semicolon) advance(p);
    }
  
    struct Expr *body;
    if (body_stmts->count == 1) {
      body = *(struct Expr **)vec_get(body_stmts, 0);  // single stmt, no Block wrap
    } else {
      body = alloc_expr(p, expr_Block, t->span);
      body->block.stmts = body_stmts;
    }
  
    // Extract optional binder from a Bind-shaped parsed result.
    struct Identifier binder = {0};
    struct Expr *type_ann = NULL;
    struct Expr *caller = parsed;
    if (parsed->kind == expr_Bind) {
      binder = parsed->bind.name;
      type_ann = parsed->bind.type_ann;
      caller = parsed->bind.value;
    }
  
    // Build the action lambda: fn(<binder?>) { body }
    struct Expr *lambda = alloc_expr(p, expr_Lambda, t->span);
    lambda->lambda.params = vec_new_in(p->arena, sizeof(struct Param));
    if (binder.string_id != 0) {
      struct Param param = { .name = binder, .kind = PARAM_RUNTIME, .type_ann = type_ann };
      vec_push(lambda->lambda.params, &param);
    }
    lambda->lambda.body = body;
  
    // applyToContinuation: append to existing Call, or wrap atom as Call.
    if (caller->kind == expr_Call) {
      vec_push(caller->call.args, &lambda);
      return caller;
    } else {
      struct Expr *call = alloc_expr(p, expr_Call, t->span);
      call->call.callee = caller;
      call->call.args = vec_new_in(p->arena, sizeof(struct Expr *));
      vec_push(call->call.args, &lambda);
      return call;
    }
  }

  // If/else/elif
  // if (cond) ... else ...
  // if (optional) |capture| ... else ...
  case If:
  case Elif: {
    advance(p); // consume if or elif

    // Parse condition in parens
    expect(p, LParen);
    struct Expr *condition = parse_expr_prec(p, PREC_NONE);
    expect(p, RParen);

    struct Expr *then_branch = parse_expr_prec(p, PREC_NONE);

    struct Expr *else_branch = NULL;
    if (check(p, Elif)) {
      else_branch = parse_primary(p);
    } else if (match(p, Else)) {
      else_branch = parse_expr_prec(p, PREC_NONE);
    }

    struct Expr *e = alloc_expr(p, expr_If, t->span);
    e->if_expr.condition = condition;
    e->if_expr.then_branch = then_branch;
    e->if_expr.else_branch = else_branch;
    return e;
  }

  case LBracket: {
    struct Token *start_tok = advance(p); // consume [

    // ---- [^]T : many-pointer type ----
    if (check(p, Caret)) {
      advance(p); // consume ^
      if (!expect(p, RBracket))
        return NULL;

      struct Expr *elem = parse_expr_prec(p, PREC_POSTFIX);
      if (!elem)
        return NULL;

      if (check(p, LBrace)) {
        parser_error(
            p, start_tok->span,
            "many-pointer types ([^]T) cannot have literal initializers");
        return NULL;
      }

      struct Expr *e = alloc_expr(p, expr_ManyPtrType, start_tok->span);
      e->many_ptr_type.elem = elem;
      return e;
    }

    // ---- []T : slice type ----
    if (check(p, RBracket)) {
      advance(p); // consume ]

      struct Expr *elem = parse_expr_prec(p, PREC_POSTFIX);
      if (!elem)
        return NULL;

      struct Expr *e = alloc_expr(p, expr_SliceType, start_tok->span);
      e->slice_type.elem = elem;
      return e;
    }

    // ---- [_]T{...} : array literal with inferred size ----
    if (check(p, Underscore)) {
      advance(p); // consume _
      if (!expect(p, RBracket))
        return NULL;

      if (p->parsing_type) {
        parser_error(p, start_tok->span,
                     "inferred-size arrays ([_]T) are only valid in literal "
                     "expressions; "
                     "use [N]T for an explicit size or []T for a slice");
        return NULL;
      }

      struct Expr *elem = parse_expr_prec(p, PREC_POSTFIX);
      if (!elem)
        return NULL;

      if (!check(p, LBrace)) {
        parser_error(
            p, start_tok->span,
            "[_]T requires a literal initializer; "
            "use []T for an unsized type or [N]T for an explicit size");
        return NULL;
      }

      return parse_array_literal(p, /*size=*/NULL, /*size_inferred=*/true, elem,
                                 start_tok->span);
    }

    // ---- [N]T : either array type or array literal ----
    struct Expr *size = parse_expr_prec(p, PREC_BITWISE);
    if (!size)
      return NULL;
    if (!expect(p, RBracket))
      return NULL;

    struct Expr *elem = parse_expr_prec(p, PREC_POSTFIX);
    if (!elem)
      return NULL;

    if (!p->parsing_type && check(p, LBrace)) {
      // [N]T{...} — array literal (only in value position)
      return parse_array_literal(p, size, /*size_inferred=*/false, elem,
                                 start_tok->span);
    }

    // [N]T — array type
    struct Expr *e = alloc_expr(p, expr_ArrayType, start_tok->span);
    e->array_type.size = size;
    e->array_type.elem = elem;
    return e;
  }

  // initially/finally — handler lifecycle blocks. Only legal at
  // statement-head inside a `handler { ... }` or `handle (t) { ... }`
  // body. Outside that context, emit an error instead of producing
  // a stray Bind that would baffle later passes.
  case Initially:
  case Finally: {
    if (p->in_handler_block_depth == 0) {
      const char *name = (t->kind == Initially) ? "initially" : "finally";
      parser_error(p, t->span, "'%s' is only valid inside a handler block",
                   name);
      advance(p); // consume keyword so synchronize doesn't loop
      synchronize(p);
      return NULL;
    }
    advance(p);
    struct Expr *body = parse_expr_prec(p, PREC_NONE);
    // Wrap in a named Bind so reshape_to_handler can route it
    // into the matching lifecycle slot by name.
    struct Expr *e = alloc_expr(p, expr_Bind, t->span);
    e->bind.kind = bind_Const;
    e->bind.name =
        (struct Identifier){.string_id = t->string_id, .span = t->span};
    e->bind.type_ann = NULL;
    e->bind.value = body;
    return e;
  }

  // break
  case Break: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Break, t->span);
    return e;
  }

  // continue
  case Continue: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Continue, t->span);
    return e;
  }

  // defer expr
  case Defer: {
    advance(p);
    struct Expr *value = parse_expr_prec(p, PREC_NONE);
    struct Expr *e = alloc_expr(p, expr_Defer, t->span);
    e->defer_expr.value = value;
    return e;
  }

  // Product literal: .{ x, y } or .{ .name = val, .age = val }
  case Dot: {
    if (peek(p) && p->current + 1 < p->tokens->count) {
      struct Token *next = (struct Token *)vec_get(p->tokens, p->current + 1);
      if (next && next->kind == LBrace) {
        advance(p); // consume .
        advance(p); // consume {

        struct Expr *e = alloc_expr(p, expr_Product, t->span);
        e->product.type_expr = NULL;
        Vec *fields = vec_new_in(p->arena, sizeof(struct ProductField));

        if (!check(p, RBrace)) {
          for (;;) {
            struct ProductField field = {
                .name = {0}, .value = NULL, .is_spread = false};

            if (match(p, DotDotDot)) {
              field.is_spread = true;
            } else if (check(p, Dot)) {
              // Named field: .name = expr
              advance(p); // consume .
              struct Token *fname = expect(p, Identifier);
              if (fname) {
                field.name = (struct Identifier){.string_id = fname->string_id,
                                                 .span = fname->span};
              }
              expect(p, Equal);
            }

            field.value = parse_expr_prec(p, PREC_NONE);
            vec_push(fields, &field);

            if (!match(p, Comma))
              break;
          }
        }
        expect(p, RBrace);
        e->product.Fields = fields;
        return e;
      } else if (next && next->kind == Identifier) {
        advance(p); // consume '.'
        struct Token *refname = expect(p, Identifier);
        if (!refname)
          return NULL;

        struct Expr *e = alloc_expr(p, expr_EnumRef, t->span);
        e->enum_ref_expr.name = (struct Identifier){
            .string_id = refname->string_id, .span = refname->span};
        return e;
      }
    }
    // Plain dot — fall through to default error
    advance(p);
    parser_error(p, t->span, "unexpected '.'");
    synchronize(p);
    return NULL;
  }

  case Struct: {
    const struct Token *start_tok = peek(p);
    advance(p); // Consume 'struct'

    struct Expr *e = alloc_expr(p, expr_Struct, start_tok->span);
    e->struct_expr.members = vec_new_in(p->arena, sizeof(struct StructMember));

    expect(p, LBrace);

    while (!check(p, RBrace) && !is_at_end(p)) {

      if (match(p, Union)) {
        struct StructMember union_member = {0};
        union_member.kind = member_Union;
        union_member.span = previous(p)->span;

        union_member.union_def.variants =
            vec_new_in(p->arena, sizeof(struct FieldDef));

        expect(p, LBrace);

        while (!check(p, RBrace) && !is_at_end(p)) {
          struct FieldDef field = {0};

          struct Token *name_tok = expect(p, Identifier);
          if (!name_tok)
            break;
          field.name = (struct Identifier){.string_id = name_tok->string_id,
                                           .span = name_tok->span};
          expect(p, ColonColon);
          field.type = parse_expr_prec(p, PREC_NONE);

          vec_push(union_member.union_def.variants, &field);
          match(p, Semicolon);
        }

        expect(p, RBrace);
        vec_push(e->struct_expr.members, &union_member);

      } else {
        struct StructMember field_member = {0};
        field_member.kind = member_Field;

        struct Token *name_tok = expect(p, Identifier);
        if (!name_tok)
          break;

        field_member.span = name_tok->span;
        field_member.field.name = (struct Identifier){
            .string_id = name_tok->string_id, .span = name_tok->span};
        expect(p, Colon);
        field_member.field.type = parse_expr_prec(p, PREC_NONE);

        vec_push(e->struct_expr.members, &field_member);
      }

      match(p, Semicolon);
    }

    expect(p, RBrace);
    return e;
  }

  case Enum: {
    advance(p); // consume 'enum'
    expect(p, LBrace);

    Vec *variants = vec_new_in(p->arena, sizeof(struct EnumVariant));

    while (!check(p, RBrace) && !check(p, Eof)) {

      size_t pos_before = p->current;

      struct Token *name = expect(p, Identifier);
      if (!name)
        break;

      struct Expr *explicit_value = NULL;
      if (match(p, Equal)) {
        explicit_value = parse_expr_prec(p, PREC_BITWISE);
      }

      struct EnumVariant variant = {
          .name = (struct Identifier){.string_id = name->string_id,
                                      .span = name->span},
          .explicit_value = explicit_value,
          .span = name->span,
      };

      vec_push(variants, &variant);

      match(p, Semicolon);

      if (p->current == pos_before)
        advance(p);
    }

    expect(p, RBrace);

    struct Expr *e = alloc_expr(p, expr_Enum, t->span);
    e->enum_expr.variants = variants;
    return e;
  }

  case Switch: {
    advance(p); // consume switch
    struct Expr *scrutinee = parse_expr_prec(p, PREC_NONE);

    expect(p, LBrace);

    Vec *arms = vec_new_in(p->arena, sizeof(struct SwitchArm));

    while (!check(p, RBrace) && !check(p, Eof)) {
      size_t pos_before = p->current;

      struct SwitchArm arm = {.patterns = NULL, .body = NULL};
      Vec *patterns = vec_new_in(p->arena, sizeof(struct Expr *));

      // Parse patterns separated by | (or)
      for (;;) {
        struct Expr *pat = parse_primary(p);
        if (pat)
          vec_push(patterns, &pat);
        if (!match(p, Pipe))
          break;
      }
      arm.patterns = patterns;
      expect(p, FatArrow);
      arm.body = parse_expr_prec(p, PREC_NONE);
      match(p, Semicolon);
      vec_push(arms, &arm);

      if (p->current == pos_before)
        advance(p);
    }

    struct Expr *e = alloc_expr(p, expr_Switch, t->span);
    e->switch_expr.scrutinee = scrutinee;
    e->switch_expr.arms = arms;

    expect(p, RBrace);

    return e;
  }

  // ctl — handler operation (non-resuming)
  case Ctl: {
    advance(p);
    struct Expr *e = alloc_expr(p, expr_Ctl, t->span);

    expect(p, LParen);
    Vec *params = parse_param_list(p);
    expect(p, RParen);
    e->ctl.params = params;

    // Optional ret_type via `->`, then optional body — same
    // arrow grammar as `fn`. Forms:
    //   ctl(...) -> T          // signature
    //   ctl(...) body          // impl (ret inferred)
    //   ctl(...) -> T body     // impl with explicit ret_type
    e->ctl.ret_type = NULL;
    e->ctl.body = NULL;
    if (match(p, RightArrow)) {
      p->parsing_type = true;
      e->ctl.ret_type = parse_expr_prec(p, PREC_BITWISE);
      p->parsing_type = false;
    }
    struct Token *nx = peek(p);
    if (nx && nx->kind != RParen && nx->kind != Comma && nx->kind != Greater &&
        nx->kind != Semicolon && nx->kind != RBrace && nx->kind != Pipe &&
        nx->kind != Eof) {
      e->ctl.body = parse_expr_prec(p, PREC_NONE);
    }

    return e;
  }

  // Effect declaration: effect, named effect, scoped effect, named scoped
  // effect<s>
  //
  // `Named` is also the prefix for instance handlers (`named handle ...`,
  // `named handler ...`), so a one-token lookahead disambiguates.
  case Effect:
  case Named:
  case Linear:
  case Scoped: {
  
    struct Expr *e = alloc_expr(p, decl_Effect, t->span);
    e->effect.is_named = false;
    e->effect.is_scoped = false;
    e->effect.is_linear = false;
  
    // Modifiers in canonical order: named? scoped? linear?
    if (peek(p)->kind == Named) {
      advance(p);
      e->effect.is_named = true;
    }
    if (peek(p)->kind == Scoped) {
      advance(p);
      e->effect.is_scoped = true;
    }
    if (peek(p)->kind == Linear) {
      advance(p);
      e->effect.is_linear = true;
    }
  
    // Modifiers must appear in order. Any leftover modifier here is
    // a misordering error.
    if (peek(p)->kind == Named ||
        peek(p)->kind == Scoped ||
        peek(p)->kind == Linear) {
      parser_error(p, peek(p)->span,
                   "effect modifiers must appear in order: named, scoped, linear",
                   NULL);
      return NULL;
    }
  
    expect(p, Effect);
  
    // (2) Optional `in type` clause (only legal for named effects)
    if (peek(p)->kind == In) {
      if (!e->effect.is_named) {
        parser_error(p, peek(p)->span,
                     "'in' clause is only valid for named effects",
                     NULL);
        return NULL;
      }
      advance(p);
      struct Expr *in_type = parse_expr_prec(p, PREC_NONE);
      if (!in_type) return NULL;
  
      struct Expr **types = arena_alloc(p->arena, sizeof(struct Expr *));
      types[0] = in_type;
      e->effect.extra.tag = EFFECT_REPLACE;
      e->effect.extra.types = types;
      e->effect.extra.type_count = 1;
    } else {
      // Named effect without `in` clause.
      // Koka adds a `partial` effect for the non-scoped case, marking the
      // signature as "may fail due to handler being out of scope." Ore
      // doesn't track partial in its type system; runtime/escape analysis
      // handles staleness instead. Both scoped and non-scoped land here as
      // EFFECT_EXTRA.
      e->effect.extra.tag = EFFECT_EXTRA;
      e->effect.extra.types = NULL;
      e->effect.extra.type_count = 0;
    }
    
  
    // (3) Operations: { op1; op2; ... }
    e->effect.op_declaration = vec_new_in(p->arena, sizeof(struct OpDecl *));
    expect(p, LBrace);
    // Skip leading semicolons.
    while (peek(p)->kind == Semicolon) advance(p);
    
    while (peek(p)->kind != RBrace && peek(p)->kind != Eof) {
      struct OpDecl *op = parse_op_decl(p, e->effect.is_linear);
      if (op != NULL) {
        vec_push(e->effect.op_declaration, &op);
      } else {
        // Error recovery: skip to the next `;` or `}` so we don't spin.
        while (peek(p)->kind != Semicolon &&
               peek(p)->kind != RBrace &&
               peek(p)->kind != Eof) {
          advance(p);
        }
      }
      // Consume one or more separators before trying the next op.
      while (peek(p)->kind == Semicolon) advance(p);
    }
    expect(p, RBrace);
  
    return e;
  }

  // Loop: loop (cond), loop (opt) |capture|, loop (init; cond; step)
  case Loop: {
    advance(p); // consume 'loop'

    struct Expr *init = NULL;
    struct Expr *condition = NULL;
    struct Expr *step = NULL;
    struct Identifier capture = {0};

    // Loop with parens: could be (cond), (init; cond; step), or (cond) |cap|
    if (match(p, LParen)) {
      struct Expr *first = parse_expr_prec(p, PREC_NONE);

      if (match(p, Semicolon)) {
        // C-style: loop (init; cond; step)
        init = first;
        condition = parse_expr_prec(p, PREC_NONE);
        expect(p, Semicolon);
        step = parse_expr_prec(p, PREC_NONE);
      } else {
        // Single-expression: loop (cond) [|capture|]
        condition = first;
      }

      expect(p, RParen);

      // Optional capture: loop (expr) |name|
      if (match(p, Pipe)) {
        struct Token *cap_name = expect(p, Identifier);
        if (cap_name) {
          capture = (struct Identifier){
              .string_id = cap_name->string_id,
              .span = cap_name->span,
          };
        }
        expect(p, Pipe);
      }
    }
    // else: bare `loop body` — no parens, no condition, no init/step

    struct Expr *body = parse_expr_prec(p, PREC_NONE);

    struct Expr *e = alloc_expr(p, expr_Loop, t->span);
    e->loop_expr.init = init;
    e->loop_expr.condition = condition;
    e->loop_expr.step = step;
    e->loop_expr.body = body;
    e->loop_expr.capture = capture;
    return e;
  }

  // Builtins: @sizeOf(T), @ptrcast(x), @null, etc.
  case At: {
    advance(p); // consume @
    struct Token *name = expect(p, Identifier);
    if (!name)
      return NULL;

    struct Expr *e = alloc_expr(p, expr_Builtin, t->span);
    e->builtin.name_id = name->string_id;

    // Some builtins have no args (like @null)
    if (check(p, LParen)) {
      advance(p);
      Vec *args = vec_new_in(p->arena, sizeof(struct Expr *));

      if (!check(p, RParen)) {
        for (;;) {
          struct Expr *arg = parse_expr_prec(p, PREC_NONE);
          if (arg)
            vec_push(args, &arg);
          if (!match(p, Comma))
            break;
        }
      }
      expect(p, RParen);
      e->builtin.args = args;
    } else {
      e->builtin.args = NULL;
    }
    return e;
  }

  // Prefix unary operators: *T, &x, -x, !x, ~x, const T
  case Caret:
  case Star:
  case Ampersand:
  case Minus:
  case Bang:
  case Tilde:
  case Const:
  case Question: {
    advance(p);
    enum UnaryOp op;
    switch (t->kind) {
    case Caret:
      op = unary_Ptr;
      break;
    case Star:
      op = unary_Deref;
      break;
    case Ampersand:
      op = unary_Ref;
      break;
    case Minus:
      op = unary_Neg;
      break;
    case Bang:
      op = unary_Not;
      break;
    case Tilde:
      op = unary_BitNot;
      break;
    case Const:
      op = unary_Const;
      break;
    case Question:
      op = unary_Optional;
      break;
    default:
      op = unary_Not;
      break;
    }
    struct Expr *operand = parse_expr_prec(p, PREC_UNARY);
    struct Expr *e = alloc_expr(p, expr_Unary, t->span);
    e->unary.op = op;
    e->unary.operand = operand;
    e->unary.postfix = false;
    return e;
  }
  // comptime expr — modifier, mark the inner expression
  case Comptime: {
    advance(p);
    struct Expr *inner = parse_primary(p);
    if (inner)
      inner->is_comptime = true;
    return inner;
  }

  // return expr
  case Return: {
    advance(p);
    struct Expr *value = NULL;
    // return with no value — check if next token could start an expr
    struct Token *next_tok = peek(p);
    if (next_tok && next_tok->kind != Semicolon && next_tok->kind != RBrace &&
        next_tok->kind != Eof) {
      value = parse_expr_prec(p, PREC_NONE);
    }
    struct Expr *e = alloc_expr(p, expr_Return, t->span);
    e->return_expr.value = value;
    return e;
  }

  default: {
    parser_error(p, t->span, "unexpected token %s", token_kind_to_str(t->kind));
    synchronize(p);
    return NULL;
  }
  }
}

static struct Expr *parse_expr_prec(struct Parser *p,
                                    enum Precedence min_prec) {
  struct Expr *left = parse_primary(p);
  if (!left)
    return NULL;

  for (;;) {
    struct Token *t = peek(p);
    if (!t)
      break;

    // Postfix: function call foo(x, y)
    if (t->kind == LParen && min_prec < PREC_POSTFIX) {
      advance(p); // consume (
      Vec *args = vec_new_in(p->arena, sizeof(struct Expr *));

      if (!check(p, RParen)) {
        for (;;) {
          struct Expr *arg = parse_expr_prec(p, PREC_NONE);
          if (arg)
            vec_push(args, &arg);
          if (!match(p, Comma))
            break;
        }
      }
      expect(p, RParen);
      expect(p, Semicolon);

      struct Expr *call = alloc_expr(p, expr_Call, left->span);
      call->call.callee = left;
      call->call.args = args;
      left = call;
      continue;
    }

    // Postfix: field access x.field, x->field, x.0, or composite literal
    // x.{...}
    if ((t->kind == Dot || t->kind == RightArrow) && min_prec < PREC_POSTFIX) {
      advance(p); // consume . or ->
      struct Token *next_tok = peek(p);

      // Composite literal: Type.{values}
      if (next_tok && next_tok->kind == LBrace) {
        advance(p); // consume {
        struct Expr *e = alloc_expr(p, expr_Product, left->span);
        e->product.type_expr = left;
        Vec *fields = vec_new_in(p->arena, sizeof(struct ProductField));

        if (!check(p, RBrace)) {
          for (;;) {
            struct ProductField field_item = {
                .name = {0}, .value = NULL, .is_spread = false};
            if (match(p, DotDotDot)) {
              field_item.is_spread = true;
            } else if (check(p, Dot)) {
              advance(p);
              struct Token *fname = expect(p, Identifier);
              if (fname) {
                field_item.name = (struct Identifier){
                    .string_id = fname->string_id, .span = fname->span};
              }
              expect(p, Equal);
            }
            field_item.value = parse_expr_prec(p, PREC_NONE);
            vec_push(fields, &field_item);
            if (!match(p, Comma))
              break;
          }
        }
        expect(p, RBrace);
        e->product.Fields = fields;
        left = e;
        continue;
      }

      if (!next_tok ||
          (next_tok->kind != Identifier && next_tok->kind != IntLit)) {
        struct Span span = next_tok ? next_tok->span : t->span;
        parser_error(p, span, "expected field name after '.'");
        break;
      }
      advance(p);

      struct Expr *field = alloc_expr(p, expr_Field, left->span);
      field->field.object = left;
      field->field.field = (struct Identifier){.string_id = next_tok->string_id,
                                               .span = next_tok->span};
      left = field;
      continue;
    }

    // Postfix: index x[i] or slice x[0..n]
    if (t->kind == LBracket && min_prec < PREC_POSTFIX) {
      advance(p); // consume [
      struct Expr *index = parse_expr_prec(p, PREC_NONE);
      expect(p, RBracket);

      struct Expr *idx = alloc_expr(p, expr_Index, left->span);
      idx->index.object = left;
      idx->index.index = index;
      left = idx;
      continue;
    }

    // Postfix: x++ (increment)
    if (t->kind == PlusPlus && min_prec < PREC_POSTFIX) {
      advance(p);
      struct Expr *e = alloc_expr(p, expr_Unary, left->span);
      e->unary.op = unary_Inc;
      e->unary.operand = left;
      e->unary.postfix = true;
      left = e;
      continue;
    }

    // Postfix: x^ (dereference)
    if (t->kind == Caret && min_prec < PREC_POSTFIX) {
      advance(p);
      struct Expr *e = alloc_expr(p, expr_Unary, left->span);
      e->unary.op = unary_Deref;
      e->unary.operand = left;
      e->unary.postfix = true;
      left = e;
      // Don't break — check for .field next in the loop
      continue;
    }

    // Postfix: x? (unwrap possible null)
    if (t->kind == Question && min_prec < PREC_POSTFIX) {
      advance(p);
      struct Expr *e = alloc_expr(p, expr_Unary, left->span);
      e->unary.op = unary_DeNil;
      e->unary.operand = left;
      e->unary.postfix = true;
      left = e;
      // Don't break - check for .field next in the loop
      continue;
    }

    // Bind: x :: expr, x := expr, x : T = expr, x : T : expr
    // Or destructure: .{q, r} := expr, .{q, r} :: expr
    // Only at top-level expression (PREC_NONE)
    if ((t->kind == ColonColon || t->kind == ColonEqual || t->kind == Colon) &&
        (left->kind == expr_Ident || left->kind == expr_Product) &&
        min_prec == PREC_NONE) {

      bool is_destructure = (left->kind == expr_Product);

      // Validate destructure pattern up front
      if (is_destructure) {
        if (t->kind == Colon) {
          parser_error(p, t->span,
                       "destructuring patterns cannot have type annotations");
          break;
        }
        if (!is_valid_binding_pattern(left)) {
          parser_error(p, t->span, "invalid destructuring pattern");
          break;
        }
      }

      if (t->kind == ColonColon) {
        advance(p);
        Visibility vis = parse_optional_visibility(p);
        struct Expr *value = parse_expr_prec(p, PREC_NONE);

        if (is_destructure) {
          struct Expr *e = alloc_expr(p, expr_DestructureBind, left->span);
          e->destructure.pattern = left;
          e->destructure.value = value;
          e->destructure.is_const = true;
          left = e;
        } else {
          struct Expr *e = alloc_expr(p, expr_Bind, left->span);
          e->bind.kind = bind_Const;
          e->bind.name = (struct Identifier){.string_id = left->ident.string_id,
                                             .span = left->span};
          e->bind.type_ann = NULL;
          e->bind.value = value;
          e->bind.visibility = vis;
          left = e;
        }
        break;
      }

      if (t->kind == ColonEqual) {
        advance(p);
        
        Visibility vis = parse_optional_visibility(p);

        struct Expr *value = parse_expr_prec(p, PREC_NONE);

        if (is_destructure) {
          struct Expr *e = alloc_expr(p, expr_DestructureBind, left->span);
          e->destructure.pattern = left;
          e->destructure.value = value;
          e->destructure.is_const = false;
          left = e;
        } else {
          struct Expr *e = alloc_expr(p, expr_Bind, left->span);
          e->bind.kind = bind_Var;
          e->bind.name = (struct Identifier){.string_id = left->ident.string_id,
                                             .span = left->span};
          e->bind.type_ann = NULL;
          e->bind.value = value;
          e->bind.visibility = vis;
          left = e;
        }
        break;
      }

      if (t->kind == Colon) {
        // Type annotations only apply to single-identifier bindings, not
        // destructures (already errored above for destructures)
        advance(p);
        struct Expr *type = parse_expr_prec(p, PREC_ASSIGN);

        enum BindKind kind;
        if (match(p, Equal)) {
          kind = bind_Var;
        } else if (match(p, Colon)) {
          kind = bind_Const;
        } else {
          parser_error(p, t->span, "expected '=' or ':' after type annotation");
          break;
        }

        struct Expr *value = parse_expr_prec(p, PREC_NONE);
        struct Expr *e = alloc_expr(p, expr_Bind, left->span);
        e->bind.kind = kind;
        e->bind.name = (struct Identifier){.string_id = left->ident.string_id,
                                           .span = left->span};
        e->bind.type_ann = type;
        e->bind.value = value;
        left = e;
        break;
      }
    }

    // Binary operators
    enum Precedence prec = get_precedence(t->kind);
    if (prec <= min_prec)
      break;

    enum TokenKind op = t->kind;
    advance(p);

    // Right-associative operators: =, <-, +=, -=, *=, /=, %=, **
    bool right_assoc =
        (op == Equal || op == LeftArrow || op == PlusEqual ||
         op == MinusEqual || op == StarEqual || op == ForwardSlashEqual ||
         op == PercentEqual || op == PipeEqual || op == AmpersandEqual ||
         op == StarStar);
    struct Expr *right = parse_expr_prec(p, right_assoc ? prec - 1 : prec);

    if (prec == PREC_ASSIGN) {
      bool is_lvalue =
          left->kind == expr_Ident || left->kind == expr_Field ||
          left->kind == expr_Index || left->kind == expr_Wildcard ||
          (left->kind == expr_Unary && left->unary.op == unary_Deref);

      if (!is_lvalue) {
        parser_error(p, left->span, "invalid assignment target");
        return NULL;
      }

      if (op != Equal && op != LeftArrow) {
        struct Expr *bin_expr = alloc_expr(p, expr_Bin, left->span);
        bin_expr->bin.Left = left;
        bin_expr->bin.Right = right;

        switch (op) {
        case PlusEqual:
          bin_expr->bin.op = Plus;
          break;
        case MinusEqual:
          bin_expr->bin.op = Minus;
          break;
        case StarEqual:
          bin_expr->bin.op = Star;
          break;
        case ForwardSlashEqual:
          bin_expr->bin.op = ForwardSlash;
          break;
        case PercentEqual:
          bin_expr->bin.op = Percent;
          break;
        case PipeEqual:
          bin_expr->bin.op = Pipe;
          break;
        case AmpersandEqual:
          bin_expr->bin.op = Ampersand;
          break;
        default:
          break;
        }
        right = bin_expr;
      }

      struct Expr *assign_expr = alloc_expr(p, expr_Assign, left->span);
      assign_expr->assign.target = left;
      assign_expr->assign.value = right;
      left = assign_expr;

    } else {
      struct Expr *bin = alloc_expr(p, expr_Bin, left->span);
      bin->bin.op = op;
      bin->bin.Left = left;
      bin->bin.Right = right;
      left = bin;
    }
  }

  return left;
}

Vec *parse(struct Parser *p) {
  Vec *stmts = vec_new_in(p->arena, sizeof(struct Expr *));

  while (!check(p, Eof)) {
    size_t pos_before = p->current;
    struct Expr *expr = parse_expr_prec(p, PREC_NONE);
    if (expr) {
      vec_push(stmts, &expr);
    }
    // Consume semicolon between top-level expressions
    match(p, Semicolon);
    // Safety: if no progress was made, skip token to prevent infinite loop
    if (p->current == pos_before)
      advance(p);
  }

  return stmts;
}
