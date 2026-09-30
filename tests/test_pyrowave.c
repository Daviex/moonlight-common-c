// Deterministic tests of production SDP, AES-GCM, RTP/FEC and depacketization.
// Only platform synchronization/clock/control callbacks are replaced. No GPU,
// sockets or codec runtime are needed; PWVF/native payloads remain opaque here.
#include "Limelight-internal.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

STREAM_CONFIGURATION StreamConfig;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
int NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
int AppVersionQuad[4] = {7, 1, 500, -1};
struct sockaddr_storage RemoteAddr;
bool HighQualitySurroundSupported, HighQualitySurroundEnabled, AudioEncryptionEnabled;
int AudioPacketDuration = 5;
uint16_t RtspPortNumber = 48010, VideoPortNumber = 47998;
uint32_t EncryptionFeaturesEnabled, EncryptionFeaturesSupported, EncryptionFeaturesRequested;
static uint64_t clockUs = 1000000;
static int completedFrames;
static int idrRequests;
void testSeedDepacketizerCounters(uint32_t frame, uint32_t streamPacket);

uint64_t PltGetMicroseconds(void) { return clockUs; }
int PltCreateMutex(PLT_MUTEX* p) { memset(p, 0, sizeof(*p)); return 0; }
void PltDeleteMutex(PLT_MUTEX* p) { (void)p; }
void PltLockMutex(PLT_MUTEX* p) { (void)p; }
void PltUnlockMutex(PLT_MUTEX* p) { (void)p; }
int PltCreateConditionVariable(PLT_COND* p, PLT_MUTEX* m) { memset(p, 0, sizeof(*p)); (void)m; return 0; }
void PltDeleteConditionVariable(PLT_COND* p) { (void)p; }
void PltSignalConditionVariable(PLT_COND* p) { (void)p; }
void PltWaitForConditionVariable(PLT_COND* p, PLT_MUTEX* m) { (void)p; (void)m; CHECK(false); }
bool isReferenceFrameInvalidationEnabled(void) { return false; }
bool isReferenceFrameInvalidationSupportedByDecoder(void) { return false; }
bool LiGetCurrentHostDisplayHdrMode(void) { return false; }
void LiRequestIdrFrame(void) { idrRequests++; }
void notifyKeyFrameReceived(void) { }
void connectionDetectedFrameLoss(uint32_t first, uint32_t last) { (void)first; (void)last; }
void connectionReceivedCompleteFrame(uint32_t frame, bool ltr) { (void)frame; (void)ltr; completedFrames++; }
void connectionSawFrame(uint32_t frame) { (void)frame; }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
bool PltSafeStrcpy(char* out, size_t size, const char* in) {
    if (strlen(in) >= size) return false;
    strcpy(out, in);
    return true;
}
void addrToUrlSafeString(struct sockaddr_storage* addr, char* text, size_t length) {
    (void)addr;
    CHECK(PltSafeStrcpy(text, length, "127.0.0.1"));
}

static void defaultConfig(void) {
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.width = 1280;
    StreamConfig.height = 720;
    StreamConfig.fps = 60;
    StreamConfig.bitrate = 800000;
    StreamConfig.packetSize = 1024;
    StreamConfig.colorSpace = COLORSPACE_REC_709;
    StreamConfig.colorRange = COLOR_RANGE_FULL;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_PYROWAVE;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    RemoteAddr.ss_family = AF_INET;
    EncryptionFeaturesEnabled = EncryptionFeaturesRequested = EncryptionFeaturesSupported = 0;
    completedFrames = 0;
    idrRequests = 0;
}

