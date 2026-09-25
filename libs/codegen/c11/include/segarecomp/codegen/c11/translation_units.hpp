#pragma once

// SEG-022-T003: deterministic generated-C translation-unit sharding.
//
// Generic, target-independent: nothing here knows Genesis, M68K or a guest address space beyond an
// opaque 64-bit `key`. An emitter keeps writing its C to one `std::ostream`; when that stream is a
// `TranslationUnitSharder` stream, the emitter brackets each independently-compilable top-level unit
// (a function plus the file-scope objects private to it) with `begin_unit` / `end_unit`, and shared
// declarations with `declare`. Everything not inside a unit is "glue" and lands in the main TU.
//
// Layout (documented, bounded): one shared header, one main TU (glue), and for each declared
// family `shards` TUs. The shard of a unit is a pure function of its (family, key) -- never of
// emission order, thread scheduling, host or path -- so output is byte-identical across runs and
// machines: shard = (key >> page_shift) % shards. There is no partition optimizer.
//
// Host shard boundaries have no guest-visible meaning: a unit is a whole C function, so no guest
// instruction is ever split, and cross-TU symbols are plain external functions declared in the shared
// header (no static-initialization ordering dependency).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp {

namespace detail { class ShardBuffer; }

struct TranslationUnitFamily {
  std::string name;             // file-name component, [a-z0-9_]+
  std::size_t shards = 1;       // TU count for this family (>= 1)
  unsigned page_shift = 10;     // key bits ignored before the modulo (contiguous key pages stay together)
};

class TranslationUnitSharder {
 public:
  // `stem` names every file: <stem>.h, <stem>_main.c, <stem>_<family>_<NN>.c, <stem>.units.
  TranslationUnitSharder(std::filesystem::path directory, std::string stem, std::vector<TranslationUnitFamily> families);
  ~TranslationUnitSharder();
  TranslationUnitSharder(const TranslationUnitSharder &) = delete;
  TranslationUnitSharder &operator=(const TranslationUnitSharder &) = delete;

  [[nodiscard]] std::ostream &stream();

  // Finalizes the set. Returns "" on success; otherwise a diagnostic and nothing is left behind.
  // Files are written as `<name>.partial` and renamed only after every file is complete; the
  // manifest `<stem>.units` (sorted TU file names, one per line, `main` first) is renamed last.
  [[nodiscard]] std::string finish();

  // Number of TU files (excluding the header) that finish() published; valid after success.
  [[nodiscard]] std::size_t translation_unit_count() const;
  // Upper bound on TU count for this layout (main + sum of family shards).
  [[nodiscard]] std::size_t max_translation_unit_count() const;

 private:
  std::unique_ptr<detail::ShardBuffer> impl_;
};

// ---- emitter-side helpers; every one is a no-op on a stream that is not a sharder stream ----
[[nodiscard]] bool sharding_active(std::ostream &out);
// Text written until `end_header` lands in the shared header instead of the main TU.
void shard_begin_header(std::ostream &out);
void shard_end_header(std::ostream &out);
// Adds one declaration (no trailing ';' required) to the shared header.
void shard_declare(std::ostream &out, std::string_view declaration);
// Begins a unit. `declaration` is the external C declaration of the unit's function without the
// trailing ';' (e.g. "GenesisControlTransfer f(GenesisRuntime *runtime)"). It is published in the
// shared header and, if the unit text defines `static <declaration>`, that `static ` is dropped so the
// definition has external linkage. Units may not nest.
void shard_begin_unit(std::ostream &out, std::string_view family, std::uint64_t key, std::string_view declaration);
void shard_end_unit(std::ostream &out);

// Scope form of begin/end so early returns and `continue` cannot leave a unit open.
class ShardUnitScope {
 public:
  ShardUnitScope(std::ostream &out, std::string_view family, std::uint64_t key, std::string_view declaration)
      : out_(out), active_(sharding_active(out)) {
    if (active_) shard_begin_unit(out, family, key, declaration);
  }
  ~ShardUnitScope() { if (active_) shard_end_unit(out_); }
  ShardUnitScope(const ShardUnitScope &) = delete;
  ShardUnitScope &operator=(const ShardUnitScope &) = delete;

 private:
  std::ostream &out_;
  bool active_;
};

}  // namespace segarecomp
