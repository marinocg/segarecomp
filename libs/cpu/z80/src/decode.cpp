#include "segarecomp/cpu/z80/decode.hpp"

#include <cstddef>

namespace segarecomp::cpu::z80 {
namespace {

constexpr std::size_t kAddressSpace = 65536;
constexpr std::uint8_t kDd = 0xDD;
constexpr std::uint8_t kFd = 0xFD;

bool is_prefix(const FetchedByte& b) { return b.kind == FetchKind::byte && (b.value == kDd || b.value == kFd); }

StartClassification blocked(const FetchedByte& b, std::uint16_t address) {
  StartClassification c;
  c.kind = b.kind == FetchKind::unresolved_mapping ? StartKind::unresolved_fetch_mapping : StartKind::mutable_code;
  c.blocking_address = address;
  return c;
}

// Decode the instruction whose first non-prefix byte is at `op_addr`, after `prefix_count` DD/FD prefixes of which
// the last is `effective`. `get(address)` is the logical fetch. Constant work (at most four bytes).
template <class Get>
StartClassification finish(const Get& get, std::uint16_t start, const FetchedByte& first, std::uint32_t prefix_count,
                           EffectivePrefix effective, std::uint16_t op_addr) {
  const FetchedByte op = get(op_addr);
  if (op.kind != FetchKind::byte) return blocked(op, op_addr);
  const Space lead = effective == EffectivePrefix::dd ? Space::dd : effective == EffectivePrefix::fd ? Space::fd : Space::base;
  const ByteClass lead_class = classify_opcode_byte(lead, op.value);

  FormId form = kNoForm;
  std::uint32_t consumed = 1;  // bytes from op_addr
  std::uint32_t extra = prefix_count;
  std::uint8_t opcode = op.value;

  auto next = [&](std::uint32_t n, FetchedByte& out) {
    out = get(static_cast<std::uint16_t>(op_addr + n));
    return out.kind == FetchKind::byte;
  };
  FetchedByte second;
  switch (lead_class.kind) {
    case ByteClassKind::form:
      form = lead_class.form;
      extra = effective == EffectivePrefix::none ? 0 : prefix_count - 1;
      break;
    case ByteClassKind::prefix_ignored:
      form = lead_class.form;  // base-space form executes; every prefix is a no-operation
      break;
    case ByteClassKind::escape_cb:
    case ByteClassKind::escape_ed:
    case ByteClassKind::prefix_ignored_before_ed: {
      if (!next(1, second)) return blocked(second, static_cast<std::uint16_t>(op_addr + 1));
      const bool cb = lead_class.kind == ByteClassKind::escape_cb;
      form = classify_opcode_byte(cb ? Space::cb : Space::ed, second.value).form;
      opcode = second.value;
      consumed = 2;
      break;
    }
    case ByteClassKind::escape_ddcb:
    case ByteClassKind::escape_fdcb: {
      FetchedByte d, last;
      if (!next(1, d)) return blocked(d, static_cast<std::uint16_t>(op_addr + 1));
      if (!next(2, last)) return blocked(last, static_cast<std::uint16_t>(op_addr + 2));
      form = classify_opcode_byte(lead_class.kind == ByteClassKind::escape_ddcb ? Space::ddcb : Space::fdcb, last.value).form;
      opcode = last.value;
      extra = prefix_count - 1;
      consumed = 3;
      break;
    }
    default:  // prefix_chain / escape_dd / escape_fd cannot be an opcode after the prefix scan
      return {};
  }

  const FormDescriptor& desc = form_descriptor(form);
  Provenance prov;
  prov.image_id = first.image_id;
  prov.address = start;
  prov.image_offset = first.image_offset;
  prov.prefix_count = prefix_count;
  prov.effective_prefix = effective;
  prov.form = form;
  prov.opcode = opcode;

  if (desc.space == Space::ddcb || desc.space == Space::fdcb) {
    prov.operands[0] = get(static_cast<std::uint16_t>(op_addr + 1)).value;  // displacement (already fetched above)
    prov.operand_count = 1;
  } else {
    // Displacement/immediate bytes follow the opcode byte(s): length - opcode_index - 1 of them.
    const std::uint8_t operand_total = static_cast<std::uint8_t>(desc.length - desc.opcode_index - 1);
    for (std::uint8_t i = 0; i < operand_total; ++i) {
      const std::uint16_t address = static_cast<std::uint16_t>(op_addr + consumed + i);
      const FetchedByte b = get(address);
      if (b.kind != FetchKind::byte) return blocked(b, address);
      prov.operands[i] = b.value;
    }
    prov.operand_count = operand_total;
    consumed += operand_total;
  }
  prov.logical_byte_count = prefix_count + consumed;

  StartClassification result;
  result.kind = StartKind::decoded;
  result.instruction.provenance = prov;
  result.instruction.form = form;
  result.instruction.extra_prefix_count = extra;
  return result;
}

}  // namespace

TableFetch::TableFetch() : table_(kAddressSpace) {}

TableFetch TableFetch::invariant(std::uint32_t image_id, std::uint8_t fill) {
  TableFetch t;
  for (std::size_t a = 0; a < kAddressSpace; ++a)
    t.table_[a] = {FetchKind::byte, fill, image_id, static_cast<std::uint32_t>(a)};
  return t;
}

StartClassification decode_at(const LogicalFetch& fetch, std::uint16_t address) {
  auto get = [&](std::uint16_t a) { return fetch.fetch(a); };
  const FetchedByte first = get(address);
  std::uint32_t prefix_count = 0;
  EffectivePrefix effective = EffectivePrefix::none;
  std::uint16_t cursor = address;
  FetchedByte byte = first;
  while (is_prefix(byte)) {
    ++prefix_count;
    effective = byte.value == kDd ? EffectivePrefix::dd : EffectivePrefix::fd;
    if (prefix_count == kAddressSpace) {  // the run revisited its start with the same effective prefix
      StartClassification lock;
      lock.kind = StartKind::prefix_lock;
      return lock;
    }
    cursor = static_cast<std::uint16_t>(cursor + 1);
    byte = get(cursor);
  }
  return finish(get, address, first, prefix_count, effective, cursor);
}

std::vector<StartClassification> classify_all(const LogicalFetch& fetch) {
  std::vector<FetchedByte> table(kAddressSpace);
  for (std::size_t a = 0; a < kAddressSpace; ++a) table[a] = fetch.fetch(static_cast<std::uint16_t>(a));
  auto get = [&](std::uint16_t a) { return table[a]; };

  // end[a]: first address at or after a (circular) whose byte is not a resolved DD/FD prefix.
  std::vector<std::uint32_t> end(kAddressSpace, 0);
  std::size_t anchor = kAddressSpace;
  for (std::size_t a = 0; a < kAddressSpace; ++a) {
    if (!is_prefix(table[a])) {
      anchor = a;
      break;
    }
  }
  std::vector<StartClassification> out(kAddressSpace);
  if (anchor == kAddressSpace) {  // the whole mapping is one DD/FD run
    for (auto& c : out) c.kind = StartKind::prefix_lock;
    return out;
  }
  end[anchor] = static_cast<std::uint32_t>(anchor);
  for (std::size_t step = 1; step < kAddressSpace; ++step) {
    const std::size_t a = (anchor + kAddressSpace - step) % kAddressSpace;
    end[a] = is_prefix(table[a]) ? end[(a + 1) % kAddressSpace] : static_cast<std::uint32_t>(a);
  }
  for (std::size_t a = 0; a < kAddressSpace; ++a) {
    const std::uint32_t e = end[a];
    const std::uint32_t k = static_cast<std::uint32_t>((e + kAddressSpace - a) % kAddressSpace);
    EffectivePrefix effective = EffectivePrefix::none;
    if (k > 0) effective = table[(e + kAddressSpace - 1) % kAddressSpace].value == kDd ? EffectivePrefix::dd : EffectivePrefix::fd;
    out[a] = finish(get, static_cast<std::uint16_t>(a), table[a], k, effective, static_cast<std::uint16_t>(e));
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> replay_bytes(const LogicalFetch& fetch, const Provenance& provenance) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(provenance.logical_byte_count);
  std::uint16_t address = provenance.address;
  for (std::uint32_t i = 0; i < provenance.logical_byte_count; ++i, address = static_cast<std::uint16_t>(address + 1)) {
    const FetchedByte b = fetch.fetch(address);
    if (b.kind != FetchKind::byte) return std::nullopt;
    bytes.push_back(b.value);
  }
  return bytes;
}

std::uint32_t logical_length(const DecodedInstruction& instruction) {
  return instruction.extra_prefix_count + form_descriptor(instruction.form).length;
}

std::optional<std::int8_t> displacement(const DecodedInstruction& instruction) {
  const FormDescriptor& f = form_descriptor(instruction.form);
  if (f.displacement_index == kNoIndex) return std::nullopt;
  return static_cast<std::int8_t>(instruction.provenance.operands[0]);
}

std::optional<std::uint16_t> immediate(const DecodedInstruction& instruction) {
  const FormDescriptor& f = form_descriptor(instruction.form);
  if (f.immediate_index == kNoIndex) return std::nullopt;
  const std::size_t base = f.displacement_index == kNoIndex ? 0 : 1;
  std::uint16_t value = instruction.provenance.operands[base];
  if (f.immediate_size == 2) value = static_cast<std::uint16_t>(value | (instruction.provenance.operands[base + 1] << 8));
  return value;
}

std::uint64_t t_states(const DecodedInstruction& instruction, TimingOutcome outcome) {
  const Timing& t = form_descriptor(instruction.form).timing;
  const std::uint64_t extra = 4ULL * instruction.extra_prefix_count;
  if (outcome == TimingOutcome::primary || t.klass == TimingClass::fixed) return t.primary + extra;
  return t.klass == TimingClass::halt ? t.alternate : t.alternate + extra;
}

std::uint32_t m1_fetches(const DecodedInstruction& instruction) {
  return instruction.extra_prefix_count + form_descriptor(instruction.form).m1_fetches;
}

}  // namespace segarecomp::cpu::z80
