#include "Test.h"
#include "Tone.h"

#include "vae/svc/Audio.h"

#include <fstream>

using namespace vae;

namespace {

    namespace fs = std::filesystem;

    fs::path Scratch() {
        const fs::path dir = fs::temp_directory_path() / "vae-audio-tests";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    fs::path WriteTone(const std::string& name, f32 seconds) {
        return tone::WriteTone(Scratch() / name, seconds);
    }

}

TEST(audio, a_silent_engine_opens_on_a_machine_with_no_sound_card) {
    // The whole reason the silent mode exists: every check below runs on a build machine, in a
    // container, over ssh. An audio suite that needs a device is an audio suite nobody runs.
    svc::Audio audio;
    std::string error;
    CHECK_MESSAGE(audio.OpenSilent(&error), error);
    CHECK(audio.Ready());
    CHECK(audio.Silent());
    CHECK(audio.Problem().empty());
}

TEST(audio, a_sound_plays_for_as_long_as_it_is_and_then_stops) {
    const fs::path tone = WriteTone("half-second.wav", 0.5f);

    svc::Audio audio;
    std::string error;
    CHECK_MESSAGE(audio.OpenSilent(&error), error);

    const svc::Audio::Voice voice = audio.Play(tone);
    CHECK_MESSAGE(voice != 0, audio.Problem());
    CHECK(audio.Playing(voice));
    CHECK(audio.Voices() == 1);

    // A quarter of the way in it is still going. Stepped by hand rather than slept through: the
    // engine's clock is the frames it has been asked for, so this is exact rather than hopeful.
    audio.Step(0.25f);
    CHECK(audio.Playing(voice));
    CHECK(audio.Pump() == 0);

    // Past the end it is not, and the voice is released rather than holding its decoded buffer.
    audio.Step(0.40f);
    CHECK(!audio.Playing(voice));
    CHECK(audio.Pump() == 1);
    CHECK(audio.Voices() == 0);

    // A handle outliving its sound reads as stopped rather than as anything worse.
    CHECK(!audio.Playing(voice));
    audio.Stop(voice);
}

TEST(audio, a_loop_does_not_end) {
    const fs::path tone = WriteTone("short.wav", 0.1f);

    svc::Audio audio;
    CHECK(audio.OpenSilent());

    const svc::Audio::Voice voice = audio.Play(tone, 1.0f, true);
    CHECK(voice != 0);

    audio.Step(1.0f);                                   // ten times its own length
    CHECK(audio.Playing(voice));
    CHECK(audio.Pump() == 0);

    audio.Stop(voice);
    CHECK(!audio.Playing(voice));
    CHECK(audio.Voices() == 0);
}

TEST(audio, several_sounds_at_once_are_several_sounds) {
    const fs::path a = WriteTone("a.wav", 0.2f);
    const fs::path b = WriteTone("b.wav", 0.6f);

    svc::Audio audio;
    CHECK(audio.OpenSilent());

    const svc::Audio::Voice first = audio.Play(a);
    const svc::Audio::Voice second = audio.Play(b);
    CHECK(first != second);
    CHECK(audio.Voices() == 2);

    // The short one ends and the long one does not, which is the only thing "a mixer" has to mean
    // for an application: two sounds that do not know about each other.
    audio.Step(0.35f);
    CHECK(!audio.Playing(first));
    CHECK(audio.Playing(second));
    CHECK(audio.Pump() == 1);
    CHECK(audio.Voices() == 1);

    audio.StopAll();
    CHECK(audio.Voices() == 0);
}

TEST(audio, volume_is_remembered_across_opening_a_device) {
    svc::Audio audio;
    audio.SetMasterVolume(0.25f);
    CHECK(audio.MasterVolume() == 0.25f);

    // Set before there is anything to set it on, and applied when there is: a project that saved a
    // volume restores it before the first sound, not after it.
    CHECK(audio.OpenSilent());
    CHECK(audio.MasterVolume() == 0.25f);

    audio.SetMasterVolume(-3.0f);
    CHECK(audio.MasterVolume() == 0.0f);
    audio.SetMasterVolume(99.0f);
    CHECK(audio.MasterVolume() == 4.0f);
}

TEST(audio, a_file_that_is_not_a_sound_says_so_instead_of_playing_nothing) {
    svc::Audio audio;
    CHECK(audio.OpenSilent());

    CHECK(audio.Play(Scratch() / "there-is-no-such-file.wav") == 0);
    CHECK(!audio.Problem().empty());

    const fs::path lie = Scratch() / "not-really.wav";
    { std::ofstream out(lie, std::ios::trunc); out << "this is text, not a waveform"; }
    CHECK(audio.Play(lie) == 0);
    CHECK(audio.Problem().find("not-really.wav") != std::string::npos);

    // And a real one still plays afterwards: one bad file does not poison the engine.
    CHECK(audio.Play(WriteTone("fine.wav", 0.1f)) != 0);
}
