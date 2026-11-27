// SEG-137-T004/T005 (ADR 0295): the single API table of the restricted Luau host API, its generated declaration/JSON forms and the overlay rasterizer. See luau_api.hpp.
#include "segarecomp/luau_api.hpp"

#include "sv_font.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace segarecomp::luau {

namespace {

// The ONLY list of host names a script can reach. A name is added here, in a new ABI minor if it needs a capability the catalogue does not have (native-sdk-abi-2.md section 7).
constexpr std::array<ApiEntry, 29> kApi = {{
    {"sega.log", "host.log", "function", "message:string", "", "all", "Appends one line to the host log (at most 256 bytes, 16 lines per callback; printable ASCII)."},
    {"sega.frame.value", "frame.work_ram", "function", "name:string", "number?", "render",
     "Reads one named integer the host published for this frame (a read-only copy; nil when the host publishes no such value)."},
    {"sega.overlay.width", "overlay.rgba", "value", "number", "", "render", "Width of the HUD overlay in pixels (128)."},
    {"sega.overlay.height", "overlay.rgba", "value", "number", "", "render", "Height of the HUD overlay in pixels (256)."},
    {"sega.overlay.rect", "overlay.rgba", "function", "x:number,y:number,w:number,h:number,rgba:number", "", "render",
     "Requests a filled rectangle; integers only, clamped to the overlay, rgba is 0xRRGGBBAA, at most 64 requests per callback."},
    {"sega.overlay.text", "overlay.rgba", "function", "x:number,y:number,text:string,rgba:number", "", "render",
     "Requests text in the host's bitmap font (printable ASCII, at most 64 bytes); the origin is clamped to the overlay."},
    {"sega.overlay.layer", "overlay.rgba", "function", "index:number,anchor:string,reference:string,dx:number,dy:number,w:number,h:number,z:number", "", "render",
     "Declares positioned overlay layer 1-4 (at most 128 x 128): anchor is top_left, top_center, top_right, middle_left, center, middle_right, bottom_left, bottom_center or bottom_right; reference is picture or presentation (the picture plus the visible widescreen side); dx, dy are added to the anchored position; z 0-3 orders the layers. The host rejects a layer that does not lie wholly inside its reference or exceeds the overlay budget, with a diagnostic; it never moves or clips it."},
    {"sega.overlay.target", "overlay.rgba", "function", "index:number", "", "render",
     "Selects the layer the following sega.overlay.rect and sega.overlay.text draw into: 0 is the fixed HUD overlay (the default at the start of every callback), 1-4 a layer declared earlier in the same callback."},
    {"sega.sprite.budget", "overlay.rgba", "function", "mode:string", "", "render",
     "Selects the original per-line sprite budget for this frame's virtual sprites (\"limit\", the default) or an explicit \"extended\" budget with no per-line limit; over-budget lines are still reported. Presentation only: the game's own sprites and the VDP status are never touched."},
    {"sega.sprite.space", "overlay.rgba", "function", "mode:string", "", "render",
     "Selects the coordinate space of this frame's virtual sprites. Both have the same origin, the top-left pixel of the native picture. \"picture\" (the default) draws inside the native picture only; \"presentation\" may also draw into the widescreen columns the viewer actually presents for the current aspect and side style, at x < 0 (left) and x >= 320 (right). A pixel in a column that is not presented is never drawn or counted; a behind_planes or between sprite that reaches a column rejects the whole set (no native priority exists there)."},
    {"sega.sprite.tile", "overlay.rgba", "function", "index:number,patterns:string", "", "render",
     "Writes your OWN pattern pool for mod-authored sprites: 64 hexadecimal digits per 8x8 pattern (4 bits per pixel, the VDP layout), consecutive patterns from `index` (0-2047). The pool is yours and one frame long; it never touches VRAM."},
    {"sega.sprite.palette", "overlay.rgba", "function", "line:number,colours:string", "", "render",
     "Writes your OWN palette line 0-3 for mod-authored sprites: 4 hexadecimal digits per colour (the VDP 0BGR word), at most 16 colours."},
    {"sega.sprite.add", "overlay.rgba", "function", "provenance:string,layer:string,x:number,y:number,width_cells:number,height_cells:number,tile:number,palette:number,flags:number", "", "render",
     "Adds one virtual sprite for this frame: provenance \"authored\" (your own pool) or \"verified\" (the game's art in the frame's VRAM/CRAM; the host rejects the whole set unless it established a verified, exactly current snapshot); layer \"behind_planes\", \"between\" or \"above_all\" against the native priority classes; x, y are the top-left picture pixel; 1-4 cells wide and high; flags bit 0 horizontal flip, bit 1 vertical flip. At most 64 per frame; a larger set, or any invalid command, is rejected as a whole with a stable reason and nothing is drawn."},
    {"sega.sprite.clip", "overlay.rgba", "function", "x:number,y:number,width:number,height:number", "", "render",
     "Clips the most recently added sprite to a rectangle of picture pixels."},
    {"sega.scene", "overlay.rgba", "function", "id:string", "", "render",
     "Names the scene this frame belongs to (a token [a-z0-9_-], at most 32 characters; the last call wins). The host uses it only to count what was delivered per scene against the package's declared geometry; it is a claim, never inferred, and grants nothing."},
    {"sega.mem.read", "memory.read.m68k.work_ram", "function", "space:string,offset:number,width:number", "number", "reset,frame-begin,input,post-input,frame-end",
     "Reads 1, 2 or 4 bytes (big-endian) of a declared work-RAM range; the space is named (\"m68k.work_ram\"); sees this callback's own staged writes; a violation is a fault."},
    {"sega.mem.write", "memory.write.m68k.work_ram", "function", "space:string,offset:number,width:number,value:number", "", "frame-begin,post-input,frame-end",
     "Stages a 1, 2 or 4 byte big-endian write to a declared work-RAM range; committed atomically when the callback returns without a fault (at most 1024 writes, 16384 bytes); a violation is a fault."},
    {"sega.input.get", "input.read", "function", "port:number", "number", "frame-begin,input,post-input",
     "Reads the pad (button bits) of port 1 or 2: the previous frame's final input in frame-begin, the sample (with your staged change) in input, the final input in post-input."},
    {"sega.input.set", "input.intercept", "function", "port:number,buttons:number", "", "input",
     "Replaces the pad (button bits 0-255) the guest will read this frame; only in input; a violation is a fault."},
    {"sega.audio.play_sfx", "audio.command.send", "function", "name:string", "", "frame-begin,post-input,frame-end",
     "Sends the effect `name` of the package's declared command mapping \"sfx\": stages the declared value into the declared work-RAM mailbox (committed with the callback). An undeclared mapping or name is a fault."},
    {"sega.audio.play_music", "audio.command.send", "function", "name:string", "", "frame-begin,post-input,frame-end",
     "Sends the music `name` of the package's declared command mapping \"music\", as sega.audio.play_sfx does for \"sfx\"."},
    {"sega.audio.stop_music", "audio.command.send", "function", "", "", "frame-begin,post-input,frame-end",
     "Sends the value named \"stop\" of the declared command mapping \"music\" (a fault when the mapping declares none)."},
    {"sega.audio.event", "audio.event.read", "function", "mapping:string", "string?", "frame-end,render",
     "Reads which declared event of the event mapping `mapping` the observed location holds now (nil when none matches); a level, not a queue. Read-only; render sees the copy taken after frame-end."},
    {"reset", "", "callback", "", "", "reset", "Optional. Called once before the first frame."},
    {"frame_begin", "", "callback", "frame:number", "", "frame-begin", "Optional. Called at the start of each guest frame, before its input sample."},
    {"input", "", "callback", "frame:number", "", "input", "Optional. Called after the pad was sampled for the frame and before the guest can read it."},
    {"post_input", "", "callback", "frame:number", "", "post-input", "Optional. Called after the input phase, before the guest runs the frame."},
    {"frame_end", "", "callback", "frame:number", "", "frame-end", "Optional. Called when the guest finished the frame, before its render snapshot."},
    {"render", "", "callback", "frame:number", "", "render", "Optional. Called once per frame after its snapshot; `frame` is the frame counter."},
}};

std::string_view trim_type(std::string_view p, std::string_view &name) {   // "x:number" -> name x, type number
  const std::size_t colon = p.find(':');
  name = p.substr(0, colon);
  return colon == std::string::npos ? p : p.substr(colon + 1);
}

std::vector<std::pair<std::string, std::string>> params_of(const ApiEntry &e) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string_view rest = e.params;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    std::string_view one = rest.substr(0, comma), name;
    const std::string_view type = trim_type(one, name);
    out.emplace_back(std::string(name), std::string(type));
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  return out;
}

