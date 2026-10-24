# Installs the git hooks: pre-commit refuses game-derived files, pre-push runs
# the local CI gate (tools/ci_local.py) on every commit being pushed.
$repo = Split-Path -Parent $PSScriptRoot
$hooks = Join-Path $repo ".git\hooks"
if (-not (Test-Path $hooks)) { throw "not a git repository: $repo" }
$hook = @'
#!/bin/sh
# sa1-psp: refuse to commit game data or game-derived code
exec python "$(git rev-parse --show-toplevel)/tools/check_clean.py"
'@
Set-Content -Path (Join-Path $hooks "pre-commit") -Value $hook -Encoding ascii -NoNewline
$push = @'
#!/bin/sh
# sa1-psp: run the local CI gate on each pushed commit (tools/ci_local.py).
# SA1_SKIP_CI=1 skips it, for emergencies only.
if [ "${SA1_SKIP_CI:-}" = 1 ]; then
  echo "pre-push: SA1_SKIP_CI=1, local CI skipped" >&2
  exit 0
fi
top=$(git rev-parse --show-toplevel)
while read -r lref lsha rref rsha; do
  case "$lsha" in *[!0]*) ;; *) continue ;; esac
  python "$top/tools/ci_local.py" --commit "$lsha" || {
    echo "pre-push: local CI FAIL for $lsha; push refused" >&2
    exit 1
  }
done
exit 0
'@
Set-Content -Path (Join-Path $hooks "pre-push") -Value ($push -replace "`r`n", "`n") -Encoding ascii -NoNewline
Write-Host "pre-commit and pre-push hooks installed in $hooks"
