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
        // ---- what is declared, before what is drawn ---------------------------------------------
        for (std::size_t i = 0; i < m_Blueprint.functions.size(); ++i) {
            const BlueprintFunction& function = m_Blueprint.functions[i];
            if (function.name.empty()) {
                m_Diagnostics.push_back({ {}, 0, true, "a function with no name" });
                continue;
            }
            for (std::size_t j = i + 1; j < m_Blueprint.functions.size(); ++j)
                if (m_Blueprint.functions[j].name == function.name)
                    m_Diagnostics.push_back({ {}, 0, true, "there are two functions called '"
                                                           + function.name + "'" });
            if (function.event && !function.returns.empty())
                m_Diagnostics.push_back({ function.name, 0, true,
                    "a custom event hands nothing back — it is an entry point, not a question" });
            if (function.event && !function.locals.empty())
                m_Diagnostics.push_back({ function.name, 0, true,
                    "a custom event has no local variables: it can be suspended by a Delay, and "
                    "what a local held would not be there when it woke up" });

            const BlueprintNode* entry = function.body.FindType("func.entry");
            if (!entry)
                m_Diagnostics.push_back({ function.name, 0, true,
                    "this has no Entry, so there is nowhere for it to start" });
            std::size_t entries = 0, returns = 0;
            for (const BlueprintNode& node : function.body.nodes) {
                if (node.type == "func.entry")  ++entries;
                if (node.type == "func.return") ++returns;
            }
            if (entries > 1)
                m_Diagnostics.push_back({ function.name, 0, true, "two Entry nodes, and only one "
                                                                  "of them would be the start" });
            if (returns > 1)
                m_Diagnostics.push_back({ function.name, 0, true, "two Return nodes" });
            if (!function.returns.empty() && returns == 0)
                m_Diagnostics.push_back({ function.name, 0, false,
                    "this hands something back but has no Return, so it always hands back the "
                    "defaults" });
        }

        for (std::size_t i = 0; i < m_Blueprint.variables.size(); ++i) {
            const BlueprintVariable& variable = m_Blueprint.variables[i];
            if (variable.name.empty()) {
                m_Diagnostics.push_back({ {}, 0, true, "a variable with no name" });
                continue;
            }
            for (std::size_t j = i + 1; j < m_Blueprint.variables.size(); ++j)
                if (m_Blueprint.variables[j].name == variable.name)
                    m_Diagnostics.push_back({ {}, 0, true, "there are two variables called '"
                                                           + variable.name + "'" });
        }

        // ---- then every canvas ------------------------------------------------------------------
        for (const auto& [name, canvas] : m_Blueprint.Canvases()) CheckCanvas(name, *canvas);
    }

    void BlueprintProgram::CheckCanvas(std::string_view function, const BlueprintCanvas& canvas) {
        const std::string owner(function);
        const auto Error = [&](u32 node, std::string message) {
            m_Diagnostics.push_back({ owner, node, true, std::move(message) });
        };
        const auto Warn = [&](u32 node, std::string message) {
            m_Diagnostics.push_back({ owner, node, false, std::move(message) });
        };
        const BlueprintFunction* inside = m_Blueprint.FindFunction(function);

        std::unordered_map<u32, const BlueprintNode*> byId;
        for (const BlueprintNode& node : canvas.nodes) {
            if (byId.contains(node.id)) {
                Error(node.id, "two nodes share the id " + std::to_string(node.id));
                continue;
            }
            byId[node.id] = &node;
        }

        for (const BlueprintNode& node : canvas.nodes) {
            const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
            if (!type) { Error(node.id, "there is no node called " + Quote(node.type)); continue; }

            if (node.type == "var.get" || node.type == "var.set") {
                if (node.target.empty())
                    Error(node.id, "this reads a variable but does not say which");
                else if (!m_Blueprint.FindVariable(node.target, function))
                    Error(node.id, "there is no variable called " + Quote(node.target));
                else if (node.type == "var.set" && inside)
                    for (const BlueprintParam& param : inside->params)
                        if (param.name == node.target)
                            Error(node.id, "a parameter is what this was handed, not somewhere to "
                                           "put something — copy it into a local first");
            }

            if (node.type == "func.call") {
                if (node.target.empty()) Error(node.id, "this calls something but does not say what");
                else if (!m_Blueprint.FindFunction(node.target))
                    Error(node.id, "there is no function called " + Quote(node.target));
            }

            // Where a node is allowed to be. Both directions are worth saying, because both are
            // things somebody will draw.
            const bool isEvent = type->category == BlueprintCategory::Event;
            if (isEvent && inside)
                Error(node.id, "an event is where the app reaches in, so it belongs on the event "
                               "graph rather than inside a function");
            if ((node.type == "func.entry" || node.type == "func.return") && !inside)
                Error(node.id, std::string(type->title)
                             + " belongs inside a function, not on the event graph");

            // A function runs to completion inside the statement that called it, so there is
            // nowhere for the caller to have got to while a Delay waits. Unreal refuses this for
            // the same reason, and its answer is the same: use a custom event.
            if (type->latent) {
                m_UsesTimers = true;
                if (inside && !inside->event)
                    Error(node.id, std::string(type->title)
                                 + " cannot be inside a function — a function has to finish before "
                                   "its caller carries on. A custom event can wait.");
            }
            if (node.type == "app.after") m_UsesTimers = true;
            // Calling something that waits is the same problem one step removed.
            if (node.type == "func.call" && inside && !inside->event)
                if (const BlueprintFunction* called = m_Blueprint.FindFunction(node.target))
                    if (called->event)
                        Error(node.id, "a function cannot call a custom event: an event may wait, "
                                       "and a function has to finish");

            // An event that binds to nothing answers to nothing. Worth a warning rather than an
            // error: it is what a half-drawn blueprint looks like a second after the node was
            // dropped, and refusing to run the rest over it would be unusable.
            if (isEvent) {
                for (const PinSpec& pin : BlueprintInputs(m_Blueprint, node, function))
                    if (pin.fixed) {
                        const Value literal = BlueprintLiteral(m_Blueprint, node, pin, function);
                        const std::string* text = std::get_if<std::string>(&literal);
                        if (text && text->empty())
                            Warn(node.id, std::string(type->title) + " does not say which "
                                        + std::string(pin.name) + " it answers to, so it never runs");
                    }
            }
        }

        // Two events of the same kind bound to the same thing is a blueprint that runs one of them
        // twice as far as anyone reading it is concerned.
        for (std::size_t i = 0; i < canvas.nodes.size(); ++i) {
            const BlueprintNode& a = canvas.nodes[i];
            const BlueprintNodeType* type = FindBlueprintNodeType(a.type);
            if (!type || type->category != BlueprintCategory::Event) continue;
            for (std::size_t j = i + 1; j < canvas.nodes.size(); ++j) {
                const BlueprintNode& b = canvas.nodes[j];
                if (b.type != a.type) continue;
                bool same = true;
                for (const PinSpec& pin : BlueprintInputs(m_Blueprint, a, function))
                    if (pin.fixed && BlueprintLiteral(m_Blueprint, a, pin, function)
                                  != BlueprintLiteral(m_Blueprint, b, pin, function))
                        same = false;
                if (same)
                    Error(b.id, "there is already a " + std::string(type->title)
                              + " for this, and two of them would both run");
            }
        }

        // ---- wires -------------------------------------------------------------------------
        for (const BlueprintLink& link : canvas.links) {
            const auto from = byId.find(link.from);
            const auto to   = byId.find(link.to);
            if (from == byId.end() || to == byId.end()) {
                Error(0, "a wire ends at a node that is not here");
                continue;
            }
            const std::vector<PinSpec> outputs = BlueprintOutputs(m_Blueprint, *from->second, function);
            const std::vector<PinSpec> inputs  = BlueprintInputs(m_Blueprint, *to->second, function);
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
        // so this is about a file — hand-edited markup, or one from a build that did not.
        std::unordered_map<std::string, u32> seen;
        for (const BlueprintLink& link : canvas.links) {
            const auto to = byId.find(link.to);
            if (to == byId.end()) continue;
            const PinSpec* in = FindPin(BlueprintInputs(m_Blueprint, *to->second, function), link.toPin);
            if (!in || in->type == PinType::Exec) continue;
            const std::string key = std::to_string(link.to) + "\n" + link.toPin;
            if (seen[key]++ > 0)
                Error(link.to, Quote(link.toPin) + " has more than one thing wired into it");
        }
        seen.clear();
        for (const BlueprintLink& link : canvas.links) {
            const auto from = byId.find(link.from);
            if (from == byId.end()) continue;
            const PinSpec* out = FindPin(BlueprintOutputs(m_Blueprint, *from->second, function),
                                         link.fromPin);
            if (!out || out->type != PinType::Exec) continue;
            const std::string key = std::to_string(link.from) + "\n" + link.fromPin;
            if (seen[key]++ > 0)
                Error(link.from, Quote(link.fromPin) + " goes to more than one place, and only "
                                 "one of them would run");
        }

        // ---- data cycles ---------------------------------------------------------------------
        std::vector<u32> done;
        for (const BlueprintNode& node : canvas.nodes) {
            std::vector<u32> path;
            if (Cycles(canvas, function, node.id, path, done))
                Error(node.id, "this value is worked out from itself");
        }

        // ---- what will never run, and what has nothing to break out of --------------------------
        const std::vector<u32> inLoop = InsideLoops(canvas);
        for (const BlueprintNode& node : canvas.nodes) {
            const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
            if (!type) continue;
            if (node.type == "flow.break"
                && std::ranges::find(inLoop, node.id) == inLoop.end())
                Error(node.id, "there is no loop around this to break out of");
            if (IsPureNode(m_Blueprint, node) || type->category == BlueprintCategory::Event)
                continue;
            if (node.type == "func.entry") continue;
            bool reachable = false;
            for (const PinSpec& pin : BlueprintInputs(m_Blueprint, node, function))
                if (pin.type == PinType::Exec && canvas.LinkInto(node.id, pin.name))
                    reachable = true;
            if (!reachable)
                Warn(node.id, "nothing reaches this, so it never runs");
        }
    }

    std::vector<u32> BlueprintProgram::InsideLoops(const BlueprintCanvas& canvas) const {
        // Everything execution can get to from a loop's body, following execution wires only. A
        // Break anywhere in there has a loop; a Break anywhere else is a statement with no meaning.
        std::vector<u32> inside;
        std::vector<u32> stack;
        for (const BlueprintNode& node : canvas.nodes) {
            const bool loop = node.type == "flow.forLoop" || node.type == "flow.while"
                           || node.type == "flow.forEach";
            if (!loop) continue;
            for (const BlueprintLink* link : canvas.LinksOutOf(node.id, "Body"))
                stack.push_back(link->to);
        }
        while (!stack.empty()) {
            const u32 at = stack.back();
            stack.pop_back();
            if (std::ranges::find(inside, at) != inside.end()) continue;
            inside.push_back(at);
            const BlueprintNode* node = canvas.Find(at);
            if (!node) continue;
            for (const PinSpec& pin : BlueprintOutputs(m_Blueprint, *node)) {
                if (pin.type != PinType::Exec) continue;
                for (const BlueprintLink* link : canvas.LinksOutOf(at, pin.name))
                    stack.push_back(link->to);
            }
        }
        return inside;
    }

    bool BlueprintProgram::Cycles(const BlueprintCanvas& canvas, std::string_view function, u32 node,
                                  std::vector<u32>& path, std::vector<u32>& done) {
        if (std::ranges::find(done, node) != done.end()) return false;
        if (std::ranges::find(path, node) != path.end()) return true;
        path.push_back(node);

        const BlueprintNode* current = canvas.Find(node);
        if (current) {
            for (const PinSpec& pin : BlueprintInputs(m_Blueprint, *current, function)) {
                if (pin.type == PinType::Exec) continue;
                const BlueprintLink* link = canvas.LinkInto(node, pin.name);
                if (!link) continue;
                // Only a PURE source can loop: an impure node's data output is whatever it left
                // there when it ran, which is a value, not a question asked again.
                const BlueprintNode* source = canvas.Find(link->from);
                if (!source || !IsPureNode(m_Blueprint, *source)) continue;
                if (Cycles(canvas, function, link->from, path, done)) return true;
            }
        }

        path.pop_back();
        done.push_back(node);
        return false;
    }

    std::vector<u32> BlueprintProgram::Entries(std::string_view type) const {
        std::vector<u32> out;
        for (const BlueprintNode& node : m_Blueprint.graph.nodes)
            if (node.type == type) out.push_back(node.id);
        return out;
    }

    std::vector<u32> BlueprintProgram::EntriesFor(std::string_view type,
                                                  std::string_view target) const {
        std::vector<u32> out;
        for (const BlueprintNode& node : m_Blueprint.graph.nodes) {
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
        return std::ranges::any_of(m_Blueprint.graph.nodes,
                                   [&](const BlueprintNode& node) { return node.type == type; });
    }

}
