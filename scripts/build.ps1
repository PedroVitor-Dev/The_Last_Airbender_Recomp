# Configura o ambiente MSVC x64 e compila com CMake + Ninja.
# Uso: .\scripts\build.ps1 [-Config Debug|Release] [-BuildDir <pasta>]
#   -BuildDir permite compilar em outra pasta enquanto build\release está em uso
#   (ex.: o jogo aberto trava o tlagba.exe).
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio Build Tools com C++ não encontrado.' }

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$build = if ($BuildDir) { Join-Path $root $BuildDir } else { Join-Path $root "build\$($Config.ToLower())" }

cmd /c "`"$vcvars`" >nul 2>nul && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { throw "Build falhou ($LASTEXITCODE)" }
Write-Host "Build OK: $build"
