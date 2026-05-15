# Ore Parser Grammar Mapping

This is the planning doc for refactoring `src/parser/parse_*.c` to match
Ore's actual syntax. Distilled from `parser.c.backup` (the working old
parser) with the new token/AST kinds substituted in.

The Gemini-generated `parse_decl.c` / `parse_expr.c` / `parse_stmt.c` got
the **plumbing** right (cursor primitives, Pratt loop shape, AstNodeData
extras packing, span recording) but the **grammar** wrong: it's
C/Zig-style `[pub] fn NAME(...)`, when Ore is Odin-style `NAME ::
<body>`. As a result, every decl other than `fn` is silently dropped.

---

## 1 · Top-level grammar (one paragraph)

There is **no separate top-level decl path**. Top-level parses by
calling `parse_expr(p, PREC_NONE)` in a loop. Bindings emerge as
**infix forms** inside the Pratt parser:

```
<ident> :: <body>           — const bind         (DECL_CONST or DECL_FN/STRUCT/... by RHS kind)
<ident> := <body>           — var bind           (DECL_VAL)
<ident> : T : <body>        — typed const bind
<ident> : T = <body>        — typed var bind
<ident> : T                 — typed declaration without initializer
.{pat} := <body>            — destructure var
.{pat} :: <body>            — destructure const
<pub>                       — modifier; appears AFTER `::` / `:=`, not before the name
```

`pub` is **not** a prefix keyword on the LHS — it sits between the bind
operator and the value: `add :: pub fn(...) { ... }`. That falls out of
treating bindings as expressions: the LHS is just an identifier
(possibly with a type annotation), and the RHS is a full expression that
happens to be allowed to lead with `pub`.

The RHS of `::` decides the decl kind:

| RHS form                  | Decl kind            |
|---------------------------|----------------------|
| `fn(...) -> T body`       | `AST_DECL_FN`        |
| `struct { ... }`          | `AST_DECL_STRUCT`    |
| `enum { ... }`            | `AST_DECL_ENUM`      |
| `union { ... }`           | `AST_DECL_UNION`     |
| `effect { ... }` (& mods) | `AST_DECL_EFFECT`    |
| `handler { ... }`         | `AST_EXPR_HANDLER`   |
| `distinct T`              | `AST_DECL_DISTINCT`  |
| any other expression      | `AST_DECL_CONST`     |

This means **`fn`, `struct`, `enum`, `union`, `effect`, `handler`,
`distinct` are all `parse_primary` cases** — they're expressions that
*can* sit on the RHS of a bind, or be passed around as values. The
"binding" classification happens *after* parsing the RHS, by inspecting
its kind.

---

## 2 · Token kind → AST kind reference

### 2.1 · Prefix forms (`parse_primary` cases)

