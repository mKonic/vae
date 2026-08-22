#pragma once

#include "vae/base/Base.h"
#include "vae/base/Math.h"
#include "vae/base/Uuid.h"

#include <string>
#include <variant>

namespace vae::doc {

    // A reference to a design token rather than a literal. Stored as the token's name so a document
    // survives a token being renamed only if the rename rewrites references — which is exactly why
    // this is its own type and not a magic string inside a Value.
    struct TokenRef {
        std::string name;
        bool operator==(const TokenRef&) const = default;
    };

    // A binding expression over the reactive graph, e.g. "user.name". Stored unevaluated; the
    // runtime compiles it against the app's state.
    struct Binding {
        std::string expression;
        bool operator==(const Binding&) const = default;
    };

    // An asset reference (image, font, icon), by id rather than path so moving a file on disk does
    // not break every node that used it.
    struct AssetRef {
        Uuid id = Uuid::Invalid();
        bool operator==(const AssetRef&) const = default;
    };

    using Value = std::variant<
        std::monostate,     // unset — distinct from "set to a default", which matters for overrides
        bool,
        f32,
        Vec2,
        Color,
        std::string,
        Uuid,               // a reference to another node
        AssetRef,
        TokenRef,
        Binding>;

    enum class ValueType : u8 {
        Unset = 0, Bool, Number, Vector2, Colour, Text, NodeRef, Asset, Token, Bound
    };

    inline ValueType TypeOf(const Value& v) { return static_cast<ValueType>(v.index()); }
    inline bool IsSet(const Value& v) { return v.index() != 0; }

    const char* ValueTypeName(ValueType type);

}
