// Compile the real depacketizer and seed only its counters to simulate a
// long-running stream crossing integer wraps without sending billions of frames.
#include "../src/VideoDepacketizer.c"

void testSeedDepacketizerCounters(uint32_t frame, uint32_t streamPacket) {
    nextFrameNumber = frame;
    startFrameNumber = frame;
    lastPacketInStream = U24(streamPacket - 1);
}
