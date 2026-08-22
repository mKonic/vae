#pragma once

#include "vae/rx/Node.h"

#include <functional>
#include <utility>

namespace vae::rx {

    // A side effect that re-runs when its dependencies change. Runs once immediately on
    // construction to record what it reads.
    //
    // Effects are deferred to the end of the current batch, so a handler that writes five signals
    // repaints once. Everything the renderer and layout invalidate hangs off this.
    class Effect final : public Node {
    public:
        template<typename Fn>
        explicit Effect(Fn&& fn) : m_Fn(std::forward<Fn>(fn)) {
            m_State = State::Dirty;
            Pull();                 // initial run records dependencies
        }

        bool IsEffect() const override { return true; }

        void Update() override {
            ClearSources();
            TrackScope track(this);
            m_Fn();
            ++m_Version;
        }

    private:
        std::function<void()> m_Fn;
    };

}
