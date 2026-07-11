/* ash.h: the host API of libashrt, the Ashford intermediary runtime. A host
 * loads compiled .ash modules through the runtime, signs contracts with vow
 * overrides, fulfills pledges through futures or synchronously, and breaks
 * what it signed. The future form is the API's real shape; today it runs the
 * pledge before it returns and the future only carries the outcome, but a
 * host written against it keeps working when the threading milestone makes
 * fulfillment concurrent for real.
 *
 * Ownership is one rule deep. Everything a pledge allocates hangs off the
 * contract instance it ran against and is freed by ash_contract_break. That
 * covers futures too, so a future must be waited before its instance breaks.
 * A host that wants to keep a result copies it out before breaking. */

#ifndef ASH_H
#define ASH_H

#include "ash_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AshRuntime  AshRuntime;
typedef struct AshContract AshContract;
typedef struct AshFuture   AshFuture;

/* ---- runtime lifecycle ---- */

/* cfg is reserved and NULL today. */
AshStatus ash_runtime_init(const void* cfg, AshRuntime** out);
void      ash_runtime_shutdown(AshRuntime* rt);

/* dlopens a compiled module and calls its ash_module_register. The module
 * stays mapped until shutdown. */
AshStatus ash_module_load(AshRuntime* rt, const char* so_path);

/* Called by a module's registrar. The runtime keeps the descriptor pointer,
 * it does not copy the tables. */
AshStatus ash_register_contract(AshRuntime* rt, const AshContractDesc* desc);

/* ---- contracts ---- */

/* Finds the contract by name, checks the shape hash when expected_hash is
 * nonzero, applies the vow overrides over the declared defaults, and returns
 * a signed instance. An override naming no vow is ASH_ERR_NAME, an override
 * of the wrong type is ASH_ERR_TYPE, and a vow with no default and no
 * override is ASH_ERR_UNBOUND. Pass NULL and 0 to sign on defaults alone. */
AshStatus ash_contract_sign(AshRuntime* rt, const char* contract_name,
                            const AshVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, AshContract** out);

AshContractState ash_contract_state(const AshContract* c);

/* The signature the instance carries: the shape hash it was signed under and
 * the Unix time the signing happened. */
uint64_t ash_contract_hash(const AshContract* c);
int64_t  ash_contract_signed_at(const AshContract* c);

/* Frees every allocation the instance owns and latches the state at Broken.
 * Every later fulfillment on it reports ASH_ERR_STATE. The instance itself
 * stays valid for state queries until shutdown. */
AshStatus ash_contract_break(AshContract* c);

/* ---- pledges ---- */

/* Binds a host implementation to a pledge, the way an abstract pledge gets a
 * body and a concrete one gets overridden. pledge_name is either
 * "Contract.pledge" or the pledge's mangled symbol; ASH_ERR_NAME when no
 * registered contract carries it. The bound fn runs in the uniform thunk
 * frame with ctx = the signed instance, exactly like a compiled body; a
 * per-binding userdata is deferred, a host that needs state reaches it
 * through its own globals for now. Rebinding replaces. Bindings resolve at
 * sign: an instance signed before the bind keeps dispatching what it was
 * signed with, and a binding beats the compiled body for instances signed
 * after it. */
AshStatus ash_pledge_bind(AshRuntime* rt, const char* pledge_name,
                          AshPledgeFn fn);

/* Starts a fulfillment and hands back its future. Today the pledge runs to
 * completion inside this call; the future carries the outcome either way, so
 * every error a fulfillment can hit, wrong state, unknown pledge, argument
 * mismatch, is reported by the wait, not here. Returns NULL only when c or
 * pledge_name is NULL or the future itself cannot be allocated. The future
 * is owned by the instance and dies with it at break.
 *
 * Every value argument is deep copied onto the instance at this call, on the
 * caller's thread, so nothing the host owns needs to outlive the call. refs
 * pass trailing parameters by reference; nargs plus nrefs must equal the
 * pledge's declared parameter count. Each ref is copied in here and written
 * back to host memory by the wait that delivers the outcome, on the waiting
 * thread, never in between. Pass NULL and 0 for a call with no refs. */
AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs,
                              const AshRef* refs, size_t nrefs);

/* Delivers a future's outcome exactly once. out receives the pledge's value
 * on ASH_OK; everything inside it is owned by the instance. A second wait on
 * the same future is ASH_ERR_STATE. When the fulfillment carried refs their
 * slots are written back to host memory here, before the value is delivered;
 * a slot whose type no longer matches its ref is ASH_ERR_TYPE and nothing is
 * written. */
AshStatus ash_future_wait(AshFuture* f, AshValue* out);

/* The synchronous form: fulfill and wait in one call. Ref write back happens
 * before this returns, on the calling thread. */
AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
                                  const AshRef* refs, size_t nrefs,
                                  AshValue* out);

/* ---- vows ---- */

/* The signed value of a vow, or NULL when the instance has no vow by that
 * name. The pointer is owned by the instance and dies at break; compiled
 * thunks read their contract's vows through this. */
const AshValue* ash_vow_ref(AshContract* c, const char* name);

/* ---- allocation helpers for pledge bodies ---- */

/* Instance owned bytes, freed at break. Returns NULL on OOM. */
uint8_t*  ash_bytes(AshContract* c, uint64_t n);

/* Instance owned boxed value for Option and Result payloads. */
AshValue* ash_box(AshContract* c);

/* Copies utf8[0..len) into instance owned memory and returns a string value. */
AshValue  ash_string_copy(AshContract* c, const uint8_t* utf8, uint64_t len);

/* Joins two string values into instance owned memory. ASH_ERR_TYPE when
 * either operand is not a string, ASH_ERR_OOM when the bytes cannot be had. */
AshStatus ash_string_concat(AshContract* c, const AshValue* a,
                            const AshValue* b, AshValue* out);

/* Whether two string values hold the same bytes; 1 or 0. */
int ash_string_eq(const AshValue* a, const AshValue* b);

/* ---- deep values ---- */

/* An instance owned list with room for cap elements of elem_ty. The element
 * storage is an AshValue array behind out->as.list.data. */
AshStatus ash_list_new(AshContract* c, uint32_t elem_ty, uint64_t cap,
                       AshValue* out);

/* Appends one element, growing instance owned storage as needed; a block a
 * grown list leaves behind stays on the instance until break, the price of
 * the one-walk reclaim. The element struct is copied as is: its contents
 * must already be instance owned or otherwise outlive their use, and
 * ash_value_deep_copy is the tool when they are not. ASH_ERR_TYPE when list
 * is not a list or the element's tag disagrees with elem_ty. */
AshStatus ash_list_push(AshContract* c, AshValue* list, const AshValue* elem);

/* The element at idx of a list or tuple, or NULL when out of range or the
 * value is neither. The pointer is into instance owned storage. */
const AshValue* ash_list_get(const AshValue* v, uint64_t idx);

/* An instance owned tuple of count zeroed slots, filled in place through
 * ash_list_get or the data pointer. */
AshStatus ash_tuple_new(AshContract* c, uint64_t count, AshValue* out);

/* Recursively copies src into instance owned memory: scalars by value,
 * string bytes copied, list and tuple elements copied element by element,
 * Option and Result payloads reboxed. After it returns, nothing in dst
 * aliases memory the instance does not own. Map and record values are not
 * supported in v1 and report ASH_ERR_TYPE. */
AshStatus ash_value_deep_copy(AshContract* c, const AshValue* src,
                              AshValue* dst);

#ifdef __cplusplus
}
#endif

#endif /* ASH_H */
