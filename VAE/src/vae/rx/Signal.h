#pragma once

#include "vae/rx/Node.h"

#include <concepts>
#include <utility>

namespace vae::rx {

    template<typename T>
    concept EqualityComparable = requires(const T& a, const T& b) { { a == b } -> std::convertible_to<bool>; };

    // A mutable reactive value. Reading inside a Computed or Effect records a dependency.
    template<typename T>
    class Signal final : public Node {
    public:
        Signal() = default;
        explicit Signal(T value) : m_Value(std::move(value)) {}

        const T& Get() { Observe(); return m_Value; }
        const T& Peek() const { return m_Value; }        // read without subscribing

        void Set(T value) {
            // Equality cutoff: writing the same value must not wake the graph. Without it, every
            // mouse-move that re-assigns an unchanged hover target would relayout the screen.
            if constexpr (EqualityComparable<T>) {
                if (m_Value == value) return;
            }
            m_Value = std::move(value);
            ++m_Version;
            PropagateChange();
            Flush();
        }

        template<typename Fn>
        void Update(Fn&& fn) { T next = m_Value; fn(next); Set(std::move(next)); }

        // For types too large or non-comparable to round-trip through Set. The caller promises the
        // value really changed; there is no equality cutoff on this path.
        T& MutateUnchecked() { return m_Value; }
        void NotifyMutated() { ++m_Version; PropagateChange(); Flush(); }

        const T& operator()() { return Get(); }

    private:
        T m_Value{};
    };

}
