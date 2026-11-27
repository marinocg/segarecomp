-- SEG-145-T008: sound through the game's own driver, only by DECLARED names.
--   C pressed      -> the effect "drum"
--   Right pressed  -> the effect "chime"
--   guest frame 24 -> the music "alt"; guest frame 44 -> stop the music
-- frame_end logs every change of what the game says it forwarded to its driver (a read-only level).
local held = 0
local last = ""

function post_input(frame: number)
  local pad = sega.input.get(1)
  local pressed = bit32.band(pad, bit32.bnot(held))
  held = pad
  if bit32.band(pressed, 0x20) ~= 0 then sega.audio.play_sfx("drum") end
  if bit32.band(pressed, 0x08) ~= 0 then sega.audio.play_sfx("chime") end
  if frame == 24 then sega.audio.play_music("alt") end
  if frame == 44 then sega.audio.stop_music() end
end

function frame_end(frame: number)
  local now = sega.audio.event("accepted") or ""
  if now ~= last then
    last = now
    sega.log("accepted " .. now)
  end
end

function save()
  return { held = held, last = last }
end

function load(state)
  held = state.held
  last = state.last
end