std::string json_escape(std::string_view s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

struct Node {
  std::map<std::string, Node> children;
  const ApiEntry *leaf = nullptr;
};

void emit(const Node &n, int depth, std::string &out) {
  const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
  for (const auto &[name, child] : n.children) {
    if (child.leaf != nullptr) {
      out += pad + "-- " + std::string(child.leaf->capability) + " (" + std::string(child.leaf->phases) + "): " + std::string(child.leaf->doc) + "\n";
      out += pad + name + ": " + api_signature(*child.leaf) + ",\n";
    } else {
      out += pad + name + ": {\n";
      emit(child, depth + 1, out);
      out += pad + "},\n";
    }
  }
}

}  // namespace

std::span<const ApiEntry> api_entries() { return kApi; }

const ApiEntry *find_api(std::string_view name) {
  for (const ApiEntry &e : kApi)
    if (e.name == name) return &e;
  return nullptr;
}

std::string api_signature(const ApiEntry &e) {
  if (e.kind == "value") return std::string(e.params);
  std::string s = "(";
  bool first = true;
  for (const auto &[name, type] : params_of(e)) {
    s += (first ? "" : ", ") + name + ": " + type;
    first = false;
  }
  s += ") -> " + (e.returns.empty() ? std::string("()") : std::string(e.returns));
  return s;
}

