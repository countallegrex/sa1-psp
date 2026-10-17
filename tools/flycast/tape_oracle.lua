-- tape_oracle.lua - replays an sa1psp tape in Flycast (2.7, Lua 5.5) and
-- traces the watched addresses every vblank, for tools/tape_diff.py.
--
-- Flycast runs it as its Lua script (tools/flycast/run_oracle.py passes
-- -config config:LuaFileName=tape_oracle.lua).  Paths come from the
-- environment:
--   SA1_ORACLE_GPAD   game-frame pad table (tools/tape_diff.py gpad)
--   SA1_ORACLE_WATCH  watch list (tests/replay/watch.txt)
--   SA1_ORACLE_OUT    trace to write (same columns as sa1psp --watch-trace)
--   SA1_ORACLE_MAX    optional: stop after this many vblanks
--
-- The pad is chosen by the game's own frame counter (the game_frame watch),
-- not by vblank: every vblank, read the counter G and present the pad the
-- desktop run presented in game frame G - offset.  Flycast's own GD-ROM
-- timing then does not shift the input.  The offset is re-anchored at each
-- game-mode change the desktop run also had ("sync" lines): the desktop
-- skips phases the console plays (boot screens, the intro movie).  In a mode
-- the desktop never entered the row is marked unaligned (oracle_g = -1) and
-- the pad is neutral, except that after 120 vblanks Start is tapped every 60:
-- such a mode can be a console-only prompt (game mode 13 at boot waits for
-- a button; the desktop build never shows it).  Buttons go to kcode (active low, same bits as
-- the tape), the stick and triggers to Flycast's 16-bit axes (tape value
-- times 256: the controller byte is then the tape value plus 128).
--
-- Part of sa1-psp (MIT).

local env = os.getenv
local GPAD, WATCH, OUT = env("SA1_ORACLE_GPAD"), env("SA1_ORACLE_WATCH"), env("SA1_ORACLE_OUT")
local MAX = tonumber(env("SA1_ORACLE_MAX") or "") or 0

local pads, first_g, last_g = {}, nil, -1
local watches, counter, modew = {}, nil, nil
local syncs, next_sync, offset, aligned, last_mode, unaligned_since = {}, 1, 0, true, nil, 0
local START_TAP = {8, 0, 0, 0, 0}
local LOOKAHEAD = 4          -- desktop mode changes the console may skip before a match
local out, vblank, done = nil, 0, false
local NEUTRAL = {0, 0, 0, 0, 0}

local function load()
  for line in io.lines(GPAD) do
    local sm, sg = line:match("^sync%s+(%d+)%s+(%d+)")
    if sm then syncs[#syncs + 1] = {mode = tonumber(sm), g = tonumber(sg)} end
    local g, b, x, y, lt, rt = line:match("^(%d+)%s+(%x+)%s+(%-?%d+)%s+(%-?%d+)%s+(%d+)%s+(%d+)")
    if g then
      g = tonumber(g)
      pads[g] = {tonumber(b, 16), tonumber(x), tonumber(y), tonumber(lt), tonumber(rt)}
      if not first_g or g < first_g then first_g = g end
      if g > last_g then last_g = g end
    end
  end
  for line in io.lines(WATCH) do
    local addr, size, name = line:match("^%s*(%x+)%s+(%S+)%s+(%S+)")
    if addr and not line:match("^%s*#") then
      local w = {addr = tonumber(addr, 16), size = size, name = name}
      watches[#watches + 1] = w
      if name == "game_frame" then counter = w end
      if name == "game_mode" then modew = w end
    end
  end
  assert(counter and modew, "watch list needs game_frame and game_mode rows")
  out = assert(io.open(OUT, "w"))
  local names = {"frame"}
  for _, w in ipairs(watches) do names[#names + 1] = w.name end
  names[#names + 1] = "oracle_g"
  out:write(table.concat(names, "\t"), "\n")
end

local mem, input = flycast.memory, flycast.input

local function value(w)
  local s, a = w.size, w.addr
  if s == "f" then return string.format("%.9g", string.unpack("<f", string.pack("<I4", mem.read32(a)))) end
  local v
  if s == "1" or s == "s1" then v = mem.read8(a)
  elseif s == "2" or s == "s2" then v = mem.read16(a)
  else v = mem.read32(a) end
  if s == "s1" and v >= 0x80 then v = v - 0x100 end
  if s == "s2" and v >= 0x8000 then v = v - 0x10000 end
  if s == "s4" and v >= 0x80000000 then v = v - 0x100000000 end
  return string.format("%d", v)
end

local function finish(why)
  if done then return end
  done = true
  out:close()
  print("tape_oracle: " .. why .. " after " .. vblank .. " vblanks")
  flycast.emulator.exit()
end

local function on_vblank()
  if done then return end
  vblank = vblank + 1
  local g = mem.read32(counter.addr)
  local mode = tonumber(value(modew))
  if mode ~= last_mode then
    last_mode = mode
    local found = nil
    for k = next_sync, math.min(#syncs, next_sync + LOOKAHEAD) do
      if syncs[k].mode == mode then found = k; break end
    end
    if found then
      offset, aligned, next_sync = g - syncs[found].g, true, found + 1
      print(string.format("tape_oracle: vblank %d, game mode %d at game frame %d = desktop %d", vblank, mode, g, syncs[found].g))
    else
      aligned = next_sync > 1 and syncs[next_sync - 1].mode == mode
      unaligned_since = vblank
    end
  end
  local ag = aligned and g - offset or -1
  local p = aligned and pads[ag] or NEUTRAL
  local waited = vblank - unaligned_since
  if not aligned and waited >= 120 and waited % 60 < 2 then p = START_TAP end
  input.releaseButtons(1, 0xFFFF)
  input.pressButtons(1, p[1])
  input.setAxis(1, 1, p[2] * 256)
  input.setAxis(1, 2, p[3] * 256)
  input.setAxis(1, 5, p[4] * 256)
  input.setAxis(1, 6, p[5] * 256)
  local row = {tostring(vblank)}
  for i, w in ipairs(watches) do row[i + 1] = value(w) end
  row[#row + 1] = tostring(ag)
  out:write(table.concat(row, "\t"), "\n")
  if vblank % 600 == 0 then out:flush() end
  if ag > last_g then finish("tape ended (game frame " .. g .. ")")
  elseif MAX > 0 and vblank >= MAX then finish("vblank limit") end
end

local ok, err = pcall(load)
if not ok then
  print("tape_oracle: " .. tostring(err))
else
  flycast_callbacks = {
    vblank = on_vblank,
    terminate = function() if not done then done = true; out:close() end end,
  }
end
