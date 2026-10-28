#include "segarecomp/machine/sega_cd/loader_scan.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <optional>
#include <span>
#include <tuple>
#include <string_view>
#include <unordered_set>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/iso9660.hpp"
#include "segarecomp/machine/sega_cd/modules.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {

const char *loader_refusal_name(LoaderRefusalKind kind) noexcept {
  switch (kind) {
  case LoaderRefusalKind::lba_unknown: return "lba_unknown";
  case LoaderRefusalKind::destination_unknown: return "destination_unknown";
  case LoaderRefusalKind::destination_unsupported: return "destination_unsupported";
  case LoaderRefusalKind::length_unsupported: return "length_unsupported";
  case LoaderRefusalKind::cdd_command_invalid: return "cdd_command_invalid";
  case LoaderRefusalKind::cdc_state_unknown: return "cdc_state_unknown";
  case LoaderRefusalKind::loaded_not_executed: return "loaded_not_executed";
  case LoaderRefusalKind::entry_unknown: return "entry_unknown";
  case LoaderRefusalKind::entry_not_code: return "entry_not_code";
  case LoaderRefusalKind::source_outside_track: return "source_outside_track";
  case LoaderRefusalKind::too_many_operations: return "too_many_operations";
  case LoaderRefusalKind::too_many_modules: return "too_many_modules";
  case LoaderRefusalKind::scan_limit_exceeded: return "scan_limit_exceeded";
  case LoaderRefusalKind::computed_control_unresolved: return "computed_control_unresolved";
  }
  return "unknown";
}

