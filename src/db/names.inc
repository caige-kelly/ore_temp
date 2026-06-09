// Identifier-spelled primitive type names (matches ip_primitives.def).
// Pre-interned at db_init so name resolution compares StrId.idx fields
// against s->names.X instead of doing strcmp on the buffer bytes.
#define PRIMITIVE_LIST(X) \
    X(BOOL,           "bool") \
    X(ANYTYPE,        "anytype") \
    X(VOID,           "void") \
    X(NORETURN,       "noreturn") \
    X(TYPE_NAME,      "type") \
    X(COMPTIME_INT,   "comptime_int") \
    X(COMPTIME_FLOAT, "comptime_float") \
    X(ERROR_NAME,     "error") \
    X(F32,            "f32") \
    X(F64,            "f64") \
    X(U8,             "u8") \
    X(U16,            "u16") \
    X(U32,            "u32") \
    X(U64,            "u64") \
    X(USIZE,          "usize") \
    X(I8,             "i8") \
    X(I16,            "i16") \
    X(I32,            "i32") \
    X(I64,            "i64") \
    X(ISIZE,          "isize")

#define BUILTIN_LIST(X) \
    X(SIZEOF,   "sizeOf") \
    X(ALIGNOF,  "alignOf") \
    X(TYPEOF,   "TypeOf") \
    X(TYPENAME, "typeName") \
    X(INTCAST,  "intCast") \
    X(PTRCAST,  "ptrCast") \
    X(IMPORT,   "import")

// Built-in field names — slice/array `.len`/`.ptr` etc. Pre-interned so
// field-access dispatch can compare StrId.idx instead of strcmp.
#define FIELD_LIST(X) \
    X(LEN, "len") \
    X(PTR, "ptr")

#define CONTEXT_LIST(X) \
    X(VAL, "val") \
    X(CTL, "ctl") \
    X(FINAL, "final") \
    X(FINAL_CTL, "final-ctl") \
    X(PUB, "pub") \
    X(PVT, "pvt") \
    X(ABSTRACT, "abstract") \
    X(NAMED, "named") \
    X(SCOPED, "scoped") \
    X(LINEAR, "linear") \
    X(DISTINCT, "distinct") \
    X(OVERRIDE, "override") \
    X(IN, "in")