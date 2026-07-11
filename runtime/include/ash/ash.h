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

/* Starts a fulfillment and hands back its future. Today the pledge runs to
 * completion inside this call; the future carries the outcome either way, so
 * every error a fulfillment can hit, wrong state, unknown pledge, argument
 * mismatch, is reported by the wait, not here. Returns NULL only when c or
 * pledge_name is NULL or the future itself cannot be allocated. The future
 * is owned by the instance and dies with it at break. */
AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs);

/* Delivers a future's outcome exactly once. out receives the pledge's value
 * on ASH_OK; everything inside it is owned by the instance. A second wait on
 * the same future is ASH_ERR_STATE. */
AshStatus ash_future_wait(AshFuture* f, AshValue* out);

/* The synchronous form: fulfill and wait in one call. */
AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
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

#ifdef __cplusplus
}
#endif

#endif /* ASH_H */
