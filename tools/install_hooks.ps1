# Installs the pre-commit guard that refuses game-derived files.
$repo = Split-Path -Parent $PSScriptRoot
$hooks = Join-Path $repo ".git\hooks"
if (-not (Test-Path $hooks)) { throw "not a git repository: $repo" }
$hook = @'
#!/bin/sh
# sa1-psp: refuse to commit game data or game-derived code
exec python "$(git rev-parse --show-toplevel)/tools/check_clean.py"
'@
Set-Content -Path (Join-Path $hooks "pre-commit") -Value $hook -Encoding ascii -NoNewline
Write-Host "pre-commit hook installed in $hooks"