std::string declarations() {
  std::string out =
      "-- segarecomp.d.luau: GENERATED by `segarecomp mod api --declarations` from the Luau API list (ADR 0295). Do not edit; a test fails when this file drifts.\n"
      "-- Each member names the capability that must be declared in mod.toml (an undeclared one is rejected by `mod check`) and the phases in which it may be called.\n\n";
  Node root;
  std::vector<const ApiEntry *> callbacks;
  for (const ApiEntry &e : kApi) {
    if (e.kind == "callback") {
      callbacks.push_back(&e);
      continue;
    }
    Node *n = &root;
    std::string_view rest = e.name;
    rest.remove_prefix(5);   // "sega."
    while (true) {
      const std::size_t dot = rest.find('.');
      n = &n->children[std::string(rest.substr(0, dot))];
      if (dot == std::string_view::npos) break;
      rest = rest.substr(dot + 1);
    }
    n->leaf = &e;
  }
  out += "declare sega: {\n";
  emit(root, 1, out);
  out += "}\n\n-- Lifecycle callbacks: define any of these as a global function; the host calls them.\n";
  for (const ApiEntry *c : callbacks) {
    std::string name;   // snake_case -> PascalCase
    for (std::size_t i = 0; i < c->name.size(); ++i) {
      if (c->name[i] == '_') continue;
      name += (i == 0 || c->name[i - 1] == '_') ? static_cast<char>(c->name[i] - 'a' + 'A') : c->name[i];
    }
    out += "-- " + std::string(c->doc) + "\n";
    out += "type " + name + "Callback = " + api_signature(*c) + "\n";
  }
  return out;
}

std::string api_json() {
  std::string o = "{\n  \"schema\": 1,\n  \"api\": [\n";
  for (std::size_t i = 0; i < kApi.size(); ++i) {
    const ApiEntry &e = kApi[i];
    o += "    {\"name\": \"" + std::string(e.name) + "\", \"capability\": \"" + std::string(e.capability) + "\", \"kind\": \"" + std::string(e.kind) + "\", \"signature\": \"" + json_escape(api_signature(e)) +
         "\", \"phases\": \"" + std::string(e.phases) + "\", \"doc\": \"" + json_escape(e.doc) + "\"}" + (i + 1 < kApi.size() ? "," : "") + "\n";
  }
  o += "  ],\n  \"limits\": {\"log_line_bytes\": " + std::to_string(kLogLineBytes) + ", \"log_lines_per_callback\": " + std::to_string(kLogLinesPerCallback) + ", \"overlay_ops_per_callback\": " +
       std::to_string(kOverlayOpsPerCallback) + ", \"overlay_text_bytes\": " + std::to_string(kOverlayTextBytes) + ", \"overlay_width\": " + std::to_string(kOverlayWidth) + ", \"overlay_height\": " +
       std::to_string(kOverlayHeight) + ", \"frame_value_name_bytes\": " + std::to_string(kFrameValueNameBytes) + "}\n}\n";
  return o;
}

