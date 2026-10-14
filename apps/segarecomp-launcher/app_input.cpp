// Input diagnostics: why a controller does or does not work. The detection engine is libs/viewer's sv_input_diag (shared with the in-game viewer);
// this file only presents it. It never claims a controller "works" from enumeration alone: only events that arrived from the device count.

#include "app.hpp"

#include "app_helpers.hpp"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;
using namespace ui;
using namespace launcher::uih;

namespace launcher {

namespace {

ImU32 state_color(SvPadState state) {
  switch (state) {
    case SV_PAD_LIVE: return col::ok;
    case SV_PAD_NO_EVENTS: return col::info;
    case SV_PAD_BINDINGS:
    case SV_PAD_MENU_NAV: return col::warn;
    case SV_PAD_NO_JOYSTICK: return col::dim;
    default: return col::err;
  }
}

const char *connection_text(int c) {
  switch (c) {
    case SDL_JOYSTICK_CONNECTION_WIRED: return "wired (USB)";
    case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
    case SV_DIAG_NOT_OPENED: return "not opened";
    case SDL_JOYSTICK_CONNECTION_UNKNOWN: return "link type not reported";
    default: return "invalid";
  }
}

const char *gamepad_type_text(int t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_STANDARD: return "standard layout";
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One";
    case SDL_GAMEPAD_TYPE_PS3: return "PlayStation 3";
    case SDL_GAMEPAD_TYPE_PS4: return "PlayStation 4";
    case SDL_GAMEPAD_TYPE_PS5: return "PlayStation 5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Nintendo Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Nintendo Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Nintendo Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Nintendo Joy-Con pair";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return "GameCube";
    default: return "type unknown";
  }
}

bool is_nintendo(const SvDiagDevice &d) { return d.vendor == 0x057eU || d.gamepad_type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO; }

std::string hex4(unsigned v) {
  char b[16];
  std::snprintf(b, sizeof b, "%04x", v);
  return b;
}

} // namespace

SvPadState App::input_state() const {
  if (diag_ == nullptr) return SV_PAD_NO_JOYSTICK;
  SvDiagContext ctx{};
  ctx.settings = &viewer_settings_;
  ctx.system = SV_SYSTEM_GENESIS;
  ctx.player = 0;
  ctx.nav_checked = 1;
  SvPadState state = sv_diag_overall(diag_, &ctx);
  if (state == SV_PAD_LIVE) {  // the Master System bindings are audited too: either console with unusable bindings is state E
    int missing = 0, conflicts = 0;
    sv_pad_binding_audit(&viewer_settings_, SV_SYSTEM_SMS, 0, &missing, &conflicts);
    if (missing > 0 || conflicts > 0) state = SV_PAD_BINDINGS;
  }
  return state;
}

// Called once per frame: when SDL delivers pad input AND Dear ImGui's own gamepad navigation sees a gamepad key at the same time, the launcher's
// menu navigation demonstrably receives that pad. Until then a pad that produces SDL events is state F, not "OK".
void App::update_input_diag() {
  if (diag_ == nullptr) return;
  bool imgui_pad = false;
  for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickRight && !imgui_pad; ++key) imgui_pad = ImGui::GetKeyData(static_cast<ImGuiKey>(key))->Down;
  if (!imgui_pad) return;
  for (int i = 0; i < sv_diag_count(diag_); ++i) {
    const SvDiagDevice *d = sv_diag_device(diag_, i);
    if (d != nullptr && d->is_gamepad && (d->buttons != 0 || std::abs(d->axis[SDL_GAMEPAD_AXIS_LEFTX]) > 8000 || std::abs(d->axis[SDL_GAMEPAD_AXIS_LEFTY]) > 8000)) sv_diag_note_nav(diag_, d->id);
  }
}

std::string App::input_report() const {
  char text[2400];
  (void)sv_diag_summary(diag_, text, sizeof text);
  std::string out = std::string("Segarecomp ") + SEGARECOMP_LAUNCHER_VERSION + "\n" + text;
  out += std::string("Host: ") + SDL_GetPlatform() + "\n";
  out += std::string("State: ") + sv_pad_state_code(input_state()) + " - " + sv_pad_state_title(input_state()) + "\n";
  if (hid_state_ >= 0) {
    for (const auto &h : hid_)
      out += "OS device " + hex4(h.vendor) + ":" + hex4(h.product) + " " + (h.bus == 1 ? "USB" : (h.bus == 2 ? "Bluetooth" : "bus unknown")) + " " + h.family + (h.matched_joystick ? " (SDL exposes a joystick)" : " (SDL exposes NO joystick for it)") + "\n";
  } else if (hid_state_ == -1) out += "OS device scan failed: " + hid_error_ + "\n";
  return out;
}

