#include "vaepch.h"
#include "vae/doc/Blueprint.h"

#include "vae/doc/ValueText.h"

#include <algorithm>

namespace vae::doc {

    using namespace vae::doc::text;

    const char* PinTypeName(PinType type) {
        switch (type) {
            case PinType::Exec:   return "exec";
            case PinType::Bool:   return "bool";
            case PinType::Number: return "number";
            case PinType::Text:   return "text";
            case PinType::Colour: return "colour";
            case PinType::Any:    return "any";
        }
        return "exec";
    }

    std::optional<PinType> PinTypeFromName(std::string_view name) {
        if (name == "exec")   return PinType::Exec;
        if (name == "bool")   return PinType::Bool;
        if (name == "number") return PinType::Number;
        if (name == "text")   return PinType::Text;
        if (name == "colour") return PinType::Colour;
        if (name == "any")    return PinType::Any;
        return std::nullopt;
    }

    ValueType ValueTypeOf(PinType type) {
        switch (type) {
            case PinType::Bool:   return ValueType::Bool;
            case PinType::Number: return ValueType::Number;
            case PinType::Text:   return ValueType::Text;
            case PinType::Colour: return ValueType::Colour;
            case PinType::Exec:
            case PinType::Any:    return ValueType::Unset;
        }
        return ValueType::Unset;
    }

    PinType PinTypeOf(ValueType type) {
        switch (type) {
            case ValueType::Bool:   return PinType::Bool;
            case ValueType::Number: return PinType::Number;
            case ValueType::Colour: return PinType::Colour;
            case ValueType::Text:   return PinType::Text;
            default: break;
        }
        return PinType::Text;
    }

    bool PinsCompatible(PinType from, PinType to) {
        // Execution is its own world. A wire that says "now" cannot be read as a value and a value
        // cannot say "now", and refusing that at the wire is what stops it becoming a run-time
        // surprise in a blueprint nobody is watching.
        if ((from == PinType::Exec) != (to == PinType::Exec)) return false;
        if (from == PinType::Exec) return true;
        if (from == PinType::Any || to == PinType::Any) return true;
        if (from == to) return true;
        // The conversions every language does silently, and only those. A number reads as text and
        // as a truth value; text reads as a number when it says one. A colour converts to nothing,
        // because "#3366ff as a number" is a question with no answer anyone means.
        if (to == PinType::Text)   return from == PinType::Number || from == PinType::Bool;
        if (to == PinType::Number) return from == PinType::Bool || from == PinType::Text;
        if (to == PinType::Bool)   return from == PinType::Number || from == PinType::Text;
        return false;
    }

    const char* BlueprintCategoryName(BlueprintCategory category) {
        switch (category) {
            case BlueprintCategory::Event:    return "Events";
            case BlueprintCategory::Flow:     return "Flow";
            case BlueprintCategory::Variable: return "Variables";
            case BlueprintCategory::Widget:   return "Widgets";
            case BlueprintCategory::App:      return "App";
            case BlueprintCategory::Data:     return "Data";
            case BlueprintCategory::Service:  return "Services";
            case BlueprintCategory::Count:    break;
        }
        return "";
    }

    namespace {

        // Shorthands, so the table below reads as a table rather than as ninety constructor calls.
        constexpr PinSpec In()  { return { "In",  PinType::Exec, "", false }; }
        constexpr PinSpec Out() { return { "Out", PinType::Exec, "", false }; }
        constexpr PinSpec Exec(std::string_view name) { return { name, PinType::Exec, "", false }; }
        constexpr PinSpec Num(std::string_view name, std::string_view literal = "0") {
            return { name, PinType::Number, literal, false };
        }
        constexpr PinSpec Txt(std::string_view name, std::string_view literal = "") {
            return { name, PinType::Text, literal, false };
        }
        constexpr PinSpec Yes(std::string_view name, std::string_view literal = "false") {
            return { name, PinType::Bool, literal, false };
        }
        constexpr PinSpec Col(std::string_view name, std::string_view literal = "#ffffffff") {
            return { name, PinType::Colour, literal, false };
        }
        constexpr PinSpec Any(std::string_view name) { return { name, PinType::Any, "", false }; }
        // A design-time fact rather than a value: which widget, which property, which timer. No
        // wire may end here — see PinSpec::fixed.
        constexpr PinSpec Fix(std::string_view name, std::string_view literal = "") {
            return { name, PinType::Text, literal, true };
        }

        // The node the widget calls act on. Empty is the component's own root, which is what a
        // script means by self, and is why the literal is empty rather than a name.
        constexpr PinSpec Node() { return Fix("Node"); }

