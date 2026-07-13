/* ash_abi.h: the single source of ABI truth. Every party that touches an
 * Ashford value includes this header and nothing else defines a layout: the
 * runtime, the C that ashc emits, and any foreign host. A change here is a
 * change to the wire, so it lands here first and everywhere else follows.
 *
 * Everything crosses the boundary by value in an AshValue. Strings are a fat
 * value, pointer plus byte length, UTF-8, no terminator. Numeric widths are
 * fixed forever: Int is 64 bit signed, UInt 64 bit unsigned, Float is double,
 * Bool and Byte one byte, Char a 32 bit Unicode scalar value. */

#ifndef ASH_ABI_H
#define ASH_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AshTypeTag {
    ASH_TY_UNIT = 0,
    ASH_TY_INT,
    ASH_TY_UINT,
    ASH_TY_FLOAT,
    ASH_TY_BOOL,
    ASH_TY_BYTE,
    ASH_TY_CHAR,
    ASH_TY_STRING,
    ASH_TY_LIST,
    ASH_TY_MAP,
    ASH_TY_TUPLE,
    ASH_TY_OPTION,
    ASH_TY_RESULT,
    ASH_TY_RECORD,
    ASH_TY_PLEDGE_REF,
    ASH_TY_SUM,

    /* Internal only, never crosses the ABI. A signed instance handle inside a
     * compiled module: as.box holds the AshContract*. The language already
     * rejects a contract type at every boundary position, pledge signatures,
     * vows, record and variant fields, so no frame, vow, or descriptor value
     * ever carries this tag; it exists so a pledge body can hold the instance
     * a cross-contract sign returned. Deep copy is the handle copy, equality
     * is handle identity. */
    ASH_TY_INSTANCE
} AshTypeTag;

/* UTF-8 bytes, length counted in bytes, no NUL terminator. When the runtime
 * owns the bytes they live on a contract instance and die at break(). */
typedef struct AshString {
    uint8_t* ptr;
    uint64_t len;
} AshString;

typedef struct AshList {
    void*    data;
    uint64_t len;
    uint64_t cap;
    uint32_t elem_ty;
} AshList;

/* Lists carry their elements as a contiguous AshValue array behind data, len
 * of them live, cap allocated, elem_ty the declared element tag. A tuple
 * rides the same arm: data is the AshValue array, len is the arity, cap
 * equals len, and elem_ty is 0 because a tuple's elements need not agree.
 *
 * A record rides the list arm too: data is the AshValue array of its fields
 * in declaration order, len is the field count, cap equals len, elem_ty is 0.
 * A declared sum is ASH_TY_SUM: tag is the variant's declaration index, and
 * the list arm carries the variant's payload fields in declaration order, an
 * empty arm, data NULL and len 0, for a variant with no payload.
 *
 * A map, ASH_TY_MAP, rides the list arm as well, and this is its canonical
 * repr: data holds interleaved key, value pairs in insertion order, the key
 * of pair i at slot 2i and its value at 2i+1, len counts slots so it is
 * always twice the pair count, cap is allocated slots, and elem_ty is the
 * key's tag alone, one of the keyable scalars Int, UInt, Bool, Byte, Char,
 * String; the value type is enforced by the checker and never recorded here.
 * Insertion order is normative: it is the order serialization sees and the
 * order equality compares. */

/* The one value shape. ty picks the union arm. tag carries the variant for
 * the sum shaped types: Option 0 None 1 Some, Result 0 Ok 1 Err, the
 * declaration index of the variant for a declared sum, and 0 for everything
 * else. Option and Result payloads ride in box as a pointer to a single
 * AshValue the runtime owns. */
typedef struct AshValue {
    uint32_t ty;
    uint32_t tag;
    union {
        int64_t   i;
        uint64_t  u;
        double    f;
        uint8_t   b;
        uint32_t  ch;
        AshString s;
        AshList   list;
        void*     box;
    } as;
} AshValue;

typedef enum AshStatus {
    ASH_OK = 0,
    ASH_ERR_STATE,     /* operation illegal in the contract's current state */
    ASH_ERR_TYPE,      /* argument count or type mismatch at a thunk */
    ASH_ERR_VERSION,   /* shape hash or version disagreed at sign or load */
    ASH_ERR_UNBOUND,   /* signing with an abstract pledge nothing bound */
    ASH_ERR_NAME,      /* no contract or pledge under that name */
    ASH_ERR_DEADLOCK,
    ASH_ERR_OOM,
    ASH_ERR_LOAD,      /* dlopen or registrar failure on a module */
    ASH_ERR_NET,       /* the connection carrying this operation failed */
    ASH_ERR_STORE      /* the backend could not complete a store operation */
} AshStatus;

