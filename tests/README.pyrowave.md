# PyroWave common-c validation

From this submodule in the Moonlight Qt checkout, run:

```powershell
./tests/run-pyrowave-tests.ps1
```

Requires MinGW GCC on PATH and the existing Moonlight Windows x64 OpenSSL SDK
at `../../libs/windows`. Objects and the executable are written below the OS
temporary directory. The script restores PATH after testing and requires the
checked-in fixture corpus; it does not silently skip transport tests.

The harness compiles production `SdpGenerator.c`, `PlatformCrypto.c`,
`RtpVideoQueue.c`, `VideoDepacketizer.c`, `LinkedBlockingQueue.c`, `ByteBuffer.c`
and the bundled Reed-Solomon implementation. It stubs only the deterministic
clock, single-thread synchronization, address rendering and control callbacks.
No server, GPU, user desktop or live network is accessed.

The fixture generator links the actual VibePollo packetizer, FEC and encryption
implementation; see [provenance and regeneration](fixtures/pyrowave/README.md).
Tests cover:

- Exact SDP version/revision/profile/transport for legacy v1 and all 128 v2
  profiles (420/444, p8/p16, range, siting, primaries, matrix, transfer), MTU thresholds,
  rate/size validation and all ten standard H.264/HEVC/AV1 profiles' codec,
  HDR/chroma masks and legacy bitrate cap, without the private extension.
- Byte-exact PWVF reconstruction through AES-GCM, FEC and depacketization for
  a real encoded synthetic image and a four-block transport stress frame.
- Reordered/duplicated data shards within each block and recovery of each
  block's first data shard using production parity.
- Missing data beyond recovery, 100 ms incomplete-frame expiry, and recovery
  on the next independent frame without decoder reference state.
- Inconsistent timestamp/block count/data count/sequence-base rejection before
  FEC allocation, malformed short/PWVF headers, frame-index disagreement,
  oversized declared payload/packet count, and subsequent-frame recovery.
- GCM ciphertext and tag tampering plus the unauthenticated prefix frame index
  being checked against the authenticated NV header.
- Actual server fixtures at FEC 0/1/20/100/255%, plus accepted maximum shard
  counts and rejection immediately above each percentage's 255-shard bound.
- Frame-index 32-bit, RTP sequence 16-bit and stream-packet 24-bit wrap, followed
  by rejection of a stale frame. A test translation unit includes the real
  depacketizer and seeds its private counters; it adds no production hooks.
- Byte-exact v2 PWPF slots through the real server's selective FEC and GCM at
  base FEC 0/1/20/100/255%, including reversed arrival across all FEC blocks.
  Conflicting duplicates, repeated frame identity, invalid record offsets,
  native sequence dimensions and allocation ceilings are rejected.
- Partial v2 delivery with the final slot, manifest slot or entire first block
  missing: available slots survive the 100 ms deadline or next frame arrival.
  Missing bytes are never synthesized. Each DU PICDATA entry contains exactly
  one fixed slot of `packetSize - 16` bytes, with no v1 short frame header.
- A decoder's `DR_NEED_IDR` result releases only the rejected PyroWave DU and
  preserves a later independent frame already queued, without requesting IDR.
  The same completion-API test verifies H.264/HEVC/AV1 retain queue flush and IDR
  request semantics; both v1 and v2 fixture sets cover the PyroWave case.

The fixture directory also contains exact clear/encrypted SDP exported by this
client and an offline validator linked to the pinned server's production
negotiation function. Its separate regeneration command requires the server
checkout; normal transport tests do not download or build server code.

V1 retains the existing sequential FEC-block scheduling: a later block arriving
before a previous block can be completed discards that frame, as with standard
codecs. V2 independently retains up to four blocks of one frame and supports
cross-block resequencing. Both accept the next all-intra frame immediately.
Physical-network performance and end-to-end authenticated RTSP sessions are
separate validation scopes. Standard-codec checks here cover SDP and masks,
not native decoding or live negotiation fallback. Native PyroWave decoding
and GPU presentation require separate application validation.

The receiver bounds PWVF/PWPF bytes by the smaller of 4 MiB, four 255-shard blocks,
and the client-requested per-frame wire budget. The protocol does not echo the
server's exact post-reservation FEC budget, so that stricter limit is enforced
by the server. Client packet/FEC/header checks and bounded queues apply before
the application envelope parser allocates its contiguous decode buffer.
V2 validates each slot's lengths and metadata before retaining its buffer;
aggregate native-item reconstruction and semantic sideband checks belong to
the application parser. Its fixture sideband is synthetic and is not evidence
of native partial-decoding quality; that needs encoder-derived application
fixtures and the GPU tests.

The portable target in the parent repository, `tests/pyrowave-common`, discovers
all checked-in transport fixtures and snapshots (currently 29 CTest cases).
Linux ASan/UBSan validation also passes with `-fsanitize=address,undefined
-fno-omit-frame-pointer -fno-pie` and link flags `-fsanitize=address,undefined
-no-pie`; the non-PIE option avoids this WSL environment's ASan startup issue.