void App::draw_input_diagnostics(float w) {
  if (hid_job_.valid() && hid_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto result = hid_job_.get();
    hid_ = std::move(result.devices);
    hid_state_ = result.count;
    hid_error_ = std::move(result.error);
    for (auto &h : hid_)
      for (int i = 0; i < sv_diag_count(diag_); ++i) {
        const auto *d = sv_diag_device(diag_, i);
        if (d->vendor == h.vendor && d->product == h.product) h.matched_joystick = 1;
      }
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float pad = ctx_.u(14.0F), lw = ctx_.u(150.0F);
  const SvPadState overall = input_state();
  const ImU32 color = state_color(overall);
  heading(ctx_, "Input diagnostics");
  paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w,
            "Works without any ROM. A controller counts as working only when button or axis events actually arrive from it: being listed is not enough. The same engine runs inside every game (Controls > Controller status).");
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  {  // overall status card
    const std::string title = std::string(sv_pad_state_title(overall));
    const auto advice = wrap(ctx_.regular, ctx_.u(12.0F), w - ctx_.u(84.0F) - pad, sv_pad_state_advice(overall));
    const float h = ctx_.u(46.0F) + static_cast<float>(advice.size()) * (ctx_.u(12.0F) + ctx_.u(3.0F)) + ctx_.u(14.0F);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    card(ctx_, dl, a, b, false, false, rgba(7, 17, 40));
    const ImVec2 c0(a.x + pad, a.y + pad), c1(c0.x + ctx_.u(46.0F), c0.y + ctx_.u(46.0F));
    dl->AddRectFilled(c0, c1, (color & 0x00FFFFFF) | 0x33000000, ctx_.u(6.0F));
    dl->AddRect(c0, c1, color, ctx_.u(6.0F), 0, std::max(1.0F, ctx_.u(1.6F)));
    const char *code = sv_pad_state_code(overall);
    const float cpx = ctx_.u(20.0F);
    draw_text(dl, ctx_.display, cpx, color, ImVec2((c0.x + c1.x) * 0.5F - text_width(ctx_.display, cpx, code) * 0.5F, (c0.y + c1.y) * 0.5F - cpx * 0.5F), code);
    draw_text(dl, ctx_.bold, ctx_.u(15.0F), col::text, ImVec2(c1.x + ctx_.u(14.0F), a.y + pad), title.c_str());
    float y = a.y + pad + ctx_.u(24.0F);
    for (const auto &line : advice) { draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(c1.x + ctx_.u(14.0F), y), line.c_str()); y += ctx_.u(12.0F) + ctx_.u(3.0F); }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ctx_.u(8.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  {  // actions + the launcher's own menu test
    const ImVec2 row0 = ImGui::GetCursorScreenPos();
    if (button(ctx_, "in_rescan", "Rescan", ImVec2(ctx_.u(110.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("reset"))) { sv_diag_scan(diag_); toast("Rescanned controllers."); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "in_copy", "Copy report", ImVec2(ctx_.u(130.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("file"))) { SDL_SetClipboardText(input_report().c_str()); toast("Report copied. Review device errors before sharing."); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "in_menu_test", "Menu test: press A / South or Enter", ImVec2(ctx_.u(290.0F), ctx_.u(30.0F)), Style::ghost)) {
      ++nav_taps_;
      nav_last_source_ = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) ? "gamepad" : (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? "mouse" : "keyboard");
    }
    if (ImGui::IsItemFocused()) focus_label_ = "Menu test";
    ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + ctx_.u(38.0F)));
    const std::string seen = nav_taps_ == 0 ? "Menu test: not pressed yet (it shows whether the launcher's own navigation receives this input)" : "Menu test: activated " + std::to_string(nav_taps_) + "x, last via " + nav_last_source_;
    paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), nav_taps_ == 0 ? col::dim : col::ok, w, seen);
  }
  rule(ctx_, w);
  heading(ctx_, "SDL");
  {
    char ver[96];
    sv_diag_sdl_version(ver, sizeof ver);
    label_value(ctx_, lw, w - lw, "Version", ver);
    label_value(ctx_, lw, w - lw, "Video subsystem", sv_diag_video_ok(diag_) ? "initialised" : "FAILED", sv_diag_video_ok(diag_) ? col::text : col::err);
    label_value(ctx_, lw, w - lw, "Gamepad subsystem", sv_diag_gamepad_subsystem_ok(diag_) ? "initialised" : std::string("FAILED: ") + (sv_diag_gamepad_init_error(diag_)[0] != '\0' ? sv_diag_gamepad_init_error(diag_) : "no SDL error text"),
                sv_diag_gamepad_subsystem_ok(diag_) ? col::text : col::err);
    label_value(ctx_, lw, w - lw, "Switch 2 driver hint", std::string(sv_diag_switch2_hint_state()) + "  (SDL_JOYSTICK_HIDAPI_SWITCH2; on by default when HIDAPI is)");
    label_value(ctx_, lw, w - lw, "libusb-1.0", sv_diag_libusb_available(diag_) ? "found at startup (SDL loads it at run time)" : "not found at startup", sv_diag_libusb_available(diag_) ? col::text : col::warn);
  }
  rule(ctx_, w);
  heading(ctx_, sv_diag_count(diag_) == 0 ? "Controllers (none seen by SDL)" : "Controllers seen by SDL");
  for (int i = 0; i < sv_diag_count(diag_); ++i) {
    const SvDiagDevice *d = sv_diag_device(diag_, i);
    SvDiagContext ctx{};
    ctx.settings = &viewer_settings_; ctx.system = SV_SYSTEM_GENESIS; ctx.player = 0; ctx.nav_checked = 1;
    const SvPadState state = sv_diag_state_of(diag_, d, &ctx);
    const float cw = w - ctx_.u(4.0F);
    const bool nin = is_nintendo(*d);
    const float chip_h = ctx_.u(34.0F);
    const float chips_per_row_w = cw - 2 * pad;
    const float chip_w = ctx_.u(88.0F);
    const int per_row = std::max(1, static_cast<int>((chips_per_row_w + ctx_.u(5.0F)) / (chip_w + ctx_.u(5.0F))));
    const int rows = (15 + per_row - 1) / per_row;
    const float h = ctx_.u(150.0F) + static_cast<float>(rows) * (chip_h + ctx_.u(5.0F)) + ctx_.u(24.0F);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + h);
    card(ctx_, dl, a, b, false, false, rgba(7, 17, 40));
    const ImU32 sc = state_color(state);
    float y = a.y + pad;
    draw_text(dl, ctx_.bold, ctx_.u(14.0F), col::text, ImVec2(a.x + pad, y), ellipsize(ctx_.bold, ctx_.u(14.0F), cw - 2 * pad - ctx_.u(130.0F), d->name).c_str());
    {
      const std::string badge_text = std::string("[") + sv_pad_state_code(state) + "] " + (state == SV_PAD_LIVE ? "receiving input" : sv_pad_state_title(state));
      const std::string shown = ellipsize(ctx_.bold, ctx_.u(11.0F), cw * 0.5F, badge_text);
      pill(ctx_, dl, ImVec2(b.x - pad - text_width(ctx_.bold, ctx_.u(11.0F), shown.c_str()) - ctx_.u(14.0F), y - ctx_.u(2.0F)), shown.c_str(), sc, nullptr, ctx_.u(11.0F));
    }
    y += ctx_.u(26.0F);
    const auto kv = [&](const char *k, const std::string &v, ImU32 vc = col::text2) {
      draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::dim, ImVec2(a.x + pad, y), k);
      draw_text(dl, ctx_.regular, ctx_.u(11.5F), vc, ImVec2(a.x + pad + lw, y), ellipsize(ctx_.regular, ctx_.u(11.5F), cw - 2 * pad - lw, v).c_str());
      y += ctx_.u(16.0F);
    };
    kv("Vendor : product", hex4(d->vendor) + " : " + hex4(d->product) + (d->version != 0 ? "  (v" + hex4(d->version) + ")" : std::string()) + "   SDL id " + std::to_string(d->id));
    kv("GUID", d->guid);
    kv("Kind", std::string(d->is_gamepad ? "gamepad, " : "raw joystick with NO gamepad mapping, ") + (d->is_gamepad ? gamepad_type_text(d->gamepad_type) : "its buttons are never used as gameplay input") + ", " + connection_text(d->connection) +
                   (d->player_index >= 0 ? ", SDL player " + std::to_string(d->player_index + 1) : std::string()));
    if (d->open_error[0] != '\0') kv("SDL error", d->open_error, col::err);
    else if (!d->is_gamepad && d->open_ok) kv("Raw shape", std::to_string(d->raw_buttons) + " buttons, " + std::to_string(d->raw_axes) + " axes, " + std::to_string(d->raw_hats) + " hats; " + std::to_string(d->raw_events) + " raw event(s)");
    else kv("Events", std::to_string(d->events) + " since opened" + (d->last_event_ms > 0 ? ", last " + std::to_string((SDL_GetTicks() - d->last_event_ms) / 1000U) + " s ago" : ", none yet"), d->events > 0 ? col::ok : col::dim);
    // live buttons by SDL position (+ the printed Nintendo labels where the controller is a Nintendo one)
    static const int buttons[15] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE,
                                    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                                    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    y += ctx_.u(4.0F);
    for (int k = 0; k < 15; ++k) {
      const int r = k / per_row, c = k % per_row;
      const ImVec2 p0(a.x + pad + static_cast<float>(c) * (chip_w + ctx_.u(5.0F)), y + static_cast<float>(r) * (chip_h + ctx_.u(5.0F))), p1(p0.x + chip_w, p0.y + chip_h);
      const bool down = d->is_gamepad && ((d->buttons >> static_cast<unsigned>(buttons[k])) & 1U) != 0;
      dl->AddRectFilled(p0, p1, down ? col::btn : rgba(10, 22, 52), ctx_.u(5.0F));
      dl->AddRect(p0, p1, down ? col::btn_top : rgba(34, 70, 140), ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
      const char *pos = sv_pad_position_name(buttons[k]);
      const float ppx = ctx_.u(10.5F);
      draw_text(dl, ctx_.regular, ppx, down ? col::btn_text : col::text2, ImVec2((p0.x + p1.x) * 0.5F - text_width(ctx_.regular, ppx, pos) * 0.5F, p0.y + ctx_.u(4.0F)), pos);
      const char *label = sv_pad_nintendo_label(buttons[k]);
      if (nin && label != nullptr) {
        const std::string printed = std::string("prints ") + label;
        draw_text(dl, ctx_.regular, ctx_.u(9.5F), down ? col::btn_text : col::dim, ImVec2((p0.x + p1.x) * 0.5F - text_width(ctx_.regular, ctx_.u(9.5F), printed.c_str()) * 0.5F, p0.y + ctx_.u(18.0F)), printed.c_str());
      }
    }
    y += static_cast<float>(rows) * (chip_h + ctx_.u(5.0F)) + ctx_.u(4.0F);
    {  // left stick (with the viewer's dead zone) and triggers
      const float s = ctx_.u(40.0F);
      const ImVec2 c0(a.x + pad, y), c1(c0.x + s, c0.y + s);
      dl->AddRectFilled(c0, c1, rgba(5, 12, 30), ctx_.u(4.0F));
      dl->AddRect(c0, c1, rgba(34, 70, 140), ctx_.u(4.0F));
      const float dz = static_cast<float>(viewer_settings_.deadzone) / 100.0F;
      dl->AddCircle(ImVec2((c0.x + c1.x) * 0.5F, (c0.y + c1.y) * 0.5F), s * 0.5F * dz, rgba(90, 120, 190, 160), 24);
      const float fx = std::clamp(static_cast<float>(d->axis[SDL_GAMEPAD_AXIS_LEFTX]) / 32767.0F, -1.0F, 1.0F), fy = std::clamp(static_cast<float>(d->axis[SDL_GAMEPAD_AXIS_LEFTY]) / 32767.0F, -1.0F, 1.0F);
      dl->AddCircleFilled(ImVec2((c0.x + c1.x) * 0.5F + fx * s * 0.45F, (c0.y + c1.y) * 0.5F + fy * s * 0.45F), ctx_.u(4.0F), col::btn_top);
      char buf[160];
      std::snprintf(buf, sizeof buf, "Left stick x %+d%%  y %+d%% (dead zone %d%%)   Right stick x %+d%% y %+d%%   Triggers L %d%% R %d%%", d->axis[SDL_GAMEPAD_AXIS_LEFTX] * 100 / 32767, d->axis[SDL_GAMEPAD_AXIS_LEFTY] * 100 / 32767,
                    viewer_settings_.deadzone, d->axis[SDL_GAMEPAD_AXIS_RIGHTX] * 100 / 32767, d->axis[SDL_GAMEPAD_AXIS_RIGHTY] * 100 / 32767, std::max(0, static_cast<int>(d->axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER])) * 100 / 32767,
                    std::max(0, static_cast<int>(d->axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER])) * 100 / 32767);
      draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::text2, ImVec2(c1.x + ctx_.u(12.0F), y + s * 0.5F - ctx_.u(6.0F)), ellipsize(ctx_.regular, ctx_.u(11.0F), cw - 2 * pad - s - ctx_.u(12.0F), buf).c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ctx_.u(8.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  if (sv_diag_count(diag_) == 0) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::dim, w, "Plug in or wake a controller: it appears here the moment SDL reports it (hot-plug, no restart).");
  rule(ctx_, w);
  heading(ctx_, "Operating-system view (Nintendo, read-only)");
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, w,
            "Lists the Nintendo HID devices the OS exposes, with the bus (USB or Bluetooth), even when SDL offers no joystick for them. It opens nothing and installs nothing.");
  if (button(ctx_, "in_hid", hid_job_.valid() ? "Scanning..." : "Scan OS devices", ImVec2(ctx_.u(170.0F), ctx_.u(30.0F)), Style::secondary, !hid_job_.valid(), ctx_.icon("search"))) {
    hid_job_ = std::async(std::launch::async, [] {
      HidResult r;
      r.devices.resize(SV_DIAG_MAX_HID);
      char err[160];
      r.count = sv_diag_hid_scan(nullptr, r.devices.data(), SV_DIAG_MAX_HID, err, sizeof err);
      r.error = err;
      r.devices.resize(r.count >= 0 ? static_cast<std::size_t>(r.count) : 0);
      return r;
    });
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
  bool switch2_seen = false, switch2_bluetooth = false, switch2_hidden = false;
  if (hid_state_ == -1) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, w, "The OS scan failed: " + hid_error_);
  else if (hid_state_ == 0) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "No Nintendo HID device is visible to the OS right now.");
  for (const auto &h : hid_) {
    const bool s2 = h.product >= 0x2066U && h.product <= 0x2073U;
    switch2_seen = switch2_seen || s2;
    switch2_bluetooth = switch2_bluetooth || (s2 && h.bus == 2);
    switch2_hidden = switch2_hidden || (s2 && !h.matched_joystick);
    label_value(ctx_, lw, w - lw, (hex4(h.vendor) + ":" + hex4(h.product)).c_str(),
                std::string(h.bus == 1 ? "USB" : (h.bus == 2 ? "Bluetooth" : "bus not reported")) + ", " + (h.family[0] != '\0' ? h.family : (h.product_string[0] != '\0' ? h.product_string : "Nintendo device")) +
                    (h.matched_joystick ? " - SDL exposes a joystick for it" : " - SDL exposes NO joystick for it"),
                h.matched_joystick ? col::ok : col::warn);
  }
  if (switch2_seen && switch2_hidden) {
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w,
              std::string("A Nintendo Switch 2 controller is visible to the OS but SDL exposes no joystick for it. In the SDL 3.4 series Switch 2 controllers are supported by a USB-only driver that loads libusb at run time ") +
                  "(over Bluetooth SDL reports \"not supported over Bluetooth\"), and the driver is only compiled into an SDL built with libusb. " +
                  (switch2_bluetooth ? "This one is on Bluetooth: connect it by USB cable, or use another controller. " : "") + std::string("libusb-1.0 on this system: ") +
                   (sv_diag_libusb_available(diag_) ? "found at startup" : "NOT found at startup") + ". Segarecomp does not install drivers or change USB driver bindings. See docs/testing/input-diagnostics.md.");
  }
  rule(ctx_, w);
  heading(ctx_, "Mapping file (for a controller SDL lists as a joystick only)");
  {
    const std::string path = u8s(library_dir_ / "gamecontrollerdb.txt");
    paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, w, "A mapping line for the GUID above, in SDL's gamecontrollerdb.txt format, turns a raw joystick into an explicit gamepad. Segarecomp ships no mapping database and never guesses one. File (checked on Reload): " + path);
    if (button(ctx_, "in_map_reload", "Reload mapping file", ImVec2(ctx_.u(190.0F), ctx_.u(30.0F)), Style::secondary, true)) {
      const int n = sv_diag_load_mapping_file(path.c_str());
      sv_diag_scan(diag_);
      toast(n < 0 ? "The mapping file could not be read." : "Loaded " + std::to_string(n) + " mapping line(s).", n < 0);
    }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "in_open_lib", "Open library folder", ImVec2(ctx_.u(190.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) { std::error_code mk; fs::create_directories(library_dir_, mk); SDL_OpenURL(("file://" + u8s(library_dir_)).c_str()); }
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w,
            "In a game: Back (Select, Nintendo -) or Home opens the pause menu; Start (Nintendo +) is a game button. Face buttons are SDL positions: South is Nintendo B, East is Nintendo A. Set SEGARECOMP_INPUT_TRACE=1 to log every pad event (no ROM data is logged).");
  (void)pad;
}

} // namespace launcher
