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
--   SA1_ORACLE_STOP   optional: stop once the aligned desktop game frame
--                     reaches this value
--   SA1_ORACLE_POKE_EC  optional, 1: when the level variable becomes 15
--                     (Chaos 0) rewrite the stage variables to Emerald Coast
--                     act 1, what the scripted desktop route's --poke 5290
--                     does (tools/replay_test.py ROUTE_POKES)
--   SA1_ORACLE_SAVE   optional: "slot:g,slot:g,...": save a Flycast state
--                     into that slot when the aligned desktop game frame
--                     first reaches g (tools/flycast/oracle_states.py builds
--                     the state library with it).  The save runs from the
--                     overlay callback (the UI thread: a save from the vblank
--                     callback deadlocks the emulator), and a line per state
--                     goes to SA1_ORACLE_STATE_OUT: the script's alignment
--                     variables, which a later run restores through
--                     SA1_ORACLE_RESUME.
--   SA1_ORACLE_LOAD_SLOT  optional: load that Flycast state slot from the
--                     first overlay callback (the UI thread again) before any
--                     vblank is processed, with
--   SA1_ORACLE_RESUME "vblank offset next_sync last_mode aligned
--                     unaligned_since", the line the save wrote, so the pad
--                     table and the trace continue as if the run had never
--                     stopped.  (Flycast's Dreamcast.AutoLoadState setting
--                     does not take effect from the command line.)
--   SA1_ORACLE_AUDIO_MAP  optional: write a line per vblank, "vblank
--                     game_frame aligned_desktop_g samples" (aligned -1 when
--                     unaligned), samples being the stereo samples the AICA
--                     mixer has produced so far (flycast.emulator.audioSamples,
--                     a local Flycast patch that also writes them to the WAV
--                     named by SA1_WAV; -1 on a Flycast without it), to place
--                     the console's audio against desktop game frames.
--
-- The pad is chosen by the game's own frame counter (the game_frame watch),
-- not by vblank: every vblank, read the counter G and present the pad the
-- desktop run presented in game frame G - offset.  Flycast's own GD-ROM
-- timing then does not shift the input.  The offset is re-anchored at each
-- game-mode change the desktop run also had ("sync" lines): the desktop
-- skips phases the console plays (boot screens, the intro movie).  The
-- anchor is the first game frame after the last one seen in the old mode:
-- where the counter steps by more than one between two vblanks (a short
-- loop pass, often the mode change itself), the change happened in the
-- first unseen count, not in the one the vblank shows.  In a mode
-- the desktop never entered the row is marked unaligned (oracle_g = -1) and
-- the pad is neutral, except that after 120 vblanks Start is tapped every 60:
-- such a mode can be a console-only prompt (game mode 13 at boot waits for
-- a button; the desktop build never shows it).  Buttons go to kcode (active low, same bits as
-- the tape), the stick and triggers to Flycast's 16-bit axes (tape value
-- times 256: the controller byte is then the tape value plus 128).
-- Flycast latches the pad one vblank after this script sets it, so each
-- vblank presents what the desktop had at the next one: in the first vblank
-- of game frame G the pad of G's last vblank ("G ..." lines), in a later
-- vblank the pad of G+1's first vblank ("first G+1 ..." lines).
-- When the watch list has an rng_seed row (the game's rand() state) and the
-- sync line carries the desktop's seed, the console's seed is set to it at
-- that sync, and the gap is printed as a count of rand() calls.  Loads let
-- vblanks pass on the console (a blocking 200 KB read is ~6 vblanks in
-- Flycast, instant on the desktop), and the adventure field then runs
-- catch-up logic updates (0x8C04EA90): a different number of rand() calls
-- before the mode change, so without the resync every object that picks a
-- random target diverges after the first load.  Mode changes happen at
-- loads, where no rand() call runs, so both sides are at rest there.
--
-- Part of sa1-psp (MIT).

local env = os.getenv
local GPAD, WATCH, OUT = env("SA1_ORACLE_GPAD"), env("SA1_ORACLE_WATCH"), env("SA1_ORACLE_OUT")
local MAX = tonumber(env("SA1_ORACLE_MAX") or "") or 0
local STOP = tonumber(env("SA1_ORACLE_STOP") or "") or -1
local POKE_EC = (env("SA1_ORACLE_POKE_EC") or "") ~= "" and env("SA1_ORACLE_POKE_EC") ~= "0"
local STATE_OUT = env("SA1_ORACLE_STATE_OUT")
local LOAD_SLOT = tonumber(env("SA1_ORACLE_LOAD_SLOT") or "") or -1
local loaded = LOAD_SLOT < 0          -- vblanks before the state is in are not the run

local pads, firsts, first_g, last_g = {}, {}, nil, -1
local cur_g, phase = nil, 0   -- the counter and the vblanks since it changed
local watches, counter, modew, rngw = {}, nil, nil, nil
local syncs, next_sync, offset, aligned, last_mode, unaligned_since = {}, 1, 0, true, nil, 0
local START_TAP = {8, 0, 0, 0, 0}
local LOOKAHEAD = 4          -- desktop mode changes the console may skip before a match
local out, vblank, done = nil, 0, false
local last_ag = -1
local prev_g = nil           -- the counter at the previous vblank (the sync anchor)
function sa1_oracle_aligned_g() return last_ag end   -- for wrapper scripts (screenshots at a desktop game frame)
local NEUTRAL = {0, 0, 0, 0, 0}
local saves, save_pending, poked = {}, nil, false
local AUDIO_MAP = env("SA1_ORACLE_AUDIO_MAP")
local amap = nil
local audio_samples = flycast.emulator and flycast.emulator.audioSamples
local function samples_now()
  if not audio_samples then return -1 end
  local ok, n = pcall(audio_samples)
  return ok and tonumber(n) or -1
end

local function load()
  for line in io.lines(GPAD) do
    local sm, sg, ss = line:match("^sync%s+(%d+)%s+(%d+)%s*(%x*)")
    if sm then syncs[#syncs + 1] = {mode = tonumber(sm), g = tonumber(sg), seed = ss ~= "" and tonumber(ss, 16) or nil} end
    local fg, fb, fx, fy, flt, frt = line:match("^first%s+(%d+)%s+(%x+)%s+(%-?%d+)%s+(%-?%d+)%s+(%d+)%s+(%d+)")
    if fg then firsts[tonumber(fg)] = {tonumber(fb, 16), tonumber(fx), tonumber(fy), tonumber(flt), tonumber(frt)} end
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
      if name == "rng_seed" then rngw = w end
    end
  end
  assert(counter and modew, "watch list needs game_frame and game_mode rows")
  for slot, g in string.gmatch(env("SA1_ORACLE_SAVE") or "", "(%d+):(%d+)") do
    saves[#saves + 1] = {slot = tonumber(slot), g = tonumber(g), done = false}
  end
  local r = env("SA1_ORACLE_RESUME")
  if r and r ~= "" then
    local v, o, ns, lm, al, us = r:match("(%-?%d+)%s+(%-?%d+)%s+(%d+)%s+(%-?%d+)%s+(%d)%s+(%-?%d+)")
    assert(v, "SA1_ORACLE_RESUME: expected 6 numbers")
    vblank, offset, next_sync = tonumber(v), tonumber(o), tonumber(ns)
    last_mode = tonumber(lm) >= 0 and tonumber(lm) or nil
    aligned, unaligned_since = al == "1", tonumber(us)
    print(string.format("tape_oracle: resumed at vblank %d, offset %d, next sync %d, mode %s", vblank, offset, next_sync, tostring(last_mode)))
  end
  out = assert(io.open(OUT, "w"))
  if AUDIO_MAP and AUDIO_MAP ~= "" then amap = assert(io.open(AUDIO_MAP, "w")) end
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

-- rand() calls from seed a to seed b (the game's LCG, 0x8C10D334), or nil
local function rng_steps(a, b)
  for n = 0, 100000 do
    if a == b then return n end
    a = (a * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
  end
  return nil
end

local function rng_resync(seed, g)
  local cur = mem.read32(rngw.addr)
  if cur == seed then return end
  local ahead, behind = rng_steps(seed, cur), rng_steps(cur, seed)
  local gap = ahead and string.format("%d calls ahead", ahead) or behind and string.format("%d calls behind", behind) or "unrelated"
  print(string.format("tape_oracle: rng resync at game frame %d: console seed %08x (%s), set to %08x", g, cur, gap, seed))
  mem.write32(rngw.addr, seed)
end

local function finish(why)
  if done then return end
  done = true
  out:close()
  if amap then amap:close() end
  print("tape_oracle: " .. why .. " after " .. vblank .. " vblanks")
  flycast.emulator.exit()
end

local function on_vblank()
  if done or not loaded then return end
  vblank = vblank + 1
  if POKE_EC and not poked and mem.read16(0x8c7493ba) == 15 then
    mem.write16(0x8c7493ba, 1); mem.write16(0x8c7493bc, 0); mem.write16(0x8c7493be, 1); mem.write16(0x8c7493c0, 0)
    poked = true
    print("tape_oracle: Chaos 0 -> Emerald Coast act 1 at vblank " .. vblank)
  end
  local g = mem.read32(counter.addr)
  local mode = tonumber(value(modew))
  if mode ~= last_mode then
    last_mode = mode
    local found = nil
    for k = next_sync, math.min(#syncs, next_sync + LOOKAHEAD) do
      if syncs[k].mode == mode then found = k; break end
    end
    if found then
      local at = (prev_g and g > prev_g + 1) and prev_g + 1 or g
      offset, aligned, next_sync = at - syncs[found].g, true, found + 1
      print(string.format("tape_oracle: vblank %d, game mode %d at game frame %d = desktop %d", vblank, mode, at, syncs[found].g))
      if rngw and syncs[found].seed then rng_resync(syncs[found].seed, syncs[found].g) end
    else
      aligned = next_sync > 1 and syncs[next_sync - 1].mode == mode
      unaligned_since = vblank
    end
  end
  prev_g = g
  local ag = aligned and g - offset or -1
  last_ag = ag
  if g == cur_g then phase = phase + 1 else cur_g, phase = g, 0 end
  local p = NEUTRAL
  if aligned then
    if phase == 0 then p = pads[ag] or NEUTRAL
    else p = firsts[ag + 1] or pads[ag + 1] or pads[ag] or NEUTRAL end
  end
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
  if amap then amap:write(string.format("%d %d %d %d\n", vblank, g, ag, samples_now())) end
  if vblank % 600 == 0 then out:flush(); if amap then amap:flush() end end
  if not save_pending then
    for _, s in ipairs(saves) do
      if not s.done and ag >= s.g then s.done = true; save_pending = s; break end
    end
  end
  if ag > last_g then finish("tape ended (game frame " .. g .. ")")
  elseif STOP >= 0 and ag >= STOP then finish("reached desktop game frame " .. ag)
  elseif MAX > 0 and vblank >= MAX then finish("vblank limit") end
end

-- The UI thread: a pending state save runs here (flycast.emulator.saveState
-- pauses the emulator, which the vblank callback cannot do to itself).
local function on_overlay()
  if not loaded then
    flycast.emulator.loadState(LOAD_SLOT)
    loaded = true
    print(string.format("tape_oracle: loaded slot %d: game frame %d, mode %d", LOAD_SLOT, mem.read32(counter.addr), tonumber(value(modew))))
    return
  end
  local s = save_pending
  if not s or done then return end
  save_pending = nil
  local g = mem.read32(counter.addr)
  local ag = aligned and g - offset or -1
  flycast.emulator.saveState(s.slot)
  out:flush()
  local line = string.format("%d %d %d %d %d %d %d %d %d %d", s.slot, s.g, vblank, g, ag, offset, next_sync,
                             last_mode or -1, aligned and 1 or 0, unaligned_since)
  print("tape_oracle: saved slot " .. s.slot .. " (" .. line .. ")")
  if STATE_OUT then local f = io.open(STATE_OUT, "a"); if f then f:write(line, "\n"); f:close() end end
end

local ok, err = pcall(load)
if not ok then
  print("tape_oracle: " .. tostring(err))
else
  flycast_callbacks = {
    vblank = on_vblank,
    overlay = on_overlay,
    terminate = function() if not done then done = true; out:close(); if amap then amap:close() end end end,
  }
end