namespace {

// Straight-alpha source-over in integers: identical on every host.
void blend(std::uint8_t *dst, std::uint32_t rgba) {
  const std::uint64_t sr = (rgba >> 24U) & 255U, sg = (rgba >> 16U) & 255U, sb = (rgba >> 8U) & 255U, sa = rgba & 255U;
  if (sa == 0) return;
  if (sa == 255) {
    dst[0] = static_cast<std::uint8_t>(sr);
    dst[1] = static_cast<std::uint8_t>(sg);
    dst[2] = static_cast<std::uint8_t>(sb);
    dst[3] = 255;
    return;
  }
  const std::uint64_t da = dst[3], scaled = sa * 255U + da * (255U - sa);   // output alpha times 255
  dst[0] = static_cast<std::uint8_t>((sr * sa * 255U + dst[0] * da * (255U - sa) + scaled / 2U) / scaled);
  dst[1] = static_cast<std::uint8_t>((sg * sa * 255U + dst[1] * da * (255U - sa) + scaled / 2U) / scaled);
  dst[2] = static_cast<std::uint8_t>((sb * sa * 255U + dst[2] * da * (255U - sa) + scaled / 2U) / scaled);
  dst[3] = static_cast<std::uint8_t>((scaled + 127U) / 255U);
}

bool atlas_on(int x, int y) {
  const int row_bytes = (sv_font_atlas_w + 7) / 8;
  return ((sv_font_atlas_bits[y * row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1) != 0;
}

}  // namespace

static OverlayImage raster_ops(int width, int height, int layer, std::span<const OverlayOp> ops) {
  OverlayImage img;
  img.width = width;
  img.height = height;
  img.rgba.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4U, 0);
  const auto pixel = [&](int x, int y) -> std::uint8_t * {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return nullptr;
    return &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)) * 4U];
  };
  const SvFontFace &face = sv_font_faces[SV_FONT_BODY];
  for (const OverlayOp &op : ops) {
    if (op.layer != layer) continue;
    if (op.kind == OverlayOp::Kind::bitmap) {   // SEG-152-T005: exact replacement of the destination bytes (alpha 0 included), never the blend
      for (int row = 0; row < op.h; ++row) {
        std::uint8_t *dst = pixel(op.x, op.y + row);
        if (dst != nullptr) std::memcpy(dst, &op.pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(op.w) * 4U], static_cast<std::size_t>(op.w) * 4U);
      }
      continue;
    }
    if (op.kind == OverlayOp::Kind::rect) {
      for (int y = op.y; y < op.y + op.h; ++y)
        for (int x = op.x; x < op.x + op.w; ++x)
          if (std::uint8_t *p = pixel(x, y)) blend(p, op.rgba);
      continue;
    }
    int pen = op.x;
    for (const char ch : op.text) {
      const SvGlyph *g = sv_font_glyph(SV_FONT_BODY, static_cast<unsigned char>(ch));
      for (int gy = 0; gy < g->h; ++gy)
        for (int gx = 0; gx < g->w; ++gx)
          if (atlas_on(g->x + gx, g->y + gy))
            if (std::uint8_t *p = pixel(pen + g->xoff + gx, op.y + face.ascent + g->yoff + gy)) blend(p, op.rgba);
      pen += g->advance;
    }
  }
  return img;
}

namespace {
constexpr std::int64_t kCoord = 1 << 20;   // the largest coordinate a call may name
OverlayCall bad(OverlayCall::Status status, std::string message) { return OverlayCall{status, std::move(message)}; }
void note_to(Output &out, const char *cause, std::string message) {
  if (out.notes.size() < kNotesMax) out.notes.push_back({cause, std::move(message)});
}
const OverlayLayerDecl *layer_of(const Output &out, int index) {
  for (const OverlayLayerDecl &d : out.layers)
    if (d.index == index) return &d;
  return nullptr;
}
int width_of(const Output &out, int target) { const OverlayLayerDecl *d = target != 0 ? layer_of(out, target) : nullptr; return d != nullptr ? d->w : kOverlayWidth; }
int height_of(const Output &out, int target) { const OverlayLayerDecl *d = target != 0 ? layer_of(out, target) : nullptr; return d != nullptr ? d->h : kOverlayHeight; }
std::size_t layer_pixels(const Output &out) {
  std::size_t n = 0;
  for (const OverlayLayerDecl &d : out.layers) n += static_cast<std::size_t>(d.w) * static_cast<std::size_t>(d.h);
  return n;
}
OverlayCall add_op(Output &out, OverlayOp op) {
  // bitmap ops live in the same list but do not consume the rect/text budget
  if (out.overlay.size() - out.bitmap_requests >= kOverlayOpsPerCallback) return bad(OverlayCall::Status::budget, "sega.overlay request limit exceeded");
  out.overlay.push_back(std::move(op));
  return {};
}
}  // namespace