        std::vector<BlueprintNodeType> BuildTypes() {
            std::vector<BlueprintNodeType> t;
            const auto Add = [&](BlueprintNodeType type) { t.push_back(std::move(type)); };

            // ---- events ----------------------------------------------------------------------
            // An entry point. No execution input, because nothing in the blueprint calls these: the
            // app does.
            const auto Event = [&](std::string_view id, std::string_view title,
                                   std::vector<PinSpec> in, std::vector<PinSpec> out,
                                   std::string_view summary, std::string_view call) {
                Add({ id, title, BlueprintCategory::Event, false, std::move(in), std::move(out),
                      summary, call });
            };
            Event("event.mount", "On Mount", {}, { Out() },
                  "The instance has arrived on screen.", "OnMount()");
            Event("event.update", "On Update", {}, { Out(), Num("Delta") },
                  "Every frame, with the seconds since the last one.", "OnUpdate(dt)");
            Event("event.unmount", "On Unmount", {}, { Out() },
                  "The instance is leaving the screen.", "OnUnmount()");
            Event("event.clicked", "On Clicked", { Node() }, { Out(), Num("Row") },
                  "A widget inside this component was clicked.", "e.Clicked(\"node\")");
            Event("event.changed", "On Changed", { Node() },
                  { Out(), Txt("Text"), Num("Number") },
                  "A field or a slider changed value.", "e.Changed(\"node\")");
            Event("event.submitted", "On Submitted", { Node() }, { Out(), Txt("Text") },
                  "Enter was pressed in a text field.", "e.Is(VAE_EVENT_SUBMITTED)");
            Event("event.rowClicked", "On Row Clicked", { Fix("List") }, { Out(), Num("Row") },
                  "A row of a repeated container was clicked.", "e.ClickedRow(\"list\")");
            Event("event.timer", "On Timer", { Fix("Timer", "tick") }, { Out() },
                  "A timer this blueprint started has come due.", "e.Timer(\"tick\")");
            Event("event.signal", "On Signal", { Fix("Signal") },
                  { Out(), Num("Number"), Txt("Text") },
                  "Another component emitted this signal.", "e.Is(VAE_EVENT_SIGNAL)");
            Event("event.answer", "On Answer", { Fix("Tag", "get") },
                  { Out(), Num("Status"), Txt("Body") },
                  "An http request tagged with this name came back.", "e.Answered(\"tag\")");
            Event("event.socketOpen", "On Socket Open", { Fix("Socket", "relay") }, { Out() },
                  "A live connection is up.", "e.Is(VAE_EVENT_SOCKET_OPEN)");
            Event("event.socketMessage", "On Socket Message", { Fix("Socket", "relay") },
                  { Out(), Txt("Text") },
                  "A message arrived on a live connection.", "e.Is(VAE_EVENT_SOCKET_MESSAGE)");
            Event("event.socketClosed", "On Socket Closed", { Fix("Socket", "relay") },
                  { Out(), Txt("Reason") },
                  "A live connection went away.", "e.Is(VAE_EVENT_SOCKET_CLOSED)");
            Event("event.opened", "On Opened", {}, { Out() },
                  "This screen was presented.", "e.Is(VAE_EVENT_OPENED)");
            Event("event.closed", "On Closed", {}, { Out() },
                  "This screen was closed.", "e.Is(VAE_EVENT_CLOSED)");
            Event("event.dismissed", "On Dismissed", {}, { Out() },
                  "This overlay was dismissed by clicking off it or pressing Escape.",
                  "e.Is(VAE_EVENT_DISMISSED)");

            // ---- flow ------------------------------------------------------------------------
            Add({ "flow.branch", "Branch", BlueprintCategory::Flow, false,
                  { In(), Yes("Condition") }, { Exec("True"), Exec("False") },
                  "Runs one side or the other.", "if (condition) … else …" });
            Add({ "flow.sequence", "Sequence", BlueprintCategory::Flow, false,
                  { In() }, { Exec("Then 0"), Exec("Then 1") },
                  "Runs each output in order, one after the other.", "one statement after another",
                  false, true });
            Add({ "flow.forLoop", "For Loop", BlueprintCategory::Flow, false,
                  { In(), Num("First", "0"), Num("Last", "10") },
                  { Exec("Body"), Num("Index"), Exec("Done") },
                  "Runs the body once for each number from first to last.",
                  "for (int i = first; i <= last; ++i)" });
            Add({ "flow.while", "While Loop", BlueprintCategory::Flow, false,
                  { In(), Yes("Condition") }, { Exec("Body"), Exec("Done") },
                  "Runs the body for as long as the condition holds.", "while (condition)" });
            Add({ "flow.doOnce", "Do Once", BlueprintCategory::Flow, false,
                  { In(), Exec("Reset") }, { Out() },
                  "Passes the first execution through and swallows the rest until it is reset.",
                  "if (!done) { done = true; … }" });
            Add({ "flow.delay", "Delay", BlueprintCategory::Flow, false,
                  { In(), Num("Seconds", "1") }, { Exec("Done") },
                  "Waits, then carries on. The app keeps running in the meantime.",
                  "self.After(seconds, \"…\")", true });

            // ---- variables -------------------------------------------------------------------
            Add({ "var.get", "Get", BlueprintCategory::Variable, true,
                  {}, { Any("Value") },
                  "Reads a variable.", "self.State(\"name\")" });
            Add({ "var.set", "Set", BlueprintCategory::Variable, false,
                  { In(), Any("Value") }, { Out(), Any("Value") },
                  "Writes a variable, and passes the value along.",
                  "self.SetState(\"name\", value)" });

            // ---- the component's own tree ----------------------------------------------------
            Add({ "ui.setText", "Set Text", BlueprintCategory::Widget, false,
                  { In(), Node(), Fix("Property", "text"), Txt("Value") }, { Out() },
                  "Writes a text property on a node.", "self[\"node\"].SetText(\"text\", v)" });
            Add({ "ui.getText", "Get Text", BlueprintCategory::Widget, true,
                  { Node(), Fix("Property", "text") }, { Txt("Value") },
                  "Reads a text property from a node.", "self[\"node\"].Text(\"text\")" });
            Add({ "ui.setNumber", "Set Number", BlueprintCategory::Widget, false,
                  { In(), Node(), Fix("Property", "value"), Num("Value") }, { Out() },
                  "Writes a numeric property on a node.", "self[\"node\"].SetNumber(\"value\", v)" });
            Add({ "ui.getNumber", "Get Number", BlueprintCategory::Widget, true,
                  { Node(), Fix("Property", "value") }, { Num("Value") },
                  "Reads a numeric property from a node.", "self[\"node\"].Number(\"value\")" });
            Add({ "ui.setBool", "Set Bool", BlueprintCategory::Widget, false,
                  { In(), Node(), Fix("Property", "checked"), Yes("Value") }, { Out() },
                  "Writes a true/false property on a node.",
                  "self[\"node\"].SetBool(\"checked\", v)" });
            Add({ "ui.getBool", "Get Bool", BlueprintCategory::Widget, true,
                  { Node(), Fix("Property", "checked") }, { Yes("Value") },
                  "Reads a true/false property from a node.", "self[\"node\"].Bool(\"checked\")" });
            Add({ "ui.setColour", "Set Colour", BlueprintCategory::Widget, false,
                  { In(), Node(), Fix("Property", "fill"), Col("Value") }, { Out() },
                  "Writes a colour property on a node.", "self[\"node\"].SetColour(\"fill\", v)" });
            Add({ "ui.getColour", "Get Colour", BlueprintCategory::Widget, true,
                  { Node(), Fix("Property", "fill") }, { Col("Value") },
                  "Reads a colour property from a node.", "self[\"node\"].Colour(\"fill\")" });
            // A component's own knobs, on one instance of it. Separate from the property nodes
            // above on purpose: those write a property on a node, and this sets a value the
            // component declared and everything inside it answers to. It is how a screen talks to
            // what is on it without knowing a single one of its node names.
            Add({ "ui.setProperty", "Set Component Property", BlueprintCategory::Widget, false,
                  { In(), Node(), Fix("Property"), Txt("Value") }, { Out() },
                  "Sets a knob the component declares, on one instance of it.",
                  "self[\"node\"].SetProperty(\"name\", v)" });
            Add({ "ui.getProperty", "Get Component Property", BlueprintCategory::Widget, true,
                  { Node(), Fix("Property") }, { Txt("Value") },
                  "Reads a knob the component declares, from one instance of it.",
                  "self[\"node\"].Property(\"name\")" });
            Add({ "ui.setVisible", "Set Visible", BlueprintCategory::Widget, false,
                  { In(), Node(), Yes("Visible", "true") }, { Out() },
                  "Shows or hides a node.", "self[\"node\"].SetVisible(v)" });
            Add({ "ui.setEnabled", "Set Enabled", BlueprintCategory::Widget, false,
                  { In(), Node(), Yes("Enabled", "true") }, { Out() },
                  "Enables or disables a node.", "self[\"node\"].SetEnabled(v)" });
            Add({ "ui.focus", "Focus", BlueprintCategory::Widget, false,
                  { In(), Node() }, { Out() },
                  "Gives a node the keyboard.", "self[\"node\"].Focus()" });
            Add({ "ui.scrollTo", "Scroll To", BlueprintCategory::Widget, false,
                  { In(), Node(), Num("Y") }, { Out() },
                  "Scrolls a container to an offset.", "self[\"node\"].ScrollTo(y)" });
            Add({ "ui.scrollToEnd", "Scroll To End", BlueprintCategory::Widget, false,
                  { In(), Node() }, { Out() },
                  "Scrolls a container to the bottom and keeps it there.",
                  "self[\"node\"].ScrollToEnd()" });
            Add({ "ui.setRows", "Set Rows", BlueprintCategory::Widget, false,
                  { In(), Node(), Txt("Rows") }, { Out() },
                  "Hands a repeated container its rows. The first line names the columns and "
                  "cells are separated by |.", "self[\"node\"].SetRows(rows)" });
            Add({ "ui.clearRows", "Clear Rows", BlueprintCategory::Widget, false,
                  { In(), Node() }, { Out() },
                  "Empties a repeated container.", "self[\"node\"].ClearRows()" });
            Add({ "ui.rowCount", "Row Count", BlueprintCategory::Widget, true,
                  { Node() }, { Num("Count") },
                  "How many rows a repeated container is showing.",
                  "self[\"node\"].RowCount()" });
            Add({ "ui.exists", "Node Exists", BlueprintCategory::Widget, true,
                  { Node() }, { Yes("Exists") },
                  "Whether a node by this name is on screen.", "self[\"node\"].Exists()" });

            // ---- the app around it -----------------------------------------------------------
            Add({ "app.emit", "Emit", BlueprintCategory::App, false,
                  { In(), Txt("Signal"), Num("Number"), Txt("Text") }, { Out() },
                  "Sends a signal every other live script hears.",
                  "self.Emit(name, number, text)" });
            Add({ "app.navigate", "Navigate", BlueprintCategory::App, false,
                  { In(), Fix("Route") }, { Out() },
                  "Goes to a screen.", "self.Navigate(\"route\")" });
            Add({ "app.back", "Back", BlueprintCategory::App, false,
                  { In() }, { Out(), Yes("Went") },
                  "Goes back, and says whether there was anywhere to go.", "self.Back()" });
            Add({ "app.toast", "Toast", BlueprintCategory::App, false,
                  { In(), Txt("Text"), Num("Seconds", "3") }, { Out() },
                  "Shows a short message over the app.", "self.Toast(text, seconds)" });
            Add({ "app.after", "After", BlueprintCategory::App, false,
                  { In(), Num("Seconds", "1"), Txt("Timer", "tick") }, { Out() },
                  "Starts a timer that raises On Timer when it comes due.",
                  "self.After(seconds, \"tick\")" });
            Add({ "app.cancel", "Cancel Timer", BlueprintCategory::App, false,
                  { In(), Txt("Timer", "tick") }, { Out() },
                  "Stops a timer before it fires.", "self.Cancel(\"tick\")" });
            Add({ "app.time", "App Time", BlueprintCategory::App, true,
                  {}, { Num("Seconds") },
                  "The app's own clock, in seconds since it started.", "self.Time()" });
            Add({ "app.log", "Log", BlueprintCategory::App, false,
                  { In(), Fix("Level", "info"), Txt("Text") }, { Out() },
                  "Writes a line to the console. Level is info, warn or error.",
                  "self.Info(text)" });
            Add({ "app.instanceName", "Instance Name", BlueprintCategory::App, true,
                  {}, { Txt("Name") },
                  "What this copy is called.", "self.Name()" });
            Add({ "app.componentName", "Component Name", BlueprintCategory::App, true,
                  {}, { Txt("Name") },
                  "Which component this is an instance of.", "self.ComponentName()" });
            Add({ "app.clock", "Clock", BlueprintCategory::App, true,
                  {}, { Num("Seconds") },
                  "Wall-clock seconds since the epoch.", "self.Clock()" });
            Add({ "app.date", "Date", BlueprintCategory::App, true,
                  { Txt("Format", "%Y-%m-%d %H:%M:%S") }, { Txt("Date") },
                  "The date and time, formatted.", "self.Date(format)" });

            // ---- services --------------------------------------------------------------------
            Add({ "store.getNumber", "Get Stored Number", BlueprintCategory::Service, true,
                  { Txt("Key"), Num("Default") }, { Num("Value") },
                  "Reads a number from the app's durable store.", "self.Stored(key, fallback)" });
            Add({ "store.setNumber", "Store Number", BlueprintCategory::Service, false,
                  { In(), Txt("Key"), Num("Value") }, { Out() },
                  "Writes a number to the app's durable store.", "self.Store(key, value)" });
            Add({ "store.getText", "Get Stored Text", BlueprintCategory::Service, true,
                  { Txt("Key"), Txt("Default") }, { Txt("Value") },
                  "Reads text from the app's durable store.", "self.StoredText(key, fallback)" });
            Add({ "store.setText", "Store Text", BlueprintCategory::Service, false,
                  { In(), Txt("Key"), Txt("Value") }, { Out() },
                  "Writes text to the app's durable store.", "self.Store(key, value)" });
            Add({ "store.has", "Has Stored", BlueprintCategory::Service, true,
                  { Txt("Key") }, { Yes("Has") },
                  "Whether the store holds anything under this key.", "self.HasStored(key)" });
            Add({ "store.forget", "Forget", BlueprintCategory::Service, false,
                  { In(), Txt("Key") }, { Out() },
                  "Removes a key from the store.", "self.Forget(key)" });
            Add({ "file.read", "Read File", BlueprintCategory::Service, true,
                  { Txt("Path") }, { Txt("Text") },
                  "Reads a file from inside the app's own folders.", "self.ReadFile(path)" });
            Add({ "file.write", "Write File", BlueprintCategory::Service, false,
                  { In(), Txt("Path"), Txt("Text") }, { Out(), Yes("Wrote") },
                  "Writes a file inside the app's own folders.", "self.WriteFile(path, text)" });
            Add({ "file.exists", "File Exists", BlueprintCategory::Service, true,
                  { Txt("Path") }, { Yes("Exists") },
                  "Whether a file is there.", "self.FileExists(path)" });
            Add({ "net.get", "Http Get", BlueprintCategory::Service, false,
                  { In(), Txt("Url"), Txt("Tag", "get") }, { Out() },
                  "Asks a server for something. The answer arrives at On Answer.",
                  "self.Get(url, tag)" });
            Add({ "net.post", "Http Post", BlueprintCategory::Service, false,
                  { In(), Txt("Url"), Txt("Body"), Txt("Tag", "post"),
                    Fix("Content Type", "application/json") }, { Out() },
                  "Sends something to a server. The answer arrives at On Answer.",
                  "self.Post(url, body, tag, contentType)" });
            Add({ "socket.open", "Open Socket", BlueprintCategory::Service, false,
                  { In(), Txt("Url"), Txt("Socket", "relay") }, { Out() },
                  "Opens a live connection.", "self.OpenSocket(url, name)" });
            Add({ "socket.send", "Send Socket", BlueprintCategory::Service, false,
                  { In(), Txt("Socket", "relay"), Txt("Text") }, { Out() },
                  "Sends a message down a live connection.", "self.SendSocket(name, text)" });
            Add({ "socket.close", "Close Socket", BlueprintCategory::Service, false,
                  { In(), Txt("Socket", "relay") }, { Out() },
                  "Closes a live connection.", "self.CloseSocket(name)" });
            Add({ "socket.live", "Socket Live", BlueprintCategory::Service, true,
                  { Txt("Socket", "relay") }, { Yes("Live") },
                  "Whether a live connection is up.", "self.SocketLive(name)" });
            Add({ "sound.play", "Play Sound", BlueprintCategory::Service, false,
                  { In(), Txt("Sound"), Num("Volume", "1"), Yes("Loop") },
                  { Out(), Num("Voice") },
                  "Plays a sound by the name it was imported under.",
                  "self.PlaySound(asset, volume, loop)" });
            Add({ "sound.stop", "Stop Sound", BlueprintCategory::Service, false,
                  { In(), Num("Voice") }, { Out() },
                  "Stops one playing sound.", "self.StopSound(voice)" });
            Add({ "sound.stopAll", "Stop Sounds", BlueprintCategory::Service, false,
                  { In() }, { Out() },
                  "Stops everything this instance is playing.", "self.StopSounds()" });
            Add({ "sound.volume", "Sound Volume", BlueprintCategory::Service, true,
                  {}, { Num("Volume") },
                  "How loud the app is, all together.", "self.SoundVolume()" });
            Add({ "sound.setVolume", "Set Sound Volume", BlueprintCategory::Service, false,
                  { In(), Num("Volume", "1") }, { Out() },
                  "Sets how loud the app is, all together.", "self.SetSoundVolume(v)" });

            // ---- data ------------------------------------------------------------------------
            // Every one of these is pure: no execution pins, evaluated when something reads it.
            const auto Pure = [&](std::string_view id, std::string_view title,
                                  std::vector<PinSpec> in, std::vector<PinSpec> out,
                                  std::string_view summary, std::string_view call) {
                Add({ id, title, BlueprintCategory::Data, true, std::move(in), std::move(out),
                      summary, call });
            };
            Pure("math.add", "Add", { Num("A"), Num("B") }, { Num("Value") }, "A + B", "a + b");
            Pure("math.subtract", "Subtract", { Num("A"), Num("B") }, { Num("Value") },
                 "A - B", "a - b");
            Pure("math.multiply", "Multiply", { Num("A"), Num("B", "1") }, { Num("Value") },
                 "A × B", "a * b");
            Pure("math.divide", "Divide", { Num("A"), Num("B", "1") }, { Num("Value") },
                 "A ÷ B. Dividing by zero gives zero rather than an infinity.", "a / b");
            Pure("math.modulo", "Modulo", { Num("A"), Num("B", "1") }, { Num("Value") },
                 "What is left over after dividing.", "std::fmod(a, b)");
            Pure("math.min", "Min", { Num("A"), Num("B") }, { Num("Value") },
                 "The smaller of the two.", "std::min(a, b)");
            Pure("math.max", "Max", { Num("A"), Num("B") }, { Num("Value") },
                 "The larger of the two.", "std::max(a, b)");
            Pure("math.clamp", "Clamp", { Num("Value"), Num("Min", "0"), Num("Max", "1") },
                 { Num("Value") }, "Held between two bounds.", "std::clamp(v, lo, hi)");
            Pure("math.abs", "Absolute", { Num("Value") }, { Num("Value") },
                 "Without its sign.", "std::abs(v)");
            Pure("math.floor", "Floor", { Num("Value") }, { Num("Value") },
                 "Rounded down.", "std::floor(v)");
            Pure("math.ceil", "Ceiling", { Num("Value") }, { Num("Value") },
                 "Rounded up.", "std::ceil(v)");
            Pure("math.round", "Round", { Num("Value") }, { Num("Value") },
                 "Rounded to the nearest whole number.", "std::round(v)");
            Pure("math.random", "Random", { Num("Min", "0"), Num("Max", "1") }, { Num("Value") },
                 "A number somewhere between the two.", "std::uniform_real_distribution");
            Pure("compare.equal", "Equal", { Num("A"), Num("B") }, { Yes("Value") },
                 "A = B", "a == b");
            Pure("compare.notEqual", "Not Equal", { Num("A"), Num("B") }, { Yes("Value") },
                 "A ≠ B", "a != b");
            Pure("compare.less", "Less", { Num("A"), Num("B") }, { Yes("Value") },
                 "A < B", "a < b");
            Pure("compare.lessEqual", "Less Or Equal", { Num("A"), Num("B") }, { Yes("Value") },
                 "A ≤ B", "a <= b");
            Pure("compare.greater", "Greater", { Num("A"), Num("B") }, { Yes("Value") },
                 "A > B", "a > b");
            Pure("compare.greaterEqual", "Greater Or Equal", { Num("A"), Num("B") },
                 { Yes("Value") }, "A ≥ B", "a >= b");
            Pure("logic.and", "And", { Yes("A"), Yes("B") }, { Yes("Value") },
                 "True when both are.", "a && b");
            Pure("logic.or", "Or", { Yes("A"), Yes("B") }, { Yes("Value") },
                 "True when either is.", "a || b");
            Pure("logic.not", "Not", { Yes("Value") }, { Yes("Value") },
                 "The other one.", "!v");
            Pure("text.join", "Join", { Txt("A"), Txt("B") }, { Txt("Value") },
                 "Two pieces of text, one after the other.", "a + b");
            Pure("text.fromNumber", "Number To Text", { Num("Number"), Num("Decimals") },
                 { Txt("Text") }, "A number written out.", "std::to_string(n)");
            Pure("text.toNumber", "Text To Number", { Txt("Text") }, { Num("Number") },
                 "What a piece of text says as a number, or zero.", "std::strtod");
            Pure("text.equal", "Text Equal", { Txt("A"), Txt("B") }, { Yes("Value") },
                 "Whether two pieces of text say the same thing.", "a == b");
            Pure("text.empty", "Text Is Empty", { Txt("Text") }, { Yes("Value") },
                 "Whether there is nothing there.", "text.empty()");
            Pure("text.length", "Text Length", { Txt("Text") }, { Num("Length") },
                 "How many characters.", "text.size()");
            Pure("text.contains", "Text Contains", { Txt("Text"), Txt("Part") }, { Yes("Value") },
                 "Whether one piece of text is inside the other.", "text.find(part)");
            Pure("text.upper", "Upper Case", { Txt("Text") }, { Txt("Value") },
                 "Shouted.", "std::toupper");
            Pure("text.lower", "Lower Case", { Txt("Text") }, { Txt("Value") },
                 "Quietly.", "std::tolower");
            Pure("text.trim", "Trim", { Txt("Text") }, { Txt("Value") },
                 "Without the spaces at either end.", "trim(text)");
            Pure("select.number", "Select Number",
                 { Yes("Condition"), Num("True"), Num("False") }, { Num("Value") },
                 "One number or the other.", "condition ? a : b");
            Pure("select.text", "Select Text",
                 { Yes("Condition"), Txt("True"), Txt("False") }, { Txt("Value") },
                 "One piece of text or the other.", "condition ? a : b");
            Pure("make.number", "Number", { Num("Value") }, { Num("Value") },
                 "A number, written once and used in several places.", "a constant");
            Pure("make.text", "Text", { Txt("Value") }, { Txt("Value") },
                 "A piece of text, written once and used in several places.", "a constant");
            Pure("make.bool", "Bool", { Yes("Value") }, { Yes("Value") },
                 "True or false, written once and used in several places.", "a constant");
            Pure("make.colour", "Colour", { Col("Value") }, { Col("Value") },
                 "A colour, written once and used in several places.", "a constant");
            Pure("colour.rgba", "Make Colour",
                 { Num("Red"), Num("Green"), Num("Blue"), Num("Alpha", "1") }, { Col("Value") },
                 "A colour from four channels, each nought to one.", "Color{ r, g, b, a }");
            return t;
        }

    }

