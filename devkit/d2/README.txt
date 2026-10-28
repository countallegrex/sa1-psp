D2 static check (orders v77 item 1), sa1-psp-i gen 19, 2026-10-28

Question: fn_8c040784 (motion model walker), static-TRS path (node flag 0x40): its njRotateXYZ / ZXY angles come from @r15, @(4,r15), @(8,r15). Are those the live pr/r8/r9 save slots?

Static answer: YES (raw bytes in 1ST_READ.BIN).
  0c040798 4f22 sts.l pr,@-r15 is the last push; @r15 = pr, +4 = r8, +8 = r9.
  0c0407d0 65f2 / 0c0407d2 56f1 / 0c0407d4 57f2 load r5/r6/r7 from them.
  On the 0x40 path there is no add #-n,r15 and no store to those slots (the callees njPushMatrix and njTranslate write below r15).
  The sibling loop and the child recursion keep r15 balanced.
  Flags in this walker:
    0x40 set   = static: njTranslate(node+8), rotate(stack slots), njScale(node+32).
    0x40 clear = motion callback through *0x8c88fe24.
    0x20       = ZXY order (8c639ff8 instead of 8c639cf4).
    0x08       = hide the model.
  The intended angles at node+20..28 are never read: the game's own bug.
Console values:
  child nodes: ax = 0x8c040812, ay = 0x8c639cf4, az = 0x8c639c70.
  top level from fn_8c040942: ax = 0x8c040966, ay/az = the caller's r8/r9.
Ours: the emitter pushes pr = 0, so ax = 0 and the X rotation is skipped. ay/az match.
The sibling walker fn_8c040880 has no stack-slot reads.

Census: does any 0x40 node reach the rotate?
  Method: an instrumented desktop build (private\d2\out = a copy of recomp/out with a hook in game_005.c; build private\d2\bd).
  Positive control: walker entries and motion-path nodes are counted too.
  4 tapes (sonic_ec1 through Chaos 0, sonic_to_ec1_start, title_menu, ec_scripted), headless:
    0 static nodes; 5.63 M walker entries, 10.76 M motion nodes.
  44 survey places (o1 pokes, 1200 frames each, places.py -> places2.out):
    0 static nodes everywhere.
    Every place runs the walker; Chaos 0: 275,716 entries, 487,901 motion nodes.

Verdict: the divergence is real but unreached in every covered place, so it has no visible effect today.
  Flycast check: moot, there is no node to check.
  Emitter fix (real return address at call sites): not needed now.
  If ever needed, the narrow form: c->pr = ret at this walker's 3 call sites and pr = c->pr at its entry only (c->pr exists in sh4_ctx).
  Native walker: delegate any 0x40 node to the emitted body, or log it once, so the walker stays exact either way.

Step (2), the emitter guard/spill study (v77): private\d2\emitter-study.md (Opus subagent).
Lever 1 = push/pop-matched preserved r8-r14, in recomp/emitter/sh4emit/analysis.py slot_preserved().
  Branch d2-preserved, commit 9fdbddf (worktree private\d2\wt, on 2a47191). Local CI PASS. Emitter tests 56/56.
  Emitted code: r8-r14 reloads 26672 -> 19052.
  Exact:
    4 tapes are byte-identical (l1_check.sh).
    44 places, 3000 frames --no-pad watch traces: identical 44/44 (places_cmp.py).
      FE1 differed only when 8 runs shared the CPU; solo repeat runs match.
    The 58 overlays were re-emitted (emit_ovl.py); the base re-emit matches recomp/out exactly.
  HLE wraps are safe: collision.c pops r11-r14 from its own slots; set_scan restores its entry values.
  Desktop headless timing, ABBA x3 medians (tm/times.txt):
    Chaos 0 -0.4 %, Casino 2 -0.4 %, SD3 -0.7 %, EC1 -1.6 %.
  PSP PRX ready for the gate: b_psp_b (base, same source) and b_psp_l1.
    Function code is 47 KB smaller.
    .text is 0xC4000 larger: hot-order pin padding (regenerate tools/psp_hot_order.txt before a final gate).
Lever 2 (closed-world sa1_call) is unsound: 2419 of 4318 functions are not provably r8-r14-safe (emit_cw, SA1_CLOSED_WORLD=1 in the l1 copy).
Callback histogram (all places + tapes):
  *0x8c88fe24 = fn_8c040612 96 %, fn_8c0406a0 4 %.
  *0x8c890074 = NULL 99.2 %.
  fn_8c040612 dispatches again through *0x8c88fe2c, a chain, so one-site devirtualisation doesn't help.
Verdict: go to (3), the native walker. Spec: walker-spec.md.

Step (3), the native walker: walk\REPORT.md.
  d2-walker a686bed (rebased onto f95ac27), local CI PASS, exact on the desktop. Not landed: waiting for the PSP gate.
  PSP pair: b_psp_main vs b_psp_walk.
Side fixes LANDED:
  47dbd09 njRotateZXY body (was a stub; MR2).
  2f87a35 njRotateXYZ whole-register skip.
  f95ac27 ci_local from worktrees.

Gen 20 (2026-10-28): lever 1 REBASED onto f95ac27 = 6d1217d (d2-preserved; recomp/ tree hash unchanged, so out_l1/ovl emission is the same).
  Desktop bd_l1m vs bd_fix (f95ac27): 4 tapes byte-identical (l1m_tapes.out), 44/44 places identical at 3000 frames (l1m_places.out).
  Local CI PASS 6d1217d. PSP PRX: l1m/b_l1m (current pins, +845 KB padding) and repin/b_rl1 (re-pinned). See repin/README.txt.
