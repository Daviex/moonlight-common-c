// Private v2 receiver; included only by RtpVideoQueue.c. One bounded frame with
// independent FEC blocks permits cross-block reordering and partial delivery.
// Missing native bytes are never synthesized: only validated PWPF slots leave.
#pragma once

typedef struct _PYROWAVE_V2_BLOCK {
    unsigned char* packets[255];
    uint16_t data, parity, percentage, sequenceBase, received, dataReceived;
    uint16_t slotBase;
    bool seen, baseKnown;
} PYROWAVE_V2_BLOCK;

typedef struct _PYROWAVE_V2_FRAME {
    PYROWAVE_V2_BLOCK blocks[4];
    uint32_t frame, timestamp;
    uint64_t started;
    uint16_t blockCount, shardCount;
    unsigned char identity[48];
    bool hasIdentity;
} PYROWAVE_V2_FRAME;

static void pyrowaveV2Cleanup(PRTP_VIDEO_QUEUE queue) {
    if (!queue->pyrowaveV2) return;
    for (unsigned b = 0; b < 4; b++)
        for (unsigned i = 0; i < 255; i++) free(queue->pyrowaveV2->blocks[b].packets[i]);
    free(queue->pyrowaveV2);
    queue->pyrowaveV2 = NULL;
}

static void pyrowaveV2Discard(PRTP_VIDEO_QUEUE queue) {
    queue->stats.packetCountInvalid++;
    queue->currentFrameNumber = queue->pyrowaveV2->frame + 1;
    pyrowaveV2Cleanup(queue);
}

static bool pyrowaveV2AdmitSlot(PYROWAVE_V2_FRAME* frame, unsigned b, unsigned local, const unsigned char* slot) {
    unsigned slotSize = (unsigned)StreamConfig.packetSize - 16;
    if (!pyrowaveValidateFragment(slot, slotSize, frame->frame)) return false;
    unsigned count = pyrowaveReadLe16(slot + 26), index = pyrowaveReadLe16(slot + 24);
    if ((uint64_t)count * slotSize > pyrowaveFrameLimit(&StreamConfig) || index < local) return false;
    if (!frame->hasIdentity) {
        memcpy(frame->identity, slot, 48);
        frame->hasIdentity = true;
        frame->shardCount = (uint16_t)count;
    }
    else if (memcmp(frame->identity + 8, slot + 8, 16) || memcmp(frame->identity + 26, slot + 26, 4) ||
             memcmp(frame->identity + 32, slot + 32, 16)) return false;
    PYROWAVE_V2_BLOCK* block = &frame->blocks[b];
    unsigned base = index - local;
    if ((block->baseKnown && block->slotBase != base) || base + block->data > count || (b == 0 && base != 0)) return false;
    block->slotBase = (uint16_t)base;
    block->baseKnown = true;
    unsigned totalData = 0;
    for (unsigned i = 0; i < frame->blockCount; i++) {
        PYROWAVE_V2_BLOCK* other = &frame->blocks[i];
        if (other->seen) totalData += other->data;
        if (!other->baseKnown || i == b) continue;
        if (i < b ? other->slotBase + other->data > base : base + block->data > other->slotBase) return false;
    }
    return totalData <= count;
}

static void pyrowaveV2Flush(PRTP_VIDEO_QUEUE queue) {
    PYROWAVE_V2_FRAME* frame = queue->pyrowaveV2;
    if (!frame) return;
    const unsigned char* slots[1020];
    unsigned count = 0;
    // Block-local sequence is canonical; gaps remain absent rather than padded.
    for (unsigned b = 0; b < frame->blockCount; b++) {
        PYROWAVE_V2_BLOCK* block = &frame->blocks[b];
        for (unsigned i = 0; i < block->data; i++)
            if (block->packets[i]) slots[count++] = block->packets[i] + 32;
    }
    if (count) {
        queuePyrowaveFragmentFrame(frame->frame, slots, count, (unsigned)StreamConfig.packetSize - 16,
                                   frame->started, frame->timestamp);
    }
    if (frame->hasIdentity && count < frame->shardCount) queue->stats.packetCountFecFailed += frame->shardCount - count;
    queue->currentFrameNumber = frame->frame + 1;
    pyrowaveV2Cleanup(queue);
}

static void pyrowaveV2Expire(PRTP_VIDEO_QUEUE queue) {
    if (queue->pyrowaveV2 && PltGetMicroseconds() - queue->pyrowaveV2->started >= 100000) pyrowaveV2Flush(queue);
}