    const std::vector<BlueprintNodeType>& BlueprintNodeTypes() {
        static const std::vector<BlueprintNodeType> types = BuildTypes();
        return types;
    }

    const BlueprintNodeType* FindBlueprintNodeType(std::string_view id) {
        for (const BlueprintNodeType& type : BlueprintNodeTypes())
            if (type.id == id) return &type;
        return nullptr;
    }

    // --- the blueprint itself --------------------------------------------------------------------

    u32 Blueprint::MintId() { return m_NextId++; }

    void Blueprint::RecomputeNextId() {
        u32 highest = 0;
        for (const BlueprintNode& node : nodes)       highest = std::max(highest, node.id);
        for (const BlueprintLink& link : links)       highest = std::max(highest, link.id);
        for (const BlueprintComment& box : comments)  highest = std::max(highest, box.id);
        m_NextId = highest + 1;
    }

    BlueprintNode* Blueprint::Find(u32 id) {
        for (BlueprintNode& node : nodes) if (node.id == id) return &node;
        return nullptr;
    }

    const BlueprintNode* Blueprint::Find(u32 id) const {
        for (const BlueprintNode& node : nodes) if (node.id == id) return &node;
        return nullptr;
    }

    BlueprintComment* Blueprint::FindComment(u32 id) {
        for (BlueprintComment& box : comments) if (box.id == id) return &box;
        return nullptr;
    }

