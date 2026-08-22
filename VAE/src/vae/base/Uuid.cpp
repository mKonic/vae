#include "vaepch.h"
#include "vae/base/Uuid.h"

#include <random>
#include <charconv>

namespace vae {

    // splitmix64's finalizer over the two ids. Deterministic, because widget state and script state
    // are keyed by this and both have to survive a rebuild of the view tree.
    Uuid Uuid::Derive(Uuid context, Uuid node) {
        if (!context.Valid()) return node;
        u64 x = context.Value() ^ (node.Value() + 0x9E3779B97F4A7C15ull
                                   + (context.Value() << 6) + (context.Value() >> 2));
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x ^= x >> 31;
        return Uuid(x ? x : 1ull);
    }

    namespace {
        std::mt19937_64& Engine() {
            // thread_local so parallel document loads cannot collide on the same stream.
            static thread_local std::mt19937_64 engine{ std::random_device{}() };
            return engine;
        }
        std::uniform_int_distribution<u64> s_Dist{ 1, std::numeric_limits<u64>::max() };
    }

    Uuid::Uuid() : m_Value(s_Dist(Engine())) {}

    Uuid Uuid::FromString(std::string_view hex) {
        u64 value = 0;
        auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
        if (ec != std::errc{}) return Uuid::Invalid();
        return Uuid(value);
    }

    std::string Uuid::ToString() const {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016lx", static_cast<unsigned long>(m_Value));
        return std::string(buf, 16);
    }

}
