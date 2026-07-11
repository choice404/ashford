/* ash.h: the host API of libashrt, the Ashford intermediary runtime. A host
 * loads compiled .ash modules through the runtime, signs contracts, fulfills
 * pledges, and breaks what it signed. This is the M0 surface: enough to drive
 * one contract end to end from C. The future handle form of fulfill arrives
 * with the threading milestone; the sync form stays either way.
 *
 * Ownership is one rule deep. Everything a pledge allocates hangs off the
 * contract instance it ran against and is freed by ash_contract_break. A host
 * that wants to keep a result copies it out before breaking. */

#ifndef ASH_H
#define ASH_H

#include "ash_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AshRuntime  AshRuntime;
typedef struct AshContract AshContract;

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
 * nonzero, and returns a signed instance. Vow overrides arrive with M3; pass
 * NULL and 0 today. */
AshStatus ash_contract_sign(AshRuntime* rt, const char* contract_name,
                            const void* vows, size_t nvows,
                            uint64_t expected_hash, AshContract** out);

AshContractState ash_contract_state(const AshContract* c);

/* Frees every allocation the instance owns and latches the state at Broken.
 * Every later fulfillment on it reports ASH_ERR_STATE. The instance itself
 * stays valid for state queries until shutdown. */
AshStatus ash_contract_break(AshContract* c);

/* ---- pledges ---- */

/* Runs a pledge synchronously on a signed contract. out receives the pledge's
 * declared value; everything inside it is owned by the instance. */
AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
                                  AshValue* out);

/* ---- allocation helpers for pledge bodies ---- */

/* Instance owned bytes, freed at break. Returns NULL on OOM. */
uint8_t*  ash_bytes(AshContract* c, uint64_t n);

/* Instance owned boxed value for Option and Result payloads. */
AshValue* ash_box(AshContract* c);

/* Copies utf8[0..len) into instance owned memory and returns a string value. */
AshValue  ash_string_copy(AshContract* c, const uint8_t* utf8, uint64_t len);

#ifdef __cplusplus
}
#endif

#endif /* ASH_H */
