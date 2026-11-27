-- Master System gameplay example (SEG-147-T011, ADR 0325): "hold button 2 to run".
-- Gameplay: while button 2 is held the mod holds Right for the player (input interception) and raises the game's own speed byte from 1 to 3 (a bounded write in a declared work-RAM range).
-- Presentation only (never guest state): a HUD, a tinted sky, a small authored marker and a one-pixel shake while running.
local RIGHT, BUTTON2 = 0x08, 0x20
local SCORE, POSITION, SPEED = 0x10, 0x12, 0x16
local SKY = 0x30 -- the game's backdrop colour value

local running = false
local run_frames = 0

function reset()
  running = false
  run_frames = 0
  sega.log("sms gameplay example ready")
end

function input(frame: number)
  local pad = sega.input.get(1)
  running = bit32.band(pad, BUTTON2) ~= 0
  if running then
    sega.input.set(1, bit32.bor(bit32.band(pad, 0x3F - BUTTON2), RIGHT))
  end
end

function post_input(frame: number)
  sega.mem.write("z80.work_ram", SPEED, 1, if running then 3 else 1)
  if running then
    run_frames += 1
  end
end

function render(frame: number)
  local score = sega.frame.work_ram(SCORE, 2)
  local position = sega.frame.work_ram(POSITION, 2)
  sega.overlay.rect(4, 4, 150, 28, 0x000000c0)
  sega.overlay.text(8, 5, "SCORE " .. tostring(score) .. "  POS " .. tostring(position), 0xffffffff)
  sega.overlay.text(8, 18, (if running then "RUN " else "WALK ") .. tostring(run_frames), 0xffff40ff)
  if running then
    sega.present.color(SKY, 0x602000)
    sega.present.scroll(1, 0)
    sega.present.colors("00" .. "0f" .. string.rep("00", 14))
    sega.present.pattern(0, "00011000" .. "00011100" .. "11111110" .. "11111111" .. "11111111" .. "11111110" .. "00011100" .. "00011000")
    sega.present.sprite(8 + position % 232, 96, 1, 1, 0, 0)
  end
end

function save()
  return { running = running, run_frames = run_frames }
end

function load(saved)
  running = saved.running
  run_frames = saved.run_frames
end
