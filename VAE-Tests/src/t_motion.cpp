#include "Test.h"

#include "vae/motion/Driver.h"

#include <cmath>

using namespace vae;
using namespace vae::motion;

namespace {

    bool Near(f32 a, f32 b, f32 epsilon = 0.001f) { return std::abs(a - b) <= epsilon; }

    Key Slot(u16 slot = 0) {
        Key key;
        key.owner = Uuid(1);
        key.node = Uuid(2);
        key.prop = 3;
        key.slot = slot;
        return key;
    }

    f32 Number(const doc::Value& value) {
        return doc::TypeOf(value) == doc::ValueType::Number ? std::get<f32>(value) : -12345.0f;
    }

    // Runs a driver at a given frame rate and reports where it ended up and how long it took.
    struct Run {
        f32 value = 0.0f;
        f32 seconds = 0.0f;
        int frames = 0;
    };

    Run Play(Driver& driver, const Key& key, f32 dt, int limit = 4000) {
        Run run;
        while (driver.Busy() && run.frames < limit) {
            driver.Advance(dt);
            run.seconds += dt;
            ++run.frames;
            const doc::Value current = driver.Current(key);
            if (doc::IsSet(current)) run.value = Number(current);
        }
        return run;
    }

}

TEST(easing, every_curve_starts_at_zero_and_ends_at_one) {
    for (int i = 0; i < static_cast<int>(Easing::Count); ++i) {
        const auto curve = static_cast<Easing>(i);
        CHECK_MESSAGE(Near(Ease(curve, 0.0f), 0.0f), EasingName(curve));
        CHECK_MESSAGE(Near(Ease(curve, 1.0f), 1.0f), EasingName(curve));
    }
}

TEST(easing, t_is_clamped_rather_than_extrapolated) {
    // An animation fed a t past its end must hold, not fly off. Back and elastic extrapolate
    // spectacularly if you let them.
    for (int i = 0; i < static_cast<int>(Easing::Count); ++i) {
        const auto curve = static_cast<Easing>(i);
        CHECK_MESSAGE(Near(Ease(curve, -3.0f), 0.0f), EasingName(curve));
        CHECK_MESSAGE(Near(Ease(curve, 7.0f), 1.0f), EasingName(curve));
    }
}

TEST(easing, the_shapes_are_actually_different) {
    // Halfway through, an ease-in is behind linear and an ease-out is ahead of it. If that is not
    // true the curve is misnamed, and a designer picking one gets the opposite of what they asked.
    CHECK(Ease(Easing::InCubic, 0.5f) < 0.5f);
    CHECK(Ease(Easing::OutCubic, 0.5f) > 0.5f);
    CHECK(Near(Ease(Easing::InOutCubic, 0.5f), 0.5f));
    CHECK(Ease(Easing::InQuart, 0.5f) < Ease(Easing::InCubic, 0.5f));

    // Back overshoots on the way out and undershoots on the way in — that is what it is for.
    CHECK(Ease(Easing::OutBack, 0.7f) > 1.0f);
    CHECK(Ease(Easing::InBack, 0.3f) < 0.0f);
}

TEST(easing, names_round_trip) {
    for (int i = 0; i < static_cast<int>(Easing::Count); ++i) {
        const auto curve = static_cast<Easing>(i);
        const auto back = EasingFromName(EasingName(curve));
        CHECK(back.has_value() && *back == curve);
    }
    CHECK(!EasingFromName("wobble").has_value());
}

TEST(motion, a_tween_walks_from_one_value_to_the_other) {
    Driver driver;
    Options options;
    options.duration = 1.0f;
    options.curve = Easing::Linear;
    driver.To(Slot(), 0.0f, 100.0f, options);

    driver.Advance(0.25f);
    CHECK(Near(Number(driver.Current(Slot())), 25.0f, 0.01f));
    driver.Advance(0.5f);
    CHECK(Near(Number(driver.Current(Slot())), 75.0f, 0.01f));
    driver.Advance(0.25f);
    CHECK(Near(Number(driver.Current(Slot())), 100.0f, 0.01f));
    CHECK(!driver.Busy());
}

