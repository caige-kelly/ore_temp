#define BUILTIN_LIST(X) \
    X(SIZEOF, "sizeOf") \
    X(ALIGNOF, "alignOf") \
    X(TYPEOF, "typeOf")

#define CONTEXT_LIST(X) \
    X(VAL, "val") \
    X(CTL, "ctl") \
    X(FINAL, "final") \
    X(PUB, "pub") \
    X(PVT, "pvt") \
    X(ABSTRACT, "abstract") \
    X(NAMED, "named") \
    X(SCOPED, "scoped") \
    X(LINEAR, "linear")