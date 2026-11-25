# Fuzzer de instruções do emissor: gera uma ROM sintética, recompila e compara
# interpretador × recompilado em cada sequência.
# Uso: .\scripts\test_synth.ps1 [-Functions 4000] [-Runs 20] [-Seed 1] [-BuildDir build\dev]
param(
    [int]$Functions = 4000,
    [int]$Runs = 20,
    [int]$Seed = 1,
    [string]$BuildDir = 'build\dev'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'generated\synth'
$rom = Join-Path $root 'generated\synth.gba'
$cfg = Join-Path $root 'generated\synth.cfg'
$tlarecomp = Join-Path $root "$BuildDir\tlarecomp.exe"

& (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir | Out-Null
& $tlarecomp synth $rom $cfg $Functions $Seed
if ($LASTEXITCODE -ne 0) { throw 'synth falhou' }
& $tlarecomp analyze $rom --any-rom --no-scan --config $cfg -o (Join-Path $root 'generated\synth_analysis') --emit $out |
    Select-String 'Funções  |gerado'
if ($LASTEXITCODE -ne 0) { throw 'emit falhou' }
& (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir | Out-Null
& (Join-Path $root "$BuildDir\test_synth.exe") $rom --runs $Runs --seed $Seed --report (Join-Path $root 'build\synth_report.txt')
exit $LASTEXITCODE