static bool pyrowaveV2Recover(PRTP_VIDEO_QUEUE queue, unsigned b) {
    PYROWAVE_V2_FRAME* frame = queue->pyrowaveV2;
    PYROWAVE_V2_BLOCK* block = &frame->blocks[b];
    if (block->dataReceived == block->data || block->received < block->data || !block->parity) return true;
    unsigned total = block->data + block->parity, bytes = (unsigned)StreamConfig.packetSize + 16;
    unsigned char* packets[255];
    unsigned char marks[255];
    memset(packets, 0, sizeof(packets));
    bool valid = true;
    for (unsigned i = 0; i < total; i++) {
        marks[i] = block->packets[i] == NULL;
        packets[i] = marks[i] ? malloc(bytes) : block->packets[i];
        if (!packets[i]) valid = false;
    }
    reed_solomon* rs = valid ? reed_solomon_new(block->data, block->parity) : NULL;
    valid = rs && reed_solomon_decode(rs, packets, marks, (int)total, (int)bytes) == 0;
    if (rs) reed_solomon_release(rs);
    if (valid) {
        for (unsigned i = 0; i < block->data; i++) {
            if (marks[i] && !pyrowaveV2AdmitSlot(frame, b, i, packets[i] + 32)) { valid = false; break; }
        }
    }
    for (unsigned i = 0; i < total; i++) {
        if (!marks[i]) continue;
        if (valid && i < block->data) {
            // Transport headers are rewritten after server FEC encoding, so only
            // recovered PWPF payloads are authoritative. Retain a canonical header
            // for duplicate comparison against a later original data packet.
            memset(packets[i], 0, 16);
            PRTP_PACKET rtp = (PRTP_PACKET)packets[i];
            rtp->header = 0x90;
            rtp->sequenceNumber = (uint16_t)(block->sequenceBase + i);
            rtp->timestamp = frame->timestamp;
            uint32_t fec = (i << 12) | ((uint32_t)block->data << 22) | ((uint32_t)block->percentage << 4);
            for (unsigned n = 0; n < 4; n++) {
                packets[i][20 + n] = (unsigned char)(frame->frame >> (8 * n));
                packets[i][28 + n] = (unsigned char)(fec >> (8 * n));
            }
            packets[i][27] = (unsigned char)((b << 4) | ((frame->blockCount - 1) << 6));
            block->packets[i] = packets[i];
            block->dataReceived++;
            block->received++;
            queue->stats.packetCountFecRecovered++;
        }
        else free(packets[i]);
    }
    return valid;
}

static int pyrowaveV2AddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length) {
    const unsigned char* raw = (const unsigned char*)packet;
    uint32_t index = pyrowaveReadLe32(raw + 20), fec = pyrowaveReadLe32(raw + 28);
    unsigned b = (raw[27] >> 4) & 3, blocks = (raw[27] >> 6) + 1;
    unsigned data = fec >> 22, local = (fec >> 12) & 1023, percent = (fec >> 4) & 255;
    unsigned parity = (data * percent + 99) / 100;
    if ((uint64_t)data * (StreamConfig.packetSize - 16) > pyrowaveFrameLimit(&StreamConfig)) return RTPF_RET_REJECTED;
    uint16_t sequenceBase = (uint16_t)(packet->sequenceNumber - local);
    pyrowaveV2Expire(queue);
    if (isBefore32(index, queue->currentFrameNumber)) return RTPF_RET_REJECTED;
    if (queue->pyrowaveV2 && queue->pyrowaveV2->frame != index) pyrowaveV2Flush(queue);
    if (!queue->pyrowaveV2) {
        // The fixed state plus at most four 255-shard blocks bounds allocation
        // even when only authenticated parity arrives and PWPF is not yet known.
        queue->pyrowaveV2 = calloc(1, sizeof(*queue->pyrowaveV2));
        if (!queue->pyrowaveV2) return RTPF_RET_REJECTED;
        queue->pyrowaveV2->frame = index;
        queue->pyrowaveV2->timestamp = packet->timestamp;
        queue->pyrowaveV2->started = PltGetMicroseconds();
        queue->pyrowaveV2->blockCount = (uint16_t)blocks;
        queue->currentFrameNumber = index;
        connectionSawFrame(index);
    }
    PYROWAVE_V2_FRAME* frame = queue->pyrowaveV2;
    PYROWAVE_V2_BLOCK* block = &frame->blocks[b];
    if (frame->timestamp != packet->timestamp || frame->blockCount != blocks ||
        (block->seen && (block->data != data || block->parity != parity || block->percentage != percent || block->sequenceBase != sequenceBase))) {
        pyrowaveV2Discard(queue);
        return RTPF_RET_REJECTED;
    }
    if (!block->seen) {
        block->data = (uint16_t)data;
        block->parity = (uint16_t)parity;
        block->percentage = (uint16_t)percent;
        block->sequenceBase = sequenceBase;
        block->seen = true;
        queue->stats.packetCountVideo += data;
        queue->stats.packetCountFec += parity;
    }
    if (block->packets[local]) {
        if (memcmp(block->packets[local], packet, (size_t)length)) pyrowaveV2Discard(queue);
        return RTPF_RET_REJECTED;
    }
    if (local < data && !pyrowaveV2AdmitSlot(frame, b, local, raw + 32)) {
        pyrowaveV2Discard(queue);
        return RTPF_RET_REJECTED;
    }
    block->packets[local] = (unsigned char*)packet;
    block->received++;
    if (local < data) block->dataReceived++;
    if (!pyrowaveV2Recover(queue, b)) {
        pyrowaveV2Discard(queue);
        return RTPF_RET_QUEUED; // packet ownership was already taken
    }
    unsigned total = 0;
    bool complete = frame->hasIdentity;
    for (unsigned i = 0; i < frame->blockCount; i++) {
        PYROWAVE_V2_BLOCK* current = &frame->blocks[i];
        complete &= current->seen && current->baseKnown && current->dataReceived == current->data && current->slotBase == total;
        total += current->data;
    }
    if (complete && total == frame->shardCount) pyrowaveV2Flush(queue);
    return RTPF_RET_QUEUED;
}
