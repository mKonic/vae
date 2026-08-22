#pragma once

#include "vae/rx/Node.h"

#include <functional>
#include <utility>

namespace vae::rx {

    // A derived value. Lazy: the function does not run until something reads it, and only re-runs
    // when a source it actually read has changed. If a recompute produces an equal value, observers
    // are not woken — that cutoff is what keeps a deep binding chain from repainting the world.
    template<typename T>
    class Computed final : public Node {
    public:
        template<typename Fn>
        explicit Computed(Fn&& fn) : m_Fn(std::forward<Fn>(fn)) { m_State = State::Dirty; }

        const T& Get() {
            Pull();
            Observe();
            return m_Value;
        }

        const T& Peek() { Pull(); return m_Value; }
        const T& operator()() { return Get(); }

        void Update() override {
            ClearSources();
            TrackScope track(this);
            T next = m_Fn();
            if constexpr (requires(const T& a, const T& b) { { a == b } -> std::convertible_to<bool>; }) {
                if (m_Evaluated && next == m_Value) return;   // value unchanged: version stays put
            }
            m_Value = std::move(next);
            m_Evaluated = true;
            ++m_Version;
        }

    private:
        std::function<T()> m_Fn;
        T    m_Value{};
        bool m_Evaluated = false;
    };

}
