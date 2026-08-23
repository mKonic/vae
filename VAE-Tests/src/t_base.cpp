#include "Test.h"

#include "vae/base/Version.h"
#include "vae/base/FileSystem.h"
#include "vae/base/Math.h"
#include "vae/base/Uuid.h"

#include <filesystem>
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