std::string printable_ascii(std::string_view text, std::size_t limit, bool &truncated) {
  truncated = text.size() > limit;
  std::string out(text.substr(0, limit));
  for (char &c : out)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
  return out;
}

OverlayCall overlay_declare_layer(Output &out, const OverlayLayerDecl &d) {
  if (d.index < 1 || d.index > kOverlayLayersMax) return bad(OverlayCall::Status::range, "the layer index of sega.overlay.layer is out of range");
  if (d.anchor < 0 || d.anchor > 8) return bad(OverlayCall::Status::range, "argument 2 of sega.overlay.layer is not a known anchor");
  if (d.reference < 0 || d.reference > 1) return bad(OverlayCall::Status::range, "argument 3 of sega.overlay.layer must be \"picture\" or \"presentation\"");
  if (d.dx < -2048 || d.dx > 2048 || d.dy < -2048 || d.dy > 2048 || d.w < 1 || d.w > kOverlayLayerSide || d.h < 1 || d.h > kOverlayLayerSide || d.z < 0 || d.z > kOverlayLayersMax - 1)
    return bad(OverlayCall::Status::range, "an argument of sega.overlay.layer is out of range");
  if (layer_of(out, d.index) != nullptr) return bad(OverlayCall::Status::range, "sega.overlay.layer declares a layer that was already declared in this callback");
  if (layer_pixels(out) + static_cast<std::size_t>(d.w) * static_cast<std::size_t>(d.h) > kLayerPixelsMax) return bad(OverlayCall::Status::budget, "the declared layers exceed the overlay pixel budget");   // reachable only next to a bitmap layer
  out.layers.push_back(d);
  return {};
}

OverlayCall overlay_declare_bitmap_layer(Output &out, const OverlayLayerDecl &d) {
  if (d.index < 1 || d.index > kOverlayLayersMax) return bad(OverlayCall::Status::range, "the layer index of overlay_layer_bitmap is out of range");
  if (d.anchor < 0 || d.anchor > 8) return bad(OverlayCall::Status::range, "the anchor of overlay_layer_bitmap is not a known anchor");
  if (d.reference < 0 || d.reference > 1) return bad(OverlayCall::Status::range, "the reference of overlay_layer_bitmap must be 0 (picture) or 1 (presentation)");
  if (d.dx < -2048 || d.dx > 2048 || d.dy < -2048 || d.dy > 2048 || d.w < 1 || d.w > kBitmapLayerSide || d.h < 1 || d.h > kBitmapLayerSide || d.z < 0 || d.z > kOverlayLayersMax - 1)
    return bad(OverlayCall::Status::range, "an argument of overlay_layer_bitmap is out of range");
  if (layer_of(out, d.index) != nullptr) return bad(OverlayCall::Status::range, "overlay_layer_bitmap declares a layer that was already declared in this callback");
  if (layer_pixels(out) + static_cast<std::size_t>(d.w) * static_cast<std::size_t>(d.h) > kLayerPixelsMax) return bad(OverlayCall::Status::budget, "the declared layers exceed the overlay pixel budget");
  OverlayLayerDecl decl = d;
  decl.bitmap = true;
  decl.has_clip = false;
  out.layers.push_back(decl);
  return {};
}

OverlayCall overlay_layer_clip(Output &out, std::int64_t index, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h) {
  OverlayLayerDecl *decl = nullptr;
  for (OverlayLayerDecl &d : out.layers)
    if (d.index == index) decl = &d;
  if (decl == nullptr) return bad(OverlayCall::Status::range, "overlay_layer_clip names a layer that has not been declared");
  if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > decl->w || y + h > decl->h) return bad(OverlayCall::Status::range, "the clip rectangle of overlay_layer_clip lies outside its layer");
  decl->has_clip = true;
  decl->clip_x = static_cast<int>(x);
  decl->clip_y = static_cast<int>(y);
  decl->clip_w = static_cast<int>(w);
  decl->clip_h = static_cast<int>(h);
  return {};
}

