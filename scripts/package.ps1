# Monta o pacote de distribuição (zip da release do GitHub).
# Uso: .\scripts\package.ps1 [-SkipBuild]
# Requer o código recompilado em generated\recomp (passo "tlarecomp emit" do README).
# Resultado: dist\AvatarRecomp-v<versão>-Windows-x64.zip (a pasta dist\ fica fora do Git). O nome traz
# o sistema e o processador, como launchers (RecompLauncher) esperam para escolher o arquivo.
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\([^)]*VERSION\s+([0-9.]+)') { throw 'Versão não encontrada no CMakeLists.txt' }
$version = $Matches[1]

if (-not (Get-ChildItem (Join-Path $root 'generated\recomp') -Filter *.cpp -ErrorAction SilentlyContinue)) {
    throw 'Sem código recompilado em generated\recomp: rode "tlarecomp emit" antes (ver README).'
}
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') }

$name = "AvatarRecomp-v$version-Windows-x64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'roms') | Out-Null

Copy-Item (Join-Path $root 'build\release\AvatarRecomp.exe') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'recomplauncher.json') $stage   # RecompLauncher: ao lado do .exe
Copy-Item (Join-Path $root 'packaging\README.txt') $stage
Copy-Item (Join-Path $root 'packaging\roms_README.txt') (Join-Path $stage 'roms\PUT YOUR ROM HERE.txt')
(Get-Content (Join-Path $stage 'README.txt') -Raw).Replace('{VERSION}', $version) |
    Set-Content (Join-Path $stage 'README.txt') -NoNewline -Encoding UTF8

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# Entrada por entrada, com os caminhos separados por '/' como o formato zip pede: no PowerShell
# 5.1, o Compress-Archive e o ZipFile.CreateFromDirectory gravam a barra invertida, que outras
# ferramentas e sistemas desempacotam errado.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File) {
        $entry = $name + '/' + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry,
            [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content (Join-Path $dist "$name.zip.sha256") -Encoding ASCII
Write-Host "Pacote: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Write-Host "SHA-256: $hash"
