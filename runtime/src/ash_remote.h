/* ash_remote.h: the internal seam the daemon reaches through libashrt, the way
 * ashd already reaches the socket helpers through ash_net.h. None of this is
 * public ABI; it is the runtime handing ashd two things the host surface has no
 * call for, because a local host never needs them and the wire does.
 *
 * The scratch instance is a decode arena. A SIGN frame's vow overrides are
 * encoded values that must be decoded before ash_contract_sign can copy them
 * onto the real instance, but there is no instance yet at that moment: the
 * decoder writes onto an AshContract's block list, so the daemon rents a bare
 * one, decodes the overrides onto it, hands them to the sign as bindings the
 * sign deep copies, and frees the scratch. The scratch is never registered, so
 * shutdown never sees it.
 *
 * The vow enumeration lets the daemon answer SIGNED with the full effective vow
 * set. A signed instance holds its vows keyed to the descriptor; the host reads
 * one by name through ash_vow_ref, but the daemon must send them all, so it
 * walks them by index here. */

#ifndef ASH_REMOTE_H
#define ASH_REMOTE_H

#include <ash/ash.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A bare instance the decoder can own values on, with a live block list and
 * lock and nothing else: no descriptor, no vows, no latches, never in the
 * runtime's instance table. NULL on allocation failure. Free it with
 * ash_scratch_free, which reclaims every block decoded onto it. */
AshContract* ash_scratch_new(AshRuntime* rt);
void         ash_scratch_free(AshContract* c);

/* The effective vow set of a signed instance, for the SIGNED reply. count is
 * the number of vows; name and value read the i-th, in descriptor order. The
 * pointers are instance owned and valid until the instance breaks. A NULL or
 * unsigned instance reads as zero vows. */
size_t          ash_instance_nvows(const AshContract* c);
const char*     ash_instance_vow_name(const AshContract* c, size_t i);
const AshValue* ash_instance_vow_value(const AshContract* c, size_t i);

#ifdef __cplusplus
}
#endif

#endif /* ASH_REMOTE_H */