OverlayCall overlay_fallback(Output &out, std::int64_t x, std::int64_t y) {
  if (x < 0 || x > 65535 || y < 0 || y > 65535) return bad(OverlayCall::Status::range, "an argument of overlay_fallback is out of range");
  out.fallback_x = static_cast<int>(x);
  out.fallback_y = static_cast<int>(y);
  return {};
}

OverlayCall overlay_bitmap(Output &out, int target, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h, const std::uint8_t *rgba, std::uint64_t stride_bytes) {
  const int target_w = width_of(out, target), target_h = height_of(out, target);
  if (rgba == nullptr || x < -kCoord || x > kCoord || y < -kCoord || y > kCoord || w < 1 || h < 1 || w > target_w || h > target_h || stride_bytes < static_cast<std::uint64_t>(w) * 4U)
    return bad(OverlayCall::Status::range, "an argument of overlay_bitmap is out of range");
  const std::uint64_t bytes = static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) * 4U;
  if (out.bitmap_requests >= kBitmapRequestsMax || out.bitmap_bytes + bytes > kBitmapBytesMax) return bad(OverlayCall::Status::budget, "the overlay_bitmap budget of the callback is exhausted");
  ++out.bitmap_requests;
  out.bitmap_bytes += bytes;
  const std::int64_t x0 = std::max<std::int64_t>(x, 0), y0 = std::max<std::int64_t>(y, 0);
  const std::int64_t x1 = std::min<std::int64_t>(x + w, target_w), y1 = std::min<std::int64_t>(y + h, target_h);
  if (x1 <= x0 || y1 <= y0) {
    note_to(out, "overlay_clipped", "an overlay_bitmap has no visible area");
    return {};
  }
  if (x0 != x || y0 != y || x1 - x0 != w || y1 - y0 != h) note_to(out, "overlay_clamped", "an overlay_bitmap was clamped to the overlay");
  OverlayOp op;
  op.layer = target;
  op.kind = OverlayOp::Kind::bitmap;
  op.x = static_cast<int>(x0);
  op.y = static_cast<int>(y0);
  op.w = static_cast<int>(x1 - x0);
  op.h = static_cast<int>(y1 - y0);
  const std::size_t row_bytes = static_cast<std::size_t>(op.w) * 4U;
  op.pixels.resize(row_bytes * static_cast<std::size_t>(op.h));
  for (int row = 0; row < op.h; ++row)   // only the visible part of each row is read: never past (h - 1) * stride + 4 * w bytes
    std::memcpy(&op.pixels[static_cast<std::size_t>(row) * row_bytes], rgba + static_cast<std::uint64_t>(y0 - y + row) * stride_bytes + static_cast<std::uint64_t>(x0 - x) * 4U, row_bytes);
  out.overlay.push_back(std::move(op));
  return {};
}

OverlayCall overlay_select_target(Output &out, int &target, std::int64_t index) {
  if (index < 0 || index > kOverlayLayersMax) return bad(OverlayCall::Status::range, "argument 1 of sega.overlay.target is out of range");
  target = static_cast<int>(index);
  if (index != 0 && layer_of(out, target) == nullptr) {
    target = 0;
    return bad(OverlayCall::Status::range, "sega.overlay.target selects a layer that has not been declared");
  }
  return {};
}

OverlayCall overlay_rect(Output &out, int target, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h, std::uint32_t rgba) {
  if (x < -kCoord || x > kCoord || y < -kCoord || y > kCoord || w < 0 || w > kCoord || h < 0 || h > kCoord) return bad(OverlayCall::Status::range, "an argument of sega.overlay.rect is out of range");
  if (const OverlayLayerDecl *d = target != 0 ? layer_of(out, target) : nullptr; d != nullptr && d->bitmap) return bad(OverlayCall::Status::range, "a bitmap layer accepts bitmaps only, not sega.overlay.rect");
  const int target_w = width_of(out, target), target_h = height_of(out, target);
  const std::int64_t x0 = std::max<std::int64_t>(x, 0), y0 = std::max<std::int64_t>(y, 0);
  const std::int64_t x1 = std::min<std::int64_t>(x + w, target_w), y1 = std::min<std::int64_t>(y + h, target_h);
  if (x1 <= x0 || y1 <= y0) {
    note_to(out, "overlay_clipped", "a sega.overlay.rect has no visible area");
    return {};
  }
  if (x0 != x || y0 != y || x1 - x0 != w || y1 - y0 != h) note_to(out, "overlay_clamped", "a sega.overlay.rect was clamped to the overlay");
  OverlayOp op;
  op.layer = target;
  op.kind = OverlayOp::Kind::rect;
  op.x = static_cast<int>(x0);
  op.y = static_cast<int>(y0);
  op.w = static_cast<int>(x1 - x0);
  op.h = static_cast<int>(y1 - y0);
  op.rgba = rgba;
  return add_op(out, std::move(op));
}

