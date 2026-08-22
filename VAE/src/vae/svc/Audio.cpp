#include "vaepch.h"
#include "vae/svc/Audio.h"

#include "vae/base/Log.h"

#include <algorithm>
#include <map>
#include <vector>

#include <miniaudio.h>

namespace vae::svc {

    namespace {
        // What a silent engine mixes at. Arbitrary, and it has to be *something*: with no device
        // there is nothing to ask, and a test that steps a second of audio has to know what a
        // second is.
        constexpr u32 kSilentRate = 48000;
        constexpr u32 kSilentChannels = 2;
    }

    // Everything that knows what miniaudio is, kept off the header: it is ninety thousand lines,
    // and one translation unit is enough for anyone.
    struct Audio::State {
        ma_engine engine{};
        bool started = false;
        std::map<Voice, ma_sound*> voices;
        std::vector<f32> scratch;

        ~State() {
            for (auto& [id, sound] : voices) {
                ma_sound_uninit(sound);
                delete sound;
            }
            if (started) ma_engine_uninit(&engine);
        }
    };

    Audio::Audio() = default;
    Audio::~Audio() = default;

    bool Audio::Ready() const { return m_State && m_State->started; }
    std::size_t Audio::Voices() const { return m_State ? m_State->voices.size() : 0; }

    bool Audio::Open(std::string* error) { return OpenWith(false, error); }
    bool Audio::OpenSilent(std::string* error) { return OpenWith(true, error); }

    bool Audio::OpenWith(bool silent, std::string* error) {
        if (Ready() && m_Silent == silent) return true;
        Close();

        auto state = CreateScope<State>();

        ma_engine_config config = ma_engine_config_init();
        if (silent) {
            // No device, no device thread, no clock but ours. `Step` reads the frames a sound card
            // would have taken, which is what makes a sound testable without one.
            config.noDevice = MA_TRUE;
            config.channels = kSilentChannels;
            config.sampleRate = kSilentRate;
        }

        if (const ma_result result = ma_engine_init(&config, &state->engine); result != MA_SUCCESS) {
            m_Problem = std::string("no audio: ") + ma_result_description(result);
            if (error) *error = m_Problem;
            VAE_CORE_WARN("audio: {}", m_Problem);
            return false;
        }

        state->started = true;
        m_State = std::move(state);
        m_Silent = silent;
        m_Problem.clear();
        ma_engine_set_volume(&m_State->engine, m_Master);

        if (!silent)
            VAE_CORE_INFO("audio: {} Hz, {} channels",
                          ma_engine_get_sample_rate(&m_State->engine),
                          ma_engine_get_channels(&m_State->engine));
        return true;
    }

    void Audio::Close() {
        m_State.reset();
        m_Silent = false;
    }

    Audio::Voice Audio::Play(const std::filesystem::path& file, f32 volume, bool loop) {
        // The first sound opens the device, and only the first. A project with nothing to play
        // never touches the hardware, and one whose hardware is missing says so once.
        if (!Ready() && !m_Problem.empty()) return 0;
        if (!Ready() && !Open(nullptr)) return 0;

        auto* sound = new ma_sound{};
        // Decoded rather than streamed, and unspatialized: an interface sound is small, and a
        // click that is quieter because the listener is at the origin is a bug nobody would guess.
        const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
        const ma_result result = ma_sound_init_from_file(&m_State->engine,
                                                         file.string().c_str(), flags,
                                                         nullptr, nullptr, sound);
        if (result != MA_SUCCESS) {
            delete sound;
            m_Problem = "cannot play " + file.filename().string() + ": "
                      + ma_result_description(result);
            VAE_CORE_WARN("audio: {}", m_Problem);
            return 0;
        }

        ma_sound_set_volume(sound, std::clamp(volume, 0.0f, 4.0f));
        ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
        ma_sound_start(sound);

        const Voice id = m_Next++;
        m_State->voices.emplace(id, sound);
        return id;
    }

    void Audio::Stop(Voice voice) {
        if (!m_State) return;
        const auto it = m_State->voices.find(voice);
        if (it == m_State->voices.end()) return;
        ma_sound_stop(it->second);
        ma_sound_uninit(it->second);
        delete it->second;
        m_State->voices.erase(it);
    }

    void Audio::StopAll() {
        if (!m_State) return;
        for (auto& [id, sound] : m_State->voices) {
            ma_sound_stop(sound);
            ma_sound_uninit(sound);
            delete sound;
        }
        m_State->voices.clear();
    }

    bool Audio::Playing(Voice voice) const {
        if (!m_State) return false;
        const auto it = m_State->voices.find(voice);
        if (it == m_State->voices.end()) return false;
        // Both, because they answer different questions: `is_playing` goes false when someone
        // stops it, `at_end` goes true when it runs out, and a caller means "can I still hear it".
        return ma_sound_is_playing(it->second) && !ma_sound_at_end(it->second);
    }

    void Audio::SetVolume(Voice voice, f32 volume) {
        if (!m_State) return;
        const auto it = m_State->voices.find(voice);
        if (it != m_State->voices.end())
            ma_sound_set_volume(it->second, std::clamp(volume, 0.0f, 4.0f));
    }

    void Audio::SetMasterVolume(f32 volume) {
        m_Master = std::clamp(volume, 0.0f, 4.0f);
        if (Ready()) ma_engine_set_volume(&m_State->engine, m_Master);
    }

    f32 Audio::MasterVolume() const { return m_Master; }

    std::size_t Audio::Pump() {
        if (!m_State) return 0;
        std::size_t reaped = 0;
        for (auto it = m_State->voices.begin(); it != m_State->voices.end(); ) {
            if (!ma_sound_at_end(it->second)) { ++it; continue; }
            ma_sound_uninit(it->second);
            delete it->second;
            it = m_State->voices.erase(it);
            ++reaped;
        }
        return reaped;
    }

    void Audio::Step(f32 seconds) {
        if (!Ready() || !m_Silent || seconds <= 0.0f) return;

        auto remaining = static_cast<ma_uint64>(seconds * kSilentRate);
        constexpr ma_uint64 kChunk = 1024;
        m_State->scratch.resize(kChunk * kSilentChannels);
        while (remaining > 0) {
            const ma_uint64 frames = std::min(remaining, kChunk);
            ma_engine_read_pcm_frames(&m_State->engine, m_State->scratch.data(), frames, nullptr);
            remaining -= frames;
        }
    }

}