    u32 Blueprint::AddNode(BlueprintNode node) {
        if (node.id == 0) node.id = MintId();
        else m_NextId = std::max(m_NextId, node.id + 1);
        const u32 id = node.id;
        nodes.push_back(std::move(node));
        return id;
    }

    void Blueprint::RemoveNode(u32 id) {
        std::erase_if(links, [&](const BlueprintLink& link) {
            return link.from == id || link.to == id;
        });
        std::erase_if(nodes, [&](const BlueprintNode& node) { return node.id == id; });
    }

    u32 Blueprint::AddLink(BlueprintLink link) {
        if (link.id == 0) link.id = MintId();
        else m_NextId = std::max(m_NextId, link.id + 1);
        const u32 id = link.id;
        links.push_back(std::move(link));
        return id;
    }

    void Blueprint::RemoveLink(u32 id) {
        std::erase_if(links, [&](const BlueprintLink& link) { return link.id == id; });
    }

    void Blueprint::DisplaceAt(u32 node, std::string_view pin, bool isInput, bool exec) {
        // A data input takes one wire — a value has one source. An execution output drives one
        // target — "next" is singular. The other two directions fan out freely, which is what a
        // value read in three places and a node reached from three places both are.
        const bool single = isInput != exec;
        if (!single) return;
        std::erase_if(links, [&](const BlueprintLink& link) {
            return isInput ? (link.to == node && link.toPin == pin)
                           : (link.from == node && link.fromPin == pin);
        });
    }