OverlayCall overlay_text(Output &out, int target, std::int64_t x, std::int64_t y, std::string_view text, std::uint32_t rgba) {
  if (x < -kCoord || x > kCoord || y < -kCoord || y > kCoord) return bad(OverlayCall::Status::range, "an argument of sega.overlay.text is out of range");
  if (const OverlayLayerDecl *d = target != 0 ? layer_of(out, target) : nullptr; d != nullptr && d->bitmap) return bad(OverlayCall::Status::range, "a bitmap layer accepts bitmaps only, not sega.overlay.text");
  const std::int64_t cx = std::clamp<std::int64_t>(x, 0, width_of(out, target) - 1), cy = std::clamp<std::int64_t>(y, 0, height_of(out, target) - 1);
  if (cx != x || cy != y) note_to(out, "overlay_clamped", "a sega.overlay.text origin was clamped to the overlay");
  OverlayOp op;
  op.layer = target;
  op.kind = OverlayOp::Kind::text;
  op.x = static_cast<int>(cx);
  op.y = static_cast<int>(cy);
  bool truncated = false;
  op.text = printable_ascii(text, kOverlayTextBytes, truncated);
  if (truncated) note_to(out, "text_truncated", "a sega.overlay.text string was cut to 64 bytes");
  op.rgba = rgba;
  return add_op(out, std::move(op));
}

OverlayCall overlay_scene(Output &out, std::string_view id) {
  const bool token = !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
  if (!token) return bad(OverlayCall::Status::range, "sega.scene takes a token of [a-z0-9_-], at most 32 characters");
  out.scene = std::string(id);
  return {};
}

namespace {
bool counted(Output &out) { return ++out.sprites.calls <= kSpriteCallsMax; }
int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool hex_digits(std::string_view text) { return std::all_of(text.begin(), text.end(), [](char c) { return hex_value(c) >= 0; }); }
}  // namespace

OverlayCall sprite_space(Output &out, std::string_view mode) {
  out.sprites.used = true;
  if (!counted(out)) return bad(OverlayCall::Status::budget, "sega.sprite call limit exceeded");
  if (mode != "picture" && mode != "presentation") return bad(OverlayCall::Status::range, "sega.sprite.space takes \"picture\" or \"presentation\"");
  out.sprites.space = mode == "presentation" ? 1 : 0;
  return {};
}

OverlayCall sprite_budget(Output &out, std::string_view mode) {
  out.sprites.used = true;
  if (!counted(out)) return bad(OverlayCall::Status::budget, "sega.sprite call limit exceeded");
  if (mode != "limit" && mode != "extended") return bad(OverlayCall::Status::range, "sega.sprite.budget takes \"limit\" or \"extended\"");
  out.sprites.budget_mode = mode == "extended" ? 1 : 0;
  return {};
}

OverlayCall sprite_tile(Output &out, std::int64_t index, std::string_view hex) {
  out.sprites.used = true;
  if (!counted(out)) return bad(OverlayCall::Status::budget, "sega.sprite call limit exceeded");
  if (index < 0 || index > 2047 || hex.empty() || hex.size() % 64 != 0 || !hex_digits(hex)) return bad(OverlayCall::Status::range, "sega.sprite.tile takes a pattern index 0-2047 and 64 hexadecimal digits per 8x8 pattern");
  const std::size_t tiles = hex.size() / 64;
  if (static_cast<std::size_t>(index) + tiles > 2048) return bad(OverlayCall::Status::range, "sega.sprite.tile writes past pattern 2047");
  if (out.sprites.vram.empty()) out.sprites.vram.assign(kSpritePoolVramBytes, 0);
  for (std::size_t i = 0; i < tiles * 32; ++i)   // one byte = two pixels, high nibble first (the VDP's 4 bpp layout)
    out.sprites.vram[static_cast<std::size_t>(index) * 32 + i] = static_cast<std::uint8_t>(hex_value(hex[i * 2]) << 4 | hex_value(hex[i * 2 + 1]));
  return {};
}