| Token       | Production                                                          | AST kind                |
|-------------|---------------------------------------------------------------------|-------------------------|
| INT_LIT     | `42`                                                                | `LIT_INT`               |
| FLOAT_LIT   | `3.14`                                                              | `LIT_FLOAT`             |
| STRING_LIT  | `"hi"`                                                              | `LIT_STRING`            |
| BYTE_LIT    | `'a'`                                                               | (new — `LIT_BYTE`)      |
| ASM_LIT     | `` `...` ``                                                         | (new — `EXPR_ASM`)      |
| TRUE/FALSE  | `true`, `false`                                                     | `LIT_BOOL`              |
| NIL         | `nil`                                                               | `LIT_NIL`               |
| VOID/NORETURN/ANYTYPE/TYPE | type-position idents                                 | `TYPE_PATH`             |
| UNDERSCORE  | `_`                                                                 | (new — `EXPR_WILDCARD`) |
| IDENTIFIER  | `foo`                                                               | `EXPR_PATH`             |
| LPAREN      | `(expr)`                                                            | `EXPR_GROUP`            |
| LBRACE      | `{ stmts }` — block-expression (last value is the result)           | `STMT_BLOCK`            |
| FN_TYPE     | `Fn(T1, T2) -> R` — type-position only                              | `TYPE_FN`               |
| FN          | `fn(params) <effects> -> T body` — lambda/fn literal                | `EXPR_LAMBDA` *(new)*   |
| HANDLE / HANDLER | `handle(target) { ... }` / `handler { ... }`                   | `EXPR_HANDLE` / `EXPR_HANDLER` |
| MASK        | `mask<E>{body}` / `mask behind<E>{body}`                            | `EXPR_MASK`             |
| WITH        | `with caller`  — consumes rest-of-block as cont                     | desugars to `CALL`      |
| IF / ELIF   | `if (cond) then else else_branch` — note PARENS around cond         | `STMT_IF`               |
| LOOP        | `loop body` / `loop (cond) body` / `loop (init; cond; step) body`   | `STMT_LOOP`             |
| LBRACKET    | `[]T`, `[N]T`, `[^]T`, `[_]T{...}`, `[N]T{...}`                     | `TYPE_SLICE`, `TYPE_ARRAY`, `TYPE_MANYPTR` *(new)*, array literal `EXPR_ARRAY_LIT` *(new)* |
| DOT         | `.{ fields }` — anonymous product / `.Variant` — enum ref           | `EXPR_PRODUCT`, `EXPR_ENUM_REF` *(new)* |
| BREAK / CONTINUE | `break` / `continue`                                           | `STMT_BREAK` / `STMT_CONTINUE` |
| DEFER       | `defer expr`                                                        | `STMT_DEFER`            |
| RETURN      | `return [expr]`                                                     | `STMT_RETURN`           |
| STRUCT      | `struct { fields }`                                                 | `DECL_STRUCT`           |
| ENUM        | `enum { variants }`                                                 | `DECL_ENUM`             |
| UNION       | (only inside `struct { ... }`)                                      | embedded in struct body |
| EFFECT / NAMED / SCOPED / LINEAR | `[named] [scoped] [linear] effect [in T] { ops }`      | `DECL_EFFECT`           |
| SWITCH      | `switch (expr) { pat \| pat => body, ... }`                         | `STMT_SWITCH` *(new)*   |
| AT          | `@name(args)` — compiler builtin                                    | `EXPR_BUILTIN` *(new)*  |
| COMPTIME    | `comptime expr` — marks `is_comptime` on the inner expr             | flag, not its own kind  |
| MINUS, BANG, TILDE, AMP, STAR, CARET, QUESTION | prefix unary ops                       | various `EXPR_UNARY_*`  |
| CONST       | `const T` — used in type position                                   | (new — `TYPE_CONST`)    |

### 2.2 · Infix / postfix forms (Pratt loop in `parse_expr_prec`)

