#include "Test.h"

#include "vae/rx/Rx.h"

#include <string>
#include <vector>

using namespace vae;
using namespace vae::rx;

TEST(rx, signal_reads_and_writes) {
    Signal<int> s{ 3 };
    CHECK_EQ(s.Get(), 3);
    s.Set(7);
    CHECK_EQ(s.Get(), 7);
}

TEST(rx, computed_is_lazy_until_read) {
    Signal<int> s{ 1 };
    int runs = 0;
    Computed<int> doubled{ [&] { ++runs; return s.Get() * 2; } };

    CHECK_EQ(runs, 0);                 // never read: never evaluated
    CHECK_EQ(doubled.Get(), 2);
    CHECK_EQ(runs, 1);
    CHECK_EQ(doubled.Get(), 2);        // cached
    CHECK_EQ(runs, 1);

    s.Set(5);
    CHECK_EQ(doubled.Get(), 10);
    CHECK_EQ(runs, 2);
}

TEST(rx, diamond_evaluates_the_join_once) {
    // s -> a, s -> b, (a,b) -> sum. A naive push-only graph runs sum twice per write.
    Signal<int> s{ 1 };
    int aRuns = 0, bRuns = 0, sumRuns = 0;

    Computed<int> a{ [&] { ++aRuns; return s.Get() * 2; } };
    Computed<int> b{ [&] { ++bRuns; return s.Get() + 10; } };
    Computed<int> sum{ [&] { ++sumRuns; return a.Get() + b.Get(); } };

    CHECK_EQ(sum.Get(), 13);
    CHECK_EQ(sumRuns, 1);

    s.Set(2);
    CHECK_EQ(sum.Get(), 16);
    CHECK_EQ(sumRuns, 2);              // once, not twice
    CHECK_EQ(aRuns, 2);
    CHECK_EQ(bRuns, 2);
}

TEST(rx, equal_write_does_not_wake_the_graph) {
    Signal<int> s{ 4 };
    int runs = 0;
    Computed<int> c{ [&] { ++runs; return s.Get(); } };
    CHECK_EQ(c.Get(), 4);
    CHECK_EQ(runs, 1);

    s.Set(4);                          // same value
    CHECK_EQ(c.Get(), 4);
    CHECK_EQ(runs, 1);
}

TEST(rx, unchanged_computed_result_stops_propagation) {
    // parity only changes every other write, so its observer must not re-run in between.
    Signal<int> s{ 0 };
    int parityRuns = 0, downstreamRuns = 0;

    Computed<int> parity{ [&] { ++parityRuns; return s.Get() % 2; } };
    Computed<int> downstream{ [&] { ++downstreamRuns; return parity.Get() * 100; } };

    CHECK_EQ(downstream.Get(), 0);
    CHECK_EQ(downstreamRuns, 1);

    s.Set(2);                          // parity still 0
    CHECK_EQ(downstream.Get(), 0);
    CHECK_EQ(parityRuns, 2);
    CHECK_EQ(downstreamRuns, 1);       // cut off at parity

    s.Set(3);                          // parity now 1
    CHECK_EQ(downstream.Get(), 100);
    CHECK_EQ(downstreamRuns, 2);
}

TEST(rx, effect_runs_immediately_then_on_change) {
    Signal<int> s{ 1 };
    std::vector<int> seen;
    Effect e{ [&] { seen.push_back(s.Get()); } };

    CHECK_EQ(seen.size(), 1u);
    CHECK_EQ(seen[0], 1);

    s.Set(2);
    CHECK_EQ(seen.size(), 2u);
    CHECK_EQ(seen[1], 2);
}

TEST(rx, batch_coalesces_effect_runs) {
    Signal<int> x{ 0 };
    Signal<int> y{ 0 };
    int runs = 0;
    Effect e{ [&] { x.Get(); y.Get(); ++runs; } };
    CHECK_EQ(runs, 1);

    Batch([&] { x.Set(1); y.Set(2); x.Set(3); });
    CHECK_EQ(runs, 2);                 // one re-run for three writes

    x.Set(4);                          // outside a batch: immediate
    CHECK_EQ(runs, 3);
}

TEST(rx, effect_tracks_only_the_branch_it_read) {
    // Dynamic dependencies: while `useA` is true the effect must not wake on `b` at all.
    Signal<bool> useA{ true };
    Signal<int>  a{ 1 };
    Signal<int>  b{ 100 };
    int runs = 0, last = 0;

    Effect e{ [&] { ++runs; last = useA.Get() ? a.Get() : b.Get(); } };
    CHECK_EQ(runs, 1);
    CHECK_EQ(last, 1);

    b.Set(200);                        // not a dependency right now
    CHECK_EQ(runs, 1);

    a.Set(2);
    CHECK_EQ(runs, 2);
    CHECK_EQ(last, 2);

    useA.Set(false);
    CHECK_EQ(runs, 3);
    CHECK_EQ(last, 200);

    a.Set(3);                          // no longer a dependency
    CHECK_EQ(runs, 3);
    b.Set(300);
    CHECK_EQ(runs, 4);
    CHECK_EQ(last, 300);
}

TEST(rx, untracked_read_creates_no_dependency) {
    Signal<int> tracked{ 1 };
    Signal<int> hidden{ 1 };
    int runs = 0;
    Effect e{ [&] { ++runs; tracked.Get(); Untracked([&] { return hidden.Get(); }); } };
    CHECK_EQ(runs, 1);

    hidden.Set(2);
    CHECK_EQ(runs, 1);
    tracked.Set(2);
    CHECK_EQ(runs, 2);
}

TEST(rx, peek_does_not_subscribe) {
    Signal<int> s{ 1 };
    int runs = 0;
    Effect e{ [&] { ++runs; (void)s.Peek(); } };
    CHECK_EQ(runs, 1);
    s.Set(9);
    CHECK_EQ(runs, 1);
}

TEST(rx, destroying_a_node_unlinks_it) {
    Signal<int> s{ 1 };
    int runs = 0;
    {
        Effect e{ [&] { ++runs; s.Get(); } };
        CHECK_EQ(runs, 1);
        s.Set(2);
        CHECK_EQ(runs, 2);
    }
    s.Set(3);                          // effect is gone; must not crash or run
    CHECK_EQ(runs, 2);
}

TEST(rx, effect_writing_a_signal_cascades_within_one_flush) {
    Signal<int> src{ 0 };
    Signal<int> mirror{ 0 };
    int mirrorRuns = 0;

    Effect copy{ [&] { mirror.Set(src.Get() * 2); } };
    Effect watch{ [&] { mirror.Get(); ++mirrorRuns; } };
    CHECK_EQ(mirrorRuns, 1);

    src.Set(5);
    CHECK_EQ(mirror.Peek(), 10);
    CHECK_EQ(mirrorRuns, 2);
}

TEST(rx, string_signal_uses_value_equality) {
    Signal<std::string> s{ "a" };
    int runs = 0;
    Effect e{ [&] { ++runs; s.Get(); } };
    CHECK_EQ(runs, 1);
    s.Set("a");
    CHECK_EQ(runs, 1);
    s.Set("b");
    CHECK_EQ(runs, 2);
}
