# D3 native SHC divide helpers (0x8C10CA5C signed, 0x8C10CB10 unsigned)

Verdict: done, bit-exact on the desk. Commit 6039866 on div-helpers (worktree private\d3\wt, on d2-walker a686bed). Not pushed. Lands after the walker's PSP gate. Written by an Opus subagent; saved here by the session (sa1-psp-i gen 20).

## What was built
- hle/game/sh_div_inl.h: the two natives (static inline). One host divide; the signed one works on magnitudes in uint32 (INT_MIN / -1 = 0x80000000, no trap). t, q, m from the closed forms of div-helpers-spec.md 1.1.
  - Memory as the emitted bodies: r2 pushed below r15 (r3 too in the signed one, after the zero test); d = 0: WR32 0x44E to 0x8C7AC108.
  - Registers as the emitted bodies per path. d = 0: r1 = address (signed) / 0x44E (unsigned), t = 1, r3/q/m not written. pr never written.
- hle/game/sh_div.c: wrappers, mode, check mode, call counter.
  - Native in every mode incl. --strict (hle_math_mode_exact_ok). Kill switch: --math-exact 8c10ca5c,8c10cb10. Desktop: SA1_DIV_MODE=exact|native|check.
  - Check mode (--math check or SA1_DIV_MODE=check): saves sh4_ctx + 8 stack bytes + error word, runs _orig, restores, runs native, compares, keeps _orig's result.
  - Counter: on in check mode, or SA1_DIV_COUNT=1; calls per frame and per caller (host return address -> guest function via sa1_fn_table + loaded overlays). Report via hle_math_report.
- Wiring: hle_wraps.tsv, both CMake foreach lists, a comment in settings.c.
- Re-emit (out_div): diff vs d2\out_walk = only the two _orig renames in game_030.c, two prototypes in game.h, report.txt (57 wrapped, was 55). Overlays copied from out_walk; 229 overlay call sites call fn_8c10ca5c by symbol and get the native.

## Oracle (private\d3\oracle, before any game run)
Sliced _orig bodies (-O1, as the game) vs shipped sh_div_inl.h (-O3). Every ctx word random incl. entry flags; random 4-aligned P1 r15; stack bytes and error word random; compare whole ctx + both stack words + error word.

| Set | Cases per routine | Differ |
|---|---|---|
| divsim.py set (411,342 pairs) | 411,342 | 0 |
| n, d in [-200, 200] + edge pairs | 204,901 | 0 |
| d = 0 / -1 / INT_MIN x random n, r15 next to the error word | 402,822,064 | 0 |
| +-(2^k + {-1,0,1}) x 1 M n | 199,229,440 | 0 |
| d in [-4096, 4096] x 65,536 stratified n | 536,936,448 | 0 |
| 10^9 random pairs | 1,000,000,000 | 0 |
| exhaustive 16-bit x 16-bit | 4,294,967,296 | 0 |
| Total | 6,434,571,491 | 0 |

Positive control: a broken native (one q flip, one r3 flip) is caught. ~6 minutes.

## Game runs (silent, hidden, <= 4 at a time; base d2\walk\bd = a686bed + out_walk)
- Tapes: 4/4 byte-identical in native, exact, check. Check: 2,622,309 signed + 15,373 unsigned calls, 0 differing.
- 44 places, 3000 frames: 44/44 identical (Eggviper's existing trap in both). Check: 2,628,337 signed + 55,915 unsigned, 0 differing.
- --dump WV2 (2:1:4, --no-pad, --strict both arms) f1000/3000/6000: byte-identical native vs kill switch.
  Without --strict one arm differs from itself in timer words 0x8C78C600..05, 0x8C88FE1C/20 (host time). Not a bug.
- Call counter (6000 frames):
  - WV2: 291 calls/frame (max 297); fn_8c05fed0 97.9 % (285/frame), then fn_8c05fdc0, fn_8c08a724, fn_8c089f20, STG02 fn_8c900128. Spec guessed ~700 (different spot).
  - SD3: ~6 calls in 1,425 of 5,307 frames (fn_8c089f20 / fn_8c08a724). Null control.
  - Other heavy places (3000 f, signed): ice1 433 K, ice3 530 K, sd2 430 K; most others 7-11 K.

## Desktop timing (headless, 6000 frames, ABBA x3 medians, same exe)
| Place | Native | Kill switch | Delta |
|---|---|---|---|
| WV2 | 1.679 s | 1.764 s | -4.85 % |
| SD3 | 1.381 s | 1.381 s | -0.04 % |

## CI and PSP
- Local CI 6039866: PASS.
- PSP PRX private\d3\psp\b_div (built, not run). .text +4,920 B vs d2\b_psp_walk; neither routine pinned in psp_hot_order.txt.
- Gate: WV2 L ABBA x3 vs the same PRX with --math-exact 8c10ca5c,8c10cb10; SD3 null control; also worth ice1/ice3/sd2 (Ice Cap, Speed Highway 2) given their call counts. GO any measured gain (exact).

## Left behind
private\d3: out_div, bd_div, psp\b_div, oracle\ (orig_div.c is game-derived: keep private), tp_*, pcmp, pchk, count, tm, dumps (6 x 18 MB), scripts and logs. WSL /root/sa1-build/g_div, /root/sa1-ci/603986693c5d; %TEMP%\sa1-ci-603986693c-*.
