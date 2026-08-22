#pragma once

#include "vae/base/Base.h"

#include <filesystem>
#include <string>

namespace vae::svc {

    // Sound, and only the three things an application actually does with it: start one, stop one,
    // and know whether it is still going. Not a mixer, not a DSP graph, no buses, no effects — an
    // app plays a click when a button is pressed and a chime when something finishes, and every
    // line beyond that is a line nobody asked for.
    //
    // miniaudio underneath, which is why this is one file: ALSA, PulseAudio, WASAPI and CoreAudio
    // behind one interface means the Windows port inherits working audio rather than owing it.
    //
    // Nothing here opens a device until something is played. A project with no sound in it never
    // touches the sound card, which matters more than it sounds like: on a machine with no audio at
    // all, an engine that opens a device at startup is an engine that logs an error at startup.
    class Audio {
    public:
        // A sound that is playing. Zero is "no voice" and is what a failed Play returns; a handle
        // is never reused, so holding one past the end of the sound is safe and reads as stopped.
        using Voice = u64;

        Audio();
        ~Audio();
        Audio(const Audio&) = delete;
        Audio& operator=(const Audio&) = delete;

        // The machine's default output. Play calls this by itself the first time it is needed.
        bool Open(std::string* error = nullptr);
        // The same engine with nothing behind it: it decodes, mixes and advances, and no device
        // thread drives it — `Step` does, by hand. What the test suite uses, so that a suite can
        // check a real decode of a real file on a machine with no sound card and no timing race.
        bool OpenSilent(std::string* error = nullptr);
        void Close();

        bool Ready() const;
        bool Silent() const { return m_Silent; }
        // Why there is no sound, or empty. A silent app with no explanation is a bug report with
        // nothing in it.
        const std::string& Problem() const { return m_Problem; }

        // Starts a file. Decoded up front rather than streamed: these are interface sounds, they
        // are small, and the resource manager keeps the decoded data so the second press of a
        // button costs nothing.
        Voice Play(const std::filesystem::path& file, f32 volume = 1.0f, bool loop = false);
        void Stop(Voice voice);
        void StopAll();
        bool Playing(Voice voice) const;
        void SetVolume(Voice voice, f32 volume);

        // Everything at once, which is what a volume slider means.
        void SetMasterVolume(f32 volume);
        f32 MasterVolume() const;

        // Releases whatever has finished. Once a frame; a sound that has ended costs a decoded
        // buffer until this runs.
        std::size_t Pump();
        // How many sounds are alive right now — playing, or finished and not yet reaped.
        std::size_t Voices() const;

        // Advances a silent engine by hand. Does nothing to a real one — a device thread is
        // already doing it, and a second caller would fight it.
        void Step(f32 seconds);

    private:
        struct State;

        bool OpenWith(bool silent, std::string* error);

        Scope<State> m_State;
        std::string m_Problem;
        bool m_Silent = false;
        f32 m_Master = 1.0f;
        Voice m_Next = 1;
    };

}
