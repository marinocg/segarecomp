-- Luau-only HUD example (restricted scripting: ADR 0295/0296). It runs while the game plays; it can only request overlay drawing and write log lines.
-- The capabilities it uses are declared in mod.toml; `segarecomp mod check` rejects any other `sega.*` use.
local PANEL = 0x000000C0     -- 0xRRGGBBAA
local WHITE = 0xFFFFFFFF
local GOLD = 0xFFD040FF

function reset()
  sega.log("hud example ready")
end

function render(frame)
  sega.overlay.rect(0, 0, 76, 24, PANEL)
  sega.overlay.text(4, 3, "LUAU HUD", WHITE)
  sega.overlay.text(4, 12, "FRAME " .. frame % 1000, GOLD)
  sega.overlay.rect(0, 24, frame % 76 + 1, 3, GOLD)   -- a bar that grows with the frame counter
end
