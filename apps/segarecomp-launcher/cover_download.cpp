#include "cover_download.hpp"

#include "covers.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace launcher {

namespace {

constexpr auto kListingMaxAge = std::chrono::hours(24 * 7);

// The region tags Libretro's Sega box arts use most, in a neutral order; reordered by the user's preferences.
const std::vector<std::string> kRegionTags = {
    "(USA, Europe)", "(World)", "(USA)", "(Europe)", "(Japan, USA)", "(Japan, Europe)", "(USA, Europe, Korea)", "(Japan)",
    "(USA, Europe, Brazil)", "(Europe, Brazil)", "(Brazil)", "(Korea)",
};

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string collapse_spaces(const std::string &text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

bool is_digest_stem(const std::string &stem) {
  return stem.size() == 64 && std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string file_stem(const std::string &name) {
  std::string base = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
  if (const auto dot = base.find_last_of('.'); dot != std::string::npos && dot > 0) base.erase(dot);
  return base;
}

// The name without its "(...)" / "[...]" tags.
std::string strip_tags(const std::string &name) {
  std::string out;
  int depth = 0;
  for (const char c : name) {
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    else if (depth == 0) out.push_back(c);
  }
  return collapse_spaces(out);
}

bool roman_numeral(const std::string &word) {
  return !word.empty() && word.size() <= 4 && std::all_of(word.begin(), word.end(), [](char c) { return c == 'I' || c == 'V' || c == 'X'; });
}

// "SONIC THE HEDGEHOG II" -> "Sonic The Hedgehog II" (Genesis headers are usually upper case; Libretro names are not). Mixed case: unchanged.
// With `minor_words`, articles and short prepositions after the first word stay lower case ("Streets of Rage"): No-Intro uses both styles.
std::string title_case(const std::string &title, bool minor_words = false) {
  const bool has_lower = std::any_of(title.begin(), title.end(), [](unsigned char c) { return std::islower(c) != 0; });
  if (has_lower) return title;
  std::string out, word;
  static const std::vector<std::string> minor = {"a", "an", "and", "as", "at", "by", "for", "in", "of", "on", "or", "the", "to", "vs", "with"};
  const auto flush = [&] {
    if (!roman_numeral(word))
      for (std::size_t i = 1; i < word.size(); ++i) word[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(word[i])));
    if (minor_words && !out.empty() && std::find(minor.begin(), minor.end(), lower(word)) != minor.end()) word = lower(word);
    out += word;
    word.clear();
  };
  for (const char c : title) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '\'') word.push_back(c);
    else { flush(); out.push_back(c); }
  }
  flush();
  return out;
}

// Rank of a name's region tag in the user's order (lower is better; "World" suits everyone).
std::size_t region_rank(const std::string &name, const std::vector<std::string> &preferred) {
  const auto open = name.find('(');
  if (open == std::string::npos) return preferred.size() + 1;
  const auto close = name.find(')', open);
  const std::string tag = name.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
  if (tag == "World") return 0;
  std::size_t best = preferred.size() + 1;
  std::stringstream parts(tag);
  for (std::string part; std::getline(parts, part, ',');) {
    part = collapse_spaces(part);
    for (std::size_t i = 0; i < preferred.size(); ++i)
      if (lower(preferred[i]) == lower(part)) best = std::min(best, i);
  }
  return best;
}

bool unreleased_or_unofficial(const std::string &name) {
  const std::string l = lower(name);
  for (const char *mark : {"(beta", "(proto", "(demo", "(sample", "(pirate", "(unl", "(hack", "[b]", "(aftermarket"})
    if (l.find(mark) != std::string::npos) return true;
  return false;
}

std::string percent_decode(const std::string &text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

std::string html_decode(const std::string &text) {
  static const std::vector<std::pair<std::string, char>> entities = {{"&amp;", '&'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}};
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    bool replaced = false;
    for (const auto &[entity, c] : entities)
      if (text.compare(i, entity.size(), entity) == 0) { out.push_back(c); i += entity.size(); replaced = true; break; }
    if (!replaced) out.push_back(text[i++]);
  }
  return out;
}

std::string read_text(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string mirror_system_dir(const std::string &system) {
  std::string out = system;
  std::replace(out.begin(), out.end(), ' ', '_');
  return out;
}

// The comparable base names of the query (titles and ROM file names, tags removed).
std::vector<std::string> query_keys(const CoverQuery &query) {
  std::vector<std::string> keys;
  const auto add = [&](const std::string &text) {
    const std::string key = libretro_base_key(text);
    if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
  };
  for (const auto &rom : query.rom_names)
    if (const std::string stem = file_stem(rom); !is_digest_stem(stem)) add(stem);
  for (const auto &title : query.titles) add(title);
  return keys;
}

} // namespace

std::string libretro_safe_name(const std::string &name) {
  std::string out;
  for (const char c : name) out.push_back(std::string("&*/:`<>?\\|\"").find(c) != std::string::npos ? '_' : c);
  return out;
}

