// SEG-069-T004: the controller-operable file picker's model (file_picker.hpp): it cannot leave its root, lists only allowed files, never follows symlinks,
// and re-checks a file when it is chosen. Synthetic folders only.
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

#include "file_picker.hpp"

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
void touch(const fs::path &p, const std::string &text = "x") { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << text; }
std::vector<std::string> names(const PickerListing &l) {
  std::vector<std::string> out;
  for (const auto &e : l.entries) out.push_back((e.directory ? "[" : "") + e.name + (e.directory ? "]" : ""));
  return out;
}
}  // namespace

int main() {
  const fs::path base = fs::temp_directory_path() / "segarecomp_picker_test";
  fs::remove_all(base);
  const fs::path root = base / "roms";
  touch(root / "Sonic.md");
  touch(root / "alpha.SMS");
  touch(root / "bundle.zip");
  touch(root / "notes.txt");
  touch(root / ".hidden.md");
  touch(root / "Zeta" / "deep.bin");
  touch(root / "beta" / "x.md");
  touch(base / "roms-evil" / "steal.md");  // shares the root's name as a prefix: not inside it
  touch(base / "outside.md");
  const std::vector<std::string> rom = {"md", "bin", "gen", "smd", "sms", "zip"};

  {  // listing: folders first, then allowed files, case-insensitive, no hidden, no other types
    const PickerListing l = picker_list(root, root, rom);
    expect(l.error.empty() && !l.truncated, "the root lists without error");
    const std::vector<std::string> expected = {"[beta]", "[Zeta]", "alpha.SMS", "bundle.zip", "Sonic.md"};
    expect(names(l) == expected, "folders first, allowed files only, sorted case-insensitively");
  }
  {  // another purpose lists other extensions only
    touch(root / "cover.PNG");
    touch(root / "cover2.jpeg");
    const PickerListing l = picker_list(root, root, {"png", "jpg", "jpeg"});
    expect(names(l) == std::vector<std::string>({"[beta]", "[Zeta]", "cover.PNG", "cover2.jpeg"}), "a cover purpose lists images only");
  }
  {  // containment is lexical and exact
    expect(picker_contains(root, root) && picker_contains(root, root / "beta" / "x.md"), "the root and its descendants are inside");
    expect(!picker_contains(root, base / "roms-evil" / "steal.md"), "a sibling that shares the root's name as a prefix is outside");
    expect(!picker_contains(root, root / ".." / "outside.md"), "a ../ escape is outside");
    expect(!picker_contains(root, root / "beta" / ".." / ".." / "outside.md"), "a nested ../ escape is outside");
    expect(picker_contains(root, root / "beta" / ".." / "Zeta"), "a ../ that stays inside is fine");
    expect(!picker_contains(root, base), "the parent of the root is outside");
    expect(!picker_contains(fs::path{}, root), "an empty root contains nothing");
  }
  {  // a folder outside the root is refused without listing it
    const PickerListing l = picker_list(root, base, rom);
    expect(!l.error.empty() && l.entries.empty(), "listing the parent of the root is refused");
    const PickerListing e = picker_list(root, root / ".." / "roms-evil", rom);
    expect(!e.error.empty() && e.entries.empty(), "a ../ detour to a sibling is refused");
    expect(!picker_list(root, root / "missing", rom).error.empty(), "a missing folder is an error, not an empty list");
  }
  {  // navigating up stops at the root
    expect(picker_parent(root, root).empty(), "the root has no parent inside the picker (the caller returns to the roots)");
    expect(picker_parent(root, root / "beta") == root.lexically_normal(), "a subfolder's parent is the root");
    expect(picker_parent(root, base).empty(), "a folder outside the root has no parent inside it");
  }
  {  // symlinks are neither followed nor offered
    std::error_code ec;
    fs::create_directory_symlink(base, root / "escape", ec);
    const bool dir_link = !ec;
    fs::create_symlink(base / "outside.md", root / "link.md", ec);
    const bool file_link = !ec;
    const PickerListing l = picker_list(root, root, rom);
    const auto n = names(l);
    expect(std::find(n.begin(), n.end(), "[escape]") == n.end() || !dir_link, "a symlinked folder is not offered");
    expect(std::find(n.begin(), n.end(), "link.md") == n.end() || !file_link, "a symlinked file is not offered");
    if (file_link) {
      std::string why;
      expect(!picker_accept(root, root / "link.md", rom, &why) && !why.empty(), "a symlinked file is refused at choose time");
    }
    expect(!picker_list(root, root / "escape", rom).error.empty() || !dir_link, "a symlinked folder cannot be entered");
  }
  {  // choose-time checks
    std::string why;
    expect(picker_accept(root, root / "Sonic.md", rom, &why), "an allowed file inside the root is accepted");
    expect(picker_accept(root, root / "beta" / "x.md", rom), "a nested allowed file is accepted");
    expect(!picker_accept(root, root / "notes.txt", rom, &why) && why.find("type") != std::string::npos, "a wrong type is refused");
    expect(!picker_accept(root, base / "outside.md", rom, &why) && why.find("outside") != std::string::npos, "a file outside the root is refused");
    expect(!picker_accept(root, root / ".." / "outside.md", rom), "a ../ path is refused");
    expect(!picker_accept(root, root / "beta", rom), "a folder is not a file");
    expect(!picker_accept(root, root / "gone.md", rom), "a vanished file is refused");
  }
  {  // a huge folder is capped, not enumerated forever
    for (int i = 0; i < static_cast<int>(picker_max_entries) + 20; ++i) touch(base / "big" / ("f" + std::to_string(i) + ".md"), "");
    const PickerListing l = picker_list(base / "big", base / "big", rom);
    expect(l.truncated && l.entries.size() <= picker_max_entries, "a folder with thousands of files is truncated");
  }
  {  // roots: the test seam first, only existing folders, no duplicates
    const auto roots = picker_roots(root);
    expect(!roots.empty() && roots.front().path == root.lexically_normal() && roots.front().label == "Selected folder", "the extra root comes first");
    const auto twice = picker_roots(root);
    std::size_t count = 0;
    for (const auto &r : twice) if (r.path == root.lexically_normal()) ++count;
    expect(count == 1, "a root is never listed twice");
    expect(picker_roots(base / "does-not-exist").empty() || picker_roots(base / "does-not-exist").front().label != "Selected folder", "a missing extra root is ignored");
  }
  fs::remove_all(base);
  if (failures == 0) std::cout << "file_picker_test: ok\n";
  return failures == 0 ? 0 : 1;
}
