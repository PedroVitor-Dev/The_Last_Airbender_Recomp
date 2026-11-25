# Teste de mutação: confere se o fuzzer de instruções (test_synth) detecta bugs conhecidos.
# Cada mutação precisa gerar pelo menos uma divergência. Restaura o código no fim.
# Uso: .\scripts\mutation_test.ps1 [-BuildDir build\dev]   (requer scripts\test_synth.ps1 antes)
param([string]$BuildDir = 'build\dev')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mutate = Join-Path $root 'tools\mutate.py'
$synth = Join-Path $root "$BuildDir\test_synth.exe"
$rom = Join-Path $root 'generated\synth.gba'
$failed = @()
try {
    foreach ($m in 'M1', 'M2', 'M3') {
        python -I $mutate $root $m
        & (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir | Out-Null
        $out = & $synth $rom --runs 20 --report (Join-Path $root "build\mutation_$m.txt") 2>$null
        $line = $out | Select-String 'DIVERGENTES\s*:\s*(\d+)'
        $n = [int]$line.Matches[0].Groups[1].Value
        if ($n -gt 0) { Write-Host "  $m detectada ($n divergências)" } else { Write-Host "  $m NÃO detectada"; $failed += $m }
    }
} finally {
    python -I $mutate $root revert | Out-Null
    & (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir | Out-Null
}
if ($failed.Count) { Write-Host "Mutações não detectadas: $($failed -join ', ')"; exit 1 }
Write-Host 'Todas as mutações foram detectadas.'
