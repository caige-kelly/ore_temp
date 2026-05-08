// HIR dumper for `--dump-hir`. See dump.h for context.

#include "dump.h"

#include <stdio.h>

#include "../common/hashmap.h"
#include "../common/stringpool.h"
#include "../sema/sema.h"
#include "../sema/type/type.h"
#include "hir.h"

static void print_indent(int n) {
  for (int i = 0; i < n; i++)
    printf("  ");
}

static const char *def_name(struct Sema *s, DefId d) {
  if (!s || !def_id_is_valid(d))
    return "?";
  struct DefInfo *di = def_info(s, d);
  if (!di)
    return "?";
  return pool_get(s->pool, di->name_id, 0);
}

static const char *type_str(struct Sema *s, struct Type *t, char *buf,
                            size_t cap) {
  if (!t)
    return "?";
  //return sema_type_display_name(s, t, buf, cap);
}

static void dump_instr(struct Sema *s, struct HirInstr *h, int indent);

static void dump_block(struct Sema *s, Vec *block, int indent) {
  if (!block)
    return;
  for (size_t i = 0; i < block->count; i++) {
    struct HirInstr **hp = (struct HirInstr **)vec_get(block, i);
    if (hp && *hp)
      dump_instr(s, *hp, indent);
  }
}

static const char *token_kind_name(enum TokenKind k) {
  return token_kind_to_str(k);
}

static const char *unary_op_name(enum UnaryOp op) {
  switch (op) {
  case unary_Ref:
    return "&";
  case unary_Deref:
    return "*";
  case unary_Ptr:
    return "^";
  case unary_ManyPtr:
    return "[^]";
  case unary_Neg:
    return "-";
  case unary_Not:
    return "!";
  case unary_BitNot:
    return "~";
  case unary_Const:
    return "const";
  case unary_Optional:
    return "?";
  case unary_Inc:
    return "++";
  case unary_DeNil:
    return "?";
  case unary_Dec:
    return "--";
  return "?";
  }
}

