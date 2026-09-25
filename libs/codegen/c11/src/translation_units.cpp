#include "segarecomp/codegen/c11/translation_units.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <streambuf>
#include <system_error>
#include <utility>

namespace segarecomp {
namespace {

constexpr std::string_view rejection_prefix = "/* translation rejected";

std::string two_digits(std::size_t value) {
  std::string text = std::to_string(value);
  return text.size() < 2 ? "0" + text : text;
}

std::string guard_name(const std::string &stem) {
  std::string name = "SEGARECOMP_GENERATED_";
  for (const char c : stem) name += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
  return name + "_H";
}

}  // namespace

namespace detail {
class ShardBuffer final : public std::streambuf {
 public:
  ShardBuffer(std::filesystem::path directory, std::string stem, std::vector<TranslationUnitFamily> families)
      : directory_(std::move(directory)), stem_(std::move(stem)), families_(std::move(families)), out_(this) {
    for (const auto &family : families_)
      if (family.name.empty() || family.shards == 0U) error_ = "invalid translation-unit family";
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    // Invalidate any earlier set with this stem first (manifest, then the files it named).
    std::filesystem::remove(directory_ / (stem_ + ".units"), ignored);
    for (const auto &entry : std::filesystem::directory_iterator(directory_, ignored)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with(stem_ + "_") && (name.ends_with(".c") || name.ends_with(".c.partial"))) std::filesystem::remove(entry.path(), ignored);
    }
    std::filesystem::remove(directory_ / (stem_ + ".h"), ignored);
    header_ = open(stem_ + ".h");
    if (header_ != nullptr)
      *header_ << "#ifndef " << guard_name(stem_) << "\n#define " << guard_name(stem_) << "\n";
    main_ = tu("main");
  }

  ~ShardBuffer() override {
    if (!finished_) discard();
  }

  std::ostream &stream() { return out_; }

  void header(bool on) {
    if (unit_open_) fail("shard header block inside a unit");
    in_header_ = on;
  }
  void declare(std::string_view declaration) {
    if (header_ != nullptr) *header_ << declaration << ";\n";
  }
  void begin_unit(std::string_view family, std::uint64_t key, std::string_view declaration) {
    if (unit_open_ || in_header_) { fail("nested or misplaced translation unit"); return; }
    const auto found = std::ranges::find_if(families_, [&](const auto &f) { return f.name == family; });
    if (found == families_.end()) { fail("unknown translation-unit family"); return; }
    unit_open_ = true;
    unit_target_ = tu(std::string(family) + "_" + two_digits(static_cast<std::size_t>((key >> found->page_shift) % found->shards)));
    unit_declaration_ = std::string(declaration);
    unit_text_.clear();
    declare(declaration);
  }
  void end_unit() {
    if (!unit_open_) { fail("unit end without begin"); return; }
    unit_open_ = false;
    if (unit_text_.starts_with(rejection_prefix)) { fail("translation rejected inside a unit"); return; }
    const std::string definition = "static " + unit_declaration_;
    const auto at = unit_text_.find(definition);
    if (at != std::string::npos) unit_text_.erase(at, 7U);
    if (unit_target_ != nullptr) *unit_target_ << unit_text_;
    unit_text_.clear();
  }

  std::string finish() {
    if (unit_open_) fail("unterminated translation unit");
    if (!error_.empty()) { discard(); return error_; }
    if (header_ != nullptr) *header_ << "#endif\n";
    std::vector<std::string> names;
    for (auto &[name, file] : files_) {
      file->flush();
      if (!*file) { error_ = "generated-C write failed"; break; }
      file->close();
    }
    if (error_.empty()) {
      std::error_code failure;
      for (const auto &[name, file] : files_) {
        std::filesystem::rename(directory_ / (name + ".partial"), directory_ / name, failure);
        if (failure) { error_ = "cannot finalize generated-C output"; break; }
      }
      for (const auto &[name, file] : files_)
        if (name.ends_with(".c")) names.push_back(name);
    }
    if (!error_.empty()) { discard(); return error_; }
    std::ranges::sort(names, [&](const auto &a, const auto &b) {
      const bool a_main = a == stem_ + "_main.c", b_main = b == stem_ + "_main.c";
      return a_main != b_main ? a_main : a < b;
    });
    {
      std::ofstream manifest(directory_ / (stem_ + ".units.partial"), std::ios::binary | std::ios::trunc);
      for (const auto &name : names) manifest << name << '\n';
      manifest.flush();
      if (!manifest) { error_ = "generated-C manifest write failed"; discard(); return error_; }
    }
    std::error_code failure;
    std::filesystem::rename(directory_ / (stem_ + ".units.partial"), directory_ / (stem_ + ".units"), failure);
    if (failure) { error_ = "cannot finalize generated-C manifest"; discard(); return error_; }
    finished_ = true;
    unit_count_ = names.size();
    return {};
  }

