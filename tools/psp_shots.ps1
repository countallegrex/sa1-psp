# Console screenshots of one build, for render-only levers: Station Square
# frames 200/400/600 and title_menu.tape frames 1500/2500.
#
#   powershell -File tools/psp_shots.ps1 -Dir BUILD -Into FOLDER [-Stop FILE] [-Against FOLDER]
#
# -Against compares each shot with the same file in another folder (by hash).
# -Stop FILE is checked before each console run; the PSPLink host is never
# stopped.
#
# Part of sa1-psp (MIT).
param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Into,
    [string]$Stop = '',
    [string]$Against = ''
)
Set-Location (Join-Path $PSScriptRoot '..')
$Run = 'build-psp\sa1psp\psplink-run'
New-Item -ItemType Directory -Force $Into | Out-Null
if ($Stop -and (Test-Path $Stop)) { 'STOP'; exit }
python tools/psp_run.py --from $Dir -- --stage ss --no-pace --frames 610 --screenshot 200:ss200.ppm --screenshot 400:ss400.ppm --screenshot 600:ss600.ppm *> $null
"ss rc=$LASTEXITCODE"
Move-Item -Force "$Run\ss*.ppm" $Into
if ($Stop -and (Test-Path $Stop)) { 'STOP'; exit }
Copy-Item -Force tests\replay\title_menu.tape $Run
python tools/psp_run.py --from $Dir -- --replay title_menu.tape --no-pace --frames 2510 --screenshot 1500:tm1500.ppm --screenshot 2500:tm2500.ppm *> $null
"tm rc=$LASTEXITCODE"
Move-Item -Force "$Run\tm*.ppm" $Into
foreach ($f in 'ss200', 'ss400', 'ss600', 'tm1500', 'tm2500') {
    if (-not (Test-Path "$Into\$f.ppm")) { "$f missing"; continue }
    if ($Against) {
        $same = (Get-FileHash "$Into\$f.ppm").Hash -eq (Get-FileHash "$Against\$f.ppm").Hash
        "$f $(if ($same) { 'same' } else { 'DIFF' })"
    }
}
