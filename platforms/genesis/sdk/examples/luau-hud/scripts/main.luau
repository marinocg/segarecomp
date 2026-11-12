-- Luau-only HUD example (restricted scripting: ADR 0295). Runs while you play; it can only read the values the host publishes and request overlay drawing.
-- The capabilities it uses are declared in mod.toml; `segarecomp mod check` rejects any other `sega.*` use.
local PANEL = 0x000000A0     -- 0xRRGGBBAA
local WHITE = 0xFFFFFFFF
local GOLD = 0xFFD040FF

function reset()
  sega.log("hud example ready")
end

function render(frame)
  sega.overlay.rect(0, 0, 96, 28, PANEL)
  local score = sega.frame.value("score") or 0
  local lives = sega.frame.value("lives") or 0
  sega.overlay.text(4, 3, "SCORE " .. string.format("%06d", score), WHITE)
  sega.overlay.text(4, 15, "LIVES " .. lives .. " F" .. frame % 100, GOLD)
end
