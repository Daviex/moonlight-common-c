# Production-server PyroWave transport fixtures

The v1 corpus was generated offline from VibePollo server commit
`205c510c9a2aa3d9b37011c4cca0986ebbb65a6c`, directly linking its production
`pyrowave_protocol.cpp`, `pyrowave_transport.cpp`, `stream_protocol.cpp`,
`crypto.cpp` and `rswrapper.c`. No server is started; no desktop, network traffic
or secret keys are captured. Files, generator and original synthetic image
fixture are distributed under GPL-3.0; PyroWave retains its MIT license.

Each `.pwrt` file begins with ASCII `PWRT`, then four LE32 fields: version `1`,
GameStream packet size `1024`, encryption `0` or `1`, datagram count. Each record
is LE32 byte length followed by the complete UDP payload, including the 32-byte
encryption prefix when enabled. Expected reconstruction is the corresponding
`.pwvf`, excluding GameStream's 8-byte short header.

All cases: frame index 1, initial RTP sequence 0, RTP timestamp `0x10203040`,
PWVF monotonic timestamp 123456789 us, processing latency 123, MTU 1280, IPv6
header budget 40, default FEC 20%, minimum parity 2, AES key `00 01 ... 0f`, initial
IV counter 0. The key is public test data. Data and parity datagrams remain in
sender order so receiver tests can independently inject loss/reorder/duplicates.

- `720p`: the server's real encoded 1280x720 synthetic gradient, with a newly
  serialized frame index/timestamp; two native packets, 112200 PWVF bytes,
  one FEC block and 135 datagrams. The native codec payload remains decodable.
- `four-block`: one 800000-byte deterministic native record with byte value
  `(i * 31 + i / 256) & 255`, 800036 PWVF bytes, four FEC blocks and 954 datagrams.
  This is a transport stress case, **not a valid native PyroWave image**.
- `720p-fec0`, `720p-fec1`, `720p-fec100`, `720p-fec255` reuse the 720p PWVF
  with the named percentage: respectively 112, 114, 224 and 398 datagrams.
  FEC 255% needs two blocks; the others need one. Each has clear and encrypted
  variants. FEC 0% validates loss discard/recovery on the next frame, while the
  nonzero cases also recover deliberately omitted data using actual parity.

`manifest.json` records all fourteen binary output SHA-256 hashes. Regenerate
using MSYS2 UCRT64 GCC and a clean checkout of the exact server commit with
its pinned nanors submodule; from the Moonlight Qt repository:

```powershell
moonlight-common-c/moonlight-common-c/tests/fixtures/pyrowave/generate.ps1 `
  -ServerRoot C:/path/to/Vibepollo2 `
  -SourceFixture tests/pyrowave/fixtures/720p-gradient/frame.pwvf `
  -BuildDirectory build/pyrowave-transport-generator