  std::size_t unit_count() const { return unit_count_; }
  std::size_t max_units() const {
    std::size_t total = 1U;
    for (const auto &family : families_) total += family.shards;
    return total;
  }

 protected:
  int_type overflow(int_type c) override {
    if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
    const char ch = traits_type::to_char_type(c);
    return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
  }
  std::streamsize xsputn(const char *data, std::streamsize count) override {
    if (!error_.empty()) return 0;
    if (in_header_) { if (header_ != nullptr) header_->write(data, count); }
    else if (unit_open_) unit_text_.append(data, static_cast<std::size_t>(count));
    else if (main_ != nullptr) main_->write(data, count);
    return count;
  }

 private:
  void fail(const char *why) {
    if (error_.empty()) error_ = why;
    out_.setstate(std::ios::badbit);
  }
  std::ofstream *open(const std::string &name) {
    auto file = std::make_unique<std::ofstream>(directory_ / (name + ".partial"), std::ios::binary | std::ios::trunc);
    if (!*file) { fail("cannot open generated-C output"); return nullptr; }
    auto *raw = file.get();
    files_.emplace(name, std::move(file));
    return raw;
  }
  std::ofstream *tu(const std::string &suffix) {
    const auto name = stem_ + "_" + suffix + ".c";
    if (const auto found = files_.find(name); found != files_.end()) return found->second.get();
    auto *file = open(name);
    // Every TU starts identically: feature-test macro first, then the shared header.
    if (file != nullptr) *file << "#define _POSIX_C_SOURCE 200809L\n#include \"" << stem_ << ".h\"\n";
    return file;
  }
  void discard() {
    for (auto &[name, file] : files_) file->close();
    std::error_code ignored;
    for (const auto &[name, file] : files_) {
      std::filesystem::remove(directory_ / (name + ".partial"), ignored);
      std::filesystem::remove(directory_ / name, ignored);
    }
    std::filesystem::remove(directory_ / (stem_ + ".units.partial"), ignored);
    std::filesystem::remove(directory_ / (stem_ + ".units"), ignored);
    files_.clear();
    header_ = nullptr;
    main_ = nullptr;
    unit_target_ = nullptr;
  }

  std::filesystem::path directory_;
  std::string stem_;
  std::vector<TranslationUnitFamily> families_;
  std::ostream out_;
  std::map<std::string, std::unique_ptr<std::ofstream>> files_;  // ordered by name: deterministic
  std::ofstream *header_ = nullptr, *main_ = nullptr, *unit_target_ = nullptr;
  bool in_header_ = false, unit_open_ = false, finished_ = false;
  std::string unit_declaration_, unit_text_, error_;
  std::size_t unit_count_ = 0;
};
}  // namespace detail

TranslationUnitSharder::TranslationUnitSharder(std::filesystem::path directory, std::string stem, std::vector<TranslationUnitFamily> families)
    : impl_(std::make_unique<detail::ShardBuffer>(std::move(directory), std::move(stem), std::move(families))) {}
TranslationUnitSharder::~TranslationUnitSharder() = default;
std::ostream &TranslationUnitSharder::stream() { return impl_->stream(); }
std::string TranslationUnitSharder::finish() { return impl_->finish(); }
std::size_t TranslationUnitSharder::translation_unit_count() const { return impl_->unit_count(); }
std::size_t TranslationUnitSharder::max_translation_unit_count() const { return impl_->max_units(); }

namespace {
detail::ShardBuffer *impl_of(std::ostream &out) { return dynamic_cast<detail::ShardBuffer *>(out.rdbuf()); }
}  // namespace

bool sharding_active(std::ostream &out) { return impl_of(out) != nullptr; }
void shard_begin_header(std::ostream &out) { if (auto *i = impl_of(out)) i->header(true); }
void shard_end_header(std::ostream &out) { if (auto *i = impl_of(out)) i->header(false); }
void shard_declare(std::ostream &out, std::string_view declaration) { if (auto *i = impl_of(out)) i->declare(declaration); }
void shard_begin_unit(std::ostream &out, std::string_view family, std::uint64_t key, std::string_view declaration) {
  if (auto *i = impl_of(out)) i->begin_unit(family, key, declaration);
}
void shard_end_unit(std::ostream &out) { if (auto *i = impl_of(out)) i->end_unit(); }

}  // namespace segarecomp
