param(
    [Parameter(Mandatory = $true)][string]$ServerRoot,
    [Parameter(Mandatory = $true)][string]$ClientTestExecutable,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string]$Cxx = 'C:/msys64/ucrt64/bin/g++.exe',
    [switch]$V2
)
$ErrorActionPreference = 'Stop'
$expectedCommit = if ($V2) { '724d4c67858b0a3595a45bfbb1bbe47758ea2467' } else { '205c510c9a2aa3d9b37011c4cca0986ebbb65a6c' }
if ((& git -C $ServerRoot rev-parse HEAD).Trim() -ne $expectedCommit) {
    throw 'Use the pinned server commit for the requested protocol'
}
& git -C $ServerRoot diff --quiet HEAD -- src
if ($LASTEXITCODE -ne 0) { throw 'Use clean production server sources' }
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent (Get-Command $Cxx).Source) + ';' + $previousPath
    $sdps = @()
    foreach ($mode in @('clear', 'encrypted')) {
        $stem = if ($V2) { "announce-v2-$mode" } else { "announce-$mode" }
        $sdp = Join-Path $PSScriptRoot "$stem.sdp"
        $option = if ($V2) { '--write-v2-sdp' } else { '--write-sdp' }
        $arguments = @($option, $sdp)
        if ($mode -eq 'encrypted') { $arguments += 'encrypted' }
        & $ClientTestExecutable @arguments
        if ($LASTEXITCODE -ne 0) { throw 'Client SDP export failed' }
        $sdps += $sdp
    }
    $defines = @()
    if ($V2) {
        $defines += '-DPYROWAVE_VALIDATE_V2'
        $profileDirectory = Join-Path $BuildDirectory 'profiles'
        New-Item -ItemType Directory -Force -Path $profileDirectory | Out-Null
        $pairs = @(@('yuv420','yuv444'), @('p8','p16'), @('limited','full'), @('center','left'),
                   @('bt709','bt2020'), @('bt709','bt2020'), @('bt709','pq'))
        foreach ($bits in 0..127) {
            $parts = @()
            foreach ($bit in 0..6) { $parts += $pairs[$bit][[int](($bits -band (1 -shl $bit)) -ne 0)] }
            $profile = $parts -join '-'
            foreach ($mode in @('clear','encrypted')) {
                $sdp = Join-Path $profileDirectory "$bits-$mode.sdp"
                $arguments = @('--write-v2-sdp', $sdp, $profile)
                if ($mode -eq 'encrypted') { $arguments += 'encrypted' }
                & $ClientTestExecutable @arguments
                if ($LASTEXITCODE -ne 0) { throw "Profile SDP export failed: $profile" }
                $sdps += $sdp
            }
        }
    }
    $exe = Join-Path $BuildDirectory 'validate-pyrowave-sdp.exe'
    & $Cxx -std=c++23 -O2 @defines -I $ServerRoot (Join-Path $PSScriptRoot 'validate_sdp.cpp') `
        (Join-Path $ServerRoot 'src/pyrowave_negotiation.cpp') (Join-Path $ServerRoot 'src/pyrowave_protocol.cpp') -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Pinned server negotiation harness compilation failed' }
    $transcript = Join-Path $BuildDirectory 'validation.log'
    & $exe @sdps *> $transcript
    if ($LASTEXITCODE -ne 0) { throw 'Generated client SDP rejected by pinned server negotiation' }
    Get-Content -LiteralPath $transcript -Tail 4
    Write-Host "PASS $($sdps.Count) actual client SDP requests and $($sdps.Count * 12) negative server negotiation cases. Transcript: $transcript"
} finally { $env:PATH = $previousPath }
