#pragma once

#include "Limelight.h"
#include <stddef.h>
#include <string.h>
#include <limits.h>

// VibePollo v1/v2 limits bound transport memory before envelope parsing in the
// renderer. The server additionally enforces its exact FEC/wire budget.
#define PYROWAVE_MAX_FRAME_BYTES (4U * 1024U * 1024U)
#define PYROWAVE_MAX_FEC_SHARDS 255U
#define PYROWAVE_MAX_FEC_BLOCKS 4U

static inline uint32_t pyrowaveReadLe32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint16_t pyrowaveReadLe16(const unsigned char* p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

typedef struct _PYROWAVE_PROFILE_INFO {
    bool chroma444, highPrecision, fullRange, chromaLeft, primaries2020, matrix2020, transferPq;
} PYROWAVE_PROFILE_INFO;

static inline int pyrowaveVersion(const STREAM_CONFIGURATION* config) {
    return config->pyrowaveProtocolVersion ? config->pyrowaveProtocolVersion : PYROWAVE_PROTOCOL_VERSION;
}

static inline const char* pyrowaveProfileName(const STREAM_CONFIGURATION* config) {
    return config->pyrowaveProfile[0] ? config->pyrowaveProfile : PYROWAVE_PROFILE;
}

static inline bool pyrowaveParseProfile(const STREAM_CONFIGURATION* config, PYROWAVE_PROFILE_INFO* profile) {
    if (!memchr(config->pyrowaveProfile, 0, sizeof(config->pyrowaveProfile))) return false;
    const char* name = pyrowaveProfileName(config);
    memset(profile, 0, sizeof(*profile));
    if (strcmp(name, PYROWAVE_PROFILE) == 0) {
        profile->fullRange = profile->chromaLeft = true;
        return true;
    }
    const char* choices[7][2] = {{"yuv420", "yuv444"}, {"p8", "p16"}, {"limited", "full"},
                               {"center", "left"}, {"bt709", "bt2020"}, {"bt709", "bt2020"}, {"bt709", "pq"}};
    bool* fields[] = {&profile->chroma444, &profile->highPrecision, &profile->fullRange,
                     &profile->chromaLeft, &profile->primaries2020, &profile->matrix2020, &profile->transferPq};
    for (int i = 0; i < 7; i++) {
        const char* end = strchr(name, '-');
        size_t length = end ? (size_t)(end - name) : strlen(name);
        if ((i < 6) != (end != NULL)) return false;
        if (strlen(choices[i][0]) == length && memcmp(name, choices[i][0], length) == 0) *fields[i] = false;
        else if (strlen(choices[i][1]) == length && memcmp(name, choices[i][1], length) == 0) *fields[i] = true;
        else return false;
        if (end) name = end + 1;
    }
    return true;
}

static inline bool pyrowaveValidateConfig(const STREAM_CONFIGURATION* config) {
    PYROWAVE_PROFILE_INFO profile;
    int version = pyrowaveVersion(config);
    if (!pyrowaveParseProfile(config, &profile) || (version != 1 && version != 2) ||
        (version == 1 && strcmp(pyrowaveProfileName(config), PYROWAVE_PROFILE) != 0)) return false;
    int mtu = config->pyrowavePathMtu ? config->pyrowavePathMtu : 1280;
    uint64_t pixels = (uint64_t)(unsigned)config->width * (unsigned)config->height;
    uint64_t planes = (profile.chroma444 ? pixels * 3 : pixels + pixels / 2) * (profile.highPrecision ? 2 : 1);
    uint64_t fpsX100 = config->clientRefreshRateX100 ? (uint64_t)(unsigned)config->clientRefreshRateX100 : (uint64_t)(unsigned)config->fps * 100;
    return config->supportedVideoFormats == VIDEO_FORMAT_PYROWAVE &&
           config->width >= 1 && config->width <= 16384 && config->height >= 1 && config->height <= 16384 &&
           (profile.chroma444 || !((config->width | config->height) & 1)) && planes <= 512ULL * 1024 * 1024 &&
           config->fps >= 1 && config->fps <= INT_MAX / 1000 && config->clientRefreshRateX100 >= 0 &&
           fpsX100 <= INT_MAX / 10 && (fpsX100 + 50) / 100 == (unsigned)config->fps && config->bitrate > 0 &&
           config->colorSpace == (profile.matrix2020 ? COLORSPACE_REC_2020 : COLORSPACE_REC_709) &&
           config->colorRange == (profile.fullRange ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED) &&
           mtu >= 1280 && mtu <= 1500;
}

static inline bool pyrowaveValidatePacketSize(int packetSize, int mtu, int ipHeaderSize, bool encrypted) {
    if (!mtu) mtu = 1280;
    return mtu >= 1280 && mtu <= 1500 && packetSize >= 200 && packetSize <= 1500 &&
           (ipHeaderSize == 20 || ipHeaderSize == 40) &&
           packetSize + 16 + (encrypted ? 32 : 0) + 8 + ipHeaderSize <= mtu;
}

static inline uint32_t pyrowaveFrameLimit(const STREAM_CONFIGURATION* config) {
    if (config->packetSize < 200 || config->packetSize > 1500 || config->fps < 1) return 0;
    uint32_t transportLimit = PYROWAVE_MAX_FEC_BLOCKS * PYROWAVE_MAX_FEC_SHARDS *
                              (uint32_t)(config->packetSize - 16) - (pyrowaveVersion(config) == 2 ? 0 : 8);
    // An upper bound on the server's per-frame video-wire budget. Audio/control
    // reservations and actual FEC overhead can only reduce this bound.
    uint32_t fpsX100 = config->clientRefreshRateX100 > 0 ? (uint32_t)config->clientRefreshRateX100 : (uint32_t)config->fps * 100;
    uint64_t wireLimit = (uint64_t)config->bitrate * 1000 * 100 / (8 * (uint64_t)fpsX100);
    if (wireLimit < transportLimit) transportLimit = (uint32_t)wireLimit;
    return transportLimit < PYROWAVE_MAX_FRAME_BYTES ? transportLimit : PYROWAVE_MAX_FRAME_BYTES;
}

// Validate an authenticated (when encryption is enabled) complete video shard.
// RTP sequence/timestamp may already be host endian; NV fields are still LE.
static inline bool pyrowaveValidateShard(const void* packet, size_t length, int packetSize) {
    const unsigned char* p = (const unsigned char*)packet;
    if (packetSize < 200 || packetSize > 1500 || length != (size_t)packetSize + 16 || p[0] != 0x90) return false;
    uint32_t fec = pyrowaveReadLe32(p + 28);
    uint32_t count = fec >> 22;
    uint32_t index = (fec >> 12) & 0x3ff;
    uint32_t percentage = (fec >> 4) & 0xff;
    uint32_t parity = (count * percentage + 99) / 100;
    uint32_t block = (p[27] >> 4) & 3;
    uint32_t lastBlock = p[27] >> 6;
    if (!count || count + parity > PYROWAVE_MAX_FEC_SHARDS || index >= count + parity ||
        block > lastBlock || (p[27] & 15) || (fec & 15)) return false;
    if (index < count) {
        unsigned char expectedFlags = (unsigned char)(1 | (index == 0 ? 4 : 0) | (index + 1 == count ? 2 : 0));
        if (p[24] != expectedFlags || p[25] != 0 || p[26] != 0x10) return false;
    }
    return true;
}

static inline bool pyrowaveAuthenticatedFrameMatches(const void* packet, size_t length, uint32_t prefixFrame) {
    return length >= 32 && pyrowaveReadLe32((const unsigned char*)packet + 20) == prefixFrame;
}

// Fixed-slot admission before retaining transport buffers. Native packet
// reconstruction and aggregate item allocation are the renderer's next gate.
static inline bool pyrowaveValidateFragment(const unsigned char* p, size_t length, uint32_t frame) {
    if (length < 68 || length > 1484 || memcmp(p, "PWPF", 4) ||
        pyrowaveReadLe16(p + 4) != 2 || pyrowaveReadLe16(p + 6) != 48 ||
        pyrowaveReadLe32(p + 8) != frame || p[35] != 0) return false;
    uint32_t count = pyrowaveReadLe16(p + 26), packets = pyrowaveReadLe16(p + 28);
    uint32_t records = pyrowaveReadLe16(p + 30), critical = pyrowaveReadLe16(p + 32);
    uint32_t active = pyrowaveReadLe32(p + 36), bands = p[34];
    if (!count || count > 1020 || pyrowaveReadLe16(p + 24) >= count ||
        !packets || packets > 4096 || !critical || critical > packets || bands > 4 || active > 32768 * 32 ||
        !records || records > (length - 48) / 20) return false;
    uint32_t first = pyrowaveReadLe32(p + 40), second = pyrowaveReadLe32(p + 44);
    uint32_t width = (first & 0x3fff) + 1, height = ((first >> 14) & 0x3fff) + 1;
    if (!(first & 0x80000000U) || (second & 0x03000000U) ||
        (!(second & 0x04000000U) && ((width | height) & 1))) return false;
    uint32_t expectedActive = 0;
    if (bands) {
        uint32_t aw = (width + 31) & ~31U, ah = (height + 31) & ~31U;
        if (aw < 128) aw = 128;
        if (ah < 128) ah = 128;
        int last = 5 - (bands > 2 ? (int)bands - 1 : 1);
        for (int level = 4; level >= last; level--) {
            uint32_t blocks = (((aw >> (level + 1)) + 31) / 32) * (((ah >> (level + 1)) + 31) / 32);
            expectedActive += blocks * (level == 4 ? (bands == 1 ? 9 : 12) : 9);
        }
    }
    if (active != expectedActive) return false;
    uint32_t manifestSize = 20 + ((active + 31) / 32) * 4;
    size_t offset = 48;
    for (uint32_t i = 0; i < records; i++) {
        if (length - offset < 16) return false;
        uint32_t kind = p[offset], flags = p[offset + 1], bytes = pyrowaveReadLe16(p + offset + 2);
        uint32_t index = pyrowaveReadLe32(p + offset + 4), size = pyrowaveReadLe32(p + offset + 8);
        uint32_t position = pyrowaveReadLe32(p + offset + 12);
        offset += 16;
        if (kind > 1 || flags > 1 || !bytes || (bytes & 3) || bytes > length - offset ||
            !size || size > PYROWAVE_MAX_FRAME_BYTES || (size & 3) || (position & 3) || position > size || bytes > size - position ||
            (kind == 1 ? index != 0 || size != manifestSize || flags != 1 :
                         index >= packets || size < 8 || flags != (index < critical))) return false;
        offset += bytes;
    }
    for (; offset < length; offset++) if (p[offset]) return false;
    return true;
}
