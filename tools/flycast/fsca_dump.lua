-- fsca_dump.lua - the oracle's (Flycast 2.7) fsca for every 16-bit angle,
-- by running fsca itself in the guest: no emulator code or data is copied.
--
-- Run by tools/flycast/fsca_dump.py (Flycast with Dynarec.Enabled=no, so the
-- guest code patched here runs at once).  After the boot it writes a small
-- SH-4 routine to 0x8C008100 (the IP.BIN area, unused after the boot) and
-- points njSin (0x8C63A9B8, itself lds r4,FPUL; fsca) at it.  Each time Lua
-- sets the control word to a start angle, the next njSin call runs fsca for
-- 4096 angles from there into the last 32 KB of sound RAM (sin, cos pairs),
-- sets the control word back to -1 and then does njSin's own work.  Lua
-- collects the 16 chunks into SA1_FSCA_OUT: 65536 x (sin, cos) as
-- little-endian float bit patterns (512 KB).
--
--   SA1_FSCA_OUT  output file (required)
--   SA1_ORACLE_GPAD and the other tape_oracle.lua variables: play a tape
--                 (fsca_dump.py --tape) and arm 300 vblanks into the first
--                 gameplay (game mode 5), where the game calls njSin/njCos;
--                 without them it arms at vblank 600 (the title calls neither)
--
-- Part of sa1-psp (MIT).

local OUT = os.getenv("SA1_FSCA_OUT")
local mem = flycast.memory
local CODE, CODE2, CTL, BUF, NJSIN, NJCOS = 0x8C008100, 0x8C008180, 0x8C0080F0, 0xA09F8000, 0x8C63A9B8, 0x8C63A970
local IDLE = 0xFFFFFFFF

-- the routine (hand-assembled; see fsca_dump.py for the listing)
local routine = {
  0xD10D,           -- 00 mov.l @(lit_ctl),r1
  0x6212,           -- 02 mov.l @r1,r2           start angle
  0xE3FF,           -- 04 mov #-1,r3
  0x3320,           -- 06 cmp/eq r2,r3
  0x890F,           -- 08 bt done                idle: njSin only
  0xD50C,           -- 0a mov.l @(lit_buf),r5
  0xE601,           -- 0c mov #1,r6
  0x4618,           -- 0e shll8 r6
  0x4608,           -- 10 shll2 r6
  0x4608,           -- 12 shll2 r6               4096
  0x425A,           -- 14 loop: lds r2,FPUL
  0xF0FD,           -- 16 fsca FPUL,DR0
  0xF50A,           -- 18 fmov.s fr0,@r5         sin
  0x7504,           -- 1a add #4,r5
  0xF51A,           -- 1c fmov.s fr1,@r5         cos
  0x7504,           -- 1e add #4,r5
  0x7201,           -- 20 add #1,r2
  0x4610,           -- 22 dt r6
  0x8BF6,           -- 24 bf loop
  0xE3FF,           -- 26 mov #-1,r3
  0x2132,           -- 28 mov.l r3,@r1           done
  0x5011,           -- 2a done: mov.l @(4,r1),r0  calls, at CTL+4
  0x7001,           -- 2c add #1,r0
  0x1101,           -- 2e mov.l r0,@(4,r1)
  0x644D,           -- 30 extu.w r4,r4           njSin's own body
  0x445A,           -- 32 lds r4,FPUL
  0x000B,           -- 34 rts
  0xF0FD,           -- 36 fsca FPUL,DR0
}
-- the same for njCos (0x8C63A970: ... fsca; rts; fmov fr1,fr0), at CODE2
local routine2 = {}
for i = 1, 24 do routine2[i] = routine[i] end
routine2[1], routine2[6] = 0xD10E, 0xD50D          -- literals at +0x3c, +0x40
for _, h in ipairs({ 0x644D, 0x445A, 0xF0FD, 0x000B, 0xF01C, 0x0009 }) do routine2[#routine2 + 1] = h end
local vblank, chunk, armed, out, done, play = 0, 0, false, nil, false, 0
local inner
if os.getenv("SA1_ORACLE_GPAD") then
  dofile("tape_oracle.lua")
  inner = flycast_callbacks.vblank
end

local function finish(why)
  if done then return end
  done = true
  if out then out:close() end
  print("fsca_dump: " .. why .. " after " .. vblank .. " vblanks, " .. chunk .. " chunks")
  flycast.emulator.exit()
end

local function arm()
  for i, h in ipairs(routine) do mem.write16(CODE + 2 * (i - 1), h) end
  mem.write32(CODE + 0x38, CTL)
  mem.write32(CODE + 0x3C, BUF)
  mem.write32(CTL, IDLE)
  mem.write32(CTL + 4, 0)
  for i, h in ipairs(routine2) do mem.write16(CODE2 + 2 * (i - 1), h) end
  mem.write32(CODE2 + 0x3C, CTL)
  mem.write32(CODE2 + 0x40, BUF)
  -- njCos: mov.l @(1,pc),r0; jmp @r0; nop; nop; .long CODE2 (its own fmov and pad at +8)
  mem.write16(NJCOS, 0xD001); mem.write16(NJCOS + 2, 0x402B)
  mem.write16(NJCOS + 4, 0x0009); mem.write16(NJCOS + 6, 0x0009)
  mem.write32(NJCOS + 8, CODE2)
  -- njSin: mov.l @(1,pc),r0; jmp @r0; nop; nop; .long CODE (over the unreferenced rts; nop at +8)
  mem.write16(NJSIN, 0xD001); mem.write16(NJSIN + 2, 0x402B)
  mem.write16(NJSIN + 4, 0x0009); mem.write16(NJSIN + 6, 0x0009)
  mem.write32(NJSIN + 8, CODE)
  armed = true
  mem.write32(CTL, 0)
  print(string.format("fsca_dump: armed at vblank %d: njSin %04x %04x, routine %04x", vblank, mem.read16(NJSIN), mem.read16(NJSIN + 2), mem.read16(CODE)))
end

local function on_vblank()
  if done then return end
  vblank = vblank + 1
  if inner then inner() end
  if not armed then
    if inner then
      if mem.read16(0x8C760970) == 5 then play = play + 1 else play = 0 end
      if play == 300 then arm() end
    elseif vblank == 600 then arm() end
    return
  end
  if vblank % 600 == 0 then print(string.format("fsca_dump: vblank %d ctl %08x calls %d", vblank, mem.read32(CTL), mem.read32(CTL + 4))) end
  if mem.read32(CTL) ~= IDLE then
    if vblank > 60 * 600 then finish("timeout (njSin not called?)") end
    return
  end
  local words = mem.readTable32(BUF, 8192)
  local parts = {}
  for i = 1, 8192 do parts[i] = string.pack("<I4", words[BUF + (i - 1) * 4]) end   -- keyed by address
  out:write(table.concat(parts))
  chunk = chunk + 1
  if chunk == 16 then finish("done") return end
  mem.write32(CTL, chunk * 4096)
end

if not OUT then error("SA1_FSCA_OUT is not set") end
out = io.open(OUT, "wb")
flycast_callbacks = flycast_callbacks or {}
flycast_callbacks.vblank = on_vblank
