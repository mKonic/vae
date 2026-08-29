#include "Test.h"

#include "vae/base/Version.h"
#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Math.h"
#include "vae/base/Platform.h"
#include "vae/base/Uuid.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_set>

using namespace vae;

TEST(base, the_build_says_what_it_is_or_says_it_does_not_know) {
    // Two states, and no third: a build made from a git checkout knows its number, and one made
    // without git says "unknown" rather than inventing a plausible one. KernelSU's hardcoded 16
    // fallback is the reason that rule exists — it produced mismatch reports nobody could read.
    const std::string text = vae::Version::String();
    CHECK(!text.empty());
    if (vae::Version::Known()) {
        CHECK(vae::Version::Code() >= 10000u);          // 10000 + commits, never below the base
        CHECK(text.find("build ") != std::string::npos);
        CHECK(std::string(vae::Version::Name()) != "unknown");
    } else {
        CHECK_EQ(vae::Version::Code(), 0u);
        CHECK_EQ(text, std::string("unknown build"));
    }
}

TEST(base, uuid_round_trips_through_hex) {
    const Uuid id;
    CHECK(id.Valid());
    CHECK_EQ(id.ToString().size(), 16u);
    CHECK_EQ(Uuid::FromString(id.ToString()), id);
}

TEST(base, uuid_invalid_is_falsey_and_distinct) {
    CHECK(!Uuid::Invalid().Valid());
    CHECK(Uuid{} != Uuid::Invalid());
}

TEST(base, uuids_do_not_collide_in_bulk) {
    std::unordered_set<Uuid> seen;
    for (int i = 0; i < 10000; ++i) seen.insert(Uuid{});
    CHECK_EQ(seen.size(), 10000u);
}

TEST(base, rect_edges_and_containment) {
    const Rect r{ {10.0f, 20.0f}, {100.0f, 50.0f} };
    CHECK_NEAR(r.Right(), 110.0f);
    CHECK_NEAR(r.Bottom(), 70.0f);
    CHECK_NEAR(r.Center().x, 60.0f);
    CHECK(r.Contains({10.0f, 20.0f}));        // top-left is inside
    CHECK(!r.Contains({110.0f, 70.0f}));      // bottom-right is not — half-open, so adjacent
                                              // rects never both claim the same pixel
}

TEST(base, rect_intersection_of_disjoint_rects_is_empty) {
    const Rect a{ {0.0f, 0.0f}, {10.0f, 10.0f} };
    const Rect b{ {20.0f, 20.0f}, {10.0f, 10.0f} };
    CHECK(a.Intersect(b).Empty());

    const Rect c{ {5.0f, 5.0f}, {10.0f, 10.0f} };
    const Rect hit = a.Intersect(c);
    CHECK_NEAR(hit.pos.x, 5.0f);
    CHECK_NEAR(hit.size.x, 5.0f);
}

TEST(base, edges_and_corners_defaults) {
    CHECK_NEAR(Edges{4.0f}.Horizontal(), 8.0f);
    CHECK_NEAR(Edges(2.0f, 6.0f).Vertical(), 12.0f);
    CHECK(!Corners{}.Any());
    CHECK(Corners{8.0f}.Any());
    CHECK_NEAR(Corners(1.0f, 2.0f, 3.0f, 4.0f).AsVec4().z, 3.0f);
}

TEST(base, filesystem_finds_the_engine_root_marker) {
    const auto root = FileSystem::EngineRoot();
    CHECK(std::filesystem::exists(root / FileSystem::kRootMarker));
    CHECK(std::filesystem::exists(FileSystem::Asset("VAE/src/vaepch.h")));
}

// --- the log file is named for the process that owns it --------------------------------------
//
// The Studio and the app it launches (Ctrl+F5) are two processes sharing one config directory. A
// rotating sink is safe across threads and not across processes: rotation renames the file, and
// the other writer goes on writing into the one that no longer has a name.

TEST(base, a_log_file_is_named_for_its_program_and_pid) {
    CHECK_EQ(Log::FileNameFor("VAE-Studio", 4131), std::string("vae-studio-4131.log"));
    CHECK_EQ(Log::FileNameFor("VAE-Player", 9), std::string("vae-player-9.log"));
    // An app is called whatever its author called it, and that is not always a file name.
    CHECK_EQ(Log::FileNameFor("My App (2)", 7), std::string("my-app-2-7.log"));
    CHECK_EQ(Log::FileNameFor("", 7), std::string("vae-7.log"));
}

TEST(base, pruning_keeps_the_recent_runs_and_never_a_live_one) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vae-log-prune-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Eight finished runs, one live one, and a file this code did not write.
    const auto touch = [&](const std::string& name, int minutesAgo) {
        const fs::path path = dir / name;
        std::ofstream(path) << "x";
        fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::minutes(minutesAgo));
    };
    for (int i = 0; i < 8; ++i) touch("vae-studio-" + std::to_string(100 + i) + ".log", 100 - i);
    touch("vae-studio-100.1.log", 100);            // the rotated half of the oldest run
    touch("vae-player-999.log", 500);              // ancient, but its process is still there
    touch("notes.txt", 0);
    touch("mystery.log", 0);                       // no pid in the name: not ours, not ours to delete
    touch("vae.log", 400);                         // the one fixed name every process used to share
    touch("vae.1.log", 400);

    Log::Prune(dir, 5, [](std::uint32_t pid) { return pid == 999; });

    // The five newest finished runs survive; the three oldest go, rotated files with them.
    for (int i = 3; i < 8; ++i) CHECK(fs::exists(dir / ("vae-studio-" + std::to_string(100 + i) + ".log")));
    for (int i = 0; i < 3; ++i) CHECK(!fs::exists(dir / ("vae-studio-" + std::to_string(100 + i) + ".log")));
    CHECK(!fs::exists(dir / "vae-studio-100.1.log"));
    CHECK(fs::exists(dir / "vae-player-999.log"));
    CHECK(fs::exists(dir / "notes.txt"));
    CHECK(fs::exists(dir / "mystery.log"));
    CHECK(!fs::exists(dir / "vae.log"));           // ages out with the rest, rather than forever
    CHECK(!fs::exists(dir / "vae.1.log"));

    fs::remove_all(dir);
}

TEST(base, pruning_an_empty_or_missing_directory_is_not_an_error) {
    namespace fs = std::filesystem;
    Log::Prune(fs::temp_directory_path() / "vae-log-prune-nothing-here", 5,
               [](std::uint32_t) { return false; });
    CHECK(true);        // reaching here at all is the assertion
}

TEST(base, this_process_is_alive_and_pid_zero_is_not) {
    CHECK(platform::ProcessIdAlive(platform::CurrentProcessId()));
    CHECK(!platform::ProcessIdAlive(0));
}
