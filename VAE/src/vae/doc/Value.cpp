#include "vaepch.h"
#include "vae/doc/Node.h"

#include <array>

namespace vae::doc {

    const char* ValueTypeName(ValueType type) {
        switch (type) {
            case ValueType::Unset:   return "unset";
            case ValueType::Bool:    return "bool";
            case ValueType::Number:  return "number";
            case ValueType::Vector2: return "vec2";
            case ValueType::Colour:  return "color";
            case ValueType::Text:    return "string";
            case ValueType::NodeRef: return "node";
            case ValueType::Asset:   return "asset";
            case ValueType::Token:   return "token";
            case ValueType::Bound:   return "binding";
        }
        return "unknown";
    }

    namespace {
        // Serialized names. Changing one is a document format break, so they are spelled out here
        // rather than derived from the enumerator names.
        constexpr std::array<const char*, static_cast<std::size_t>(Prop::Count)> kPropNames{
            "fill", "fillOpacity", "stroke", "strokeWidth", "cornerRadius", "opacity", "visible",
            "clipContent", "shadowColor", "shadowOffset", "shadowBlur", "shadowSpread",
            "text", "fontFamily", "fontSize", "fontWeight", "fontItalic", "textColor", "textAlign",
            "lineHeight", "letterSpacing", "textWrap",
            "image", "imageFit",
            "enabled", "tooltip", "cursor",
            "role", "checked", "value", "minValue", "maxValue", "step", "placeholder",
            "selectedIndex", "group", "multiline", "password", "readOnly", "maxLength", "open",
            "route", "scrollX", "scrollY", "itemHeight", "itemCount", "duration", "modal",
            "columnWidth",
            "screenKind", "goTo",
            "series", "chartKind",
            "selectable", "shown", "repeat", "field", "resizable", "sample", "stickToEnd",
            "textKey", "a11yLabel",
        };

        // Declared types, in enum order and checked against kPropNames' length below. `Unset` is
        // "the shape decides" and is deliberate everywhere it appears, not a gap: Value is text on
        // a field and a number on a slider, and Series is a list the widget parses itself.
        using VT = ValueType;
        constexpr std::array<ValueType, static_cast<std::size_t>(Prop::Count)> kPropTypes{
            // paint
            VT::Colour, VT::Number, VT::Colour, VT::Number, VT::Number, VT::Number, VT::Bool,
            VT::Bool, VT::Colour, VT::Vector2, VT::Number, VT::Number,
            // text
            VT::Text, VT::Text, VT::Number, VT::Number, VT::Bool, VT::Colour, VT::Text, VT::Number,
            VT::Number, VT::Text,
            // image
            VT::Asset, VT::Text,
            // interaction
            VT::Bool, VT::Text, VT::Text,
            // widgets
            VT::Text, VT::Bool, VT::Unset, VT::Number, VT::Number, VT::Number, VT::Text,
            VT::Number, VT::Text, VT::Bool, VT::Bool, VT::Bool, VT::Number, VT::Bool,
            VT::Text, VT::Number, VT::Number,
            VT::Number, VT::Number, VT::Number, VT::Bool, VT::Number,
            // screens
            VT::Text, VT::Text,
            // charts
            VT::Unset, VT::Text,
            VT::Bool,       // selectable
            VT::Text,       // shown
            VT::Number,     // repeat
            VT::Text,       // field
            VT::Bool,       // resizable
            VT::Text,       // sample
            VT::Bool,       // stickToEnd
            VT::Text,       // textKey
            VT::Text,       // a11yLabel
        };
        static_assert(kPropTypes.size() == kPropNames.size());
    }

    ValueType PropValueType(Prop prop) {
        const auto index = static_cast<std::size_t>(prop);
        return index < kPropTypes.size() ? kPropTypes[index] : ValueType::Unset;
    }

    namespace {
        constexpr std::array<const char*, static_cast<std::size_t>(ScreenKind::Count)> kScreenKinds{
            "page", "modal", "alert", "sheet", "popover",
        };
    }

    const char* ScreenKindName(ScreenKind kind) {
        const auto index = static_cast<std::size_t>(kind);
        return index < kScreenKinds.size() ? kScreenKinds[index] : "page";
    }

    std::optional<ScreenKind> ScreenKindFromName(std::string_view name) {
        for (std::size_t i = 0; i < kScreenKinds.size(); ++i)
            if (name == kScreenKinds[i]) return static_cast<ScreenKind>(i);
        return std::nullopt;
    }

    const char* PropName(Prop prop) {
        const auto index = static_cast<std::size_t>(prop);
        return index < kPropNames.size() ? kPropNames[index] : "unknown";
    }

    std::optional<Prop> PropFromName(std::string_view name) {
        for (std::size_t i = 0; i < kPropNames.size(); ++i)
            if (name == kPropNames[i]) return static_cast<Prop>(i);
        return std::nullopt;
    }

    bool PropBag::Has(std::string_view key) const {
        return m_Custom.find(std::string(key)) != m_Custom.end();
    }

    const Value* PropBag::Find(Prop prop) const {
        auto it = m_Props.find(prop);
        return it == m_Props.end() ? nullptr : &it->second;
    }

    const Value* PropBag::Find(std::string_view key) const {
        auto it = m_Custom.find(std::string(key));
        return it == m_Custom.end() ? nullptr : &it->second;
    }

    void PropBag::Set(Prop prop, Value value) {
        if (!IsSet(value)) { m_Props.erase(prop); return; }
        m_Props[prop] = std::move(value);
    }

    void PropBag::Set(std::string key, Value value) {
        if (!IsSet(value)) { m_Custom.erase(key); return; }
        m_Custom[std::move(key)] = std::move(value);
    }

    void PropBag::Unset(Prop prop) { m_Props.erase(prop); }
    void PropBag::Unset(std::string_view key) { m_Custom.erase(std::string(key)); }

    f32 PropBag::Number(Prop prop, f32 fallback) const {
        const Value* value = Find(prop);
        if (const f32* number = value ? std::get_if<f32>(value) : nullptr) return *number;
        return fallback;
    }

    bool PropBag::Flag(Prop prop, bool fallback) const {
        const Value* value = Find(prop);
        if (const bool* flag = value ? std::get_if<bool>(value) : nullptr) return *flag;
        return fallback;
    }

    Color PropBag::Colour(Prop prop, Color fallback) const {
        const Value* value = Find(prop);
        if (const Color* colour = value ? std::get_if<Color>(value) : nullptr) return *colour;
        return fallback;
    }

    std::string PropBag::Text(Prop prop, std::string fallback) const {
        const Value* value = Find(prop);
        if (const std::string* text = value ? std::get_if<std::string>(value) : nullptr) return *text;
        return fallback;
    }

    void PropBag::MergeInto(PropBag& into) const {
        for (const auto& [prop, value] : m_Props)  into.m_Props[prop] = value;
        for (const auto& [key, value] : m_Custom) into.m_Custom[key] = value;
    }

    void PropBag::Clear() { m_Props.clear(); m_Custom.clear(); }

}