TEST(motion, a_delay_holds_the_start_value) {
    Driver driver;
    Options options;
    options.duration = 1.0f;
    options.delay = 0.5f;
    options.curve = Easing::Linear;
    driver.To(Slot(), 10.0f, 20.0f, options);

    driver.Advance(0.4f);
    CHECK(Near(Number(driver.Current(Slot())), 10.0f, 0.01f));
    driver.Advance(0.6f);   // 0.5s of animation elapsed
    CHECK(Near(Number(driver.Current(Slot())), 15.0f, 0.01f));
}

TEST(motion, colours_and_vectors_interpolate_channel_by_channel) {
    Driver driver;
    Options options;
    options.duration = 1.0f;
    options.curve = Easing::Linear;

    driver.To(Slot(0), Color{ 0.0f, 0.0f, 0.0f, 1.0f }, Color{ 1.0f, 0.5f, 0.0f, 0.0f }, options);
    driver.To(Slot(1), Vec2{ 0.0f, 10.0f }, Vec2{ 100.0f, 0.0f }, options);
    driver.Advance(0.5f);

    const Color colour = std::get<Color>(driver.Current(Slot(0)));
    CHECK(Near(colour.r, 0.5f) && Near(colour.g, 0.25f) && Near(colour.b, 0.0f)
          && Near(colour.a, 0.5f));

    const Vec2 point = std::get<Vec2>(driver.Current(Slot(1)));
    CHECK(Near(point.x, 50.0f) && Near(point.y, 5.0f));
}

TEST(motion, a_value_with_nothing_to_interpolate_switches_at_the_end) {
    Driver driver;
    Options options;
    options.duration = 0.5f;
    driver.To(Slot(), std::string("before"), std::string("after"), options);

    driver.Advance(0.25f);
    CHECK(!doc::IsSet(driver.Current(Slot())));     // still whatever it was
    driver.Advance(0.3f);
    CHECK(std::get<std::string>(driver.Current(Slot())) == "after");
}

TEST(motion, the_final_value_is_readable_before_the_track_is_dropped) {
    // Otherwise the one value nobody ever sees is the one the animation was aiming at.
    Driver driver;
    Options options;
    options.duration = 0.1f;
    options.curve = Easing::Linear;
    driver.To(Slot(), 0.0f, 42.0f, options);

    driver.Advance(0.1f);
    CHECK(Near(Number(driver.Current(Slot())), 42.0f, 0.001f));
    CHECK(!driver.Busy());
    driver.Advance(0.1f);
    CHECK(!driver.Animating(Slot()));
}

TEST(motion, retargeting_a_tween_continues_from_where_it_is) {
    Driver driver;
    Options options;
    options.duration = 1.0f;
    options.curve = Easing::Linear;
    driver.To(Slot(), 0.0f, 100.0f, options);
    driver.Advance(0.5f);
    CHECK(Near(Number(driver.Current(Slot())), 50.0f, 0.01f));

    // Redirected halfway: it must set off from 50, not snap back to 0.
    driver.To(Slot(), 0.0f, 0.0f, options);
    CHECK(Near(Number(driver.Current(Slot())), 50.0f, 0.01f));
    driver.Advance(0.5f);
    CHECK(Near(Number(driver.Current(Slot())), 25.0f, 0.01f));
}

TEST(motion, a_spring_settles_on_its_target) {
    Driver driver;
    Options options;
    options.spring = true;
    options.physics = { 0.4f, 0.0f };
    driver.To(Slot(), 0.0f, 1.0f, options);

    const Run run = Play(driver, Slot(), 1.0f / 60.0f);
    CHECK_MESSAGE(Near(run.value, 1.0f, 0.01f), std::to_string(run.value));
    // "duration" means what it says: settled by about then, not eventually.
    CHECK_MESSAGE(run.seconds < 0.8f, std::to_string(run.seconds));
}