static void testConfigAndSdp(void) {
    defaultConfig();
    CHECK(pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.width = 65;
    CHECK(!pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.width = 1280;
    StreamConfig.supportedVideoFormats |= VIDEO_FORMAT_H264;
    CHECK(!pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_PYROWAVE;
    StreamConfig.clientRefreshRateX100 = 5994;
    CHECK(pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.clientRefreshRateX100 = 12000;
    CHECK(!pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.clientRefreshRateX100 = 0;
    CHECK(pyrowaveValidatePacketSize(1184, 1280, 40, true));
    CHECK(!pyrowaveValidatePacketSize(1200, 1280, 40, true));
    CHECK(!pyrowaveValidatePacketSize(192, 1280, 20, false));
    CHECK(!pyrowaveValidatePacketSize(1024, 1279, 20, false));
    CHECK(pyrowaveFrameLimit(&StreamConfig) == 4 * 255 * (1024 - 16) - 8);
    StreamConfig.bitrate = 24000;
    CHECK(pyrowaveFrameLimit(&StreamConfig) == 50000);
    StreamConfig.bitrate = 300000;
    int length;
    char* sdp = getSdpPayloadForStreamConfig(14, &length);
    CHECK(sdp && length == (int)strlen(sdp));
    CHECK(strstr(sdp, "x-nv-vqos[0].bitStreamFormat:3"));
    CHECK(strstr(sdp, "x-vp-pyrowave.version:1"));
    CHECK(strstr(sdp, "x-vp-pyrowave.bitstreamRevision:" PYROWAVE_BITSTREAM_REVISION));
    CHECK(strstr(sdp, "x-vp-pyrowave.profile:" PYROWAVE_PROFILE));
    CHECK(strstr(sdp, "x-vp-pyrowave.pathMtu:1280"));
    CHECK(strstr(sdp, "x-nv-video[0].encoderCscMode:3"));
    CHECK(strstr(sdp, "x-nv-video[0].dynamicRangeMode:0"));
    CHECK(strstr(sdp, "x-ss-video[0].chromaSamplingType:0"));
    CHECK(strstr(sdp, "x-nv-vqos[0].bw.maximumBitrateKbps:240000"));
    free(sdp);
    // A standard stream retains its bitrate cap and never sends the extension.
    const int formats[] = {VIDEO_FORMAT_H264, VIDEO_FORMAT_H264_HIGH8_444,
        VIDEO_FORMAT_H265, VIDEO_FORMAT_H265_MAIN10, VIDEO_FORMAT_H265_REXT8_444, VIDEO_FORMAT_H265_REXT10_444,
        VIDEO_FORMAT_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN10, VIDEO_FORMAT_AV1_HIGH8_444, VIDEO_FORMAT_AV1_HIGH10_444};
    CHECK(!(VIDEO_FORMAT_PYROWAVE & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265 |
                                   VIDEO_FORMAT_MASK_AV1 | VIDEO_FORMAT_MASK_10BIT | VIDEO_FORMAT_MASK_YUV444)));
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        NegotiatedVideoFormat = formats[i];
        sdp = getSdpPayloadForStreamConfig(14, &length);
        CHECK(sdp && !strstr(sdp, "x-vp-pyrowave"));
        CHECK(strstr(sdp, "x-nv-vqos[0].bw.maximumBitrateKbps:100000"));
        CHECK(strstr(sdp, (formats[i] & VIDEO_FORMAT_MASK_H265) ? "x-nv-vqos[0].bitStreamFormat:1" :
                         (formats[i] & VIDEO_FORMAT_MASK_AV1) ? "x-nv-vqos[0].bitStreamFormat:2" :
                                                               "x-nv-vqos[0].bitStreamFormat:0"));
        CHECK(strstr(sdp, (formats[i] & VIDEO_FORMAT_MASK_10BIT) ? "x-nv-video[0].dynamicRangeMode:1" :
                                                               "x-nv-video[0].dynamicRangeMode:0"));
        CHECK(strstr(sdp, (formats[i] & VIDEO_FORMAT_MASK_YUV444) ? "x-ss-video[0].chromaSamplingType:1" :
                                                                "x-ss-video[0].chromaSamplingType:0"));
        free(sdp);
    }
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    StreamConfig.packetSize = 1408;
    CHECK(getSdpPayloadForStreamConfig(14, &length) == NULL);
}

static void testExtendedProfiles(void) {
    for (unsigned bits = 0; bits < 128; bits++) {
        defaultConfig();
        StreamConfig.pyrowaveProtocolVersion = 2;
        snprintf(StreamConfig.pyrowaveProfile, sizeof(StreamConfig.pyrowaveProfile), "yuv%s-p%s-%s-%s-%s-%s-%s",
                 bits & 1 ? "444" : "420", bits & 2 ? "16" : "8", bits & 4 ? "full" : "limited",
                 bits & 8 ? "left" : "center", bits & 16 ? "bt2020" : "bt709", bits & 32 ? "bt2020" : "bt709",
                 bits & 64 ? "pq" : "bt709");
        StreamConfig.colorSpace = bits & 32 ? COLORSPACE_REC_2020 : COLORSPACE_REC_709;
        StreamConfig.colorRange = bits & 4 ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;
        CHECK(pyrowaveValidateConfig(&StreamConfig));
        int length;
        char* sdp = getSdpPayloadForStreamConfig(14, &length);
        CHECK(sdp && strstr(sdp, "x-vp-pyrowave.version:2") && strstr(sdp, "x-vp-pyrowave.transport:fragments-v2"));
        CHECK(strstr(sdp, StreamConfig.pyrowaveProfile));
        CHECK(strstr(sdp, bits & 1 ? "x-ss-video[0].chromaSamplingType:1" : "x-ss-video[0].chromaSamplingType:0"));
        CHECK(strstr(sdp, bits & 64 ? "x-nv-video[0].dynamicRangeMode:1" : "x-nv-video[0].dynamicRangeMode:0"));
        char csc[64];
        snprintf(csc, sizeof(csc), "x-nv-video[0].encoderCscMode:%d", (bits & 32 ? 4 : 2) + !!(bits & 4));
        CHECK(strstr(sdp, csc));
        free(sdp);
        StreamConfig.colorRange ^= 1;
        CHECK(!pyrowaveValidateConfig(&StreamConfig));
        CHECK(getSdpPayloadForStreamConfig(14, &length) == NULL);
        StreamConfig.colorRange ^= 1;
        StreamConfig.pyrowaveProtocolVersion = 1;
        CHECK(!pyrowaveValidateConfig(&StreamConfig)); // extended names never imply v1
    }
    defaultConfig();
    StreamConfig.pyrowaveProtocolVersion = 3;
    CHECK(!pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.pyrowaveProtocolVersion = 2;
    strcpy(StreamConfig.pyrowaveProfile, "yuv444-p16-full-left-bt2020-bt709-pq");
    StreamConfig.width = StreamConfig.height = 1;
    StreamConfig.fps = 500;
    StreamConfig.bitrate = 1000000;
    CHECK(pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.width = StreamConfig.height = 16384;
    CHECK(!pyrowaveValidateConfig(&StreamConfig)); // nominal planes exceed 512 MiB
    StreamConfig.width = StreamConfig.height = 1024;
    StreamConfig.fps = INT_MAX;
    CHECK(!pyrowaveValidateConfig(&StreamConfig));
    StreamConfig.fps = 60;
    memset(StreamConfig.pyrowaveProfile, 'x', sizeof(StreamConfig.pyrowaveProfile));
    CHECK(!pyrowaveValidateConfig(&StreamConfig));
}

typedef struct { unsigned char* bytes; uint32_t length; } Datagram;
typedef struct {
    uint32_t packetSize, encrypted, count;
    Datagram* datagrams;
    unsigned char* frame;
    size_t frameBytes;
    bool fragmented;
} Fixture;

static uint32_t readWord(FILE* file) {
    unsigned char value[4];
    CHECK(fread(value, 1, 4, file) == 4);
    return pyrowaveReadLe32(value);
}

static Fixture loadFixture(const char* path, const char* expected) {
    Fixture f = {0};
    FILE* file = fopen(path, "rb");
    CHECK(file);
    unsigned char magic[4];
    CHECK(fread(magic, 1, 4, file) == 4 && memcmp(magic, "PWRT", 4) == 0);
    CHECK(readWord(file) == 1);
    f.packetSize = readWord(file);
    f.encrypted = readWord(file);
    f.count = readWord(file);
    CHECK(f.packetSize >= 200 && f.packetSize <= 1500 && f.encrypted <= 1 && f.count > 0 && f.count <= 1020);
    f.datagrams = calloc(f.count, sizeof(*f.datagrams));
    CHECK(f.datagrams);
    for (uint32_t i = 0; i < f.count; i++) {
        f.datagrams[i].length = readWord(file);
        CHECK(f.datagrams[i].length == f.packetSize + 16 + (f.encrypted ? 32 : 0));
        f.datagrams[i].bytes = malloc(f.datagrams[i].length);
        CHECK(f.datagrams[i].bytes);
        CHECK(fread(f.datagrams[i].bytes, 1, f.datagrams[i].length, file) == f.datagrams[i].length);
    }
    CHECK(fgetc(file) == EOF);
    fclose(file);
    file = fopen(expected, "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size >= 40 && size <= (long)PYROWAVE_MAX_FRAME_BYTES);
    f.frameBytes = (size_t)size;
    rewind(file);
    f.frame = malloc(f.frameBytes);
    CHECK(f.frame && fread(f.frame, 1, f.frameBytes, file) == f.frameBytes);
    f.fragmented = memcmp(f.frame, "PWPF", 4) == 0;
    CHECK(pyrowaveReadLe32(f.frame + (f.fragmented ? 8 : 16)) == 1);
    fclose(file);
    return f;
}

static unsigned char* decryptShard(const Fixture* f, const Datagram* datagram, PPLT_CRYPTO_CONTEXT crypto) {
    unsigned char* bytes = calloc(1, f->packetSize + 16 + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(bytes);
    if (f->encrypted) {
        unsigned char key[16];
        for (int i = 0; i < 16; i++) key[i] = (unsigned char)i;
        PENC_VIDEO_HEADER prefix = (PENC_VIDEO_HEADER)datagram->bytes;
        int plainLength;
        if (!PltDecryptMessage(crypto, ALGORITHM_AES_GCM, 0, key, 16,
            prefix->iv, 12, prefix->tag, 16, datagram->bytes + 32, (int)datagram->length - 32,
            bytes, &plainLength)) {
            free(bytes);
            return NULL;
        }
        CHECK(plainLength == (int)f->packetSize + 16);
        if (!pyrowaveAuthenticatedFrameMatches(bytes, (size_t)plainLength, LE32(prefix->frameNumber))) {
            free(bytes);
            return NULL;
        }
    }
    else {
        memcpy(bytes, datagram->bytes, datagram->length);
    }
    return bytes;
}

static int submitPlain(RTP_VIDEO_QUEUE* queue, const Fixture* f, unsigned char* bytes) {
    PRTP_PACKET packet = (PRTP_PACKET)bytes;
    packet->sequenceNumber = BE16(packet->sequenceNumber);
    packet->timestamp = BE32(packet->timestamp);
    packet->ssrc = BE32(packet->ssrc);
    int status = RtpvAddPacket(queue, packet, (int)f->packetSize + 16,
                              (PRTPV_QUEUE_ENTRY)(bytes + f->packetSize + 16));
    if (status != RTPF_RET_QUEUED) free(bytes);
    return status;
}

static int submitShard(RTP_VIDEO_QUEUE* queue, const Fixture* f, const Datagram* datagram, PPLT_CRYPTO_CONTEXT crypto) {
    unsigned char* bytes = decryptShard(f, datagram, crypto);
    CHECK(bytes);
    return submitPlain(queue, f, bytes);
}

static void compareFrame(const Fixture* f) {
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT du;
    CHECK(completedFrames == 1 && LiPollNextVideoFrame(&handle, &du));
    CHECK(du->frameType == FRAME_TYPE_IDR && (uint32_t)du->frameNumber == pyrowaveReadLe32(f->frame + (f->fragmented ? 8 : 16)) && !du->hdrActive);
    CHECK(du->fullLength == (int)f->frameBytes);
    size_t offset = 0;
    for (PLENTRY entry = du->bufferList; entry; entry = entry->next) {
        CHECK(entry->bufferType == BUFFER_TYPE_PICDATA && entry->length > 0);
        CHECK(offset + entry->length <= f->frameBytes);
        CHECK(memcmp(f->frame + offset, entry->data, entry->length) == 0);
        offset += entry->length;
    }
    CHECK(offset == f->frameBytes);
    LiCompleteVideoFrame(handle, DR_OK);
    CHECK(!LiPollNextVideoFrame(&handle, &du));
}

static void putWord(unsigned char* out, uint32_t value) {
    for (int i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static void testFecBounds(void) {
    const uint32_t percentages[] = {0, 1, 20, 100, 255};
    const uint32_t maxData[] = {255, 252, 212, 127, 71};
    unsigned char packet[1040] = {0};
    packet[0] = 0x90;
    packet[24] = 5;
    packet[26] = 0x10;
    for (size_t i = 0; i < sizeof(percentages) / sizeof(percentages[0]); i++) {
        uint32_t fec = percentages[i], count = maxData[i];
        putWord(packet + 28, (count << 22) | (fec << 4));
        CHECK(pyrowaveValidateShard(packet, sizeof(packet), 1024));
        putWord(packet + 28, ((count + 1) << 22) | (fec << 4));
        CHECK(!pyrowaveValidateShard(packet, sizeof(packet), 1024));
        uint32_t total = count + (count * fec + 99) / 100;
        putWord(packet + 28, (count << 22) | (total << 12) | (fec << 4));
        CHECK(!pyrowaveValidateShard(packet, sizeof(packet), 1024));
    }
}

static void testCounterWraps(const Fixture* f) {
    defaultConfig();
    StreamConfig.packetSize = (int)f->packetSize;
    StreamConfig.fps = 30;
    StreamConfig.pyrowaveProtocolVersion = f->fragmented ? 2 : 1;
    RTP_VIDEO_QUEUE queue;
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&queue);
    queue.currentFrameNumber = UINT32_MAX - 1;
    queue.nextContiguousSequenceNumber = 65530;
    testSeedDepacketizerCounters(UINT32_MAX - 1, 0xfffff0);
    PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
    CHECK(crypto);
    for (uint32_t frame = 0; frame < 4; frame++) {
        uint64_t index = (uint64_t)UINT32_MAX - 1 + frame;
        Fixture expected = *f;
        expected.frame = malloc(f->frameBytes);
        CHECK(expected.frame);
        memcpy(expected.frame, f->frame, f->frameBytes);
        for (size_t offset = 0; offset < (f->fragmented ? f->frameBytes : 1); offset += f->packetSize - 16) {
            putWord(expected.frame + offset + (f->fragmented ? 8 : 16), (uint32_t)index);
            putWord(expected.frame + offset + (f->fragmented ? 12 : 20), (uint32_t)(index >> 32));
        }
        completedFrames = 0;
        for (uint32_t i = 0; i < f->count; i++) {
            unsigned char* bytes = decryptShard(f, &f->datagrams[i], crypto);
            CHECK(bytes);
            uint32_t fec = pyrowaveReadLe32(bytes + 28);
            if (((fec >> 12) & 1023) >= (fec >> 22)) { free(bytes); continue; }
            putWord(bytes + 20, (uint32_t)index);
            uint16_t sequence = (uint16_t)(65530 + frame * f->count + i);
            bytes[2] = (unsigned char)(sequence >> 8);
            bytes[3] = (unsigned char)sequence;
            putWord(bytes + 16, (0xfffff0 + frame * f->count + i) << 8);
            if (f->fragmented || i == 0) {
                putWord(bytes + (f->fragmented ? 40 : 56), (uint32_t)index);
                putWord(bytes + (f->fragmented ? 44 : 60), (uint32_t)(index >> 32));
            }
            submitPlain(&queue, &expected, bytes);
        }
        compareFrame(&expected);
        free(expected.frame);
    }
    CHECK(queue.currentFrameNumber == 2);
    unsigned char* stale = decryptShard(f, &f->datagrams[0], crypto);
    CHECK(stale);
    putWord(stale + 20, UINT32_MAX);
    stale[2] = (unsigned char)(queue.nextContiguousSequenceNumber >> 8);
    stale[3] = (unsigned char)queue.nextContiguousSequenceNumber;
    CHECK(submitPlain(&queue, f, stale) == RTPF_RET_REJECTED);
    PltDestroyCryptoContext(crypto);
    RtpvCleanupQueue(&queue);
    stopVideoDepacketizer();
    destroyVideoDepacketizer();
}

static void nextIndependentFrame(RTP_VIDEO_QUEUE* queue, const Fixture* f, PPLT_CRYPTO_CONTEXT crypto) {
    Fixture next = *f;
    next.frame = malloc(f->frameBytes);
    CHECK(next.frame);
    memcpy(next.frame, f->frame, f->frameBytes);
    for (size_t offset = 0; offset < (f->fragmented ? f->frameBytes : 1); offset += f->packetSize - 16)
        putWord(next.frame + offset + (f->fragmented ? 8 : 16), 2);
    for (uint32_t i = 0; i < f->count; i++) {
        unsigned char* bytes = decryptShard(f, &f->datagrams[i], crypto);
        CHECK(bytes);
        uint32_t fec = pyrowaveReadLe32(bytes + 28);
        if (((fec >> 12) & 1023) >= (fec >> 22)) { free(bytes); continue; }
        putWord(bytes + 20, 2); // authenticated NV frame index for the next frame
        uint16_t sequence = (uint16_t)(((bytes[2] << 8) | bytes[3]) + f->count);
        bytes[2] = (unsigned char)(sequence >> 8);
        bytes[3] = (unsigned char)sequence;
        putWord(bytes + 16, ((pyrowaveReadLe32(bytes + 16) >> 8) + f->count) << 8);
        if (f->fragmented || i == 0) putWord(bytes + (f->fragmented ? 40 : 56), 2);
        submitPlain(queue, &next, bytes);
    }
    compareFrame(&next);
    free(next.frame);
}

static void replayFixture(const Fixture* f, int mode) {
    defaultConfig();
    StreamConfig.packetSize = (int)f->packetSize;
    StreamConfig.fps = 30;
    RTP_VIDEO_QUEUE queue;
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&queue);
    PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
    CHECK(crypto);
    // Block boundaries are taken from authenticated production datagrams.
    for (uint32_t start = 0; start < f->count;) {
        unsigned char* first = decryptShard(f, &f->datagrams[start], crypto);
        CHECK(first);
        uint32_t fec = pyrowaveReadLe32(first + 28);
        uint32_t data = fec >> 22;
        uint32_t parity = (data * ((fec >> 4) & 255) + 99) / 100;
        uint32_t end = start + data + parity;
        CHECK(end <= f->count && data);
        free(first);
        for (uint32_t j = start; j < end; j++) {
            // Reorder data within each block and recover the missing first
            // data shard using actual production parity (including its header).
            uint32_t i = mode == 1 && j < start + data ? start + data - 1 - (j - start) : j;
            if (mode == 1 && parity && i == start) continue;
            if (mode == 2 && i >= start + 1) continue; // unrecoverable frame
            submitShard(&queue, f, &f->datagrams[i], crypto);
            if (mode == 1) submitShard(&queue, f, &f->datagrams[i], crypto); // duplicate
        }
        start = end;
    }
    if (mode < 2) {
        compareFrame(f);
    }
    else {
        CHECK(completedFrames == 0);
        clockUs += 100001;
        RtpvExpirePyrowaveFrame(&queue);
        CHECK(queue.pendingFecBlockList.count == 0 && queue.completedFecBlockList.count == 0);
        nextIndependentFrame(&queue, f, crypto);
    }
    PltDestroyCryptoContext(crypto);
    RtpvCleanupQueue(&queue);
    stopVideoDepacketizer();
    destroyVideoDepacketizer();
}

static void testMalformedFrame(const Fixture* f) {
    // Mutate fixed PWVF and short headers before production reassembly. Every
    // malformed frame must be discarded before allocating/copying a full PWVF.
    const int offsets[] = {32, 35, 40, 44, 46, 48, 52, 56};
    for (size_t mutation = 0; mutation < sizeof(offsets) / sizeof(offsets[0]); mutation++) {
        defaultConfig();
        StreamConfig.packetSize = (int)f->packetSize;
        StreamConfig.fps = 30;
        RTP_VIDEO_QUEUE queue;
        initializeVideoDepacketizer(StreamConfig.packetSize);
        RtpvInitializeQueue(&queue);
        PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
        CHECK(crypto);
        for (uint32_t i = 0; i < f->count; i++) {
            unsigned char* bytes = decryptShard(f, &f->datagrams[i], crypto);
            CHECK(bytes);
            uint32_t fec = pyrowaveReadLe32(bytes + 28);
            if (((fec >> 12) & 1023) >= (fec >> 22)) { free(bytes); continue; }
            if (i == 0) {
                if (offsets[mutation] == 48) putWord(bytes + 48, 0xffffffff); // allocation ceiling
                else if (offsets[mutation] == 52) putWord(bytes + 52, 4097); // packet count ceiling
                else bytes[offsets[mutation]] ^= 1;
            }
            submitPlain(&queue, f, bytes);
        }
        CHECK(completedFrames == 0 && LiGetPendingVideoFrames() == 0);
        nextIndependentFrame(&queue, f, crypto);
        PltDestroyCryptoContext(crypto);
        RtpvCleanupQueue(&queue);
        stopVideoDepacketizer();
        destroyVideoDepacketizer();
    }
}

static void testInconsistentBlock(const Fixture* f) {
    defaultConfig();
    StreamConfig.packetSize = (int)f->packetSize;
    StreamConfig.fps = 30;
    RTP_VIDEO_QUEUE queue;
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&queue);
    PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
    CHECK(crypto);
    CHECK(submitShard(&queue, f, &f->datagrams[0], crypto) == RTPF_RET_QUEUED);
    const int offsets[] = {4, 27, 31, 2}; // RTP time, block count, data count, sequence base
    for (size_t m = 0; m < sizeof(offsets) / sizeof(offsets[0]); m++) {
        unsigned char* bytes = decryptShard(f, &f->datagrams[1], crypto);
        CHECK(bytes);
        bytes[offsets[m]] ^= offsets[m] == 27 ? 64 : 1;
        CHECK(submitPlain(&queue, f, bytes) == RTPF_RET_REJECTED);
        CHECK(queue.pendingFecBlockList.count == 1);
    }
    for (uint32_t i = 1; i < f->count; i++) submitShard(&queue, f, &f->datagrams[i], crypto);
    compareFrame(f);
    PltDestroyCryptoContext(crypto);
    RtpvCleanupQueue(&queue);
    stopVideoDepacketizer();
    destroyVideoDepacketizer();
}

static void testTampering(const Fixture* f) {
    PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
    CHECK(crypto);
    unsigned char* clear = decryptShard(f, &f->datagrams[0], crypto);
    CHECK(clear && pyrowaveValidateShard(clear, f->packetSize + 16, (int)f->packetSize));
    CHECK(!pyrowaveValidateShard(clear, f->packetSize + 15, (int)f->packetSize));
    clear[28] |= 1; // reserved bit
    CHECK(!pyrowaveValidateShard(clear, f->packetSize + 16, (int)f->packetSize));
    free(clear);
    if (f->encrypted) {
        // The frame number is outside the GCM tag: explicitly bind it to the
        // authenticated NV header; separately verify tag/ciphertext corruption.
        f->datagrams[0].bytes[12] ^= 1;
        CHECK(decryptShard(f, &f->datagrams[0], crypto) == NULL);
        f->datagrams[0].bytes[12] ^= 1;
        f->datagrams[0].bytes[32 + 40] ^= 1;
        CHECK(decryptShard(f, &f->datagrams[0], crypto) == NULL);
        f->datagrams[0].bytes[32 + 40] ^= 1;
        f->datagrams[0].bytes[16] ^= 1;
        CHECK(decryptShard(f, &f->datagrams[0], crypto) == NULL);
        f->datagrams[0].bytes[16] ^= 1;
    }
    PltDestroyCryptoContext(crypto);
}

static void testDecodeFailurePreservesIndependentQueue(const Fixture* f) {
    const int formats[] = {VIDEO_FORMAT_PYROWAVE, VIDEO_FORMAT_H264, VIDEO_FORMAT_H265, VIDEO_FORMAT_AV1_MAIN8};
    for (unsigned format = 0; format < sizeof(formats) / sizeof(formats[0]); format++) {
        defaultConfig();
        StreamConfig.packetSize = (int)f->packetSize;
        StreamConfig.fps = 30;
        StreamConfig.pyrowaveProtocolVersion = f->fragmented ? 2 : 1;
        initializeVideoDepacketizer(StreamConfig.packetSize);
        RTP_VIDEO_QUEUE queue;
        RtpvInitializeQueue(&queue);
        PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
        CHECK(crypto);
        for (unsigned frame = 1; frame <= 2; frame++) {
            for (unsigned i = 0; i < f->count; i++) {
                unsigned char* bytes = decryptShard(f, &f->datagrams[i], crypto);
                CHECK(bytes);
                unsigned fec = pyrowaveReadLe32(bytes + 28);
                if (((fec >> 12) & 1023) >= (fec >> 22)) { free(bytes); continue; }
                if (frame == 2) {
                    putWord(bytes + 20, 2);
                    uint16_t sequence = (uint16_t)(((bytes[2] << 8) | bytes[3]) + f->count);
                    bytes[2] = (unsigned char)(sequence >> 8); bytes[3] = (unsigned char)sequence;
                    putWord(bytes + 16, ((pyrowaveReadLe32(bytes + 16) >> 8) + f->count) << 8);
                    if (f->fragmented || i == 0) putWord(bytes + (f->fragmented ? 40 : 56), 2);
                }
                submitPlain(&queue, f, bytes);
            }
        }
        CHECK(completedFrames == 2 && LiGetPendingVideoFrames() == 2);
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;
        CHECK(LiPollNextVideoFrame(&handle, &du) && du->frameNumber == 1);
        // The completion decision is independent of payload syntax. Switch only
        // at this API boundary to verify standard-codec refresh semantics too.
        NegotiatedVideoFormat = formats[format];
        LiCompleteVideoFrame(handle, DR_NEED_IDR);
        NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
        CHECK(idrRequests == (format == 0 ? 0 : 1));
        if (format == 0) {
            CHECK(LiPollNextVideoFrame(&handle, &du) && du->frameNumber == 2 && du->fullLength == (int)f->frameBytes);
            LiCompleteVideoFrame(handle, DR_OK);
        }
        CHECK(!LiPollNextVideoFrame(&handle, &du));
        RtpvCleanupQueue(&queue); stopVideoDepacketizer(); destroyVideoDepacketizer();
        PltDestroyCryptoContext(crypto);
    }
}

static void replayFragments(const Fixture* f, int mode) {
    defaultConfig();
    StreamConfig.pyrowaveProtocolVersion = 2;
    StreamConfig.packetSize = (int)f->packetSize;
    StreamConfig.fps = 30;
    unsigned slotSize = f->packetSize - 16, slotCount = (unsigned)(f->frameBytes / slotSize);
    CHECK(f->frameBytes % slotSize == 0 && slotCount <= 1020);
    bool expected[1020];
    for (unsigned i = 0; i < slotCount; i++) expected[i] = true;
    RTP_VIDEO_QUEUE queue;
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&queue);
    PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
    CHECK(crypto);
    for (unsigned j = 0; j < f->count; j++) {
        unsigned i = mode == 1 ? f->count - 1 - j : j;
        unsigned char* bytes = decryptShard(f, &f->datagrams[i], crypto);
        CHECK(bytes);
        unsigned fec = pyrowaveReadLe32(bytes + 28), data = fec >> 22, local = (fec >> 12) & 1023;
        bool parity = local >= data;
        if (mode >= 2 && parity) { free(bytes); continue; }
        if (!parity) {
            unsigned slot = pyrowaveReadLe16(bytes + 32 + 24);
            CHECK(slot < slotCount);
            if (((mode == 2 || mode == 5) && slot + 1 == slotCount) || (mode == 3 && slot == 0) ||
                (mode == 4 && ((bytes[27] >> 4) & 3) == 0)) {
                expected[slot] = false;
                free(bytes);
                continue;
            }
            if (mode == 1 && ((fec >> 4) & 255) && local == 0) { free(bytes); continue; }
        }
        submitPlain(&queue, f, bytes);
        if (mode == 1) submitShard(&queue, f, &f->datagrams[i], crypto);
    }
    unsigned expectedCount = 0;
    for (unsigned i = 0; i < slotCount; i++) if (expected[i]) expectedCount++;
    if (mode >= 2) {
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;
        CHECK(!LiPollNextVideoFrame(&handle, &du));
        if (mode == 5) {
            unsigned char* newer = decryptShard(f, &f->datagrams[0], crypto);
            CHECK(newer);
            putWord(newer + 20, 2);
            putWord(newer + 40, 2);
            submitPlain(&queue, f, newer); // partial frame 1 flushes before frame 2
        }
        else {
            clockUs += 99999;
            RtpvExpirePyrowaveFrame(&queue);
            CHECK(!LiPollNextVideoFrame(&handle, &du));
            clockUs++;
            RtpvExpirePyrowaveFrame(&queue);
        }
    }
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT du;
    CHECK(expectedCount && completedFrames == 1 && LiPollNextVideoFrame(&handle, &du));
    CHECK(du->frameNumber == 1 && du->frameType == FRAME_TYPE_IDR && du->fullLength == (int)(expectedCount * slotSize));
    unsigned received = 0;
    for (PLENTRY entry = du->bufferList; entry; entry = entry->next) {
        CHECK(entry->bufferType == BUFFER_TYPE_PICDATA && entry->length == (int)slotSize);
        unsigned slot = pyrowaveReadLe16((const unsigned char*)entry->data + 24);
        CHECK(slot < slotCount && expected[slot]);
        expected[slot] = false;
        CHECK(memcmp(entry->data, f->frame + slot * slotSize, slotSize) == 0);
        received++;
    }
    CHECK(received == expectedCount);
    LiCompleteVideoFrame(handle, DR_OK);
    CHECK(!LiPollNextVideoFrame(&handle, &du));
    if (mode >= 2 && mode != 5) CHECK(queue.stats.packetCountFecFailed == slotCount - expectedCount);
    PltDestroyCryptoContext(crypto);
    RtpvCleanupQueue(&queue);
    stopVideoDepacketizer();
    destroyVideoDepacketizer();
}

static void testFragmentConflicts(const Fixture* f) {
    for (int mode = 0; mode < 6; mode++) {
        defaultConfig();
        StreamConfig.pyrowaveProtocolVersion = 2;
        StreamConfig.packetSize = (int)f->packetSize;
        StreamConfig.fps = 30;
        RTP_VIDEO_QUEUE queue;
        initializeVideoDepacketizer(StreamConfig.packetSize);
        RtpvInitializeQueue(&queue);
        PPLT_CRYPTO_CONTEXT crypto = PltCreateCryptoContext();
        CHECK(crypto);
        unsigned char* bytes = decryptShard(f, &f->datagrams[0], crypto);
        CHECK(bytes);
        if (mode == 0) {
            submitPlain(&queue, f, bytes);
            bytes = decryptShard(f, &f->datagrams[0], crypto);
            bytes[100] ^= 1; // conflicting duplicate body
        }
        else if (mode == 1) bytes[32 + 35] = 1; // reserved header byte
        else if (mode == 2) { bytes[32 + 26] = 0xff; bytes[32 + 27] = 0xff; } // shard allocation ceiling
        else if (mode == 3) putWord(bytes + 32 + 48 + 12, 0xfffffffcU); // fragment offset overflow
        else if (mode == 4) bytes[32 + 40] ^= 1; // odd 420 native dimensions
        else {
            submitPlain(&queue, f, bytes);
            bytes = decryptShard(f, &f->datagrams[1], crypto);
            bytes[32 + 16] ^= 1; // inconsistent repeated timestamp
        }
        CHECK(submitPlain(&queue, f, bytes) == RTPF_RET_REJECTED);
        CHECK(queue.pyrowaveV2 == NULL);
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;
        CHECK(!LiPollNextVideoFrame(&handle, &du));
        nextIndependentFrame(&queue, f, crypto);
        PltDestroyCryptoContext(crypto);
        RtpvCleanupQueue(&queue);
        stopVideoDepacketizer();
        destroyVideoDepacketizer();
    }
}

int main(int argc, char** argv) {
    if (argc >= 3 && argc <= 5 &&
        (strcmp(argv[1], "--write-sdp") == 0 || strcmp(argv[1], "--check-sdp") == 0 ||
         strcmp(argv[1], "--write-v2-sdp") == 0 || strcmp(argv[1], "--check-v2-sdp") == 0)) {
        defaultConfig();
        StreamConfig.bitrate = 300000;
        StreamConfig.clientRefreshRateX100 = 5994;
        if (strstr(argv[1], "-v2-")) {
            StreamConfig.pyrowaveProtocolVersion = 2;
            strcpy(StreamConfig.pyrowaveProfile, "yuv444-p16-full-left-bt2020-bt2020-pq");
        }
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "encrypted") == 0) {
                StreamConfig.encryptionFlags = ENCFLG_VIDEO;
                EncryptionFeaturesSupported = SS_ENC_VIDEO;
            }
            else {
                CHECK(StreamConfig.pyrowaveProtocolVersion == 2 && strlen(argv[i]) < sizeof(StreamConfig.pyrowaveProfile));
                strcpy(StreamConfig.pyrowaveProfile, argv[i]);
            }
        }
        PYROWAVE_PROFILE_INFO profile;
        CHECK(pyrowaveParseProfile(&StreamConfig, &profile));
        StreamConfig.colorSpace = profile.matrix2020 ? COLORSPACE_REC_2020 : COLORSPACE_REC_709;
        StreamConfig.colorRange = profile.fullRange ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;
        int length;
        char* sdp = getSdpPayloadForStreamConfig(14, &length);
        CHECK(sdp);
        if (strncmp(argv[1], "--write", 7) == 0) {
            FILE* out = fopen(argv[2], "wb");
            CHECK(out && fwrite(sdp, 1, (size_t)length, out) == (size_t)length);
            CHECK(fclose(out) == 0);
        }
        else {
            FILE* expected = fopen(argv[2], "rb");
            CHECK(expected);
            for (int i = 0; i < length; i++) CHECK(fgetc(expected) == (unsigned char)sdp[i]);
            CHECK(fgetc(expected) == EOF && fclose(expected) == 0);
            printf("PASS byte-exact SDP %s\n", argv[2]);
        }
        free(sdp);
        return 0;
    }
    CHECK(argc == 1 || (argc >= 3 && argc % 2 == 1));
    testConfigAndSdp();
    testExtendedProfiles();
    testFecBounds();
    for (int a = 1; a < argc; a += 2) {
        Fixture f = loadFixture(argv[a], argv[a + 1]);
        testTampering(&f);
        testCounterWraps(&f);
        if (f.fragmented) {
            testFragmentConflicts(&f);
            for (int mode = 0; mode < 6; mode++) replayFragments(&f, mode);
        }
        else {
            testMalformedFrame(&f);
            testInconsistentBlock(&f);
            replayFixture(&f, 0);
            replayFixture(&f, 1);
            replayFixture(&f, 2);
        }
        testDecodeFailurePreservesIndependentQueue(&f);
        for (uint32_t i = 0; i < f.count; i++) free(f.datagrams[i].bytes);
        free(f.datagrams);
        free(f.frame);
        printf("PASS %s (exact, reorder/duplicate/FEC recovery, expiry, tampering)\n", argv[a]);
    }
    puts("PASS PyroWave config, SDP and standard-codec regression");
    return 0;
}