namespace {

constexpr std::uint32_t kGateArraySub = 0x00FF8000U;
constexpr std::uint32_t kGateArraySubEnd = 0x00FF8200U;
constexpr std::uint32_t kWordRamSub = 0x00080000U;       // 2M mode view of the sub CPU
constexpr std::uint32_t kWordRamSubEnd = 0x000C0000U;
constexpr std::uint32_t kDmaAddressMask = 0x3FFFEU;      // runtime: ((dma_address << 3) & mask)

using Width = M68kMemoryAccessWidth;

// A 32-bit value with a mask of the bits the walk knows (a MOVE.W into a register defines only its low half).
struct Val {
  std::uint32_t v{};
  std::uint32_t mask{};
  [[nodiscard]] bool known() const { return mask == 0xFFFFFFFFU; }
};
Val K(std::uint32_t v) { return Val{v, 0xFFFFFFFFU}; }

std::uint32_t width_mask(Width w) { return w == Width::byte ? 0xFFU : w == Width::word ? 0xFFFFU : 0xFFFFFFFFU; }
std::uint32_t mask_of(Width w, std::uint32_t v) { return v & width_mask(w); }
// The value of `x` as a `w`-wide quantity, when every bit of that width is known (the result is then a fully known Val).
Val exact(const Val &x, Width w) { return (x.mask & width_mask(w)) == width_mask(w) ? K(x.v & width_mask(w)) : Val{}; }

// The machine's CD interface as the walk sees it (contract section 7). Unknown until a constant write defines it.
struct Hw {
  std::array<Val, 10> tx{};     // TX0..TX9 as written
  Val reg_ptr{K(0)};            // CDC register address ($FF8005)
  Val dest{};                   // CDC destination ($FF8004 & 7)
  Val dma_address{};            // $FF800A
  Val ifctrl{};                 // CDC register 1
  Val dbcl{}, dbch{};
  Val lba{};                    // the drive position after the latest play command
  bool play_invalid{};          // the latest play command could not be decoded
};

struct Loaded {
  std::uint32_t op{};
  std::uint32_t base{};
  std::uint32_t bytes{};
  bool operator==(const Loaded &) const = default;
};

struct State {
  std::uint32_t pc{};
  std::array<Val, 8> d{};
  std::array<Val, 8> a{};
  Hw hw;
  std::vector<std::uint32_t> ret;
  std::vector<Loaded> loaded;
};

std::string serialize(const State &s) {
  std::string out;
  const auto put = [&](std::uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
  const auto putv = [&](const Val &v) { put(v.mask); put(v.v & v.mask); };
  put(s.pc);
  for (const auto &v : s.d) putv(v);
  for (const auto &v : s.a) putv(v);
  for (const auto &v : s.hw.tx) putv(v);
  putv(s.hw.reg_ptr); putv(s.hw.dest); putv(s.hw.dma_address); putv(s.hw.ifctrl); putv(s.hw.dbcl); putv(s.hw.dbch); putv(s.hw.lba);
  out.push_back(s.hw.play_invalid ? 1 : 0);
  put(static_cast<std::uint32_t>(s.ret.size()));
  for (const auto r : s.ret) put(r);
  put(static_cast<std::uint32_t>(s.loaded.size()));
  for (const auto &l : s.loaded) { put(l.op); put(l.base); put(l.bytes); }
  return out;
}

class Walker {
public:
  Walker(const DiscImage &disc, const ProcessorImages &sub, const LoaderScanOptions &options) : disc_(disc), sub_(sub), options_(options) {
    const auto &image = sub.images.images.front();
    const auto &mapping = image.mappings.front();
    bytes_ = executable_image_bytes(sub.images, image);
    base_ = mapping.execution_base + mapping.first_offset;
    end_ = base_ + mapping.length;
    first_offset_ = mapping.first_offset;
    (void)disc_;
  }

  LoaderScan run() {
    // Registers other than the stack pointer are unknown at entry (the BIOS hands over nothing the walk may rely on).
    std::vector<std::uint32_t> roots;
    if (options_.module_table_roots) roots = module_table_roots();
    if (roots.empty()) roots.push_back(sub_.entry_pc);
    scan_.survey.roots = static_cast<std::uint32_t>(roots.size());
    std::vector<State> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
      State start;
      start.pc = *it;
      if (!options_.module_table_roots) { stack.push_back(std::move(start)); continue; }   // the legacy single root: states and pruning exactly as in M1
      if (seen_.insert(serialize(start)).second) { ++scan_.states; stack.push_back(std::move(start)); }
    }
    while (!stack.empty() && scan_.complete) {
      State s = std::move(stack.back());
      stack.pop_back();
      walk(std::move(s), stack);
    }
    auto &sv = scan_.survey;
    sv.cdd_commands = static_cast<std::uint32_t>(cdd_sites_.size());
    sv.cdc_transfer_triggers = static_cast<std::uint32_t>(trigger_sites_.size());
    for (const auto &[site, fn] : bios_sites_) { (void)site; ++sv.bios_service_calls; if (fn == 0xFFFFFFFFU) ++sv.bios_service_unknown_function; }
    sv.foreign_calls_other = static_cast<std::uint32_t>(foreign_other_.size());
    sv.foreign_calls_unknown = static_cast<std::uint32_t>(foreign_unknown_.size());
    sv.copy_loop_candidates = static_cast<std::uint32_t>(copy_sites_.size());
    sv.counted_loops = static_cast<std::uint32_t>(loop_sites_.size());
    sv.dispatch_resolved = static_cast<std::uint32_t>(dispatch_sites_.size());
    sv.table_reads = static_cast<std::uint32_t>(table_read_sites_.size());
    sv.memory_carried_reads = static_cast<std::uint32_t>(memory_read_sites_.size());
    return std::move(scan_);
  }

private:
  const DiscImage &disc_;
  const ProcessorImages &sub_;
  const LoaderScanOptions &options_;
  std::set<std::pair<std::string, std::uint32_t>> ends_;
  void end_at(const char *kind, std::uint32_t pc) { if (ends_.insert({kind, pc}).second) ++scan_.survey.path_ends[kind]; }
  std::set<std::uint32_t> table_read_sites_, dispatch_sites_, memory_read_sites_;
  std::set<std::uint32_t> cdd_sites_, trigger_sites_, copy_sites_, loop_sites_, foreign_other_, foreign_unknown_;
  std::set<std::pair<std::uint32_t, std::uint32_t>> bios_sites_;
  std::span<const std::uint8_t> bytes_;
  std::uint32_t base_{}, end_{}, first_offset_{};
  LoaderScan scan_;
  std::unordered_set<std::string> seen_;
  std::set<std::pair<LoaderRefusalKind, std::uint32_t>> refused_;

  // The SP module jump table (the BIOS enters an SP module through it): header word $18 is the table offset from the module start; the table
  // is four word offsets (init, main, int2, user) from the table start, zero meaning none. Only offsets that land inside the image count.
  std::vector<std::uint32_t> module_table_roots() {
    std::vector<std::uint32_t> roots;
    const std::uint32_t size = end_ - base_;
    const auto be32 = [&](std::uint32_t o) { return (std::uint32_t{bytes_[first_offset_ + o]} << 24) | (std::uint32_t{bytes_[first_offset_ + o + 1U]} << 16) | (std::uint32_t{bytes_[first_offset_ + o + 2U]} << 8) | bytes_[first_offset_ + o + 3U]; };
    const auto be16 = [&](std::uint32_t o) { return (std::uint32_t{bytes_[first_offset_ + o]} << 8) | bytes_[first_offset_ + o + 1U]; };
    if (size < 0x20U || bytes_.size() < first_offset_ + size) return roots;
    if (std::string_view(reinterpret_cast<const char *>(bytes_.data()) + first_offset_, 4U) != "MAIN") return roots;   // the module header's name field
    const std::uint32_t table = be32(0x18U);
    if ((table & 1U) != 0U || table < 0x20U || table + 8U > size) return roots;
    scan_.survey.module_table_found = true;
    for (std::uint32_t i = 0; i < 4U; ++i) {
      const std::uint32_t w = be16(table + 2U * i);
      const std::uint32_t at = table + w;
      if (w == 0U || (at & 1U) != 0U || at >= size) continue;
      if (std::find(roots.begin(), roots.end(), base_ + at) == roots.end()) roots.push_back(base_ + at);
    }
    return roots;
  }

  void refuse(LoaderRefusalKind kind, std::uint32_t pc) {
    if (refused_.insert({kind, pc}).second) scan_.refusals.push_back({kind, pc});
  }
  void exhausted(std::uint32_t pc) {
    refuse(LoaderRefusalKind::scan_limit_exceeded, pc);
    scan_.complete = false;
  }

  // ---- values ------------------------------------------------------------------------------------------------------------
  // A word/long at `address` inside the boot image (a constant when the table is not rewritten before the read; see LoaderScanOptions::constant_dispatch).
  Val image_read(std::uint32_t address, Width w) {
    const std::uint32_t n = w == Width::byte ? 1U : w == Width::word ? 2U : 4U;
    if (w == Width::byte || (address & 1U) != 0U || address < base_ || static_cast<std::uint64_t>(address) + n > end_) return {};
    const std::uint32_t off = first_offset_ + (address - base_);
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < n; ++i) v = (v << 8) | bytes_[off + i];
    table_read_sites_.insert(address);
    return K(v);
  }