OverlayCall sprite_palette(Output &out, std::int64_t line, std::string_view hex) {
  out.sprites.used = true;
  if (!counted(out)) return bad(OverlayCall::Status::budget, "sega.sprite call limit exceeded");
  if (line < 0 || line > 3 || hex.empty() || hex.size() % 4 != 0 || hex.size() > 64 || !hex_digits(hex)) return bad(OverlayCall::Status::range, "sega.sprite.palette takes a palette line 0-3 and 4 hexadecimal digits per colour (at most 16)");
  if (out.sprites.cram.empty()) out.sprites.cram.assign(kSpritePoolCramBytes, 0);
  for (std::size_t i = 0; i < hex.size() / 2; ++i)
    out.sprites.cram[static_cast<std::size_t>(line) * 32 + i] = static_cast<std::uint8_t>(hex_value(hex[i * 2]) << 4 | hex_value(hex[i * 2 + 1]));
  return {};
}

OverlayCall sprite_add(Output &out, std::string_view provenance, std::string_view layer, std::int64_t x, std::int64_t y, std::int64_t w_cells, std::int64_t h_cells, std::int64_t tile, std::int64_t palette,
                       std::int64_t flags) {
  out.sprites.used = true;
  if (!counted(out)) return bad(OverlayCall::Status::budget, "sega.sprite call limit exceeded");
  if ((provenance != "authored" && provenance != "verified") || (layer != "behind_planes" && layer != "between" && layer != "above_all"))
    return bad(OverlayCall::Status::range, "sega.sprite.add takes provenance \"authored\" or \"verified\" and layer \"behind_planes\", \"between\" or \"above_all\"");
  if (x < -32768 || x > 32767 || y < -32768 || y > 32767 || w_cells < 0 || w_cells > 255 || h_cells < 0 || h_cells > 255 || tile < 0 || tile > 65535 || palette < 0 || palette > 255 || flags < 0 || flags > 3)
    return bad(OverlayCall::Status::range, "an argument of sega.sprite.add is outside what a command can carry");
  if (out.sprites.total < 255) ++out.sprites.total;
  if (out.sprites.commands.size() < kSpriteCommandsMax) {
    SpriteCommand c;
    c.provenance = provenance == "verified" ? 1 : 2;
    c.layer = layer == "behind_planes" ? 0 : layer == "between" ? 1 : 2;
    c.x = static_cast<int>(x);
    c.y = static_cast<int>(y);
    c.w_cells = static_cast<int>(w_cells);
    c.h_cells = static_cast<int>(h_cells);
    c.tile = static_cast<int>(tile);
    c.palette = static_cast<int>(palette);
    c.h_flip = static_cast<int>(flags & 1);
    c.v_flip = static_cast<int>((flags >> 1) & 1);
    out.sprites.commands.push_back(c);
  }
  return {};
}

OverlayCall sprite_clip(Output &out, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h) {
  out.sprites.used = true;
  if (!counted(out)) return bad(OverlayCall::Status::budget, "sega.sprite call limit exceeded");
  if (out.sprites.total == 0 || out.sprites.commands.empty() || out.sprites.total > out.sprites.commands.size()) return bad(OverlayCall::Status::range, "sega.sprite.clip needs a sprite added before it");
  if (x < -32768 || x > 32767 || y < -32768 || y > 32767 || w < 0 || w > 65535 || h < 0 || h > 65535) return bad(OverlayCall::Status::range, "an argument of sega.sprite.clip is outside what a command can carry");
  SpriteCommand &c = out.sprites.commands.back();
  c.has_clip = true;
  c.clip_x = static_cast<int>(x);
  c.clip_y = static_cast<int>(y);
  c.clip_w = static_cast<int>(w);
  c.clip_h = static_cast<int>(h);
  return {};
}

OverlayImage rasterize_overlay(std::span<const OverlayOp> ops) { return raster_ops(kOverlayWidth, kOverlayHeight, 0, ops); }

OverlayImage rasterize_layer(const OverlayLayerDecl &decl, std::span<const OverlayOp> ops) { return raster_ops(decl.w, decl.h, decl.index, ops); }

}  // namespace segarecomp::luau
