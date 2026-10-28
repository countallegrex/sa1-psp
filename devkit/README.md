# devkit: working tools from development

These are the tools and notes used during development, published as they were with
paths made portable. They are rougher than `tools/`. Nothing here contains game
data, recompiled code, disassembly, dumps, savestates, audio or images.

**Layout.** The scripts expect a work folder `private/` next to the checkout, and
they find both through the environment:

```
SA1_ROOT/             (set SA1_ROOT; Git Bash form for .sh, Windows form for .py)
  sa1-psp/            this repository
  private/            your work folder: the dump, builds, logs (never committed)
    dump/sa1_us_1005/data/   your extracted disc
    d2/ d3/ gates/ ...       copy the folders of devkit/ here to use them as-is
```

`SA1_ROOT_WSL` (the same folder as WSL sees it, e.g. `/mnt/c/...`) is needed by
the gate runner's hot job only. Build folders named in the scripts (`b_*`, `bd*`,
`out*`, `emit_*`) are not included; rebuild them from the branch named next to
each one.

Commit hashes in these notes were mapped to this repository where the commit is in it
(others name branches that were not published). The milestone reports
under `docs/` cite hashes from before the history was rewritten for publication;
find those commits by their subject line instead.

## Tools, two lines each

| Path | What |
|---|---|
| `gates/run_gates.py`, `gates/README.txt` | PSP gate queue: ABBA timing batches and picture checks of base vs candidate builds over PSPLink, unattended and resumable after a wedge.<br>Writes `state.json` and `results.md` next to itself; `--plan` shows the jobs without a PSP. |
| `gates/repin.sh`, `gates/withlist.sh` | Desk helpers for the gates: re-pin a build's hot-code order, and build a tree's PRX against a given hot list.<br>Both build in WSL through `tools/psp_lever_build.sh`. |
| `qb/gate.py` | The single-pair gate the queue grew from: pictures (md5) plus logic-time ABBA x3 for one base/candidate pair.<br>`qb/replay_log.sh` copies the play-test log off the Memory Stick and summarises it. |
| `livefix/tools/*.py` | `psplink_ping.py` (is PSPLink alive), `card_get.py` / `card_cp.py` / `card_rm.py` (Memory Stick file copy over PSPLink, with read-back).<br>None of them resets PSPLink or stops the host. |
| `speed/add_step.py`, `history.json`, `levers.json` | The speed tracker: one row per landed speed step (ss and ec ms per frame) and the list of levers with estimates and measurements.<br>`add_step.py` appends a step; its CI and branch checks are optional (`--ci-dir`, `--require-main`). |
| `speed/index.html` | Chart of ss and ec ms per step against the 33.3 ms line, plus the lever and step tables.<br>Serve the folder: `cd devkit/speed && python -m http.server 8000`, open http://localhost:8000/ (a `file://` page cannot load the JSON). |
| `s3/survey.py` | Stage survey over the 44 places of Sonic's story (`--stage LEVEL:ACT:MODE`): idle timing, a profile run and a run-forward run per place.<br>Its `PLACES` list is imported by the d2/d3 place scripts. |
| `s3/fskip.py`, `o2/fskip2.py` | Frame-skip checks on the PSP: game speed and pictures per second with `--frame-skip auto`.<br>The second one takes the build and places on the command line. |
| `surveys/*.md` | Results: stage survey, frame-skip survey, Emerald Coast and native-path profiles, model-cost census.<br>The numbers behind the README's status section. |
| `profile/cov.py`, `profile/profile-coverage-2026-10-28.md` | Profile coverage: how much of each slow place's time the top functions hold.<br>Reads profile listings (`SA1_COV_DIR` for the ss/ec ones). |
| `d2/` | Harness of the d2 levers (preserved registers, native motion walker): tape and 44-place equality checks, size compares, the census.<br>`walker-spec.md`, `emitter-study.md` and `README.txt` record the design and results; `repin/` has the re-pinned hot list for main. |
| `d3/` | Harness of the native divide helpers: call counters, RAM-dump compares, tapes in native/exact/check modes, timing.<br>`cg.py` / `hot.py` build a call graph from emitted code and rank hot callers; `REPORT.md` and the spec record the result. |
| `tx2/chain_*.sh`, `tx2/chainrec/` | Chain test: a poked level chain SS -> EC1 -> SS -> EC2 -> SS -> EC3 -> SS, on the desktop or the PSP, for texture-memory soak tests.<br>Recipe: poke the next level/act at 0x8C7493C2/C4 and the exit request 0x8C19DE24 = 3; the game loads the level itself. |
| `tx2/capacity-notes.md` | Notes on the card-vs-PSPLink memory gap and the ARK-4 high-memory lead.<br>Unfinished when the project paused. |
| `tapes/*.tape` | Extra recorded input tapes (text): a human run to the Emerald Coast start and experimental splices or quantised variants.<br>A tape replays only with a build matching the one it was recorded on (same game version, same timing rules); the maintained tapes are in `tests/replay/`. |
