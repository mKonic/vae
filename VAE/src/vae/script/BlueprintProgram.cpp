#include "vaepch.h"
#include "vae/script/BlueprintProgram.h"

#include <algorithm>
#include <unordered_map>

namespace vae::script {

    using namespace vae::doc;

    namespace {

        const PinSpec* FindPin(const std::vector<PinSpec>& pins, std::string_view name) {
            for (const PinSpec& pin : pins) if (pin.name == name) return &pin;
            return nullptr;
        }

        std::string Quote(std::string_view s) { return "'" + std::string(s) + "'"; }

    }

    bool BlueprintProgram::Compile(const doc::Blueprint& blueprint, std::string component) {
        m_Blueprint = blueprint;
        m_Component = std::move(component);
        m_Diagnostics.clear();
        m_UsesTimers = false;
        Check();
        m_Ok = ErrorCount() == 0;
        return m_Ok;
    }

    std::size_t BlueprintProgram::ErrorCount() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(m_Diagnostics, [](const Diagnostic& d) { return d.error; }));
    }

    void BlueprintProgram::Check() {
        const auto Error = [&](u32 node, std::string message) {
            m_Diagnostics.push_back({ node, true, std::move(message) });
        };
        const auto Warn = [&](u32 node, std::string message) {
            m_Diagnostics.push_back({ node, false, std::move(message) });
        };

        std::unordered_map<u32, const BlueprintNode*> byId;
        for (const BlueprintNode& node : m_Blueprint.nodes) {
            if (byId.contains(node.id)) {
                Error(node.id, "two nodes share the id " + std::to_string(node.id));
                continue;
            }
            byId[node.id] = &node;
        }

        for (const BlueprintNode& node : m_Blueprint.nodes) {
            const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
            if (!type) { Error(node.id, "there is no node called " + Quote(node.type)); continue; }

            if (node.type == "var.get" || node.type == "var.set") {
                if (node.target.empty())
                    Error(node.id, "this reads a variable but does not say which");
                else if (!m_Blueprint.FindVariable(node.target))
                    Error(node.id, "there is no variable called " + Quote(node.target));
            }
            if (type->latent) m_UsesTimers = true;
            if (node.type == "app.after" || node.type == "flow.delay") m_UsesTimers = true;

            // An event that binds to nothing answers to nothing. Worth a warning rather than an
            // error: it is what a half-drawn blueprint looks like a second after the node was dropped,
            // and refusing to run the rest of the blueprint over it would be unusable.
            if (type->category == BlueprintCategory::Event) {
                const std::vector<PinSpec> inputs = BlueprintInputs(m_Blueprint, node);
                for (const PinSpec& pin : inputs)
                    if (pin.fixed) {
                        const Value literal = BlueprintLiteral(m_Blueprint, node, pin);
                        const std::string* text = std::get_if<std::string>(&literal);
                        if (text && text->empty())
                            Warn(node.id, std::string(type->title) + " does not say which "
                                        + std::string(pin.name) + " it answers to, so it never runs");
                    }
            }
        }

        // Two events of the same kind bound to the same thing is a blueprint that runs one of them
        // twice as far as anyone reading it is concerned. An error, because it is never intended
        // and it is invisible in a big blueprint.
        for (std::size_t i = 0; i < m_Blueprint.nodes.size(); ++i) {
            const BlueprintNode& a = m_Blueprint.nodes[i];
            const BlueprintNodeType* type = FindBlueprintNodeType(a.type);
            if (!type || type->category != BlueprintCategory::Event) continue;
            for (std::size_t j = i + 1; j < m_Blueprint.nodes.size(); ++j) {
                const BlueprintNode& b = m_Blueprint.nodes[j];
                if (b.type != a.type) continue;
                const std::vector<PinSpec> pins = BlueprintInputs(m_Blueprint, a);
                bool same = true;
                for (const PinSpec& pin : pins)
                    if (pin.fixed && BlueprintLiteral(m_Blueprint, a, pin) != BlueprintLiteral(m_Blueprint, b, pin))
                        same = false;
                if (same)
                    Error(b.id, "there is already a " + std::string(type->title)
                              + " for this, and two of them would both run");
            }
        }

        // ---- wires -------------------------------------------------------------------------
        for (const BlueprintLink& link : m_Blueprint.links) {
            const auto from = byId.find(link.from);
            const auto to   = byId.find(link.to);
            if (from == byId.end() || to == byId.end()) {
                Error(0, "a wire ends at a node that is not here");
                continue;
            }
            const std::vector<PinSpec> outputs = BlueprintOutputs(m_Blueprint, *from->second);
            const std::vector<PinSpec> inputs  = BlueprintInputs(m_Blueprint, *to->second);
            const PinSpec* out = FindPin(outputs, link.fromPin);
            const PinSpec* in  = FindPin(inputs, link.toPin);
            if (!out) {
                Error(link.from, "there is no output called " + Quote(link.fromPin) + " here");
                continue;
            }
            if (!in) {
                Error(link.to, "there is no input called " + Quote(link.toPin) + " here");
                continue;
            }
            if (in->fixed) {
                Error(link.to, std::string(in->name)
                             + " is chosen when the blueprint is drawn, so nothing can be wired to it");
                continue;
            }
            if (!PinsCompatible(out->type, in->type))
                Error(link.to, std::string("a ") + PinTypeName(out->type) + " cannot be wired into "
                             + Quote(in->name) + ", which takes a " + PinTypeName(in->type));
        }

        // A value has one source and "next" is one target. The editor enforces both as you draw,
        // so this is about a file — hand-edited markup, or a blueprint from a build that did not.
        std::unordered_map<std::string, u32> seen;
        for (const BlueprintLink& link : m_Blueprint.links) {
            const auto to = byId.find(link.to);
            if (to == byId.end()) continue;
            const PinSpec* in = FindPin(BlueprintInputs(m_Blueprint, *to->second), link.toPin);
            if (!in || in->type == PinType::Exec) continue;
            const std::string key = std::to_string(link.to) + "\n" + link.toPin;
            if (seen[key]++ > 0)
                Error(link.to, Quote(link.toPin) + " has more than one thing wired into it");
        }
        seen.clear();
        for (const BlueprintLink& link : m_Blueprint.links) {
            const auto from = byId.find(link.from);
            if (from == byId.end()) continue;
            const PinSpec* out = FindPin(BlueprintOutputs(m_Blueprint, *from->second), link.fromPin);
            if (!out || out->type != PinType::Exec) continue;
            const std::string key = std::to_string(link.from) + "\n" + link.fromPin;
            if (seen[key]++ > 0)
                Error(link.from, Quote(link.fromPin) + " goes to more than one place, and only "
                                 "one of them would run");
        }

        // ---- data cycles ---------------------------------------------------------------------
        std::vector<u32> done;
        for (const BlueprintNode& node : m_Blueprint.nodes) {
            std::vector<u32> path;
            if (Cycles(node.id, path, done))
                Error(node.id, "this value is worked out from itself");
        }

        // ---- variables -------------------------------------------------------------------------
        for (const BlueprintVariable& variable : m_Blueprint.variables) {
            if (variable.name.empty()) { Error(0, "a variable with no name"); continue; }
            const auto count = std::ranges::count_if(m_Blueprint.variables,
                [&](const BlueprintVariable& other) { return other.name == variable.name; });
            if (count > 1) Error(0, "there are two variables called " + Quote(variable.name));
        }

        // ---- what will never run -----------------------------------------------------------
        // A node with execution pins that nothing reaches is dead. Not an error — a blueprint is drawn
        // in pieces and half of it is unreachable while it is being drawn — but it is the single
        // most common reason for "why is nothing happening", so it is worth saying out loud.
        for (const BlueprintNode& node : m_Blueprint.nodes) {
            const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
            if (!type || type->pure || type->category == BlueprintCategory::Event) continue;
            const std::vector<PinSpec> inputs = BlueprintInputs(m_Blueprint, node);
            bool reachable = false;
            for (const PinSpec& pin : inputs)
                if (pin.type == PinType::Exec && m_Blueprint.LinkInto(node.id, pin.name))
                    reachable = true;
            if (!reachable)
                Warn(node.id, "nothing reaches this, so it never runs");
        }
    }

    bool BlueprintProgram::Cycles(u32 node, std::vector<u32>& path, std::vector<u32>& done) {
        if (std::ranges::find(done, node) != done.end()) return false;
        if (std::ranges::find(path, node) != path.end()) return true;
        path.push_back(node);

        const BlueprintNode* current = m_Blueprint.Find(node);
        if (current) {
            for (const PinSpec& pin : BlueprintInputs(m_Blueprint, *current)) {
                if (pin.type == PinType::Exec) continue;
                const BlueprintLink* link = m_Blueprint.LinkInto(node, pin.name);
                if (!link) continue;
                // Only a PURE source can loop: an impure node's data output is whatever it left
                // there when it last ran, which is a value, not a question asked again.
                const BlueprintNode* source = m_Blueprint.Find(link->from);
                const BlueprintNodeType* type = source ? FindBlueprintNodeType(source->type) : nullptr;
                if (!type || !type->pure) continue;
                if (Cycles(link->from, path, done)) return true;
            }
        }

        path.pop_back();
        done.push_back(node);
        return false;
    }

    std::vector<u32> BlueprintProgram::Entries(std::string_view type) const {
        std::vector<u32> out;
        for (const BlueprintNode& node : m_Blueprint.nodes)
            if (node.type == type) out.push_back(node.id);
        return out;
    }

    std::vector<u32> BlueprintProgram::EntriesFor(std::string_view type, std::string_view target) const {
        std::vector<u32> out;
        for (const BlueprintNode& node : m_Blueprint.nodes) {
            if (node.type != type) continue;
            const std::vector<PinSpec> inputs = BlueprintInputs(m_Blueprint, node);
            const PinSpec* bound = nullptr;
            for (const PinSpec& pin : inputs) if (pin.fixed) { bound = &pin; break; }
            if (!bound) { out.push_back(node.id); continue; }
            const Value literal = BlueprintLiteral(m_Blueprint, node, *bound);
            const std::string* text = std::get_if<std::string>(&literal);
            // No binding means every one of them. On Signal with no name hears them all, which is
            // what a log-everything node is for.
            if (!text || text->empty() || *text == target) out.push_back(node.id);
        }
        return out;
    }

    bool BlueprintProgram::HasEntry(std::string_view type) const {
        return std::ranges::any_of(m_Blueprint.nodes,
                                   [&](const BlueprintNode& node) { return node.type == type; });
    }

}
