// SEG-020-T004: opt-in M68k state checkpoint/digest. Project-authored synthetic values only.
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

GenesisRuntime make(bool enabled) {
  GenesisRuntime r{};
  r.m68k_checkpoint.enabled = enabled ? 1U : 0U;
  r.sr = 0x2700U;
  r.a[7] = 0xFFFF00U;
  return r;
}

void write_ram(GenesisRuntime *r, uint32_t addr, GenesisAccessWidth w, uint32_t v) {
  GenesisRuntimeStop stop{};
  check(genesis_route_access(r, addr, w, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK, "ram write");
}

// One synthetic instruction: optional register perturbations, a write, retire.
uint64_t run(void (*perturb)(GenesisRuntime *), GenesisM68kCheckpoint *out = nullptr) {
  GenesisRuntime r = make(true);
  r.d[1] = 5U;
  write_ram(&r, 0xFF0100U, GENESIS_ACCESS_WORD, 0x1234U);
  if (perturb) perturb(&r);
  genesis_runtime_retire_m68k_instruction(&r, 4U, 0x202U);
  if (out) *out = r.m68k_checkpoint;
  return genesis_m68k_checkpoint_digest(&r);
}
}  // namespace

int main() {
  const uint64_t base = run(nullptr);
  check(base != 0U, "digest present");
  check(run(nullptr) == base, "stable digest for identical runs");

  // Single-field perturbations each change the digest.
  check(run([](GenesisRuntime *r) { r->d[3] ^= 1U; }) != base, "D perturbation");
  check(run([](GenesisRuntime *r) { r->d[7] ^= 0x80000000U; }) != base, "D7 perturbation");
  check(run([](GenesisRuntime *r) { r->a[2] += 2U; }) != base, "A perturbation");
  check(run([](GenesisRuntime *r) { r->usp += 4U; }) != base, "USP perturbation");
  check(run([](GenesisRuntime *r) { r->sr ^= 1U; }) != base, "flag (C) perturbation");
  check(run([](GenesisRuntime *r) { r->sr ^= 0x0700U; }) != base, "SR mask perturbation");
  // PC via different next_pc.
  {
    GenesisRuntime r = make(true);
    r.d[1] = 5U;
    write_ram(&r, 0xFF0100U, GENESIS_ACCESS_WORD, 0x1234U);
    genesis_runtime_retire_m68k_instruction(&r, 4U, 0x204U);
    check(genesis_m68k_checkpoint_digest(&r) != base, "PC perturbation");
  }
  // Wrong memory write value / address / width with registers unchanged.
  for (int variant = 0; variant < 3; ++variant) {
    GenesisRuntime r = make(true);
    r.d[1] = 5U;
    write_ram(&r, variant == 1 ? 0xFF0102U : 0xFF0100U, variant == 2 ? GENESIS_ACCESS_BYTE : GENESIS_ACCESS_WORD,
              variant == 0 ? 0x1235U : 0x1234U);
    genesis_runtime_retire_m68k_instruction(&r, 4U, 0x202U);
    check(genesis_m68k_checkpoint_digest(&r) != base, "wrong memory write changes digest");
  }
  // Missing write also differs.
  {
    GenesisRuntime r = make(true);
    r.d[1] = 5U;
    genesis_runtime_retire_m68k_instruction(&r, 4U, 0x202U);
    check(genesis_m68k_checkpoint_digest(&r) != base, "missing write changes digest");
  }

  // Trap/exception with registers unchanged: divide-by-zero entry vs plain write-free retire.
  {
    GenesisRuntime plain = make(true);
    genesis_runtime_retire_m68k_instruction(&plain, 4U, 0x300U);
    GenesisRuntime trap = make(true);
    trap.divide_by_zero_handler_present = 1U;
    trap.divide_by_zero_handler_entry = 0x300U;
    uint32_t handler = 0;
    GenesisRuntimeStop stop{};
    check(genesis_raise_divide_by_zero(&trap, 0x2FEU, &handler, &stop) == 1, "trap raised");
    // Real generated route: DIV lowering returns the handler transfer straight after the raise
    // (no retire call), so the raise itself must have completed exactly one boundary.
    check(trap.m68k_checkpoint.valid == 1U && trap.m68k_checkpoint.boundary == 1U, "one boundary at raise");
    check(trap.m68k_checkpoint.effect_count == 0U && trap.m68k_checkpoint.unsupported_for_comparison == 0U,
          "pending accumulator empty after boundary");
    check(trap.m68k_checkpoint.pc == 0x300U, "boundary holds post-exception PC");
    GenesisRuntime restore = trap;
    check(restore.m68k_checkpoint.last_effect_count == 3U, "two frame writes + trap effect");
    check(restore.m68k_checkpoint.last_effects[2].kind == GENESIS_M68K_EFFECT_TRAP &&
              restore.m68k_checkpoint.last_effects[2].value == 5U,
          "trap effect carries vector 5");
    // Handler's first instruction does not inherit the DIV effects.
    genesis_runtime_retire_m68k_instruction(&trap, 4U, 0x302U);
    check(trap.m68k_checkpoint.boundary == 2U && trap.m68k_checkpoint.last_effect_count == 0U,
          "handler instruction starts with a clean effect set");
    // Field-level collision guard: equal digests but a differing vector field are still DIFFERENT.
    GenesisM68kCheckpoint forged = restore.m68k_checkpoint;
    forged.last_effects[2].value = 30U;
    check(forged.digest == restore.m68k_checkpoint.digest, "setup: digest deliberately unchanged");
    check(genesis_m68k_checkpoint_compare(&restore.m68k_checkpoint, &forged) == GENESIS_M68K_CHECKPOINT_DIFFERENT,
          "field compare catches differing vector despite equal digest");
  }

  // Trap-only digest difference: identical D/A/USP/SR/PC and identical write effects; the only
  // difference is the pending trap occurrence (absent / vector 5 / vector 30) at the boundary.
  {
    auto boundary = [](int trap_vector) {
      GenesisRuntime r = make(true);
      r.d[2] = 7U;
      write_ram(&r, 0xFF0100U, GENESIS_ACCESS_WORD, 0x1234U);  // identical write effect
      if (trap_vector != 0) {
        GenesisM68kCheckpoint &cp = r.m68k_checkpoint;
        cp.effects[cp.effect_count++] = GenesisM68kEffect{GENESIS_M68K_EFFECT_TRAP, 0U, 0x300U,
                                                          static_cast<uint32_t>(trap_vector)};
      }
      genesis_runtime_retire_m68k_instruction(&r, 4U, 0x300U);
      return r;
    };
    const GenesisRuntime none = boundary(0), v5 = boundary(5), v30 = boundary(30);
    for (const GenesisRuntime *r : {&v5, &v30}) {
      check(none.pc == r->pc && none.sr == r->sr && none.usp == r->usp && none.a[7] == r->a[7] &&
                none.d[2] == r->d[2] && none.m68k_checkpoint.sr == r->m68k_checkpoint.sr &&
                none.m68k_checkpoint.pc == r->m68k_checkpoint.pc,
            "setup: CPU state identical");
    }
    check(genesis_m68k_checkpoint_digest(&none) != genesis_m68k_checkpoint_digest(&v5), "trap occurrence changes digest");
    check(genesis_m68k_checkpoint_digest(&v5) != genesis_m68k_checkpoint_digest(&v30), "trap vector changes digest");
    check(genesis_m68k_checkpoint_compare(&none.m68k_checkpoint, &v5.m68k_checkpoint) ==
              GENESIS_M68K_CHECKPOINT_DIFFERENT, "trap-only compare differs");
  }

  // Compare semantics + unsupported-for-comparison never equal.
  {
    GenesisM68kCheckpoint x, y;
    run(nullptr, &x);
    run(nullptr, &y);
    check(genesis_m68k_checkpoint_compare(&x, &y) == GENESIS_M68K_CHECKPOINT_EQUAL, "equal boundaries");
    GenesisRuntime r = make(true);
    for (unsigned i = 0; i <= GENESIS_M68K_CHECKPOINT_EFFECT_CAPACITY; ++i)
      write_ram(&r, 0xFF0200U + 2U * i, GENESIS_ACCESS_WORD, i);
    genesis_runtime_retire_m68k_instruction(&r, 4U, 0x202U);
    check(r.m68k_checkpoint.last_unsupported == 1U, "overflow marks unsupported");
    check(genesis_m68k_checkpoint_compare(&r.m68k_checkpoint, &r.m68k_checkpoint) ==
              GENESIS_M68K_CHECKPOINT_UNSUPPORTED,
          "unsupported boundary is never equal even to itself");
    check(r.m68k_checkpoint.effect_count == 0U && r.m68k_checkpoint.unsupported_for_comparison == 0U,
          "pending state reset after boundary");
    GenesisM68kCheckpoint none{};
    check(genesis_m68k_checkpoint_compare(&none, &none) == GENESIS_M68K_CHECKPOINT_UNSUPPORTED, "invalid unsupported");
  }

  // Disabled: no observable change (checkpoint block stays all-zero, behavior identical).
  {
    GenesisRuntime off = make(false);
    GenesisRuntime on = make(true);
    write_ram(&off, 0xFF0100U, GENESIS_ACCESS_WORD, 0x1234U);
    write_ram(&on, 0xFF0100U, GENESIS_ACCESS_WORD, 0x1234U);
    const GenesisControlTransfer a = genesis_runtime_retire_m68k_instruction(&off, 4U, 0x202U);
    const GenesisControlTransfer b = genesis_runtime_retire_m68k_instruction(&on, 4U, 0x202U);
    check(a.kind == b.kind && a.next_pc == b.next_pc && off.pc == on.pc && off.sr == on.sr, "same behavior");
    check(genesis_m68k_checkpoint_digest(&off) == 0U && !off.m68k_checkpoint.valid &&
              off.m68k_checkpoint.effect_count == 0U,
          "disabled records nothing");
  }

  // Detail on request.
  {
    GenesisRuntime r = make(true);
    write_ram(&r, 0xFF0100U, GENESIS_ACCESS_LONG, 0xDEADBEEFU);
    genesis_runtime_retire_m68k_instruction(&r, 4U, 0x202U);
    FILE *f = std::tmpfile();
    check(f != nullptr && genesis_m68k_checkpoint_write_detail(f, &r) == 0, "detail written");
    std::rewind(f);
    char buf[2048] = {0};
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    const std::string s(buf, n);
    check(s.find("\"boundary\":1") != std::string::npos && s.find("\"effects\":[{\"k\":1,\"w\":4") != std::string::npos,
          "detail has fields and effect");
  }

  if (failures == 0) std::printf("OK\n");
  return failures == 0 ? 0 : 1;
}