  // The effective address of a (d8,PC|An,Xn) control/data operand when its base and index are known.
  std::optional<std::uint32_t> indexed_address(const State &s, const M68kEffectiveAddress &ea) const {
    Val base;
    if (ea.mode == M68kEaMode::pc_index8) base = K(ea.pc_base_address);
    else if (ea.mode == M68kEaMode::address_index8) base = s.a[ea.reg & 7U];
    else return std::nullopt;
    const Val index = ea.index_is_address ? s.a[ea.index_reg & 7U] : s.d[ea.index_reg & 7U];
    if (!base.known()) return std::nullopt;
    std::int64_t idx = 0;
    if (ea.index_is_long) {
      if (!index.known()) return std::nullopt;
      idx = static_cast<std::int32_t>(index.v);
    } else {
      const Val low = exact(index, Width::word);
      if (!low.known()) return std::nullopt;
      idx = static_cast<std::int16_t>(low.v);
    }
    return static_cast<std::uint32_t>((static_cast<std::int64_t>(base.v) + ea.displacement + idx) & 0x00FFFFFF);
  }

  Val read_ea(const State &s, const M68kEffectiveAddress &ea, Width w) {
    if (options_.constant_dispatch) {
      if (ea.mode == M68kEaMode::pc_index8 || ea.mode == M68kEaMode::address_index8) {
        const auto address = ea.mode == M68kEaMode::pc_index8 ? indexed_address(s, ea) : std::nullopt;   // only the immutable image is read
        return address ? image_read(*address, w) : Val{};
      }
      if (ea.mode == M68kEaMode::pc_disp16) return image_read(ea.absolute_address & 0x00FFFFFFU, w);
    }
    switch (ea.mode) {
    case M68kEaMode::immediate: return K(mask_of(w, ea.immediate_value));
    case M68kEaMode::data_register: return exact(s.d[ea.reg & 7U], w);
    case M68kEaMode::address_register: return exact(s.a[ea.reg & 7U], w);
    default: return {};
    }
  }

  static Val merge(const Val &old, const Val &val, Width w) {
    if (w == Width::long_word) return val;
    const std::uint32_t wm = width_mask(w);
    Val out;
    out.mask = (old.mask & ~wm) | (val.mask & wm);
    out.v = ((old.v & ~wm) | (val.v & wm)) & out.mask;
    return out;
  }

  void taint_hw(State &s) { s.hw = Hw{}; }

  // A write to memory whose location the walk may not know could be a Gate Array register: the shadow is dropped (A7 is the stack,
  // which no boot profile places in the register block).
  void write_memory_ea(State &s, const M68kEffectiveAddress &ea, Width w, const Val &val) {
    switch (ea.mode) {
    case M68kEaMode::data_register: s.d[ea.reg & 7U] = merge(s.d[ea.reg & 7U], val, w); return;
    case M68kEaMode::absolute_word:
    case M68kEaMode::absolute_long: {
      const std::uint32_t address = ea.absolute_address & 0x00FFFFFFU;
      if (address >= kGateArraySub && address < kGateArraySubEnd) hw_write(s, address - kGateArraySub, w, val);
      return;
    }
    case M68kEaMode::address_indirect:
    case M68kEaMode::address_postinc:
    case M68kEaMode::address_predec:
    case M68kEaMode::address_disp16:
    case M68kEaMode::address_index8:
      if ((ea.reg & 7U) != 7U) taint_hw(s);
      return;
    default: return;
    }
  }

  // ---- the CD interface ------------------------------------------------------------------------------------------------
  void cdd_command(State &s, std::uint32_t pc) {
    auto &h = s.hw;
    const auto digit = [&](std::size_t i, std::uint32_t &out) {
      if (!h.tx[i].known() || h.tx[i].v > 9U) return false;
      out = h.tx[i].v;
      return true;
    };
    if (!h.tx[0].known()) {  // an unknown command may be a play: the position is lost
      h.lba = {};
      h.play_invalid = false;
      return;
    }
    if ((h.tx[0].v & 0xFU) != 3U) {  // status (0) leaves the position; stop, seek, pause, TOC... move or end it: the walk no longer knows it
      if ((h.tx[0].v & 0xFU) != 0U) h.lba = {};
      return;
    }
    std::uint32_t t[8];
    bool ok = true;
    for (std::size_t i = 2; i < 8; ++i) ok = ok && digit(i, t[i]);
    if (!ok) {
      h.lba = {};
      h.play_invalid = true;
      return;
    }
    // The checksum over TX0..TX8 (the drive's own rule); an invalid checksum is refused, never repaired.
    std::uint32_t sum = 0;
    bool known_all = true;
    for (std::size_t i = 0; i < 9; ++i) {
      if (!h.tx[i].known()) { known_all = false; break; }
      sum += h.tx[i].v & 0xFU;
    }
    if (!known_all || !h.tx[9].known() || ((~sum) & 0xFU) != (h.tx[9].v & 0xFU)) {
      h.lba = {};
      h.play_invalid = true;
      refuse(LoaderRefusalKind::cdd_command_invalid, pc);
      return;
    }
    const std::int64_t msf = ((static_cast<std::int64_t>(t[2]) * 10 + t[3]) * 60 + (t[4] * 10 + t[5])) * 75 + (t[6] * 10 + t[7]);
    const std::int64_t lba = msf - 150;
    if (lba < 0) {
      h.lba = {};
      h.play_invalid = true;
      refuse(LoaderRefusalKind::cdd_command_invalid, pc);
      return;
    }
    h.lba = K(static_cast<std::uint32_t>(lba));
    h.play_invalid = false;
  }

