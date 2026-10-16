# Recompiler configuration: Sonic Adventure US v1.005

What belongs here (committed): the title configuration for the recompiler,
the seam table (`seam.md` / `seam.toml`: address → name → `native` |
`recompile`), seed addresses for functions static discovery misses, and
notes on FPU/timing quirks.

What never belongs here: the executable, any bytes copied from it, or the
generated code. Generated code goes to `../out/` (git-ignored).

Addresses are facts about the binary and are fine to commit, in the same way
N64 recompilation projects commit their symbol files.

Identity of the supported dump (fill in at M0):

| File | Size | SHA-256 |
|---|---|---|
| `1ST_READ.BIN` (descrambled) | 6,735,488 | _todo_ |
