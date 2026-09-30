[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ServerRoot,
    [Parameter(Mandatory = $true)][string]$SourceFixture,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string]$Cxx = 'C:/msys64/ucrt64/bin/g++.exe',
    [switch]$V2
)
$ErrorActionPreference = 'Stop'
$serverCommit = (& git -C $ServerRoot rev-parse HEAD).Trim()
$expectedCommit = if ($V2) { '724d4c67858b0a3595a45bfbb1bbe47758ea2467' } else { '205c510c9a2aa3d9b37011c4cca0986ebbb65a6c' }
if ($LASTEXITCODE -ne 0 -or $serverCommit -ne $expectedCommit) {
    throw 'Transport fixtures require the documented VibePollo server revision'
}
& git -C $ServerRoot diff --quiet HEAD -- src third-party/nanors
if ($LASTEXITCODE -ne 0) { throw 'Production server sources or nanors are modified; use a clean checkout of the pinned revision' }
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$SourceFixture = (Resolve-Path -LiteralPath $SourceFixture).Path
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
$compilerBin = Split-Path -Parent (Get-Command $Cxx -ErrorAction Stop).Source
$previousPath = $env:PATH
try {
    $env:PATH = $compilerBin + ';' + $previousPath
    $rsObject = Join-Path $BuildDirectory 'rswrapper.o'
    & (Join-Path $compilerBin 'gcc.exe') -std=c11 -O2 `
        -I (Join-Path $ServerRoot 'third-party/nanors/deps/obl') `
        -c (Join-Path $ServerRoot 'src/rswrapper.c') -o $rsObject
    if ($LASTEXITCODE -ne 0) { throw 'Reed-Solomon compilation failed' }
    $outputExe = Join-Path $BuildDirectory 'generate-pyrowave-transport.exe'
    $generator = if ($V2) { 'generate-v2.cpp' } else { 'generate.cpp' }
    & $Cxx -std=c++23 -O2 -pthread -I $ServerRoot `
        (Join-Path $PSScriptRoot $generator) `
        (Join-Path $ServerRoot 'src/pyrowave_transport.cpp') `
        (Join-Path $ServerRoot 'src/pyrowave_protocol.cpp') `
        (Join-Path $ServerRoot 'src/stream_protocol.cpp') `
        (Join-Path $ServerRoot 'src/crypto.cpp') $rsObject -lcrypto -lws2_32 -o $outputExe
    if ($LASTEXITCODE -ne 0) { throw 'Production transport fixture generator compilation failed' }
    & $outputExe $SourceFixture $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { throw 'Transport fixture generation failed' }
    $hashes = [ordered]@{}
    foreach ($file in Get-ChildItem -LiteralPath $PSScriptRoot -File |
        Where-Object { $_.Extension -in '.pwvf','.pwrt','.pwpf' -and $_.Name.StartsWith('v2-') -eq [bool]$V2 }) {
        $hashes[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $manifest = if ($V2) { 'manifest-v2.json' } else { 'manifest.json' }
    [ordered]@{server_commit=$serverCommit; sha256=$hashes} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot $manifest) -Encoding utf8
} finally {
    $env:PATH = $previousPath
}
