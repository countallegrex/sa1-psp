-- oracle_shots.lua - tape_oracle.lua plus console screenshots at aligned
-- desktop game frames (tools/flycast/run_oracle.py --shots).
--
--   SA1_SHOT_SPEC  "name:game_frame,...": the desktop's game-frame numbering
--   SA1_SHOTS      directory for the requests
--
--   SA1_SHOT_FB    1: read the frame from emulated VRAM (run_oracle's default)
--   SA1_SHOT_LAG   game frames to wait past a spec's frame (default 2)
--
-- The console displays a game frame's image 2 game frames after the aligned
-- counter reaches it (rain4, 2026-10-21: the Chaos 0 drops and camera in RAM
-- are identical on both sides at game frame 2557, and the console's image at
-- 2557 is the desktop's state at 2555; the searchlight and Emerald Coast shots
-- were off by the same 4 vblanks), so each shot waits SA1_SHOT_LAG game
-- frames to pair the image with the desktop's shot of the same frame.
--
-- With SA1_SHOT_FB, when the aligned game frame first reaches a spec's
-- frame the script writes the frame the console is displaying to NAME.ppm
-- itself: Flycast runs with rend.EmulateFramebuffer, which writes each
-- render into VRAM as the hardware does, and the PVR's FB_R_* registers say
-- where and how the CRTC reads it.  No window is involved, so Flycast can
-- run minimized.  Without it, the script writes NAME.req and holds the
-- emulator thread in this vblank callback (a busy wait, at most 5 s) until
-- capture_window.ps1 has copied Flycast's window to NAME.png and renamed the
-- request to NAME.done (the window keeps showing the last presented image
-- while the thread waits; the screen copy works where PrintWindow would
-- block, but it raises the window).
--
--   SA1_ORACLE_EXTRA  optional: a Lua file run last, for experiments (it can
--                     wrap flycast_callbacks.vblank, e.g. to patch guest code
--                     and see what a draw contributes to the console's image)
--
-- Part of sa1-psp (MIT).

dofile("tape_oracle.lua")
local inner = flycast_callbacks.vblank
local SHOTS = os.getenv("SA1_SHOTS")
local specs = {}
for name, g in string.gmatch(os.getenv("SA1_SHOT_SPEC") or "", "([%w_%-]+):(%d+)") do
  specs[#specs + 1] = {name = name, g = tonumber(g), done = false}
end
-- The displayed frame as a binary PPM (Holly registers: FB_R_CTRL 0x44,
-- FB_R_SOF1/2 0x50/0x54, FB_R_SIZE 0x5C, SPG_CONTROL 0xD0).  FB_R_SIZE holds
-- the line length in 32-bit words - 1, the lines - 1 (per field) and the
-- modulus (the stride is length + modulus - 1 words); an interlaced mode
-- takes its even lines from SOF1 and its odd lines from SOF2.
local function fb_dump(path)
  local rd = flycast.memory.read32
  local ctrl, sof1, sof2, size = rd(0xA05F8044), rd(0xA05F8050), rd(0xA05F8054), rd(0xA05F805C)
  local depth = (ctrl >> 2) & 3
  local words, lines, modulus = (size & 1023) + 1, ((size >> 10) & 1023) + 1, (size >> 20) & 1023
  local stride = (words + modulus - 1) * 4
  local interlace = (rd(0xA05F80D0) >> 4) & 1 == 1
  local bpp = ({2, 2, 3, 4})[depth + 1]
  local w, h = words * 4 // bpp, interlace and lines * 2 or lines
  local out = {string.format("P6\n%d %d\n255\n", w, h)}
  local row = {}
  for y = 0, h - 1 do
    local base = interlace and ((y & 1) == 0 and sof1 or sof2) + (y >> 1) * stride or sof1 + y * stride
    local t = flycast.memory.readTable32(0xA5000000 + base, words)
    local bytes, nb = {}, 0
    for i = 0, words - 1 do
      local v = t[0xA5000000 + base + i * 4]
      for b = 0, 3 do nb = nb + 1; bytes[nb] = (v >> (8 * b)) & 255 end
    end
    for x = 0, w - 1 do
      local r, g, b
      if bpp == 2 then
        local v = bytes[2 * x + 1] | (bytes[2 * x + 2] << 8)
        if depth == 0 then r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31; r, g, b = r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2
        else r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31; r, g, b = r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2 end
      else
        local o = x * bpp
        b, g, r = bytes[o + 1], bytes[o + 2], bytes[o + 3]
      end
      row[x + 1] = string.char(r, g, b)
    end
    out[#out + 1] = table.concat(row, "", 1, w)
  end
  local f = assert(io.open(path, "wb"))
  f:write(table.concat(out)); f:close()
  return w, h, depth, interlace
end

local FB = os.getenv("SA1_SHOT_FB") == "1"
local LAG = tonumber(os.getenv("SA1_SHOT_LAG") or "2")
local n = 0
flycast_callbacks.vblank = function()
  inner()
  n = n + 1
  local g = sa1_oracle_aligned_g and sa1_oracle_aligned_g() or -1
  for _, s in ipairs(specs) do
    if not s.done and g >= s.g + LAG then
      s.done = true
      if FB then
        local w, h, depth, il = fb_dump(SHOTS .. "/" .. s.name .. ".ppm")
        print(string.format("shots: %s at aligned game frame %d (vblank %d): framebuffer %dx%d depth %d%s", s.name, g, n, w, h, depth, il and " interlaced" or ""))
        break
      end
      local f = assert(io.open(SHOTS .. "/" .. s.name .. ".req", "w"))
      f:write(string.format("vblank %d game frame %d\n", n, g)); f:close()
      local t0 = os.clock()
      while os.clock() - t0 < 5 do
        local d = io.open(SHOTS .. "/" .. s.name .. ".done", "r")
        if d then d:close(); break end
      end
      print(string.format("shots: %s at aligned game frame %d (vblank %d), held %.2f s", s.name, g, n, os.clock() - t0))
      break
    end
  end
end

local extra = os.getenv("SA1_ORACLE_EXTRA")
if extra and extra ~= "" then dofile(extra) end
