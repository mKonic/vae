#pragma once

#include "vae/base/Base.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// A real WAV file, written by the test rather than vendored. A binary fixture in the repo is a
// thing nobody can read in a diff, and what the audio tests are actually checking is that a file on
// disk becomes a sound that starts and ends — not that our copy of a .wav is intact.
namespace tone {

    using namespace vae;

    inline std::filesystem::path WriteTone(const std::filesystem::path& path, f32 seconds,
                                           u32 rate = 48000) {
        const auto put = [](std::vector<u8>& out, const char* tag) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(tag[i]));
        };
        const auto put32 = [](std::vector<u8>& out, u32 value) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((value >> (8 * i)) & 0xFF));
        };
        const auto put16 = [](std::vector<u8>& out, u16 value) {
            for (int i = 0; i < 2; ++i) out.push_back(static_cast<u8>((value >> (8 * i)) & 0xFF));
        };

        const auto frames = static_cast<u32>(seconds * static_cast<f32>(rate));
        const u32 data = frames * 2;                    // mono, 16-bit

        std::vector<u8> wav;
        put(wav, "RIFF"); put32(wav, 36 + data); put(wav, "WAVE");
        put(wav, "fmt "); put32(wav, 16);
        put16(wav, 1);                                  // PCM
        put16(wav, 1);                                  // mono
        put32(wav, rate);
        put32(wav, rate * 2);                           // bytes per second
        put16(wav, 2);                                  // block align
        put16(wav, 16);                                 // bits per sample
        put(wav, "data"); put32(wav, data);

        for (u32 i = 0; i < frames; ++i) {
            // 440 Hz at a quarter of full scale. A tone rather than silence so that anyone who
            // plays one of these by hand hears something and knows the writer works.
            const f64 phase = 2.0 * 3.14159265358979 * 440.0 * i / rate;
            const auto sample = static_cast<i16>(8000.0 * std::sin(phase));
            wav.push_back(static_cast<u8>(sample & 0xFF));
            wav.push_back(static_cast<u8>((sample >> 8) & 0xFF));
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(wav.data()),
                  static_cast<std::streamsize>(wav.size()));
        return path;
    }

}
