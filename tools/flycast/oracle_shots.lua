-- oracle_shots.lua - tape_oracle.lua plus console screenshots at aligned
-- desktop game frames (tools/flycast/run_oracle.py --shots).
--
--   SA1_SHOT_SPEC  "name:game_frame,...": the desktop's game-frame numbering
--   SA1_SHOTS      directory for the requests
--
-- When the aligned game frame first reaches a spec's frame, the script
-- writes NAME.req there and holds the emulator thread in this vblank
-- callback (a busy wait, at most 5 s) until capture_window.ps1 has copied
-- Flycast's window to NAME.png and renamed the request to NAME.done, so the
-- copy shows that frame and not a few vblanks later.  (The window keeps
-- showing the last presented image while the thread waits; the screen copy
-- works where PrintWindow would block.)
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
local n = 0
flycast_callbacks.vblank = function()
  inner()
  n = n + 1
  local g = sa1_oracle_aligned_g and sa1_oracle_aligned_g() or -1
  for _, s in ipairs(specs) do
    if not s.done and g >= s.g then
      s.done = true
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