| Token              | Pattern              | AST kind                | Precedence | Assoc |
|--------------------|----------------------|-------------------------|------------|-------|
| LPAREN             | `f(args)`            | `EXPR_CALL`             | POSTFIX    | left  |
| LBRACE             | `f { ... }` trailing-lambda — wraps as zero-arg lambda call | desugars to `CALL` | POSTFIX | left |
| LBRACE (after type-shape) | `T{ .x = y }` struct literal | `EXPR_PRODUCT`   | POSTFIX | left  |
| DOT                | `x.field` / `x.0`    | `EXPR_FIELD`            | POSTFIX    | left  |
| RARROW             | `x->field` — equivalent to `x.field` (auto-deref) | `EXPR_FIELD` | POSTFIX | left |
| LBRACKET           | `x[i]` / `x[a..b]` / `x[..b]` / `x[a..]` | `EXPR_INDEX`, `EXPR_SLICE` *(new)* | POSTFIX | left |
| PLUS_PLUS          | `x++` postfix increment | `EXPR_UNARY_INC` *(new)* | POSTFIX | left |
| CARET              | `x^` postfix deref   | `EXPR_UNARY_DEREF`      | POSTFIX    | left  |
| QUESTION           | `x?` unwrap-or-trap optional | `EXPR_UNARY_DENIL` *(new)* | POSTFIX | left |
| BANG               | `x!` unwrap-or-trap error    | `EXPR_UNARY_DEERR` *(new)* | POSTFIX | left |
| COLON_COLON, COLON_EQ, COLON | `name :: rhs`, `name := rhs`, `name : T := rhs` etc | `DECL_*` (see §3) | NONE-only | — |
| ORELSE             | `a orelse b`         | `EXPR_BIN_ORELSE` *(new)* | OR       | left  |
| CATCH              | `a catch b`          | `EXPR_BIN_CATCH` *(new)* | OR        | left  |
| PIPE_PIPE          | `a \|\| b`           | `EXPR_BIN_OR`           | OR         | left  |
| AMP_AMP            | `a && b`             | `EXPR_BIN_AND`          | AND        | left  |
| EQ_EQ, BANG_EQ     | `==`, `!=`           | `EXPR_BIN_EQ/NEQ`       | EQUALITY   | left  |
| LT, LE, GT, GE     | comparisons          | `EXPR_BIN_LT/...`       | COMPARISON | left  |
| PIPE, AMP, CARET, TILDE | bitwise          | `EXPR_BIN_BIT_OR/AND/XOR` | BITWISE  | left  |
| SHL, SHR           | `<<`, `>>`           | `EXPR_BIN_SHL/SHR`      | SHIFT      | left  |
| PLUS, MINUS        | `+`, `-`             | `EXPR_BIN_ADD/SUB`      | TERM       | left  |
| STAR, SLASH, PERCENT | `*`, `/`, `%`      | `EXPR_BIN_MUL/DIV/MOD`  | FACTOR     | left  |
| STAR_STAR          | `**`                 | `EXPR_BIN_POW` *(new)*  | POWER      | right |
| EQ, LARROW, PLUS_EQ, MINUS_EQ, STAR_EQ, SLASH_EQ, PERCENT_EQ, PIPE_EQ, AMP_EQ, CARET_EQ | assignments | `EXPR_ASSIGN[_OP]` | ASSIGN | right |

### 2.3 · Precedence levels (top of file, in source order)

```
PREC_NONE       — top-level / statement
PREC_ASSIGN     — = += -= *= /= %= |= &= ^=  (right-assoc)
PREC_OR         — || orelse catch
PREC_AND        — &&
PREC_EQUALITY   — == !=
PREC_COMPARISON — < <= > >=
PREC_BITWISE    — | & ^ ~
PREC_SHIFT      — << >>
PREC_TERM       — + -
PREC_FACTOR     — * / %
PREC_POWER      — **                          (right-assoc)
PREC_UNARY      — prefix - ! ~ & * ^ ?
PREC_POSTFIX    — call, field, index, slice, ++ ^ ? !
```

The current `parse_expr.c` is missing several of these (notably POWER as
right-assoc, ORELSE, CATCH, postfix `?`/`!`/`++`/`^`, and the entire
range/slice path). The Pratt skeleton is correct — the table just needs
filling in.

---

## 3 · Bind / decl recognition (the critical missing piece)

The new parser must implement, **inside the Pratt loop**, the
infix-bind handling that today lives at backup:2683-2788. Triggered
only when:

- `min_prec == PREC_NONE` (i.e. top-level / statement position)
- `left.kind == EXPR_PATH` or `left.kind == EXPR_PRODUCT` (destructure)
- next token is one of `::`, `:=`, `:`

### 3.1 · `name :: <rhs>`

```c
advance ::                              // consume the operator
Visibility vis = parse_optional_vis();  // accept `pub`
AstNodeId rhs = parse_expr(p, PREC_NONE);
// dispatch by rhs.kind:
//   EXPR_LAMBDA   → DECL_FN     (extras: [name, sig_expr, body])
//   EXPR_STRUCT   → DECL_STRUCT (extras: [name, members...])
//   EXPR_ENUM     → DECL_ENUM   (extras: [name, variants...])
//   EXPR_EFFECT   → DECL_EFFECT (extras: [name, modifiers, ops...])
//   EXPR_HANDLER  → DECL_HANDLER (extras: [name, branches...])
//   EXPR_DISTINCT → DECL_DISTINCT(extras: [name, inner_type])
//   else          → DECL_CONST  (extras: [name, value])
expect ;                                // layout-injected
```

