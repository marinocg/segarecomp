# ADR 0327: Genesis mod-set callback dispatch and exclusive hook ownership

- Status: proposed (SEG-141-T003, first checkpoint of combined delivery F20). The runtime dispatcher and the composition rule exist; **no built program links more than one mod yet**.
- Builds on: ADR 0297 (scheduler), 0298 (hooks), 0299 (mod state), 0326 (mod sets).

## Decision

1. A set is not a second scheduler. Each member is an ordinary `GenesisModScheduler` with its own grants, declared ranges, staged effects, budgets, fault, replay witness and state. `GenesisModSet` (`mod_scheduler.h`) owns only the runtime seams and the order. Member index is the composite order.
2. **Order.** Each phase runs for all members in order before the next phase. A callback commits atomically, so later members observe earlier members' committed guest writes. Guest memory is the only channel between members. `input` takes one physical sample per frame and chains it through the members; `post-input` and the next `frame-begin` show every member the final composed pad. Each `render` starts from its own copy taken after all `frame-end` callbacks.
3. **Failure isolation.** A fault disables only the faulting member. The faulting callback commits nothing (its pad change is not chained); effects it committed earlier remain, including within the current frame. The pad latch stays while a live member has `input` and is released when none does.
4. **Hook ownership.** Exclusive per CPU and site: a `replace` hook admits no other member at that function. `before` / `after` / `before-after` hooks of several members may share a site; they run in composite order for entry and for exit. Composition refuses a conflict (`E_SET_HOOK_OWNER`, naming both mods), and the runtime refuses it again at attach. There is one hook call-frame stack, and all hooking members must use the same generated site table. Platforms without function hooks keep refusing hook declarations in the adapter; there is no second dispatcher.
5. **State.** One `MODSTATE` section bound to the composite identity holds every member's state in order plus the shared hook frames; the compatibility digest covers each member's identity, state schema and configuration, in order. Restore is all-or-nothing for the guest: if one member refuses its blob, members already loaded are put back, or disabled with cause `state`. All members' blobs share the existing section bound (`GENESIS_SNAPSHOT_MOD_BODY_MAX`); a set that does not fit is not capturable (`E_STATE_UNAVAILABLE`).
6. Single-mod programs are unchanged: same entry points, phase runner, state bytes and compatibility digest.

## Dispatch overhead

Measured by `genesis_mod_set_tests` (members with empty callbacks, five boundary callbacks per member per frame, guest instructions excluded; Linux x86-64, GCC, `dev` preset which is a Debug build): about 19-24 ns per callback, linear in the member count, about 1.5 us per frame boundary with 16 members. The test prints the numbers and asserts no host-speed threshold. Work-RAM render copies (one per member that holds `frame.work_ram`) and the members' own work are not included.

## Not decided or implemented here

Building or running a program with more than one mod (set artifacts, several Luau VMs or native providers in one link, merged hook plans, presentation from several members), the Master System set dispatcher (SEG-141-T007), namespaced settings and saves (SEG-141-T005), and composition diagnostics in `mod set build` and the launcher (SEG-141-T006). The support matrix keeps multiple-mod composition `unsupported`.

## Evidence

`tests/genesis_mod_set_test.cpp` (synthetic program and native ABI 2 providers; the hook cases drive the runtime hook seam with a hand-built site table, not a generated hooked program), `tests/mod_set_test.py` (CLI ownership refusals).
