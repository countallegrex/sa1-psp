# One perf lever on the PSP: build, profile, bench against a base build.
#
#   powershell -File tools/psp_lever.ps1 -Step a     -Name N -Base DIR -Out RECOMP_OUT -Work DIR [-Tree DIR] [-Stop FILE] [-Runs 2]
#   powershell -File tools/psp_lever.ps1 -Step b     -Name N -Base DIR -Out RECOMP_OUT -Work DIR [-Tree DIR] [-Stop FILE] [-Runs 2]
#   powershell -File tools/psp_lever.ps1 -Step check -Name N -Work DIR [-Stop FILE]
#
# -Tree is the bench checkout holding origin/main plus the lever's diff
# (default ..\sa1-psp-bench); -Out the recompiler output it builds against;
# -Work receives b_<variant>, bb_<variant>.log, prof_<variant>.* and bench.tsv;
# -Base is the base build folder.
#   a: restores the committed hot-code list in -Tree, builds N_a, profiles it
#      (prof_N_a.txt), benches base vs N_a.
#   b: regenerates the hot-code list in -Tree from prof_N_a, builds N_b,
#      benches base vs N_b.  Only worth it when N_a moved the profile or
#      regressed; the regenerated list is not always the better one, and it
#      is unpinned and from one ss profile (a new layout).  For a full
#      regeneration (ss + ec profiles, warm code, objects, pinned) use
#      tools/psp_hot_regen.sh.
#   check: title_menu.tape with its check on -Build VARIANT (b_VARIANT in
#      -Work), else N_b if built, else N_a.  Run it
#      on the winner only, before committing.
# Benches are psp_bench.py ABBA rounds of --frames 460 --windows 300,450;
# add a round by hand when |delta| < 0.15 ms or the runs look bimodal.
# -Stop FILE is checked before every console run; the PSPLink host is never
# stopped (killing a connected host wedges the PSP until a power-cycle).
#
# Part of sa1-psp (MIT).
param(
    [ValidateSet('a', 'b', 'check')][string]$Step = 'a',
    [Parameter(Mandatory)][string]$Name,
    [string]$Base,
    [string]$Out,
    [Parameter(Mandatory)][string]$Work,
    [string]$Tree = '',
    [string]$Build = '',
    [string]$Stop = '',
    [int]$Runs = 2
)
$ErrorActionPreference = 'Continue'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Tree) { $Tree = Join-Path $Repo '..\sa1-psp-bench' }
Set-Location $Repo
New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$Tree = (Resolve-Path $Tree).Path
$Run = 'build-psp\sa1psp\psplink-run'

function ToWsl([string]$p) {
    $f = [System.IO.Path]::GetFullPath($p)
    '/mnt/' + $f.Substring(0, 1).ToLower() + ($f.Substring(2) -replace '\\', '/')
}
function Stopped { if ($Stop -and (Test-Path $Stop)) { 'STOP'; return $true }; return $false }
function Build([string]$v) {
    wsl -u root sh (ToWsl "$Repo\tools\psp_lever_build.sh") (ToWsl $Tree) $v (ToWsl $Out) (ToWsl $Work) 2>&1 | Select-Object -Last 1
}
function Bench([string]$v) {
    python tools/psp_bench.py --runs $Runs --frames 460 --windows 300,450 --stop-file $(if ($Stop) { $Stop } else { "$Work\no-stop" }) `
        --tsv "$Work\bench.tsv" "base=$Base" "$v=$Work\b_$v" 2>&1 | Select-Object -Last 2
}

$a = "${Name}_a"; $b = "${Name}_b"
switch ($Step) {
    'a' {
        git -C $Tree checkout -- tools/psp_hot_order.txt
        Build $a
        if (-not (Test-Path "$Work\b_$a\sa1psp.prx")) { 'build failed'; exit 1 }
        if (Stopped) { exit }
        python tools/psp_run.py --from "$Work\b_$a" -- --stage ss --no-pace --frames 620 --stats 150 --prof "150:600:prof_$a.bin" *> $null
        Copy-Item "$Run\sa1psp.log" "$Work\prof_$a.log"
        Copy-Item "$Run\prof_$a.bin" "$Work\prof_$a.bin"
        python tools/psp_prof.py --pcs "$Work\prof_$a.bin" --log "$Work\prof_$a.log" --elf "$Work\b_$a\sa1psp_prx.elf" --top 400 --out "$Work\prof_$a.txt" *> $null
        if (Stopped) { exit }
        Bench $a
    }
    'b' {
        python tools/psp_hot_order.py --pcs "$Work\prof_$a.bin" --log "$Work\prof_$a.log" --elf "$Work\b_$a\sa1psp_prx.elf" `
            --out "$Tree\tools\psp_hot_order.txt" 2>&1 | Select-Object -Last 1
        Build $b
        if (-not (Test-Path "$Work\b_$b\sa1psp.prx")) { 'build failed'; exit 1 }
        if (Stopped) { exit }
        Bench $b
    }
    'check' {
        $v = if ($Build) { $Build } elseif (Test-Path "$Work\b_$b\sa1psp.prx") { $b } else { $a }
        if (Stopped) { exit }
        Copy-Item -Force tests\replay\title_menu.tape, tests\replay\title_menu.check $Run
        python tools/psp_run.py --from "$Work\b_$v" -- --replay title_menu.tape --check title_menu.check --no-pace *> $null
        $r = Select-String -Path "$Run\sa1psp.log" -Pattern 'check: (PASS|FAIL)' | Select-Object -Last 1
        "${v}: $(if ($r) { $r.Line.Trim() } else { 'no check line' })"
    }
}