  void cdc_register_write(State &s, std::uint32_t value, std::uint32_t pc) {
    auto &h = s.hw;
    if (!h.reg_ptr.known()) {  // every register may have been written: nothing the walk knows about the CDC survives
      h.ifctrl = h.dbcl = h.dbch = {};
      return;
    }
    const std::uint32_t reg = h.reg_ptr.v & 0xFU;
    switch (reg) {
    case 0x01: h.ifctrl = K(value & 0xFFU); break;
    case 0x02: h.dbcl = K(value & 0xFFU); break;
    case 0x03: h.dbch = K(value & 0x0FU); break;
    case 0x06: transfer(s, pc); break;
    case 0x0F: h.ifctrl = h.dbcl = h.dbch = {}; h.reg_ptr = K(0); return;  // reset: the register address is 0, no increment
    default: break;
    }
    if (h.reg_ptr.v != 0U) h.reg_ptr = K((h.reg_ptr.v + 1U) & 0xFU);
  }

  void transfer(State &s, std::uint32_t pc) {
    trigger_sites_.insert(pc);
    const auto &h = s.hw;
    // DOUTEN known clear: DTRG starts nothing. Unknown: the load is proposed anyway (a proposal can only fail closed at run time).
    if (h.ifctrl.known() && (h.ifctrl.v & 0x02U) == 0U) return;
    if (!h.lba.known()) {
      refuse(LoaderRefusalKind::lba_unknown, pc); return; }
    if (!h.dest.known() || !h.dma_address.known()) { refuse(LoaderRefusalKind::destination_unknown, pc); return; }
    if (h.dest.v != 7U) { refuse(LoaderRefusalKind::destination_unsupported, pc); return; }
    if (!h.dbcl.known() || !h.dbch.known()) { refuse(LoaderRefusalKind::length_unsupported, pc); return; }
    const std::uint32_t bytes = (((h.dbch.v & 0xFU) << 8) | h.dbcl.v) + 1U;
    if (bytes != kLoaderScanSectorBytes) { refuse(LoaderRefusalKind::length_unsupported, pc); return; }
    const std::uint32_t destination = kWordRamSub + ((h.dma_address.v << 3) & kDmaAddressMask);
    if (destination + bytes > kWordRamSubEnd) { refuse(LoaderRefusalKind::destination_unsupported, pc); return; }
    std::uint32_t op = 0;
    for (; op < scan_.operations.size(); ++op) {
      const auto &o = scan_.operations[op];
      if (o.trigger_pc == pc && o.lba == h.lba.v && o.destination == destination) break;
    }
    if (op == scan_.operations.size()) {
      if (scan_.operations.size() >= kLoaderScanMaxOperations) {
        refuse(LoaderRefusalKind::too_many_operations, pc);
        scan_.complete = false;
        return;
      }
      scan_.operations.push_back(LoadOperation{pc, h.lba.v, destination, bytes, {}});
    }
    auto &loaded = s.loaded;
    loaded.erase(std::remove_if(loaded.begin(), loaded.end(), [&](const Loaded &l) { return l.base < destination + bytes && destination < l.base + l.bytes; }),
                 loaded.end());  // a newer load replaces what it overlaps
    loaded.push_back({op, destination, bytes});
    s.hw.lba = {};  // the drive advances every 75 Hz tick: a further transfer needs a further play command
  }

  void hw_write(State &s, std::uint32_t off, Width w, const Val &val) {
    if (w == Width::long_word) {  // the runtime splits a long access into two word accesses the walk does not model: nothing known about the interface survives
      taint_hw(s);
      return;
    }
    auto &h = s.hw;
    const std::uint32_t pc = s.pc;
    if (off == 0x04U || off == 0x05U) {
      if (w == Width::word) {
        h.reg_ptr = val.known() ? K(val.v & 0xFU) : Val{};
        h.dest = val.known() ? K((val.v >> 8) & 7U) : Val{};
        h.dma_address = K(0);
      } else if (off == 0x04U) {
        h.dest = val.known() ? K(val.v & 7U) : Val{};
        h.dma_address = K(0);
      } else {
        h.reg_ptr = val.known() ? K(val.v & 0xFU) : Val{};
      }
    } else if ((off == 0x07U && w == Width::byte) || (off == 0x06U && w == Width::word)) {
      if (!val.known()) {  // an unknown data value: only the register it targets is lost
        const Val keep = h.reg_ptr;
        if (keep.known()) {
          switch (keep.v & 0xFU) {
          case 0x01: h.ifctrl = {}; break;
          case 0x02: h.dbcl = {}; break;
          case 0x03: h.dbch = {}; break;
          case 0x06: transfer(s, pc); break;
          case 0x0F: h.ifctrl = h.dbcl = h.dbch = {}; h.reg_ptr = K(0); return;
          default: break;
          }
          if (keep.v != 0U) h.reg_ptr = K((keep.v + 1U) & 0xFU);
        } else {
          h.ifctrl = h.dbcl = h.dbch = {};
        }
        return;
      }
      cdc_register_write(s, val.v, pc);
    } else if (off == 0x0AU || off == 0x0BU) {
      if (!val.known()) h.dma_address = {};
      else h.dma_address = K(w == Width::word ? (val.v & 0xFFFFU) : (((val.v & 0xFFU) << 8) | (val.v & 0xFFU)));
    } else if (off >= 0x42U && off < 0x4CU) {
      const std::size_t index = off - 0x42U;
      if (w == Width::word) {
        if (off == 0x4AU) {  // the word written to TX8/TX9 starts the command; the drive discards the pair after it checked the sum
          h.tx[8] = val.known() ? K((val.v >> 8) & 0xFFU) : Val{};
          h.tx[9] = val.known() ? K(val.v & 0xFFU) : Val{};
          cdd_sites_.insert(pc);
          cdd_command(s, pc);
          return;
        }
        h.tx[index] = val.known() ? K((val.v >> 8) & 0xFFU) : Val{};
        h.tx[index + 1U] = val.known() ? K(val.v & 0xFFU) : Val{};
      } else {
        h.tx[index] = val.known() ? K(val.v & 0xFFU) : Val{};
      }
    }
  }

