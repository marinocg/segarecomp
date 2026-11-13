-- Gameplay example (restricted scripting: ADR 0295/0297/0300). Rule: while button C is held, every frame adds BONUS to the score the fixture program keeps in work RAM at 0xFF0010
-- (a typed, bounded, big-endian 16-bit access the manifest declares); a HUD panel shows how many bonuses were granted. The mod keeps that counter, so it saves and restores it (state.mod).
local SCORE = 0x10        -- offset in work RAM: 0xFF0010 (declared in [[memory]])
local BONUS = 10
local C_BUTTON = 32       -- 0x20: button C in the pad bits sega.input.get returns
local WHITE = 0xFFFFFFFF
local GOLD = 0xFFD040FF
local PANEL = 0x000000C0

local granted = 0

function reset()
  granted = 0
  sega.log("gameplay example ready")
end

function post_input(frame: number)
  local pad = sega.input.get(1)
  if pad // C_BUTTON % 2 == 1 then
    local score = sega.mem.read("m68k.work_ram", SCORE, 2)
    sega.mem.write("m68k.work_ram", SCORE, 2, (score + BONUS) % 65536)
    granted = granted + 1
  end
end

function render(frame: number)
  sega.overlay.rect(0, 0, 76, 24, PANEL)
  sega.overlay.text(4, 3, "BONUS", WHITE)
  sega.overlay.text(4, 12, "x" .. granted, GOLD)
  sega.overlay.rect(0, 24, granted % 76 + 1, 3, GOLD)
end

function save()
  return { granted = granted }
end

function load(state)
  granted = state.granted
end