```

The source fixture is also available at the pinned server's
`tools/pyrowave/fixtures/720p-gradient/frame.pwvf`. Generation requires no GPU.
Replay tests exercise the actual common-c receiver; this corpus alone does not
qualify physical networks, pairing, decoder GPU behavior or client presentation.

## Protocol v2 fragmented fixtures

The `v2-*` files use production sources pinned to
`724d4c67858b0a3595a45bfbb1bbe47758ea2467`, including
`serialize_fragmented_frame()`, `packetize_frame()` and `encode_block()`.
The PWRT container is unchanged. Expected reconstruction is the corresponding
`.pwpf`: concatenated fixed slots of 1008 bytes (`packetSize - 16`), each with
its own 48-byte header and fragment records. There is no v1 short frame header.
The public key, frame index, timestamps and path parameters match v1; v2 has no
processing-latency field. `manifest-v2.json` records eighteen binary hashes.

| Case | FEC base / critical | Blocks | Datagrams |
| --- | --- | ---: | ---: |
| v2-720p-fec0 | 0 / 0 | 2 | 119 |
| v2-720p-fec1 | 1 / 40 | 2 | 149 |
| v2-720p-fec20 | 20 / 40 | 2 | 157 |
| v2-720p-fec100 | 100 / 100 | 2 | 238 |
| v2-720p-fec255 | 255 / 255 | 2 | 423 |
| v2-four-block | 20 / 40 | 4 | 759 |

Each case has clear and encrypted variants. The 720p native packets come from
the real synthetic encoder fixture, but partial-decoding sideband is deliberately
synthetic: counts `{1,1,1,1,nativePacketCount}`, three pristine bands, 78 active
blocks and all 78 mask bits set. This metadata qualifies transport structure,
not native decoder readiness or partial image quality. The four-block fixture
repeats one native packet ten times and is **not a decodable native image**.
Application GPU tests use separate encoder-derived sideband fixtures.

Regenerate with the same command above plus `-V2` and a clean checkout of the
v2 pin. The script checks the server revision and unchanged production sources;
no running server is required. Tests independently inject whole-block loss,
manifest loss, unrecoverable trailing fragments, duplicates, corrupt records,
counter wrap and complete reverse cross-block arrival. Partial frame delivery
is checked at exactly 100 ms and when a newer frame supersedes it.

## Client SDP against pinned server negotiation

`announce-clear.sdp` and `announce-encrypted.sdp` are the actual bytes from
common-c `getSdpPayloadForStreamConfig()`: 1280x720, 59.94 fps (nominal 60),
configured 300000 Kbps, legacy maximum 240000 Kbps, default path MTU 1280.
The packet size is 1024 clear and 992 encrypted, because common-c reserves
32 bytes for GCM. SDP has a legacy trailing space before CRLF; the pinned
server removes exactly one trailing space. These files are marked binary
in Git to preserve the exact wire bytes, including that whitespace.

After running `tests/run-pyrowave-tests.ps1`, regenerate and validate them:

```powershell
moonlight-common-c/moonlight-common-c/tests/fixtures/pyrowave/validate-sdp.ps1 `
  -ServerRoot C:/path/to/clean-pinned-Vibepollo2 `
  -ClientTestExecutable "$env:TEMP/moonlight-pyrowave-common-tests/pyrowave-common-tests.exe" `
  -BuildDirectory "$env:TEMP/pyrowave-sdp-validation-v1"
```

The harness links production `pyrowave_negotiation.cpp` and
`pyrowave_protocol.cpp`. It maps SDP as the pinned RTSP parser does and models
its bitrate reservations for stereo normal-quality audio, no host bitrate
override, FEC 20% and conservative IPv6 header size 40. Host opt-in and adapter
support are explicit positive inputs. It does not start the server or execute
the authenticated RTSP request handler, launch session, or GPU probe.

Reproducible accepted results from the pinned v1 server:

| SDP | fps x100 | Frame budget | Wire byte budget | Encoder target |
| --- | ---: | ---: | ---: | ---: |
| Clear | 5994 | 480808 | 624182 | 464392 |
| Encrypted | 5994 | 465544 | 624182 | 449128 |

Each variant also rejects twelve negative mutations: version, revision,
profile, HDR, chroma, color mode, odd width, inconsistent refresh rate,
undersized MTU, and each of the three required extension values missing.
This is an offline contract preflight; live RTSP fallback, host permission
and transport socket behavior remain separate integration coverage.

For v2, use `validate-sdp.ps1 -V2` against the clean v2 pin. It exports both
representative `announce-v2-*.sdp` snapshots and all 128 explicit profiles in
clear and encrypted form. The representative profile is
`yuv444-p16-full-left-bt2020-bt2020-pq`; its accepted results are:

| SDP | fps x100 | Frame budget | Wire byte budget | Encoder target |
| --- | ---: | ---: | ---: | ---: |
| Clear | 5994 | 411264 | 624182 | 384976 |
| Encrypted | 5994 | 398208 | 624182 | 371920 |

The reproducible v2 run accepts 258 actual client SDP documents and rejects
3096 mutations (twelve per document: unsupported version, revision, profile,
HDR, chroma, CSC, zero width, refresh mismatch, MTU and missing required
extensions). The validator writes `validation.log` in its build directory.
It maps the SDP fields as the pinned RTSP parser does and calls the production
negotiator; it does not execute an authenticated RTSP session. The server
currently derives transport mode from protocol version and does not enforce
the separate transport attribute; this client still emits the exact documented
`complete-v1` or `fragments-v2` value. Rejecting a contradictory transport
attribute is therefore not claimed as server coverage.