    const BlueprintVariable* Blueprint::FindVariable(std::string_view name) const {
        for (const BlueprintVariable& variable : variables)
            if (variable.name == name) return &variable;
        return nullptr;
    }

    void Blueprint::SetVariable(BlueprintVariable variable) {
        for (BlueprintVariable& existing : variables)
            if (existing.name == variable.name) { existing = std::move(variable); return; }
        variables.push_back(std::move(variable));
    }

    void Blueprint::RemoveVariable(std::string_view name) {
        std::erase_if(variables, [&](const BlueprintVariable& v) { return v.name == name; });
    }

    void Blueprint::RenameVariable(std::string_view from, std::string_view to) {
        for (BlueprintVariable& variable : variables)
            if (variable.name == from) variable.name = to;
        for (BlueprintNode& node : nodes)
            if (node.target == from && (node.type == "var.get" || node.type == "var.set"))
                node.target = to;
    }

    const BlueprintLink* Blueprint::LinkInto(u32 node, std::string_view pin) const {
        for (const BlueprintLink& link : links)
            if (link.to == node && link.toPin == pin) return &link;
        return nullptr;
    }

    const BlueprintLink* Blueprint::LinkOutOf(u32 node, std::string_view pin) const {
        for (const BlueprintLink& link : links)
            if (link.from == node && link.fromPin == pin) return &link;
        return nullptr;
    }

