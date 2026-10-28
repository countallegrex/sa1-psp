PSP gate queue runner (sa1-psp-i gen 20, 2026-10-28). Start ONLY after the orchestrator says "play-test over".

From Git Bash, PSP on PSPLink (started from the XMB), USB cable in:
  cd "<project-dir>/private/gates"
  python run_gates.py --plan     # jobs, runs, time left (no PSP)
  python run_gates.py            # everything not done, in order; results.md after every job
  python run_gates.py --only qe  # one job
  python run_gates.py --table    # rewrite results.md from state.json
Run it in the background; it takes ~3 h (desk guess 60 s per ABBA run, 75 s per picture run; the measured
times replace the guesses in --plan as runs finish).

Order (priority; all PRX built, nothing to build before the start except the hot job's own list):
  cardlog  private\qb\replay_log.sh: the maintainer's play-test card log, read-only copy + summary (m6b)
  b        mixer (b) qb/b_base vs qb/b_qb4 (2a47191), Casino 2 + Chaos 0, GO -1.0 L Casino 2        12 runs
  walker   d2/repin/b_rmain vs b_rwalkp vs b_rwalkp --math-exact 8c040784, pictures,
           Chaos 0 / Casino 2 / EC1, GO -1.0 L Chaos 0 (+ one more place)                         33 runs
  div      d3/psp/b_div vs itself with --math-exact 8c10ca5c,8c10cb10: WV2 (GO any gain),
           Sky Deck 3 null control, bonus Ice Cap 1/3, Sky Deck 2                                  30 runs
  qe       b_rmain vs d2/repin/b_rqe3 (qe 2ea59dd on f95ac27, self-pinned), pictures, Chaos 0 / SD3  18 runs
  l1       b_rmain vs d2/repin/b_rl1 (lever 1 6d1217d, self-pinned), pictures, 4 places              30 runs
  repin    d2/b_psp_main (old pins) vs b_rmain (layout only, frees 426 KB), pictures, ss/ec/Chaos 0  24 runs
  hot      profile b_rmain on ss + ec (--prof 200:700), build a new list (desk, in d2\wt_fix; the tree's
           list is restored), then b_rmain vs b_hot, pictures, 4 places                              32 runs

Safety:
  - Each finished run is saved in state.json at once.  A run that ends without the exit line is pinged;
    PSPLink answering = retried once; no answer = "WEDGED" in state.json and results.md, exit 5.
    Then: power-cycle the PSP (hold POWER), start PSPLink from the XMB, run the same command: it resumes.
  - The PSPLink host is never stopped or killed (psp_run.py --stop is never called).
  - A file STOP here stops it before the next run (delete it to go on).
  - Silent desktop rules do not apply (no desktop runs); the PSP plays its own sound as in every gate.

Reading results.md: per place, T medians with the 3 runs, dL = 30/29 dT vs arm 0 (ms per logic turn),
"disjoint" (all runs of one arm below all of the other), bar and verdict; picture md5 compare per job;
runs with rc != 0, 'no memory' or 'decode failed' lines are listed under the job.
Self-pinned pairs (qe, l1) differ in cache placement too: a delta inside ~1.5 ms there is layout noise.

Dry-tested against a fake PSP (scratchpad): ABBA order, a wedge at run 10 + resume (33/33, the wedged run
redone), STOP, the table, and the hot job's real desk steps (list from old o1 profiles, unpinned build,
--pin, pinned build byte-identical).  Test hooks: SA1_GATES_REPO / SA1_GATES_PING / SA1_GATES_OUT.

Builds behind the arms (desk-verified):
  b_rmain  f95ac27 re-pinned (repin.sh).          b_rwalkp walker a686bed on main's fresh pins (+112 KB pad).
  b_rqe3   qe 2ea59dd: 4 tapes byte-identical vs f95ac27, local CI PASS; on main's pins it padded +993 KB.
  b_rl1    lever 1 6d1217d: 4 tapes + 44 places identical, CI PASS.   b_div  6039866, see d3\REPORT.md.