typedef enum AshContractState {
    ASH_UNSIGNED = 0,
    ASH_SIGNED,
    ASH_FULFILLED,
    ASH_PARTIAL,
    ASH_BROKEN
} AshContractState;

struct AshRuntime;
struct AshContract;

/* Every pledge crosses the boundary in this one shape, compiled bodies and
 * host bound implementations alike. ctx is the signed contract instance the
 * fulfillment runs against; allocation helpers take it so everything a pledge
 * builds is owned by that instance and dies at break().
 *
 * args is the full frame, one slot per declared parameter, every slot an
 * instance owned deep copy. When the caller passed AshRefs they occupy the
 * trailing slots; those slots are mutable on purpose, an implementation that
 * updates a by-reference parameter casts away the const on its own slot and
 * writes the new value there. The runtime writes the slots back at delivery. */
typedef AshStatus (*AshPledgeFn)(void* ctx, const AshValue* args, size_t nargs,
                                 AshValue* out);

/* Descriptor tables. ashc emits these as static const data inside a compiled
 * module, and the registrar hands them to the runtime at load. The runtime
 * keeps the pointers, so the tables must live as long as the module. */
typedef struct AshPledgeDesc {
    const char* name;
    const char* mangled;
    uint32_t    nargs;
    AshPledgeFn fn;        /* NULL marks an abstract pledge awaiting a bind */
    int32_t     sub;       /* index into the contract's subs table, or -1 for
                            * a pledge declared outside any subcontract. An
                            * index outside [0, nsubs) reads as loose, which
                            * is what a zero-filled handwritten desc gets. */
} AshPledgeDesc;

/* One atom of a requirements policy line. An atom tests one item's latched
 * state: a subcontract (sub >= 0, every pledge inside it latched) or a loose
 * pledge (pledge >= 0, that pledge's own latch). kind picks which latch the
 * atom tests; source atoms are always ASH_ATOM_FULFILLED, the grammar's
 * "bare name means fulfilled", and ASH_ATOM_BROKEN exists so the synthesized
 * default break line, "when everything is broken", is expressible in the
 * same table. name is the item's declared name, NULL for an anonymous
 * subcontract, and is informational; evaluation reads the indices. */
enum {
    ASH_ATOM_FULFILLED = 0,
    ASH_ATOM_BROKEN    = 1
};

typedef struct AshReqAtom {
    const char* name;
    uint32_t    kind;    /* ASH_ATOM_FULFILLED or ASH_ATOM_BROKEN */
    int32_t     sub;     /* subs table index, -1 when the atom is a pledge */
    int32_t     pledge;  /* pledges table index, -1 when the atom is a sub */
} AshReqAtom;

/* One op of a policy line's postfix program. ATOM pushes atom's truth, NOT
 * negates the top, AND and OR combine the top two. A line of length 0 is a
 * line the source did not write and never fires, constant false. */
enum {
    ASH_REQ_ATOM = 0,
    ASH_REQ_NOT  = 1,
    ASH_REQ_AND  = 2,
    ASH_REQ_OR   = 3
};

typedef struct AshReqOp {
    uint8_t op;    /* ASH_REQ_* */
    uint8_t atom;  /* atoms table index, meaningful for ASH_REQ_ATOM only */
} AshReqOp;

/* One vow of a contract: its name, its value's type tag, and its default
 * when the declaration carried an initializer. A default that is a string
 * points at bytes inside the module, which stays mapped for the life of the
 * runtime; signing copies the value onto the instance either way. */
typedef struct AshVowDesc {
    const char* name;
    uint32_t    ty;           /* AshTypeTag of the vow's value */
    uint32_t    has_default;
    AshValue    default_value;
} AshVowDesc;

/* A by-reference argument a host passes to a fulfillment. host_ptr addresses
 * the host's storage for a value of type ty: the raw scalar for the numeric
 * and character types, or an AshString struct for strings. Only those types
 * cross by reference in v1; anything else is refused with ASH_ERR_TYPE.
 *
 * The runtime never holds host_ptr across a call boundary the host is not
 * blocked inside. At fulfillment entry the referenced value is copied onto
 * the instance as a mutable slot appended after the value arguments, so the
 * pledge body mutates instance memory only. At delivery, inside
 * ash_future_wait or before ash_pledge_fulfill_sync returns, on the caller's
 * thread, each slot's final value is written back: through write_back when
 * the host supplied one, with user passed through untouched, or by the
 * default otherwise. cap declares the byte capacity at host_ptr; the default
 * write back does not use it, a callback that honors a capacity reaches its
 * own bookkeeping through user. The default writes scalars in place and
 * writes a whole AshString struct for strings, whose bytes are instance
 * owned and die at break; a host that wants the bytes to outlive the
 * instance supplies a write_back that copies them out. */
