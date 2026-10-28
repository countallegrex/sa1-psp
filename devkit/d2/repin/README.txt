Hot-order check for the stacked branches (sa1-psp-i gen 20, 2026-10-28), desk only, no console.

Measured: pin padding in each gate PRX (gaps >= 256 B between .text symbols; scratchpad pad.sh / gaps.sh logic).
  PRX                        .text      functions  padding
  b_psp_main (f95ac27)      12.03 MB   11.42 MB    440 KB  (30 gaps of ~16 KB: the list is already stale on main)
  b_psp_walk (a686bed)      12.11 MB   11.46 MB    470 KB  (+30)
  qb/b_qb4 (e92ad9f)        12.05 MB   11.42 MB    454 KB  (+14)
  qe/b_qe (8c01e30)         12.89 MB   11.45 MB   1269 KB  (+829)
  l1m/b_l1m (6d1217d)       12.84 MB   11.37 MB   1285 KB  (+845)
Each ~16 KB gap is a pin that the shifted code missed by a few bytes, so the linker pads to the next 16 KB.
Padding is RAM the heap and texture pool lose (PSP user RAM is tight: MR station largest free block ~1.9 MB).

Desk re-pin (same function order as tools/psp_hot_order.txt, @ pins dropped, built, --pin to that build, rebuilt;
the pinned rebuild is byte-identical to the unpinned one in all four, as the tool promises):
  repin/b_rmain  (wt_fix f95ac27, out_b)      .text 11.61 MB, padding 17.5 KB  (-426 KB vs b_psp_main)
  repin/b_rl1    (wt 6d1217d, out_l1)         .text 11.56 MB, padding 17.5 KB  (-1.28 MB vs b_l1m)
  repin/b_rwalk  (wt_walk a686bed, out_walk)  .text 11.65 MB
  repin/b_rqe    (qe/wt 8c01e30, recomp/out)  .text 11.64 MB  (qe is still on 2a47191: no same-source re-pinned base yet)
  Lists: repin/hot_<name>.repinned.txt (not committed; trees restored with git checkout).

Lever built against main's FRESH pins (repin/hot_rmain.repinned.txt), the pairing the pin scheme intends
(same cache sets for every pinned entry; only the lever's own size changes pad):
  repin/b_rwalkp  .text 11.76 MB, padding 130 KB in 12 gaps  (+112 KB vs b_rmain): usable.
  repin/b_rl1p    .text 12.54 MB, padding 993 KB in 105 gaps (+975 KB): lever 1 DEFEATS the pin scheme.
    It shrinks functions in almost every chunk, so nearly every pinned start is missed by a few bytes
    and padded by ~16 KB (biggest gaps before sa1_off of stg03 / adv0100 / adv00 chunks).

Meaning for the gates:
  - Walker: gate b_rmain vs b_rwalkp (fair: same pins, +112 KB padding).
  - Lever 1: a pinned pair costs ~1 MB RAM, so gate b_rmain vs b_rl1 (each pinned to its own build:
    UNPINNED comparison, hot cache sets differ; label the result so). The ~1.5 ms layout swing of o1/p9
    means a small lever-1 delta is within layout noise; a regen of the list from profiles of the l1 build
    (psp_hot_regen.sh, console) would be the clean answer.
  - The old pairs (b_psp_main vs b_psp_l1 / b_l1m, qe/b_qe) carry ~830-845 KB extra padding: do not gate on them.
  - qe: rebase onto f95ac27 first, then build it against hot_rmain.repinned.txt and check its padding.
  - (b) qb4: +14 KB on the old pins, fine as is.
  - b_psp_main vs b_rmain is a layout-only change (exact; frees ~426 KB). Worth one ABBA ss/ec bench, pictures byte-identical,
    because a new layout is not always faster (o1/p9 swung ~1.5 ms).
  - A real regen (tools/psp_hot_regen.sh: console profiles of the stacked build) still needs the PSP, after "play-test over".