  // ---- decode ------------------------------------------------------------------------------------------------------------
  std::optional<M68kIrOperation> decode_at(std::uint32_t pc) const {
    if (pc < base_ || pc >= end_ || (pc & 1U) != 0U) return std::nullopt;
    const std::uint32_t offset = pc - base_ + first_offset_;
    const auto visible = bytes_.subspan(0, static_cast<std::size_t>(first_offset_) + (end_ - base_));
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(visible, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) return std::nullopt;
    auto operation = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq) return std::nullopt;
    return operation;
  }

  // ---- data effect of one instruction --------------------------------------------------------------------------------
  void apply_data(State &s, const M68kIrOperation &op) {
    std::optional<State> pre;   // the state before the instruction, for the folded arithmetic and the table read below (the footprint clears the destination first)
    std::optional<State> pre_move;
    if (options_.constant_dispatch && op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::pc_index8) pre_move = s;
    if (options_.constant_dispatch && op.destination_ea.mode == M68kEaMode::data_register &&
        (op.kind == M68kIrKind::add || op.kind == M68kIrKind::add_immediate || op.kind == M68kIrKind::add_quick || op.kind == M68kIrKind::subtract ||
         op.kind == M68kIrKind::subtract_immediate || op.kind == M68kIrKind::subtract_quick))
      pre = s;
    const auto effect = m68k_operation_effect(op);
    const bool pure = op.kind == M68kIrKind::write_moveq || op.kind == M68kIrKind::write_move || op.kind == M68kIrKind::write_movea ||
                      op.kind == M68kIrKind::write_clr || op.kind == M68kIrKind::load_effective_address;
    // Kinds that write no data or address register other than the stack pointer (compares, tests, branches, calls, returns): arguments survive.
    const bool no_register_write =
        op.kind == M68kIrKind::compare || op.kind == M68kIrKind::compare_immediate || op.kind == M68kIrKind::compare_address ||
        op.kind == M68kIrKind::test_operand || op.kind == M68kIrKind::bit_test || op.kind == M68kIrKind::no_operation ||
        op.kind == M68kIrKind::general_branch || op.kind == M68kIrKind::branch_ne_short || op.kind == M68kIrKind::branch_always_short ||
        op.kind == M68kIrKind::bsr_call || op.kind == M68kIrKind::jump_general || op.kind == M68kIrKind::call_general ||
        op.kind == M68kIrKind::return_from_subroutine ||
        // written explicitly below / by the walker (the effect owner does not advertise a complete footprint for these)
        op.kind == M68kIrKind::write_moveq || op.kind == M68kIrKind::load_effective_address || op.kind == M68kIrKind::dbcc_loop;
    // The footprint first (what the walk does not model becomes Unknown); the explicit results below override it.
    // A MOVEM store (registers to memory) writes no data or address register except an auto-decremented A7 (modelled: the stack is never a register block).
    const bool movem_store = options_.constant_dispatch && op.kind == M68kIrKind::movem_transfer && op.movem_direction == M68kMovemDirection::registers_to_memory &&
                             (op.destination_ea.mode == M68kEaMode::address_predec || op.destination_ea.mode == M68kEaMode::address_indirect) && (op.destination_ea.reg & 7U) == 7U;
    if (no_register_write || movem_store) {
      // nothing is written
    } else if (!effect.register_write_footprint_complete) {
      for (auto &v : s.d) v = {};
      for (std::size_t i = 0; i < 7; ++i) s.a[i] = {};
    } else {
      for (unsigned r = 0; r < 8; ++r) {
        if ((effect.data_register_write_mask >> r) & 1U) s.d[r] = {};
        if (r < 7 && ((effect.address_register_write_mask >> r) & 1U)) s.a[r] = {};
      }
    }
    const Width w = op.size;
    if (pre) {   // ADD/SUB into a data register with known operands: the result is known (the condition codes are not modelled)
      const unsigned r = op.destination_ea.reg & 7U;
      const Val before = exact(pre->d[r], w);
      const Val src = read_ea(*pre, op.source_ea, w);
      if (before.known() && src.known()) {
        const bool sub = op.kind == M68kIrKind::subtract || op.kind == M68kIrKind::subtract_immediate || op.kind == M68kIrKind::subtract_quick;
        const std::uint32_t result = mask_of(w, sub ? before.v - src.v : before.v + src.v);
        s.d[r] = merge(pre->d[r], K(result), w);
      }
      return;
    }
    switch (op.kind) {
    case M68kIrKind::write_moveq:
      s.d[static_cast<unsigned>(op.destination) & 7U] = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(op.operand)));
      return;
    case M68kIrKind::write_move:
      write_memory_ea(s, op.destination_ea, w, read_ea(pre_move ? *pre_move : s, op.source_ea, w));
      return;
    case M68kIrKind::write_clr:
      write_memory_ea(s, op.destination_ea, w, K(0));
      return;
    case M68kIrKind::write_movea: {
      Val v = read_ea(s, op.source_ea, w);
      if (v.known() && w == Width::word) v = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v.v))));
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg != 7U) s.a[reg] = v;
      return;
    }
    case M68kIrKind::load_effective_address: {
      const auto &ea = op.source_ea;
      Val v{};
      if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::pc_disp16) v = K(ea.absolute_address);
      else if (ea.mode == M68kEaMode::address_indirect) v = s.a[ea.reg & 7U];
      else if (ea.mode == M68kEaMode::address_disp16 && s.a[ea.reg & 7U].known())
        v = K(s.a[ea.reg & 7U].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)));
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg != 7U) s.a[reg] = v;
      return;
    }
    default: break;
    }
    if (!pure) {
      // An instruction the walk does not model that stores to memory may store to a Gate Array register.
      const auto mode = op.destination_ea.mode;
      const bool stores = mode != M68kEaMode::unused && mode != M68kEaMode::data_register && mode != M68kEaMode::address_register;
      if (stores && op.kind != M68kIrKind::compare && op.kind != M68kIrKind::compare_immediate && op.kind != M68kIrKind::compare_address &&
          op.kind != M68kIrKind::test_operand && op.kind != M68kIrKind::bit_test) {
        if (mode == M68kEaMode::absolute_word || mode == M68kEaMode::absolute_long) {
          const std::uint32_t address = op.destination_ea.absolute_address & 0x00FFFFFFU;
          if (address >= kGateArraySub && address < kGateArraySubEnd) taint_hw(s);
        } else if (!((mode == M68kEaMode::address_predec || mode == M68kEaMode::address_postinc || mode == M68kEaMode::address_indirect) &&
                     (op.destination_ea.reg & 7U) == 7U)) {
          taint_hw(s);
        }
      }
      if (op.kind == M68kIrKind::movem_transfer) taint_hw(s);
    }
  }

  // ---- control -----------------------------------------------------------------------------------------------------------
  // A transfer to `target`: records the entry when it lands in a load of this path. Returns true when it did.
  bool note_entry(State &s, std::uint32_t target) {
    bool landed = false;
    for (const auto &l : s.loaded) {
      if (target >= l.base && target < l.base + l.bytes) {
        auto &entries = scan_.operations[l.op].entries;
        if (std::find(entries.begin(), entries.end(), target) == entries.end()) entries.push_back(target);
        landed = true;
      }
    }
    return landed;
  }

  void havoc_after_foreign_call(State &s) {
    for (auto &v : s.d) v = {};
    for (std::size_t i = 0; i < 7; ++i) s.a[i] = {};
    taint_hw(s);
  }

  void push(std::vector<State> &stack, State &&next) {
    if (scan_.states >= kLoaderScanMaxStates) { exhausted(next.pc); return; }
    if (!seen_.insert(serialize(next)).second) return;
    ++scan_.states;
    stack.push_back(std::move(next));
  }

  void walk(State s, std::vector<State> &stack) {
    for (;;) {
      if (!scan_.complete) return;
      if (++scan_.steps > kLoaderScanMaxSteps) { exhausted(s.pc); return; }
      const auto decoded = decode_at(s.pc);
      if (!decoded) { end_at("undecodable_or_outside", s.pc); return; }  // outside the boot image (a loaded module's code is not walked) or not an instruction
      const M68kIrOperation &op = *decoded;
      const std::uint32_t here = s.pc;
      const std::uint32_t next = here + op.provenance.length.value;
      if (op.kind == M68kIrKind::stop_until_interrupt) { end_at("stop", here); return; }
      if (op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::address_postinc && op.destination_ea.mode == M68kEaMode::address_postinc) copy_sites_.insert(here);
      if (op.kind == M68kIrKind::dbcc_loop) loop_sites_.insert(here);
      if (options_.constant_dispatch && op.kind == M68kIrKind::write_move && op.destination_ea.mode == M68kEaMode::data_register && (op.source_ea.reg & 7U) != 7U &&
          (op.source_ea.mode == M68kEaMode::address_indirect || op.source_ea.mode == M68kEaMode::address_postinc || op.source_ea.mode == M68kEaMode::address_disp16))
        memory_read_sites_.insert(here);
      apply_data(s, op);
      const auto control = m68k_control_successors(op);
      if (control.always_raises_exception) { end_at("exception", here); return; }
      if (control.dynamic != M68kDynamicControlFamily::none) {
        if (control.dynamic == M68kDynamicControlFamily::return_from_subroutine) {
          if (s.ret.empty()) { end_at("return_to_caller", here); return; }
          s.pc = s.ret.back();
          s.ret.pop_back();
        } else if (control.dynamic == M68kDynamicControlFamily::jump_address_indirect || control.dynamic == M68kDynamicControlFamily::call_address_indirect ||
                   control.dynamic == M68kDynamicControlFamily::jump_address_disp16 || control.dynamic == M68kDynamicControlFamily::call_address_disp16) {
          const bool call = control.dynamic == M68kDynamicControlFamily::call_address_indirect || control.dynamic == M68kDynamicControlFamily::call_address_disp16;
          Val target = s.a[op.source_ea.reg & 7U];
          if (target.known() && op.source_ea.mode == M68kEaMode::address_disp16)
            target = K(target.v + static_cast<std::uint32_t>(static_cast<std::int32_t>(op.source_ea.displacement)));
          if (!transfer_to(s, target, call, next, here, stack)) { end_at("computed_jump", here); return; }
        } else if (options_.constant_dispatch && (control.dynamic == M68kDynamicControlFamily::call_pc_index || control.dynamic == M68kDynamicControlFamily::jump_pc_index ||
                                                  control.dynamic == M68kDynamicControlFamily::call_address_index || control.dynamic == M68kDynamicControlFamily::jump_address_index) &&
                   indexed_address(s, op.source_ea)) {
          const bool call = control.dynamic == M68kDynamicControlFamily::call_pc_index || control.dynamic == M68kDynamicControlFamily::call_address_index;
          dispatch_sites_.insert(here);
          if (!transfer_to(s, K(*indexed_address(s, op.source_ea)), call, next, here, stack)) { end_at("computed_jump", here); return; }
        } else {
          const bool indexed_call = control.dynamic == M68kDynamicControlFamily::call_pc_index || control.dynamic == M68kDynamicControlFamily::call_address_index;
          end_at(control.dynamic == M68kDynamicControlFamily::call_pc_index ? "computed_call_pc_index" : control.dynamic == M68kDynamicControlFamily::call_address_index ? "computed_call_address_index"
                 : control.dynamic == M68kDynamicControlFamily::jump_pc_index ? "computed_jump_pc_index" : control.dynamic == M68kDynamicControlFamily::jump_address_index ? "computed_jump_address_index" : "computed_other", here);
          if (!s.loaded.empty()) refuse(LoaderRefusalKind::entry_unknown, here);
          if (options_.constant_dispatch) refuse(LoaderRefusalKind::computed_control_unresolved, here);
          if (!(indexed_call && options_.continue_after_computed_call)) return;
          havoc_after_foreign_call(s);
          s.pc = next;
        }
        if (!continue_or_fork(s, stack)) return;
        continue;
      }
      std::vector<M68kControlSuccessor> successors = control.successors;
      if (successors.empty()) return;
      // Direct calls: BSR/JSR to a constant.
      if (successors.size() == 1U && successors.front().kind == M68kControlSuccessorKind::call_target) {
        if (!transfer_to(s, K(successors.front().target), true, next, here, stack)) return;
        if (!continue_or_fork(s, stack)) return;
        continue;
      }
      if (successors.size() == 1U) {
        const auto &only = successors.front();
        if (only.kind == M68kControlSuccessorKind::branch_target) {
          if (!transfer_to(s, K(only.target), false, next, here, stack)) return;
          if (!continue_or_fork(s, stack)) return;
        } else {
          s.pc = only.target;  // straight-line flow is not a join point: no visited-set entry
        }
        continue;
      }
      // A conditional: both arms. The not-taken arm continues here; the taken arm is queued.
      for (const auto &successor : successors) {
        if (successor.kind == M68kControlSuccessorKind::conditional_target) {
          State taken = s;
          taken.pc = successor.target;
          if (op.kind == M68kIrKind::dbcc_loop) taken.d[static_cast<unsigned>(op.destination) & 7U] = {};
          if (!note_entry(taken, successor.target)) push(stack, std::move(taken));
        } else {
          s.pc = successor.target;
        }
      }
      if (op.kind == M68kIrKind::dbcc_loop) s.d[static_cast<unsigned>(op.destination) & 7U] = {};
      if (!continue_or_fork(s, stack)) return;
    }
  }

  // Admits the state at its (already set) pc into the visited set; false ends this path.
  bool continue_or_fork(State &s, std::vector<State> &) {
    if (scan_.states >= kLoaderScanMaxStates) { exhausted(s.pc); return false; }
    if (!seen_.insert(serialize(s)).second) return false;
    ++scan_.states;
    return true;
  }

  // Control transfer to `target` (a call when `call`, with continuation `next`). Returns false when the path ends here.
  bool transfer_to(State &s, const Val &target, bool call, std::uint32_t next, std::uint32_t here, std::vector<State> &) {
    if (!target.known()) {
      foreign_unknown_.insert(here);
      if (options_.constant_dispatch) refuse(LoaderRefusalKind::computed_control_unresolved, here);
      if (!s.loaded.empty()) refuse(LoaderRefusalKind::entry_unknown, here);
      if (call) { havoc_after_foreign_call(s); s.pc = next; return true; }
      return false;
    }
    if (note_entry(s, target.v)) {  // a proven entry into a loaded range: the module's own code is not walked
      if (!call) return false;
      havoc_after_foreign_call(s);
      s.pc = next;
      return true;
    }
    const bool inside = target.v >= base_ && target.v < end_;
    if (!inside) {
      if (target.v < base_) {  // a call/jump into the BIOS area below the SP image: the BIOS service interface, function number in D0.w
        const Val fn = exact(s.d[0], Width::word);
        if (fn.known()) { if (bios_sites_.insert({here, fn.v}).second) ++scan_.survey.bios_functions[fn.v]; }
        else bios_sites_.insert({here, 0xFFFFFFFFU});
      } else {
        foreign_other_.insert(here);
      }
      if (call) { havoc_after_foreign_call(s); s.pc = next; return true; }
      return false;
    }
    if (call) {
      if (s.ret.size() >= kLoaderScanMaxCallDepth) { exhausted(here); return false; }
      s.ret.push_back(next);
    }
    s.pc = target.v;
    return true;
  }
};

