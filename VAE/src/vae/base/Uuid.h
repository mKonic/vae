#pragma once

#include "vae/base/Base.h"

#include <functional>
#include <string>
#include <string_view>

namespace vae {

    // Stable identity for every document node, component and asset. Serialized as 16 lowercase hex
    // digits so a .vae diff stays readable.
    class Uuid {
    public:
        Uuid();                       // random
        explicit Uuid(u64 value) : m_Value(value) {}
        static Uuid Invalid() { return Uuid(u64(0)); }
        static Uuid FromString(std::string_view hex);
        // Identity for one copy of a nested thing. A component placed inside another component is
        // a single document node, but every copy of the outer component puts a distinct one of it
        // on screen — so the pair (where it is, what it is) is the id, not the node alone.
        static Uuid Derive(Uuid context, Uuid node);
        // A stable id for something rebuilt from code rather than authored. The standard widget
        // catalog is compiled into the binary and re-created on every load, so its nodes have to
        // land on the same ids every time — an instance's overrides are keyed by the id of the
        // node INSIDE the component, and would go stale the moment the file was reopened.
        static Uuid FromName(std::string_view name);

        std::string ToString() const;
        u64  Value() const { return m_Value; }
        bool Valid() const { return m_Value != 0; }

        operator u64() const { return m_Value; }
        bool operator==(const Uuid& o) const { return m_Value == o.m_Value; }
        auto operator<=>(const Uuid& o) const { return m_Value <=> o.m_Value; }

    private:
        u64 m_Value;
    };

}

template<> struct std::hash<vae::Uuid> {
    std::size_t operator()(const vae::Uuid& id) const noexcept { return std::hash<vae::u64>{}(id.Value()); }
};