std::string libretro_base_key(const std::string &name) {
  std::string base = strip_tags(name);
  // No-Intro moves a leading article to the end: "Lion King, The" is "The Lion King".
  if (base.size() > 5 && lower(base.substr(base.size() - 5)) == ", the") base = "the " + base.substr(0, base.size() - 5);
  std::string key, word;
  const auto flush = [&] {
    if (word != "and") key += word;  // "&" becomes "_" in thumbnail names, so "and" / "&" / "_" are all dropped
    word.clear();
  };
  for (const char c : base) {
    if (std::isalnum(static_cast<unsigned char>(c))) word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    else if (c != '\'') flush();
  }
  flush();
  return key;
}

std::vector<std::string> libretro_cover_candidates(const CoverQuery &query, std::size_t limit) {
  std::vector<std::string> names;
  const auto add = [&](const std::string &raw) {
    const std::string name = collapse_spaces(libretro_safe_name(raw));
    if (!name.empty() && names.size() < limit && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  };
  // 1. ROM file names that already carry tags: No-Intro sets are named exactly like the thumbnails.
  std::vector<std::string> bases;
  for (const auto &rom : query.rom_names) {
    const std::string stem = collapse_spaces(file_stem(rom));
    if (stem.empty() || is_digest_stem(stem)) continue;
    if (stem.find('(') != std::string::npos) add(stem);
    bases.push_back(strip_tags(stem));
  }
  // 2. Titles (as written, then in Title Case) and tag-less file names, each with the region tags in the user's order.
  for (const auto &title : query.titles) {
    bases.push_back(title_case(collapse_spaces(title), true));
    bases.push_back(title_case(collapse_spaces(title)));
    bases.push_back(collapse_spaces(title));
  }
  std::vector<std::string> tags;
  for (const auto &region : query.preferred_regions) {
    if (region.empty() || region.size() > 24) continue;
    for (const auto &tag : kRegionTags)
      if (region_rank(tag, {region}) == 0 && std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
  }
  for (const auto &tag : kRegionTags)
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
  std::vector<std::string> unique_bases;
  for (const auto &base : bases)
    if (!base.empty() && std::find(unique_bases.begin(), unique_bases.end(), base) == unique_bases.end()) unique_bases.push_back(base);
  // The first tags of every base before the rarer tags of any base, so the likely names all fit under the limit.
  for (std::size_t t = 0; t < tags.size(); ++t)
    for (const auto &base : unique_bases) add(base + " " + tags[t]);
  return names;
}

std::vector<std::string> parse_libretro_listing(const std::string &html) {
  std::vector<std::string> names;
  for (std::size_t at = html.find("href=\""); at != std::string::npos; at = html.find("href=\"", at)) {
    at += 6;
    const auto end = html.find('"', at);
    if (end == std::string::npos) break;
    std::string target = percent_decode(html_decode(html.substr(at, end - at)));
    at = end;
    if (target.size() <= 4 || lower(target.substr(target.size() - 4)) != ".png") continue;
    if (target.find('/') != std::string::npos || target.find('\\') != std::string::npos) continue;  // entries of this folder only
    target.erase(target.size() - 4);
    if (std::find(names.begin(), names.end(), target) == names.end()) names.push_back(target);
  }
  return names;
}

std::string pick_libretro_name(const CoverQuery &query, const std::vector<std::string> &listing) {
  if (listing.empty()) return {};
  for (const auto &candidate : libretro_cover_candidates(query, 64))
    if (std::find(listing.begin(), listing.end(), candidate) != listing.end()) return candidate;
  const auto keys = query_keys(query);
  std::string best;
  std::tuple<bool, std::size_t, std::size_t, std::string> best_rank;
  for (const auto &name : listing) {
    if (std::find(keys.begin(), keys.end(), libretro_base_key(name)) == keys.end()) continue;
    auto rank = std::make_tuple(unreleased_or_unofficial(name), region_rank(name, query.preferred_regions), name.size(), name);
    if (best.empty() || rank < best_rank) { best = name; best_rank = std::move(rank); }
  }
  return best;
}

CoverFetch fetch_libretro_cover(const CoverQuery &query, const fs::path &work_dir, const HttpGet &get) {
  CoverFetch out;
  const std::string system = libretro_system_dir(query.platform_id);
  if (system.empty()) { out.error = "Libretro has no box-art set for this platform."; return out; }
  std::error_code ec;
  fs::create_directories(work_dir, ec);
  if (!fs::is_directory(work_dir, ec)) { out.error = "The download folder cannot be created."; return out; }

  // The directory listing (one request a week) turns a fuzzy title into the exact thumbnail name.
  std::vector<std::string> listing;
  const fs::path cache = work_dir / ("libretro-" + query.platform_id + ".txt");
  if (const auto stamp = fs::last_write_time(cache, ec); !ec && fs::file_time_type::clock::now() - stamp < kListingMaxAge) {
    std::stringstream lines(read_text(cache));
    for (std::string line; std::getline(lines, line);)
      if (!line.empty()) listing.push_back(line);
  }
  if (listing.empty()) {
    const fs::path page = work_dir / "listing.html";
    if (get("https://thumbnails.libretro.com/" + percent_encode(system) + "/Named_Boxarts/", page) == 0) listing = parse_libretro_listing(read_text(page));
    fs::remove(page, ec);
    if (!listing.empty()) {
      std::ofstream file(cache, std::ios::binary | std::ios::trunc);
      for (const auto &name : listing) file << name << '\n';
    }
  }

  std::vector<std::string> names;
  if (!listing.empty()) {
    const std::string picked = pick_libretro_name(query, listing);
    if (picked.empty()) { out.error = "No Libretro box art matches this game's title. Rename the game to its usual English title and try again, or choose a local image."; return out; }
    names.push_back(picked);
    out.used_index = true;
  } else {
    names = libretro_cover_candidates(query);
    if (names.empty()) { out.error = "This game has no usable title to look up."; return out; }
  }

  const fs::path image = work_dir / "download.img";
  const std::string mirror = "https://raw.githubusercontent.com/libretro-thumbnails/" + mirror_system_dir(system) + "/master/Named_Boxarts/";
  bool official_up = true, mirror_up = true;
  for (const auto &name : names) {
    const std::string file = percent_encode(name) + ".png";
    int result = 2;
    if (official_up) {
      fs::remove(image, ec);
      result = get("https://thumbnails.libretro.com/" + percent_encode(system) + "/Named_Boxarts/" + file, image);
      if (result == 0) { out.ok = true; out.image = image; out.matched_name = name; out.source = "thumbnails.libretro.com"; return out; }
      if (result == 2) official_up = false;
    }
    if (result == 2 && mirror_up) {  // the mirror is asked only while the official host cannot be reached (a 404 there is final)
      fs::remove(image, ec);
      result = get(mirror + file, image);
      if (result == 0) { out.ok = true; out.image = image; out.matched_name = name; out.source = "raw.githubusercontent.com (libretro-thumbnails)"; return out; }
      if (result == 2) mirror_up = false;
    }
    if (!official_up && !mirror_up) break;
  }
  fs::remove(image, ec);
  if (!official_up && !mirror_up) out.error = "Libretro thumbnails could not be reached (offline, blocked, or curl is not available).";
  else out.error = "No Libretro box art was found for \"" + names.front() + "\". Rename the game to its usual English title and try again, or choose a local image.";
  return out;
}

fs::path system_curl() {
#ifdef _WIN32
  const char *root = SDL_getenv("SystemRoot");
  const fs::path candidate = fs::path(root != nullptr ? root : "C:\\Windows") / "System32" / "curl.exe";
  std::error_code ec;
  return fs::is_regular_file(candidate, ec) ? candidate : fs::path();
#else
  for (const char *candidate : {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/opt/homebrew/bin/curl"}) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
  }
  return {};
#endif
}

HttpGet curl_http_get() {
  return [](const std::string &url, const fs::path &out) -> int {
    if (url.rfind("https://", 0) != 0) return 2;
    const fs::path curl = system_curl();
    if (curl.empty()) return 2;
    const auto curl_u8 = curl.u8string(), out_u8 = out.u8string();
    const std::string curl_s(reinterpret_cast<const char *>(curl_u8.data()), curl_u8.size()), out_s(reinterpret_cast<const char *>(out_u8.data()), out_u8.size());
    // No shell: a fixed argument vector. https only, also across redirects; size and time limits; the HTTP status on stdout.
    const char *argv[] = {curl_s.c_str(), "--silent", "--location", "--proto", "=https", "--proto-redir", "=https", "--max-redirs", "5",
                          "--connect-timeout", "10", "--max-time", "45", "--max-filesize", "8388608", "--user-agent", "Segarecomp",
                          "--write-out", "%{http_code}", "--output", out_s.c_str(), url.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, static_cast<void *>(argv));
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (process == nullptr) return 2;
    std::size_t size = 0;
    int exit_code = -1;
    void *data = SDL_ReadProcess(process, &size, &exit_code);
    const std::string status = data != nullptr ? std::string(static_cast<const char *>(data), std::min<std::size_t>(size, 8)) : std::string();
    SDL_free(data);
    SDL_DestroyProcess(process);
    std::error_code ec;
    if (exit_code == 0 && status == "200" && fs::is_regular_file(out, ec)) return 0;
    fs::remove(out, ec);
    // Only a definite "no such file" counts as not found; a proxy's 403, a 5xx or a failed transfer mean "unavailable".
    return exit_code == 0 && (status == "404" || status == "410") ? 1 : 2;
  };
}

} // namespace launcher
