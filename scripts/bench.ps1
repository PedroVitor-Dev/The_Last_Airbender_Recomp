# Benchmark fixo: 7000 quadros (abertura, menus, diálogos e gameplay) no modo headless.
# Uso: .\scripts\bench.ps1 [-BuildDir build\release] [-Interp] [-Repeat 3]
param(
    [string]$BuildDir = 'build\release',
    [switch]$Interp,
    [int]$Repeat = 3
)

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$BuildDir\tlagba.exe"
$rom = Get-ChildItem (Join-Path $root 'roms') -Include *.gba, *.GBA -File -Recurse | Select-Object -First 1
if (-not $rom) { throw 'ROM não encontrada em roms\' }

$p = @('--press', 'START@900-905', '--press', 'START@1300-1305', '--press', 'START@1700-1705',
       '--press', 'A@2000-2005', '--press', 'START@2500-2505')
for ($f = 2700; $f -le 6000; $f += 90) { $p += '--press'; $p += "A@$f-$($f + 4)" }
$p += '--press', 'RIGHT@6100-6400', '--press', 'DOWN@6400-6600', '--press', 'LEFT@6600-6900'
if ($Interp) { $p += '--interp' }

$best = 0
for ($i = 1; $i -le $Repeat; $i++) {
    $out = & $exe $rom.FullName --headless --frames 7000 --profile @p 2>$null
    $line = ($out | Select-String 'quadros em').Line
    $fps = [double]([regex]::Match($line, '\(([\d.]+) fps').Groups[1].Value)
    if ($fps -gt $best) { $best = $fps; $bestOut = $out }
}
$bestOut | Select-String 'quadros em|Perfil|PPU:|linhas:'
Write-Host ("Melhor de {0}: {1:N0} fps ({2:N1}x tempo real)" -f $Repeat, $best, ($best / 59.7275))