    std::vector<const BlueprintLink*> Blueprint::LinksOutOf(u32 node, std::string_view pin) const {
        std::vector<const BlueprintLink*> out;
        for (const BlueprintLink& link : links)
            if (link.from == node && link.fromPin == pin) out.push_back(&link);
        return out;
    }

    // --- pins, once the blueprint has answered for them -------------------------------------------

    std::string SequencePinName(u32 index) { return "Then " + std::to_string(index); }

    namespace {
        // A variable node's pins are the variable's type. An unknown variable stays Any rather than
        // guessing: the compiler will say which one is missing, and a guess would hide it behind a
        // type error somewhere else.
        PinType VariablePin(const Blueprint& blueprint, const BlueprintNode& node) {
            const BlueprintVariable* variable = blueprint.FindVariable(node.target);
            return variable ? PinTypeOf(variable->type) : PinType::Any;
        }

        void Resolve(const Blueprint& blueprint, const BlueprintNode& node, std::vector<PinSpec>& pins) {
            if (node.type != "var.get" && node.type != "var.set") return;
            const PinType type = VariablePin(blueprint, node);
            for (PinSpec& pin : pins)
                if (pin.type == PinType::Any) pin.type = type;
        }
    }

    std::vector<PinSpec> BlueprintInputs(const Blueprint& blueprint, const BlueprintNode& node) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return {};
        std::vector<PinSpec> pins = type->inputs;
        Resolve(blueprint, node, pins);
        return pins;
    }

    std::vector<PinSpec> BlueprintOutputs(const Blueprint& blueprint, const BlueprintNode& node) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return {};
        std::vector<PinSpec> pins = type->outputs;
        Resolve(blueprint, node, pins);
        if (type->variadicOut) {
            // The names are generated, so they have to be owned. A small static pool covers every
            // sequence anybody draws, and the table itself stays free of allocations.
            static std::vector<std::string> names = [] {
                std::vector<std::string> out;
                for (u32 i = 0; i < 64; ++i) out.push_back(SequencePinName(i));
                return out;
            }();
            pins.clear();
            const u32 count = std::min(2u + node.extraPins, static_cast<u32>(names.size()));
            for (u32 i = 0; i < count; ++i) pins.push_back({ names[i], PinType::Exec, "", false });
        }
        return pins;
    }

    std::string BlueprintNodeTitle(const Blueprint& blueprint, const BlueprintNode& node) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return node.type.empty() ? "?" : node.type;
        if (node.type == "var.get") return "Get " + (node.target.empty() ? "?" : node.target);
        if (node.type == "var.set") return "Set " + (node.target.empty() ? "?" : node.target);
        (void)blueprint;
        return std::string(type->title);
    }

    Value ParsePinLiteral(std::string_view text, PinType type) {
        switch (type) {
            case PinType::Bool:   return text == "true" || text == "1";
            case PinType::Number: return ParseNumber(text).value_or(0.0f);
            case PinType::Colour: return ColorFromHex(text).value_or(Color{ 1, 1, 1, 1 });
            case PinType::Text:   return std::string(text);
            case PinType::Exec:
            case PinType::Any:    break;
        }
        return {};
    }

    std::string PinLiteralText(const Value& value, PinType type) {
        switch (type) {
            case PinType::Bool:
                return std::get_if<bool>(&value) && std::get<bool>(value) ? "true" : "false";
            case PinType::Number:
                return Number(std::get_if<f32>(&value) ? std::get<f32>(value) : 0.0f);
            case PinType::Colour: {
                const Color* colour = std::get_if<Color>(&value);
                if (!colour) return "#ffffffff";
                return ColorToHex(*colour).value_or("#ffffffff");
            }
            case PinType::Text: {
                const std::string* text = std::get_if<std::string>(&value);
                return text ? *text : std::string();
            }
            case PinType::Exec:
            case PinType::Any:  break;
        }
        return {};
    }

    Value BlueprintLiteral(const Blueprint& blueprint, const BlueprintNode& node, const PinSpec& pin) {
        const auto it = node.literals.find(std::string(pin.name));
        if (it != node.literals.end() && IsSet(it->second)) return it->second;
        PinType type = pin.type;
        if (type == PinType::Any) type = VariablePin(blueprint, node);
        return ParsePinLiteral(pin.literal, type);
    }

}