// ---- modules -----------------------------------------------------------------------------------------------------------------

struct Hull {
  bool ok{};
  std::uint32_t lo{}, hi{};  // image offsets, [lo, hi)
};

// The executable subrange: the hull of the instructions reachable by the emitter's own traversal rules from the proven entries.
Hull closure_hull(std::span<const std::uint8_t> sector, std::uint32_t base, const std::vector<std::uint32_t> &entry_offsets) {
  Hull hull;
  std::set<std::uint32_t> seen;
  std::vector<std::uint32_t> work;
  for (const auto e : entry_offsets) work.push_back(base + e);
  while (!work.empty()) {
    const std::uint32_t pc = work.back();
    work.pop_back();
    if (!seen.insert(pc).second) continue;
    if (pc < base || pc >= base + sector.size() || (pc & 1U) != 0U) continue;
    const std::uint32_t offset = pc - base;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(sector, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) continue;
    const auto op = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && op.kind == M68kIrKind::write_moveq) continue;
    const std::uint32_t length = op.provenance.length.value;
    if (!hull.ok) { hull.ok = true; hull.lo = offset; hull.hi = offset + length; }
    hull.lo = std::min(hull.lo, offset);
    hull.hi = std::max(hull.hi, offset + length);
    const auto control = m68k_control_successors(op);
    for (const auto &s : control.successors) work.push_back(s.target);
    if (control.stacked == M68kStackedContinuationKind::call_continuation || control.stacked == M68kStackedContinuationKind::exception_continuation)
      work.push_back(control.stacked_address);
  }
  return hull;
}

