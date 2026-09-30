// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the v1 corpus writer, linking the production v2 serializer/packetizer.
#define PYROWAVE_FIXTURE_V2
#define main generate_legacy_main
#include "generate.cpp"
#undef main

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: generate-v2 INPUT_720P_PWVF OUTPUT_DIRECTORY");
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        reed_solomon_init();
        const auto encoded = read_file(argv[1]);
        const auto parsed = protocol::parse_frame(encoded);
        if (!parsed || parsed->packets.size() != 2) throw std::runtime_error("Expected two-packet 720p native fixture");
        for (bool large : {false, true}) {
            auto packets = parsed->packets;
            // Repeated native blocks are deliberately a transport-only stress
            // frame, not a decodable picture. Sideband metadata below is also
            // synthetic; GPU qualification must use encoder-derived sideband.
            if (large) for (int i = 0; i < 10; i++) packets.push_back(parsed->packets.back());
            const std::array<std::uint32_t, 3> active{0xffffffffU, 0xffffffffU, 0x3fffU};
            protocol::partial_metadata_t metadata{{1, 1, 1, 1, static_cast<std::uint32_t>(packets.size())}, 3, 78, active};
            for (unsigned fec : {0u, 1u, 20u, 100u, 255u}) {
                if (large && fec != 20) continue;
                protocol::transport_config_t config;
                config.fragmented = true;
                config.fec_percentage = fec;
                config.critical_fec_percentage = fec ? std::max(40u, fec) : 0;
                config.min_fec_packets = 2;
                const auto frame = protocol::serialize_fragmented_frame(1, 123456789, packets, metadata, config, protocol::max_frame_size);
                if (!frame) throw std::runtime_error("Production PWPF serialization failed");
                const std::string name = large ? "v2-four-block" : "v2-720p-fec" + std::to_string(fec);
                std::ofstream expected(directory / (name + ".pwpf"), std::ios::binary);
                expected.write(reinterpret_cast<const char*>(frame->data()), frame->size());
                if (!expected) throw std::runtime_error("Expected PWPF write failed");
                generate(directory, name.c_str(), *frame, false, fec, true);
                generate(directory, name.c_str(), *frame, true, fec, true);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
