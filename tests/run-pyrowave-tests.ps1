param([string]$Gcc = 'gcc', [string]$FixtureDirectory = "$PSScriptRoot/fixtures/pyrowave")
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$build = Join-Path ([IO.Path]::GetTempPath()) 'moonlight-pyrowave-common-tests'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$sdk = (Resolve-Path "$repo/../../libs/windows").Path
$sources = @('tests/test_pyrowave.c', 'tests/depacketizer_under_test.c', 'src/RtpVideoQueue.c', 'src/SdpGenerator.c',
             'src/ByteBuffer.c', 'src/LinkedBlockingQueue.c', 'src/PlatformCrypto.c',
             'nanors/rs.c', 'nanors/deps/obl/oblas_common.c', 'nanors/deps/obl/oblas_lite.c')
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $build (($source -replace '[/\\]', '_') + '.o')
    & $Gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -DHAS_SOCKLEN_T `
        "-I$repo/src" "-I$repo/enet/include" "-I$repo/nanors" "-I$repo/nanors/deps" "-I$repo/nanors/deps/obl" `
        "-I$sdk/include/x64" -c "$repo/$source" -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
    $objects += $object
}
$exe = Join-Path $build 'pyrowave-common-tests.exe'
& $Gcc @objects "$sdk/lib/x64/libcrypto.lib" -lws2_32 -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Link failed' }
$fixtureArgs = @()
if (Test-Path $FixtureDirectory) {
    foreach ($file in (Get-ChildItem -LiteralPath $FixtureDirectory -Filter '*.pwrt' | Sort-Object Name)) {
        $stem = $file.BaseName -replace '-(clear|encrypted)$', ''
        $expected = Join-Path $FixtureDirectory ($stem + '.pwpf')
        if (!(Test-Path -LiteralPath $expected)) {
            $expected = Join-Path $FixtureDirectory (($stem -replace '-fec[0-9]+$', '') + '.pwvf')
        }
        if (!(Test-Path -LiteralPath $expected)) { throw "Missing expected PWVF: $expected" }
        $fixtureArgs += @($file.FullName, $expected)
    }
}
if ($fixtureArgs.Count -eq 0) { throw "No production-server fixtures found: $FixtureDirectory" }
$previousPath = $env:PATH
try {
    $env:PATH = "$sdk/lib/x64;$env:PATH"
    & $exe @fixtureArgs
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    & $exe --check-sdp (Join-Path $FixtureDirectory 'announce-clear.sdp')
    if ($LASTEXITCODE -ne 0) { throw 'Clear SDP differs from pinned server contract fixture' }
    & $exe --check-sdp (Join-Path $FixtureDirectory 'announce-encrypted.sdp') encrypted
    if ($LASTEXITCODE -ne 0) { throw 'Encrypted SDP differs from pinned server contract fixture' }
    & $exe --check-v2-sdp (Join-Path $FixtureDirectory 'announce-v2-clear.sdp')
    if ($LASTEXITCODE -ne 0) { throw 'Clear v2 SDP differs from pinned server contract fixture' }
    & $exe --check-v2-sdp (Join-Path $FixtureDirectory 'announce-v2-encrypted.sdp') encrypted
    if ($LASTEXITCODE -ne 0) { throw 'Encrypted v2 SDP differs from pinned server contract fixture' }
} finally {
    $env:PATH = $previousPath
}