TEST(motion, a_bouncy_spring_overshoots_and_a_sluggish_one_does_not) {
    const auto peak = [](f32 bounce) {
        Driver driver;
        Options options;
        options.spring = true;
        options.physics = { 0.4f, bounce };
        driver.To(Slot(), 0.0f, 1.0f, options);

        f32 highest = 0.0f;
        for (int i = 0; i < 400 && driver.Busy(); ++i) {
            driver.Advance(1.0f / 120.0f);
            highest = std::max(highest, Number(driver.Current(Slot())));
        }
        return highest;
    };

    CHECK_MESSAGE(peak(0.8f) > 1.02f, std::to_string(peak(0.8f)));
    CHECK_MESSAGE(peak(0.0f) <= 1.005f, std::to_string(peak(0.0f)));
    CHECK_MESSAGE(peak(-0.8f) <= 1.001f, std::to_string(peak(-0.8f)));
}

TEST(motion, a_spring_lands_in_the_same_place_at_any_frame_rate) {
    // The thing that makes a spring usable: a 30fps machine and a 144fps machine have to agree, or
    // it is a per-machine animation and every bug report is unreproducible.
    const auto settle = [](f32 dt) {
        Driver driver;
        Options options;
        options.spring = true;
        options.physics = { 0.35f, 0.6f };
        driver.To(Slot(), 0.0f, 250.0f, options);
        return Play(driver, Slot(), dt);
    };

    const Run slow = settle(1.0f / 30.0f);
    const Run fast = settle(1.0f / 144.0f);
    const Run odd  = settle(0.0173f);

    CHECK(Near(slow.value, 250.0f, 0.5f));
    CHECK(Near(fast.value, 250.0f, 0.5f));
    CHECK(Near(odd.value, 250.0f, 0.5f));
    // Settling times agree to well within a frame of the slowest of them.
    CHECK_MESSAGE(std::abs(slow.seconds - fast.seconds) < 0.05f,
                  std::to_string(slow.seconds) + " vs " + std::to_string(fast.seconds));
}

TEST(motion, a_tween_is_identical_under_a_fixed_timestep) {
    const auto run = [](f32 dt, int steps) {
        Driver driver;
        Options options;
        options.duration = 0.5f;
        options.curve = Easing::InOutCubic;
        driver.To(Slot(), 0.0f, 1.0f, options);

        std::vector<f32> samples;
        for (int i = 0; i < steps; ++i) {
            driver.Advance(dt);
            samples.push_back(Number(driver.Current(Slot())));
        }
        return samples;
    };

    const auto a = run(1.0f / 60.0f, 30);
    const auto b = run(1.0f / 60.0f, 30);
    CHECK(a == b);      // bit for bit, not merely close
}

TEST(motion, a_long_stall_does_not_launch_a_spring) {
    // A breakpoint, a swap, a compositor hiccup: the frame after is a huge dt, and a spring
    // integrated over half a second in one step goes to the moon.
    Driver driver;
    Options options;
    options.spring = true;
    options.physics = { 0.3f, 0.5f };
    driver.To(Slot(), 0.0f, 1.0f, options);

    driver.Advance(4.0f);
    const f32 value = Number(driver.Current(Slot()));
    CHECK_MESSAGE(value >= -0.5f && value <= 1.5f, std::to_string(value));
}

TEST(motion, cancelling_by_owner_leaves_everything_else_running) {
    Driver driver;
    Options options;
    options.duration = 1.0f;

    Key mine = Slot();
    Key theirs = Slot();
    theirs.owner = Uuid(99);

    driver.To(mine, 0.0f, 1.0f, options);
    driver.To(theirs, 0.0f, 1.0f, options);
    CHECK(driver.Count() == 2);

    driver.CancelOwner(Uuid(1));
    CHECK(driver.Count() == 1);
    CHECK(driver.Animating(theirs));
    CHECK(!driver.Animating(mine));
}