std::string hex32(std::uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

}  // namespace

LoaderScan scan_sega_cd_loaders(const DiscImage &disc, const BootImages &images, const LoaderScanOptions &options) {
  Walker walker(disc, images.sub, options);
  LoaderScan scan = walker.run();
  if (!scan.complete) return scan;  // an exhausted walk proposes nothing: the inventory is not labelled complete

  // Group the proven loads: one module per (sector, proven entry set); several destinations of one group are several mappings.
  struct Group {
    std::uint32_t lba{};
    std::vector<std::uint32_t> entry_offsets;
    std::vector<std::uint32_t> bases;
    std::vector<std::uint32_t> ops;
  };
  std::vector<Group> groups;
  for (std::uint32_t i = 0; i < scan.operations.size(); ++i) {
    const auto &op = scan.operations[i];
    if (op.entries.empty()) {
      scan.refusals.push_back({LoaderRefusalKind::loaded_not_executed, op.trigger_pc});
      continue;
    }
    std::vector<std::uint32_t> offsets;
    for (const auto e : op.entries) offsets.push_back(e - op.destination);
    std::sort(offsets.begin(), offsets.end());
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) { return g.lba == op.lba && g.entry_offsets == offsets; });
    if (it == groups.end()) {
      groups.push_back(Group{op.lba, offsets, {}, {}});
      it = groups.end() - 1;
    }
    const std::uint32_t base = op.destination;
    if (std::find(it->bases.begin(), it->bases.end(), base) == it->bases.end()) it->bases.push_back(base);
    it->ops.push_back(i);
  }
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return scan;
  const auto &track = disc.tracks().front();
  std::map<std::uint32_t, unsigned> per_lba;
  for (auto &g : groups) {
    if (scan.modules.size() >= kModuleMaxCount) {
      scan.refusals.push_back({LoaderRefusalKind::too_many_modules, 0U});
      scan.complete = false;
      scan.modules.clear();  // an exhausted inventory proposes nothing
      return scan;
    }
    std::vector<std::uint8_t> sector(kLoaderScanSectorBytes);
    if (g.lba < kIso9660FirstDescriptor || static_cast<std::uint64_t>(g.lba) + 1U > track.sector_count ||
        disc.read_user_data(track.first_lba + g.lba, std::span<std::uint8_t, kDiscUserBytes>(sector.data(), kDiscUserBytes)) != DiscError::none) {
      scan.refusals.push_back({LoaderRefusalKind::source_outside_track, scan.operations[g.ops.front()].trigger_pc});
      continue;
    }
    std::sort(g.bases.begin(), g.bases.end());
    // The hull at each base (an absolute transfer inside the module follows the base it was linked for); the union is the subrange.
    Hull hull;
    for (const auto base : g.bases) {
      const auto h = closure_hull(sector, base, g.entry_offsets);
      if (!h.ok) continue;
      if (!hull.ok) hull = h;
      hull.lo = std::min(hull.lo, h.lo);
      hull.hi = std::max(hull.hi, h.hi);
    }
    if (!hull.ok) {
      scan.refusals.push_back({LoaderRefusalKind::entry_not_code, scan.operations[g.ops.front()].trigger_pc});
      continue;
    }
    DiscoveredModule m;
    const unsigned k = per_lba[g.lba]++;
    m.name = "lba" + std::to_string(g.lba) + (k == 0U ? "" : "_" + std::to_string(k));
    m.block = g.lba;
    m.size = kLoaderScanSectorBytes;
    m.sha256 = sha256_hex(sector);
    m.bases = g.bases;
    m.entries = g.entry_offsets;
    m.range_offset = hull.lo & ~1U;
    m.range_length = ((hull.hi - m.range_offset) + 1U) & ~1U;
    scan.modules.push_back(std::move(m));
  }
  std::sort(scan.modules.begin(), scan.modules.end(), [](const DiscoveredModule &a, const DiscoveredModule &b) { return a.name < b.name; });
  std::sort(scan.refusals.begin(), scan.refusals.end(), [](const LoaderRefusal &a, const LoaderRefusal &b) {
    return std::tie(a.kind, a.pc) < std::tie(b.kind, b.pc);
  });
  if (!scan.modules.empty()) {
    std::ostringstream text;
    text << kModuleManifestVersion << "\n# discovered by the static loader scan (SEG-097-T001, ADR 0206); admitted unchanged by the module list\n"
         << "disc " << disc.identity() << "\n";
    for (const auto &m : scan.modules) {
      text << "module " << m.name << "\nprocessor sub\nextent " << m.block << " " << m.size << "\nsha256 " << m.sha256 << "\n";
      for (const auto base : m.bases) text << "map " << hex32(base) << "\n";
      text << "range " << hex32(m.range_offset) << " " << hex32(m.range_length) << "\n";
      for (const auto entry : m.entries) text << "entry " << hex32(entry) << "\n";
      text << "verify byte_identity\nend\n";
    }
    scan.manifest = text.str();
    scan.identity = sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(scan.manifest.data()), scan.manifest.size()));
  }
  return scan;
}

}  // namespace segarecomp::machine::sega_cd