typedef void (*AshWriteBackFn)(void* host_ptr, const AshValue* v, void* user);

typedef struct AshRef {
    void*          host_ptr;
    uint32_t       ty;          /* AshTypeTag of the referenced value */
    uint64_t       cap;         /* byte capacity at host_ptr, for callbacks */
    AshWriteBackFn write_back;  /* NULL selects the default write back */
    void*          user;        /* passed through to write_back untouched */
} AshRef;

/* One column of a schema: its name and the scalar tag a row's field carries in
 * that slot. The seven scalars are the whole vocabulary a flat row holds, so ty
 * is one of Int, UInt, Float, Bool, Byte, Char, String; a composite never
 * reaches a column, refused at the type checker before a descriptor is emitted.
 * The columns ride in declaration order, and the first is the schema's primary
 * key, the convention Store.find, update, and delete key against. */
typedef struct AshSchemaCol {
    const char* name;
    uint32_t    ty;    /* AshTypeTag of the column, always a scalar */
} AshSchemaCol;

/* One schema of a store-backed contract: the table it names, its column count,
 * and its columns in declaration order. The runtime reconciles this against the
 * live database at sign, creating the table when absent and validating it
 * column for column when present, so the shape a contract signed against is the
 * shape it operates against or the sign never lands. */
typedef struct AshSchemaDesc {
    const char*         table;
    uint32_t            ncols;
    const AshSchemaCol* cols;
} AshSchemaDesc;

/* The subcontract flag bits, carried in a byte array parallel to the subs name
 * table, one entry per subcontract in declaration order. ASH_SUB_TRANSACTIONAL
 * marks a subcontract whose pledges run as one all or nothing episode: the
 * runtime opens a transaction on the first fulfillment of one of its pledges,
 * commits the instant the subcontract completes, and rolls back on the first
 * Err or at a break before the commit. The bits ride their own array so the
 * subs table and every descriptor ashc emitted before the store layer stay
 * byte identical; a NULL sub_flags reads as all zero, the loose grouping every
 * earlier contract carried. */
enum {
    ASH_SUB_TRANSACTIONAL = 1
};

/* The requirements surface rides at the tail of the contract descriptor so a
 * zero-filled handwritten descriptor stays valid: no atoms and no lines means
 * the runtime applies the structural default policy over subs and loose
 * pledges. ashc always emits the three lines, serializing the source block
 * when one was written and synthesizing the grammar's defaults when not, so
 * the compiler's static satisfiability check and the runtime evaluate the
 * same trees. subs lists every subcontract in declaration order, anonymous
 * ones as NULL entries, so a pledge's sub index and a sub atom resolve
 * against one table.
 *
 * The schema surface, nschemas and schemas, appends at the tail so a
 * zero-filled handwritten descriptor and every module ashc emitted before the
 * store layer read as nschemas 0: no schema, not store-backed, signed with no
 * database. A store-backed contract carries one entry per declared schema, and
 * the runtime reads them at sign to reconcile the tables and to route a
 * Store.* primitive against the right one. */
typedef struct AshContractDesc {
    const char*          name;
    uint64_t             shape_hash;
    uint32_t             version;
    uint32_t             npledges;
    const AshPledgeDesc* pledges;
    uint32_t             nvows;
    const AshVowDesc*    vows;
    uint32_t             nsubs;
    const char* const*   subs;         /* NULL entry = anonymous subcontract */
    uint32_t             natoms;
    const AshReqAtom*    atoms;
    uint32_t             has_reqs;     /* 1 when the source wrote the block */
    uint32_t             nfulfill;
    uint32_t             npartial;
    uint32_t             nbreak;
    const AshReqOp*      req_fulfill;
    const AshReqOp*      req_partial;
    const AshReqOp*      req_break;
    uint32_t             nschemas;
    const AshSchemaDesc* schemas;
    /* One flag byte per subcontract, ASH_SUB_* bits, indexed by the same
     * ordinal a pledge's sub field and a sub atom resolve against. NULL when no
     * subcontract carries a flag, which is every descriptor emitted before the
     * transactional modifier and every handwritten one, so the field is a pure
     * tail addition the runtime reads beside the subs table. */
    const uint8_t*       sub_flags;
} AshContractDesc;

/* A vow override a host supplies at sign. The value is copied onto the
 * instance, string bytes included; the runtime keeps nothing the host owns. */
typedef struct AshVowBinding {
    const char* name;
    AshValue    value;
} AshVowBinding;

#ifdef __cplusplus
}
#endif

#endif /* ASH_ABI_H */