For top-level (i.e. inside `parse_top_level_decls`), also push a
`TopLevelEntry`. For inner bindings (statement-level), don't.

### 3.2 · `name := <rhs>`

Same shape, kind always `DECL_VAL` (mutable var). Top-level `:=` is
legal but rare — module-level mutables.

### 3.3 · `name : T : <rhs>` / `name : T = <rhs>` / `name : T`

Three-form lookahead after the `:`:
- another `:` → const-typed-bind
- `=`         → var-typed-bind
- (next token doesn't match either) → typed declaration with no initializer (param shape)

### 3.4 · Destructure

LHS is `EXPR_PRODUCT` with all sub-fields being valid binding patterns
(idents or nested products). Same dispatch on `::` vs `:=`. No type
annotations on destructures.

---

## 4 · Mechanics worth preserving from the backup

These come up in the backup in non-obvious ways. Carry them over.

### 4.1 · `parsing_type` flag

A bool on `Parser` that gets toggled `true` while parsing the RHS of `:`,
`->`, `[N]T`, and inside `Fn(...)`. In type position, several
expressions parse differently:
- `[_]T` is forbidden (inferred-size arrays are value-only)
- `T{...}` literal disambiguation is suppressed — `[^]T{...}` is an error
- Some unary operators (`?`, `^`) parse as type constructors instead of
  postfix on a value

The current parser doesn't track this. Add a `bool parsing_type` to the
`Parser` struct and save/restore it around recursive calls per the
backup's pattern.

### 4.2 · `allow_trailing_lam` flag

Disables the postfix trailing-lambda inside `with` (so the `with` body
isn't double-eaten) and inside type contexts. Save/restore around the
`with` call.

### 4.3 · Synthetic `;` swallowing before `}`

The layout pass injects a `;` before every `}`. Comma-separated list
parsers that close with `}` need a `skip_semicolons(p)` call before
`p_consume(TK_RBRACE, ...)`. Block-shaped iterators that already do
`p_match(TK_SEMI)` per iteration don't.

### 4.4 · Synthetic-vs-source `{` disambiguation

The trailing-lambda rule needs to **not** fire on a layout-injected `{`
(which is a function body, not a `{...}` block-literal). The backup
checked `token.origin == Source`. With the new synthetic convention
(`tok.start == tok.byte_end`, predicate `token_is_synthetic`), the
check is `!token_is_synthetic(t)`.

### 4.5 · `Type { .field = val }` lookahead

Three-token lookahead inside the Pratt loop:
```
if next3 are  LBRACE Dot Identifier Equal  → struct literal, not trailing-lambda
```
This is how `Vec2{ .x = 1, .y = 2 }` parses as a product literal
*through the call/postfix slot*, distinguishing it from `f { ... }`
trailing-lambda.

### 4.6 · `if (cond) then else else_branch`

The condition is in **parens** (`if (cond)`). The current
`parse_stmt.c` `case TK_IF` doesn't expect parens. Match the backup:

```c
advance(TK_IF);
expect(TK_LPAREN);
cond = parse_expr(PREC_NONE);
expect(TK_RPAREN);
then = parse_expr(PREC_NONE);
if check(TK_ELIF):  else = parse_primary(p);     // chain
else if match(TK_ELSE):  else = parse_expr(PREC_NONE);
```

### 4.7 · `loop`

Three shapes:
- `loop body`                          — infinite
- `loop (cond) body`                   — while
- `loop (init; cond; step) body`       — C-style for

`init` is detected by parsing the parenthesized first expression and
checking whether its kind is `DECL_VAL`/`DECL_CONST` (i.e. a bind
emerged) — if so it's the C-style form.

Plus optional `|capture|` after the parens for nullable-capture: `loop
(maybe_value) |x| body`.

### 4.8 · `with` desugaring

`with caller(...) ; rest_of_block` becomes
`caller(args..., fn() { rest_of_block })`. The `with` head consumes the
rest of the enclosing block as its lambda body. If the caller's parse
result is already a `Call`, append the lambda to its args; otherwise
wrap. See backup:1721-1789.

### 4.9 · Effect modifiers in canonical order

`named scoped linear effect`, in that order. Any other ordering is an
error.

### 4.10 · `mask<E>` and `mask behind<E>`

`behind` is a contextual keyword — recognized by string_id compare
against `s->names.behind`, not by TokenKind.

### 4.11 · Op decls inside effect bodies

`name :: [pub] (fn | ctl | final ctl | raw ctl | val) (params) -> T`
inside `effect { ... }`. `val` ops take no params; `fn`/`ctl` ops take
mandatory-typed params + arrow. Linear effects forbid `ctl` family.
See backup:965-1077.

---

## 5 · What's salvageable from the current parse_expr.c

Most of the **Pratt skeleton**. Keep:
- The `parse_expr(p, precedence) → parse_prefix; loop parse_infix` shape
- The `op_index = p->pos` capture for `main_token` pre-advance
- The `left_span = vec_get(&mod->span_map, left.idx)` re-read after
  pushing — that's correct
- The extras packing patterns (`[count, ...items]` for variable-length,
  fixed-position fields for if/loop/fn)
- All literal/ident prefix cases (1:1 carryover)
- All binary op cases (1:1 carryover)
- Function-call infix handler

Replace:
- The precedence table — needs ORELSE, CATCH, POWER (right-assoc),
  postfix-only entries removed (they're not precedence-driven)
- TK_DOT and TK_LBRACKET in the precedence table — currently fall
  through to `get_binary_op_kind` which returns `AST_ERROR`. Move to
  proper postfix handlers.
- The whole prefix-unary switch — needs CARET (`^T`), QUESTION (`?T`),
  CONST (`const T`), and the value-vs-type-position dispatch on STAR/AMP

Add:
- Postfix `?`, `!`, `++`, `^`
- Slice/index/range in `[...]` postfix
- Field access `.` and `->` postfix
- Trailing-lambda recognition (LBRACE / FN after a primary, with the
  `Type{ .field = }` lookahead carve-out)
- The bind infix at PREC_NONE

---

## 6 · What's salvageable from the current parse_decl.c

Less. Replace `parse_decl` entirely — the C/Zig-style dispatch is wrong.
The new `parse_top_level_decls` body is essentially:

```c
while (!p_is_eof(p)) {
    size_t before = p->pos;
    AstNodeId decl = parse_expr(&p, PREC_NONE);
    // Bind-emitting infix already pushed a TopLevelEntry if it was a
    // top-level bind. Anything else at top-level is a parse error.
    if (decl.idx == 0 && p->pos == before) p_advance(&p);  // forward-progress
    p_match(&p, TK_SEMI);  // optional synthetic
}
```

`parse_type` becomes a thin wrapper that calls `parse_expr(p,
PREC_BITWISE)` with `parsing_type = true` saved/restored. There's no
separate type grammar — types and expressions share the same parser,
disambiguated by the flag.

Drop `parse_fn_decl` — `fn` is a `parse_primary` case that returns an
`EXPR_LAMBDA`, and the bind handler decides whether to wrap it in
`DECL_FN`.

---

## 7 · What's salvageable from the current parse_stmt.c

Mostly. Statements at block-level are just `parse_expr(p, PREC_NONE)`
followed by `match(TK_SEMI)`. The dedicated `parse_stmt` switch in the
current code is a *parse-decl-style* dispatch that does more than it
should — return/break/continue/defer/if/loop should be `parse_primary`
cases (they are in the backup), not statement cases. The statement
parser becomes:

```c
AstNodeId parse_stmt(Parser *p) {
    AstNodeId e = parse_expr(p, PREC_NONE);
    p_match(p, TK_SEMI);
    return e;
}
```

`parse_block` is correct in shape — keep it, but make sure the body
loop calls `parse_stmt` (or just `parse_expr(PREC_NONE) + match SEMI`
inline). Move `RETURN`, `IF`, `LOOP`, etc. into `parse_primary`.

---

## 8 · Missing AST kinds to add

Current `ast.h` is missing kinds for several productions. Add:

```c
// Decls
AST_DECL_HANDLER,
AST_DECL_DISTINCT,
AST_DECL_TYPE,                // `type X :: <body>` form, if we keep it separate
AST_DECL_VAL,                 // already present, but reuse for var binds — check
// Statements
AST_STMT_SWITCH,
// Expressions
AST_EXPR_LIT_BYTE,
AST_EXPR_ASM,
AST_EXPR_WILDCARD,            // `_`
AST_EXPR_LAMBDA,              // `fn(...) body` literal
AST_EXPR_HANDLER,             // `handler { ... }` value
AST_EXPR_HANDLE,              // `handle(target) { ... }`
AST_EXPR_MASK,
AST_EXPR_PRODUCT,             // `.{ ... }` and `T{ ... }`
AST_EXPR_ENUM_REF,            // `.Variant`
AST_EXPR_ARRAY_LIT,
AST_EXPR_SLICE,               // `x[a..b]`
AST_EXPR_BUILTIN,             // `@name(args)`
AST_EXPR_EFFECT_ROW,          // `<H | e>`
AST_EXPR_BIN_POW,             // `**`
AST_EXPR_BIN_ORELSE,
AST_EXPR_BIN_CATCH,
AST_EXPR_UNARY_INC,           // postfix `++`
AST_EXPR_UNARY_DENIL,         // postfix `?`
AST_EXPR_UNARY_DEERR,         // postfix `!`
AST_EXPR_UNARY_OPTIONAL,      // prefix `?T` in type position
AST_EXPR_UNARY_PTR,           // prefix `^T` in type position
// Types
AST_TYPE_MANYPTR,             // `[^]T`
AST_TYPE_CONST,               // `const T`
```

We don't need a comptime kind — keep the backup's pattern of a
`is_comptime` flag, which here means a parallel `Vec<bool>` keyed by
AstNodeId in `ModuleInfo` (cheap, doesn't bloat AstNodeData).

---

## 9 · Order of operations for the rewrite

1. **Add missing AST kinds** to `ast.h`. ~25 enum entries.
2. **Add `parsing_type` and `allow_trailing_lam` flags** to `Parser`.
3. **Rewrite the precedence table** in `parse_expr.c`. Add ORELSE,
   CATCH, POWER (right-assoc). Remove DOT/LBRACKET from precedence —
   they're postfix.
4. **Rewrite `parse_primary`** to dispatch all keyword-led forms:
   fn, struct, enum, union(only embedded), effect (with modifiers),
   handle, handler, mask, with, if, elif, loop, switch, break,
   continue, defer, return, @builtin, comptime, `.{...}`, `.Variant`,
   `[...]T` family, `Fn(...)`.
5. **Rewrite `parse_infix`** with proper postfix arms for `(`, `{`
   (trailing-lambda + struct-literal lookahead), `.`, `->`, `[`,
   `++`, `^`, `?`, `!`, plus all binary ops (largely from the current
   code).
6. **Add the bind infix** at PREC_NONE for `::`, `:=`, `:`. Dispatch
   on RHS kind to decide DECL_*.
7. **Simplify `parse_stmt`** to `parse_expr + match(SEMI)`.
8. **Simplify `parse_top_level_decls`** to a loop over `parse_expr +
   match(SEMI)`. Top-level binds push `TopLevelEntry`; non-binds at
   top-level error.
9. **Walk all 46 example .ore files**. Verify zero `AST_ERROR` nodes
   in the output of well-formed inputs, and that `top_level_indices`
   contains the right names + visibilities.

---

## 10 · Out of scope (defer)

- **Doc-comment association**: trivia already attaches via
  `trivia_offsets`. The doc-extraction renderer reads it later.
- **`with` desugaring**: shape is known (§4.8) but can be a follow-up
  if it complicates step 4. Mark it `AST_EXPR_WITH` as a passthrough
  for now and desugar in lowering.
- **Effect rows `<H|e>` in generic position**: only legal in the
  post-`fn(...)` slot. Reject elsewhere with a parser error rather
  than trying to disambiguate `<`.
- **Diagnostic shape**: `p_error` is a no-op stub today. Wire to
  `db_diag_error_*` when the diag subsystem is plumbed through —
  layout/lexer have the same TODO.