static void dump_instr(struct Sema *s, struct HirInstr *h, int indent) {
  if (!h)
    return;
  char tbuf[128];
  print_indent(indent);
  printf("%s : %s", hir_kind_str(h->kind),
         type_str(s, h->type, tbuf, sizeof(tbuf)));

  switch (h->kind) {
  case HIR_REF:
    if (def_id_is_valid(h->ref.def)) {
      printf("  -> %s", def_name(s, h->ref.def));
    }
    printf("\n");
    return;
  case HIR_BIND:
    if (def_id_is_valid(h->bind.def)) {
      printf("  '%s'", def_name(s, h->bind.def));
    }
    printf("\n");
    if (h->bind.init) {
      print_indent(indent + 1);
      printf("init:\n");
      dump_instr(s, h->bind.init, indent + 2);
    }
    return;
  case HIR_BIN:
    printf("  %s\n", token_kind_name(h->bin.op));
    print_indent(indent + 1);
    printf("left:\n");
    dump_instr(s, h->bin.left, indent + 2);
    print_indent(indent + 1);
    printf("right:\n");
    dump_instr(s, h->bin.right, indent + 2);
    return;
  case HIR_UNARY:
    printf("  %s%s\n", h->unary.postfix ? "postfix " : "",
           unary_op_name(h->unary.op));
    print_indent(indent + 1);
    printf("operand:\n");
    dump_instr(s, h->unary.operand, indent + 2);
    return;
  case HIR_ASSIGN:
    printf("\n");
    print_indent(indent + 1);
    printf("target:\n");
    dump_instr(s, h->assign.target, indent + 2);
    print_indent(indent + 1);
    printf("value:\n");
    dump_instr(s, h->assign.value, indent + 2);
    return;
  case HIR_FIELD:
    printf("  .%s\n", h->field.field_name_id
                          ? pool_get(s->pool, h->field.field_name_id, 0)
                          : "?");
    print_indent(indent + 1);
    printf("object:\n");
    dump_instr(s, h->field.object, indent + 2);
    return;
  case HIR_INDEX:
    printf("\n");
    print_indent(indent + 1);
    printf("object:\n");
    dump_instr(s, h->index.object, indent + 2);
    print_indent(indent + 1);
    printf("index:\n");
    dump_instr(s, h->index.index, indent + 2);
    return;
  case HIR_PRODUCT:
    printf("\n");
    if (h->product.fields) {
      for (size_t i = 0; i < h->product.fields->count; i++) {
        struct HirInstr **fp =
            (struct HirInstr **)vec_get(h->product.fields, i);
        print_indent(indent + 1);
        printf("[%zu]:\n", i);
        if (fp && *fp)
          dump_instr(s, *fp, indent + 2);
      }
    }
    return;
  case HIR_ARRAY_LIT:
    printf("\n");
    if (h->array_lit.size) {
      print_indent(indent + 1);
      printf("size:\n");
      dump_instr(s, h->array_lit.size, indent + 2);
    }
    if (h->array_lit.initializer) {
      print_indent(indent + 1);
      printf("init:\n");
      dump_instr(s, h->array_lit.initializer, indent + 2);
    }
    return;
  case HIR_ENUM_REF:
    printf("  .%s\n", h->enum_ref.variant_name_id
                          ? pool_get(s->pool, h->enum_ref.variant_name_id, 0)
                          : "?");
    return;
  case HIR_IF:
    printf("\n");
    print_indent(indent + 1);
    printf("cond:\n");
    dump_instr(s, h->if_instr.condition, indent + 2);
    print_indent(indent + 1);
    printf("then:\n");
    dump_block(s, h->if_instr.then_block, indent + 2);
    if (h->if_instr.else_block) {
      print_indent(indent + 1);
      printf("else:\n");
      dump_block(s, h->if_instr.else_block, indent + 2);
    }
    return;
  case HIR_LOOP:
    printf("\n");
    if (h->loop.init) {
      print_indent(indent + 1);
      printf("init:\n");
      dump_instr(s, h->loop.init, indent + 2);
    }
    if (h->loop.condition) {
      print_indent(indent + 1);
      printf("cond:\n");
      dump_instr(s, h->loop.condition, indent + 2);
    }
    if (h->loop.step) {
      print_indent(indent + 1);
      printf("step:\n");
      dump_instr(s, h->loop.step, indent + 2);
    }
    print_indent(indent + 1);
    printf("body:\n");
    dump_block(s, h->loop.body_block, indent + 2);
    return;
  case HIR_SWITCH:
    printf("\n");
    print_indent(indent + 1);
    printf("scrutinee:\n");
    dump_instr(s, h->switch_instr.scrutinee, indent + 2);
    if (h->switch_instr.arms) {
      for (size_t i = 0; i < h->switch_instr.arms->count; i++) {
        struct HirSwitchArm **ap =
            (struct HirSwitchArm **)vec_get(h->switch_instr.arms, i);
        if (!ap || !*ap)
          continue;
        print_indent(indent + 1);
        printf("arm[%zu]:\n", i);
        if ((*ap)->patterns) {
          for (size_t j = 0; j < (*ap)->patterns->count; j++) {
            struct HirInstr **pp =
                (struct HirInstr **)vec_get((*ap)->patterns, j);
            print_indent(indent + 2);
            printf("pattern:\n");
            if (pp && *pp)
              dump_instr(s, *pp, indent + 3);
          }
        }
        print_indent(indent + 2);
        printf("body:\n");
        dump_block(s, (*ap)->body_block, indent + 3);
      }
    }
    return;
  case HIR_RETURN:
    printf("\n");
    if (h->return_instr.value) {
      print_indent(indent + 1);
      printf("value:\n");
      dump_instr(s, h->return_instr.value, indent + 2);
    }
    return;
  case HIR_DEFER:
    printf("\n");
    if (h->defer.value) {
      print_indent(indent + 1);
      printf("value:\n");
      dump_instr(s, h->defer.value, indent + 2);
    }
    return;
  case HIR_TYPE_VALUE: {
    char tt[128];
    printf("  = %s\n", type_str(s, h->type_value.type, tt, sizeof(tt)));
    return;
  }
  case HIR_CALL:
    printf("\n");
    print_indent(indent + 1);
    printf("callee:\n");
    dump_instr(s, h->call.callee, indent + 2);
    if (h->call.args && h->call.args->count > 0) {
      for (size_t i = 0; i < h->call.args->count; i++) {
        struct HirInstr **ap = (struct HirInstr **)vec_get(h->call.args, i);
        print_indent(indent + 1);
        printf("arg[%zu]:\n", i);
        if (ap && *ap)
          dump_instr(s, *ap, indent + 2);
      }
    }
    return;
  case HIR_HANDLER_VALUE:
    printf("  effect=%s\n",
           def_id_is_valid(h->handler_value.effect_def)
               ? def_name(s, h->handler_value.effect_def)
               : "?");
    if (h->handler_value.operations) {
      for (size_t i = 0; i < h->handler_value.operations->count; i++) {
        struct HirHandlerOp **opp =
            (struct HirHandlerOp **)vec_get(h->handler_value.operations, i);
        if (!opp || !*opp)
          continue;
        print_indent(indent + 1);
        printf("op '%s' (%s):\n",
               def_id_is_valid((*opp)->op_def) ? def_name(s, (*opp)->op_def)
                                               : "?",
               (*opp)->is_ctl ? "ctl" : "fn");
        if ((*opp)->body_block) {
          print_indent(indent + 2);
          printf("body:\n");
          dump_block(s, (*opp)->body_block, indent + 3);
        }
      }
    }
    if (h->handler_value.initially_block) {
      print_indent(indent + 1);
      printf("initially:\n");
      dump_block(s, h->handler_value.initially_block, indent + 2);
    }
    if (h->handler_value.finally_block) {
      print_indent(indent + 1);
      printf("finally:\n");
      dump_block(s, h->handler_value.finally_block, indent + 2);
    }
    if (h->handler_value.return_block) {
      print_indent(indent + 1);
      printf("return:\n");
      dump_block(s, h->handler_value.return_block, indent + 2);
    }
    return;
  case HIR_HANDLER_INSTALL:
    printf("  effect=%s%s%s\n",
           def_id_is_valid(h->handler_install.effect_def)
               ? def_name(s, h->handler_install.effect_def)
               : "?",
           def_id_is_valid(h->handler_install.binder) ? " binder=" : "",
           def_id_is_valid(h->handler_install.binder)
               ? def_name(s, h->handler_install.binder)
               : "");
    if (h->handler_install.handler) {
      print_indent(indent + 1);
      printf("handler:\n");
      dump_instr(s, h->handler_install.handler, indent + 2);
    }
    if (h->handler_install.body_block) {
      print_indent(indent + 1);
      printf("body:\n");
      dump_block(s, h->handler_install.body_block, indent + 2);
    }
    return;
  case HIR_OP_PERFORM:
    printf("  perform %s.%s\n",
           def_id_is_valid(h->op_perform.effect_def)
               ? def_name(s, h->op_perform.effect_def)
               : "?",
           def_id_is_valid(h->op_perform.op_def)
               ? def_name(s, h->op_perform.op_def)
               : "?");
    if (h->op_perform.args) {
      for (size_t i = 0; i < h->op_perform.args->count; i++) {
        struct HirInstr **ap =
            (struct HirInstr **)vec_get(h->op_perform.args, i);
        print_indent(indent + 1);
        printf("arg[%zu]:\n", i);
        if (ap && *ap)
          dump_instr(s, *ap, indent + 2);
      }
    }
    return;
  case HIR_BUILTIN:
    printf("  @%s\n",
           h->builtin.name_id ? pool_get(s->pool, h->builtin.name_id, 0) : "?");
    if (h->builtin.args) {
      for (size_t i = 0; i < h->builtin.args->count; i++) {
        struct HirInstr **ap = (struct HirInstr **)vec_get(h->builtin.args, i);
        print_indent(indent + 1);
        printf("arg[%zu]:\n", i);
        if (ap && *ap)
          dump_instr(s, *ap, indent + 2);
      }
    }
    return;
  case HIR_LAMBDA: {
    char rt[128];
    struct HirFn *fn = h->lambda.fn;
    printf("  %s -> %s\n", h->lambda.is_ctl ? "ctl" : "fn",
           fn ? type_str(s, fn->ret_type, rt, sizeof(rt)) : "?");
    if (fn) {
      if (fn->params) {
        for (size_t i = 0; i < fn->params->count; i++) {
          struct HirParam **pp = (struct HirParam **)vec_get(fn->params, i);
          if (!pp || !*pp)
            continue;
          char pt[128];
          print_indent(indent + 1);
          printf("param '%s' : %s\n",
                 def_id_is_valid((*pp)->def) ? def_name(s, (*pp)->def) : "?",
                 type_str(s, (*pp)->type, pt, sizeof(pt)));
        }
      }
      print_indent(indent + 1);
      printf("body:\n");
      dump_block(s, fn->body_block, indent + 2);
    }
    return;
  }
  case HIR_ASM:
    printf("  \"%s\"\n", h->asm_instr.string_id
                             ? pool_get(s->pool, h->asm_instr.string_id, 0)
                             : "");
    return;
  case HIR_CONST:
  case HIR_BREAK:
  case HIR_CONTINUE:
  case HIR_ERROR:
  default:
    printf("\n");
    return;
  }
}

static void dump_fn(struct Sema *s, struct HirFn *fn) {
  if (!fn)
    return;
  char rbuf[128];
  const char *name = def_name(s, fn->source);
  printf("Fn %s -> %s\n", name ? name : "?",
         type_str(s, fn->ret_type, rbuf, sizeof(rbuf)));
  dump_block(s, fn->body_block, 1);
}

void dump_hir(struct Sema *s) {
  if (!s)
    return;
  printf("\n=== hir ===\n");
  // Iterate by ModuleId — the new identity. module_hir is now keyed
  // by ModuleId.idx (uint64_t cast); the legacy struct Module*
  // pointer-keyed lookup is gone with the resolver.
  for (uint32_t i = 1; i < sema_module_count(s); i++) {
    ModuleId mid = (ModuleId){i};
    struct HirModule *hmod = (struct HirModule *)hashmap_get(
        &s->module_hir, (uint64_t)mid.idx);
    if (!hmod || !hmod->functions)
      continue;
    for (size_t j = 0; j < hmod->functions->count; j++) {
      struct HirFn **fp = (struct HirFn **)vec_get(hmod->functions, j);
      if (fp && *fp)
        dump_fn(s, *fp);
    }
  }
}
