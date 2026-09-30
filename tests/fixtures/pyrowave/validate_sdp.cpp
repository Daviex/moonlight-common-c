// SPDX-License-Identifier: GPL-3.0-or-later
// Feed actual client-generated SDP values into the pinned production server's
// negotiation function. This is an offline contract test, not an RTSP session.
#include "src/pyrowave_negotiation.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using attributes_t = std::map<std::string, std::string>;
static void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
static int number(const attributes_t& attributes, const char* name) {
    const auto& text = attributes.at(name);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && text[0] >= '0' && text[0] <= '9' && error == std::errc{} && end == text.data() + text.size(),
            "Invalid numeric SDP attribute");
    return value;
}
static attributes_t load(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read generated SDP");
    attributes_t attributes;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.starts_with("a=")) continue;
        // Pinned rtsp.cpp removes exactly one legacy Moonlight trailing space.
        if (!line.empty() && line.back() == ' ') line.pop_back();
        const auto colon = line.find(':');
        require(colon != std::string::npos, "SDP attribute lacks separator");
        const auto [entry, inserted] = attributes.emplace(line.substr(2, colon - 2), line.substr(colon + 1));
        require(inserted || entry->second == line.substr(colon + 1), "Conflicting SDP attributes");
    }
    return attributes;
}
static pyrowave::negotiation::result_t validate(const attributes_t& a) {
    namespace protocol = pyrowave::protocol;
    require(number(a, "x-nv-vqos[0].bitStreamFormat") == 3, "Wrong wire codec");
    const int configured = number(a, "x-ml-video.configuredBitrateKbps");
    const int maximum = number(a, "x-nv-vqos[0].bw.maximumBitrateKbps");
#ifdef PYROWAVE_VALIDATE_V2
    require(configured >= 0 && maximum >= 0, "Raw bitrate must be nonnegative signed integers");
#else
    require(configured >= 0 && configured <= 800000 && maximum >= 0 && maximum <= 800000, "Raw bitrate exceeds server ceiling");
#endif
    const int channels = number(a, "x-nv-audio.surround.numChannels");
    require(channels >= 1 && channels <= 8, "Invalid audio channel count");
    const int encryption = number(a, "x-ss-general.encryptionEnabled");
    require(encryption >= 0 && encryption <= 7, "Invalid encryption flags");
    // Same reservations as pinned rtsp.cpp with no host bitrate override,
    // normal-quality stereo (RTSP Host=0.0.0.0), and host FEC=20%.
    const int wire = configured ? configured : maximum;
    auto reserve = [&](int bitrate) {
        bitrate -= std::min(96 * channels, bitrate / 5);
        return bitrate - std::min(500, bitrate / 10);
    };
    protocol::transport_config_t transport;
    transport.packet_size = number(a, "x-nv-video[0].packetSize");
    transport.path_mtu = number(a, "x-vp-pyrowave.pathMtu");
    transport.ip_header_size = 40; // conservative IPv6 budget also accepts IPv4
    transport.fec_percentage = 20;
    transport.min_fec_packets = number(a, "x-nv-vqos[0].fec.minRequiredFecPackets");
    transport.encrypted = (encryption & 2) != 0;
#ifdef PYROWAVE_VALIDATE_V2
    transport.fragmented = a.contains("x-vp-pyrowave.version") && a.at("x-vp-pyrowave.version") == "2";
    transport.critical_fec_percentage = 40;
#endif
    auto extension = [&](const char* name) -> std::string_view {
        auto found = a.find(name);
        return found == a.end() ? std::string_view{} : found->second;
    };
    return pyrowave::negotiation::negotiate({
        .width = number(a, "x-nv-video[0].clientViewportWd"),
        .height = number(a, "x-nv-video[0].clientViewportHt"),
        .framerate = number(a, "x-nv-video[0].maxFPS"),
        .framerate_x100 = number(a, "x-nv-video[0].clientRefreshRateX100"),
        .encoder_bitrate_kbps = reserve(static_cast<int>(wire / 1.25f)),
        .video_wire_bitrate_kbps = reserve(wire),
        .dynamic_range = number(a, "x-nv-video[0].dynamicRangeMode"),
        .chroma_sampling = number(a, "x-ss-video[0].chromaSamplingType"),
        .encoder_csc_mode = number(a, "x-nv-video[0].encoderCscMode"),
        .version = extension("x-vp-pyrowave.version"),
        .bitstream_revision = extension("x-vp-pyrowave.bitstreamRevision"),
        .profile = extension("x-vp-pyrowave.profile"),
        .host_enabled = true,
        .adapter_supported = true,
    }, transport);
}
int main(int argc, char** argv) {
    try {
        require(argc >= 2, "Usage: validate-sdp CLIENT_ANNOUNCE_SDP...");
        for (int i = 1; i < argc; ++i) {
            const auto attributes = load(argv[i]);
            const auto result = validate(attributes);
            require(result.accepted, result.reason.c_str());
            std::cout << argv[i] << ": ACCEPT fps_x100=" << result.fps_x100
                      << " frame_budget=" << result.frame_budget << " wire_bytes=" << result.wire_byte_budget
                      << " encoder_target=" << result.encoder_target_bytes << '\n';
            for (const auto& [key, bad] : attributes_t{
                     {"x-vp-pyrowave.version", "3"}, {"x-vp-pyrowave.bitstreamRevision", "latest"},
                     {"x-vp-pyrowave.profile", "sdr-bt709-full-left-444"},
                     {"x-nv-video[0].dynamicRangeMode", attributes.at("x-nv-video[0].dynamicRangeMode") == "0" ? "1" : "0"},
                     {"x-ss-video[0].chromaSamplingType", attributes.at("x-ss-video[0].chromaSamplingType") == "0" ? "1" : "0"},
                     {"x-nv-video[0].encoderCscMode", attributes.at("x-nv-video[0].encoderCscMode") == "2" ? "3" : "2"},
                     {"x-nv-video[0].clientViewportWd", "0"},
                     {"x-nv-video[0].clientRefreshRateX100", "12000"}, {"x-vp-pyrowave.pathMtu", "1279"}}) {
                auto invalid = attributes;
                invalid[key] = bad;
                require(!validate(invalid).accepted, "Server negotiation accepted a negative mutation");
            }
            for (const auto* key : {"x-vp-pyrowave.version", "x-vp-pyrowave.bitstreamRevision", "x-vp-pyrowave.profile"}) {
                auto missing = attributes;
                missing.erase(key);
                require(!validate(missing).accepted, "Server negotiation accepted a missing extension");
            }
            std::cout << "PASS 12 server-negotiation negative cases\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
