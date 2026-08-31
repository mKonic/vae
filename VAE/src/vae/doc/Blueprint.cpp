#include "vaepch.h"
#include "vae/doc/Blueprint.h"

#include "vae/doc/ValueText.h"

#include <algorithm>
#include <map>

namespace vae::doc {

    using namespace vae::doc::text;

    const char* PinTypeName(PinType type) {
        switch (type) {
            case PinType::Exec:   return "exec";
            case PinType::Bool:   return "bool";
            case PinType::Number: return "number";
            case PinType::Text:   return "text";
            case PinType::Colour: return "colour";
            case PinType::List:   return "list";
            case PinType::Map:    return "map";
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
        if (name == "list")   return PinType::List;
        if (name == "map")    return PinType::Map;
        if (name == "any")    return PinType::Any;
        return std::nullopt;
    }

    ValueType ValueTypeOf(PinType type) {
        switch (type) {
            case PinType::Bool:   return ValueType::Bool;
            case PinType::Number: return ValueType::Number;
            case PinType::Text:   return ValueType::Text;
            case PinType::Colour: return ValueType::Colour;
            // A list and a map are held as text — see doc::ListText below — so that is what they
            // are as a document value, and what a variable of that type stores.
            case PinType::List:
            case PinType::Map:    return ValueType::Text;
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
        //
        // A list or a map reads as text, because its text form is what it IS — that is how it is
        // stored and how Set Rows wants it. Text does not read back as a list: turning a string
        // into a collection is a decision about separators, and Split is where that decision is
        // written down rather than guessed at on a wire.
        if (from == PinType::List || from == PinType::Map) return to == PinType::Text;
        if (to == PinType::List || to == PinType::Map)     return false;
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
            case BlueprintCategory::Collection: return "Lists and maps";
            case BlueprintCategory::Function: return "Functions";
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
        constexpr PinSpec List(std::string_view name) { return { name, PinType::List, "", false }; }
        constexpr PinSpec Map(std::string_view name)  { return { name, PinType::Map,  "", false }; }
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
                  { In() }, { Exec("Then") },
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
            Add({ "switch.number", "Switch On Number", BlueprintCategory::Flow, false,
                  { In(), Num("Value"), Num("Case") },
                  { Exec("Case"), Exec("Default") },
                  "Runs the branch whose case the value equals, or Default.",
                  "switch (value) { case 1: … default: … }", false, true, true });
            Add({ "switch.text", "Switch On Text", BlueprintCategory::Flow, false,
                  { In(), Txt("Value"), Txt("Case") },
                  { Exec("Case"), Exec("Default") },
                  "Runs the branch whose case the text equals, or Default.",
                  "if (value == \"a\") … else if … else …", false, true, true });
            Add({ "flow.forEach", "For Each", BlueprintCategory::Flow, false,
                  { In(), List("List") },
                  { Exec("Body"), Any("Element"), Num("Index"), Exec("Done") },
                  "Runs the body once for every item in a list.",
                  "for (const auto& item : list)" });
            Add({ "flow.break", "Break", BlueprintCategory::Flow, false,
                  { In() }, {},
                  "Stops the loop this is inside, and carries on from its Done.", "break" });
            Add({ "flow.doN", "Do N", BlueprintCategory::Flow, false,
                  { In(), Num("N", "1"), Exec("Reset") }, { Out(), Num("Counter") },
                  "Passes the first N executions through and swallows the rest until it is reset.",
                  "if (count < n) { ++count; … }" });
            Add({ "flow.flipFlop", "Flip Flop", BlueprintCategory::Flow, false,
                  { In() }, { Exec("A"), Exec("B"), Yes("Is A") },
                  "Alternates between the two outputs, one execution each.",
                  "a = !a; if (a) … else …" });
            Add({ "flow.gate", "Gate", BlueprintCategory::Flow, false,
                  { In(), Exec("Open"), Exec("Close"), Exec("Toggle"), Yes("Start Closed", "true") },
                  { Out() },
                  "Lets execution through only while it is open.",
                  "if (open) …" });
            Add({ "flow.delay", "Delay", BlueprintCategory::Flow, false,
                  { In(), Num("Seconds", "1") }, { Exec("Done") },
                  "Waits, then carries on. The app keeps running in the meantime.",
                  "self.After(seconds, \"…\")", true });

            // A wire that goes a long way is easier to follow with a corner in it. Both of these
            // do nothing at all, which is the whole point.
            Add({ "flow.reroute", "Reroute", BlueprintCategory::Flow, false,
                  { In() }, { Out() },
                  "A corner to bend an execution wire around.", "nothing at all" });
            Add({ "flow.retriggerableDelay", "Retriggerable Delay", BlueprintCategory::Flow, false,
                  { In(), Num("Seconds", "1") }, { Exec("Done") },
                  "Waits, and starts waiting again from the beginning if it is reached before it "
                  "is done.", "self.After(seconds, \"…\")", true });

            // ---- functions and custom events -------------------------------------------------
            // Three of these have no pins in the table at all: their pins ARE a signature, filled
            // in by BlueprintInputs/BlueprintOutputs from the function they belong to or call.
            Add({ "func.entry", "Entry", BlueprintCategory::Function, false,
                  {}, {},
                  "Where this function or event starts, and what it was handed.",
                  "the function's parameters" });
            Add({ "func.return", "Return", BlueprintCategory::Function, false,
                  {}, {},
                  "What this function hands back, and the end of it.", "return …" });
            Add({ "func.call", "Call", BlueprintCategory::Function, false,
                  {}, {},
                  "Runs a function or a custom event of this blueprint.", "a function call" });

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

            // ---- lists and maps ----------------------------------------------------------------
            // Every one of these hands back a NEW list rather than changing the one it was given.
            // Wire it into a Set to keep it. Unreal mutates through a reference; this does not,
            // because a value that two wires share and one of them changes is the bug that costs
            // an afternoon, and "the answer comes out of the node" is the rule everything else in
            // a blueprint already follows.
            Add({ "list.make", "Make List", BlueprintCategory::Collection, true,
                  { Txt("Item") }, { List("Value") },
                  "A list of the items given.", "std::vector<std::string>{ … }", false, false, true });
            Add({ "list.length", "List Length", BlueprintCategory::Collection, true,
                  { List("List") }, { Num("Length") },
                  "How many items.", "list.size()" });
            Add({ "list.empty", "List Is Empty", BlueprintCategory::Collection, true,
                  { List("List") }, { Yes("Value") },
                  "Whether there is nothing in it.", "list.empty()" });
            Add({ "list.get", "List Get", BlueprintCategory::Collection, true,
                  { List("List"), Num("Index") }, { Txt("Value") },
                  "The item at an index. Out of range is empty rather than a crash.", "list[i]" });
            Add({ "list.set", "List Set", BlueprintCategory::Collection, false,
                  { In(), List("List"), Num("Index"), Txt("Value") }, { Out(), List("List") },
                  "The list with one item replaced.", "list[i] = v" });
            Add({ "list.add", "List Add", BlueprintCategory::Collection, false,
                  { In(), List("List"), Txt("Value") }, { Out(), List("List") },
                  "The list with an item on the end.", "list.push_back(v)" });
            Add({ "list.insert", "List Insert", BlueprintCategory::Collection, false,
                  { In(), List("List"), Num("Index"), Txt("Value") }, { Out(), List("List") },
                  "The list with an item put in at an index.", "list.insert(…)" });
            Add({ "list.removeAt", "List Remove At", BlueprintCategory::Collection, false,
                  { In(), List("List"), Num("Index") }, { Out(), List("List") },
                  "The list without the item at an index.", "list.erase(…)" });
            Add({ "list.remove", "List Remove", BlueprintCategory::Collection, false,
                  { In(), List("List"), Txt("Value") }, { Out(), List("List") },
                  "The list without the first item equal to this.", "std::erase(list, v)" });
            Add({ "list.clear", "List Clear", BlueprintCategory::Collection, false,
                  { In(), List("List") }, { Out(), List("List") },
                  "An empty list.", "list.clear()" });
            Add({ "list.contains", "List Contains", BlueprintCategory::Collection, true,
                  { List("List"), Txt("Value") }, { Yes("Value") },
                  "Whether an item equal to this is in it.", "std::ranges::find(list, v)" });
            Add({ "list.find", "List Find", BlueprintCategory::Collection, true,
                  { List("List"), Txt("Value") }, { Num("Index") },
                  "Where the first item equal to this is, or -1.", "std::ranges::find(list, v)" });
            Add({ "list.append", "List Append", BlueprintCategory::Collection, true,
                  { List("A"), List("B") }, { List("Value") },
                  "One list after the other.", "a.insert(a.end(), b.begin(), b.end())" });
            Add({ "list.reverse", "List Reverse", BlueprintCategory::Collection, true,
                  { List("List") }, { List("Value") },
                  "The same items, backwards.", "std::ranges::reverse" });
            Add({ "list.sort", "List Sort", BlueprintCategory::Collection, true,
                  { List("List"), Yes("Numeric") }, { List("Value") },
                  "In order — as text, or as numbers when asked.", "std::ranges::sort" });
            Add({ "list.slice", "List Slice", BlueprintCategory::Collection, true,
                  { List("List"), Num("First"), Num("Count", "1") }, { List("Value") },
                  "A run of items out of the middle.", "a sub-range" });
            Add({ "list.join", "List Join", BlueprintCategory::Collection, true,
                  { List("List"), Txt("Separator", ", ") }, { Txt("Value") },
                  "The items as one piece of text.", "joining a range" });
            Add({ "list.split", "Split Text", BlueprintCategory::Collection, true,
                  { Txt("Text"), Txt("Separator", ",") }, { List("Value") },
                  "A piece of text cut into a list. An empty separator cuts every character.",
                  "splitting a string" });
            Add({ "list.rows", "List To Rows", BlueprintCategory::Collection, true,
                  { List("List"), Txt("Column", "text") }, { Txt("Rows") },
                  "A list as the row text Set Rows takes: the column named, then one row per item.",
                  "the row-text format" });

            Add({ "map.set", "Map Set", BlueprintCategory::Collection, false,
                  { In(), Map("Map"), Txt("Key"), Txt("Value") }, { Out(), Map("Map") },
                  "The map with one key set.", "map[key] = value" });
            Add({ "map.get", "Map Get", BlueprintCategory::Collection, true,
                  { Map("Map"), Txt("Key"), Txt("Default") }, { Txt("Value") },
                  "What a key holds, or the fallback.", "map.at(key)" });
            Add({ "map.has", "Map Has", BlueprintCategory::Collection, true,
                  { Map("Map"), Txt("Key") }, { Yes("Value") },
                  "Whether the map holds this key.", "map.contains(key)" });
            Add({ "map.remove", "Map Remove", BlueprintCategory::Collection, false,
                  { In(), Map("Map"), Txt("Key") }, { Out(), Map("Map") },
                  "The map without this key.", "map.erase(key)" });
            Add({ "map.clear", "Map Clear", BlueprintCategory::Collection, false,
                  { In(), Map("Map") }, { Out(), Map("Map") },
                  "An empty map.", "map.clear()" });
            Add({ "map.length", "Map Length", BlueprintCategory::Collection, true,
                  { Map("Map") }, { Num("Length") },
                  "How many keys.", "map.size()" });
            Add({ "map.keys", "Map Keys", BlueprintCategory::Collection, true,
                  { Map("Map") }, { List("Value") },
                  "Every key, in the order they were set.", "the keys of a map" });
            Add({ "map.values", "Map Values", BlueprintCategory::Collection, true,
                  { Map("Map") }, { List("Value") },
                  "Every value, in the order their keys were set.", "the values of a map" });

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
            Pure("math.power", "Power", { Num("Base"), Num("Exponent", "2") }, { Num("Value") },
                 "Base raised to the exponent.", "std::pow(a, b)");
            Pure("math.sqrt", "Square Root", { Num("Value") }, { Num("Value") },
                 "The square root. A negative number gives zero rather than a not-a-number.",
                 "std::sqrt(v)");
            Pure("math.sin", "Sine", { Num("Radians") }, { Num("Value") }, "sin", "std::sin(v)");
            Pure("math.cos", "Cosine", { Num("Radians") }, { Num("Value") }, "cos", "std::cos(v)");
            Pure("math.tan", "Tangent", { Num("Radians") }, { Num("Value") }, "tan", "std::tan(v)");
            Pure("math.asin", "Arcsine", { Num("Value") }, { Num("Radians") },
                 "The angle whose sine this is.", "std::asin(v)");
            Pure("math.acos", "Arccosine", { Num("Value") }, { Num("Radians") },
                 "The angle whose cosine this is.", "std::acos(v)");
            Pure("math.atan2", "Arctangent 2", { Num("Y"), Num("X", "1") }, { Num("Radians") },
                 "The angle to a point, all four quadrants.", "std::atan2(y, x)");
            Pure("math.exp", "Exp", { Num("Value") }, { Num("Value") },
                 "e raised to this.", "std::exp(v)");
            Pure("math.log", "Log", { Num("Value"), Num("Base", "10") }, { Num("Value") },
                 "The logarithm. A base of zero or one means the natural log.", "std::log(v)");
            Pure("math.sign", "Sign", { Num("Value") }, { Num("Value") },
                 "-1, 0 or 1.", "(v > 0) - (v < 0)");
            Pure("math.truncate", "Truncate", { Num("Value") }, { Num("Value") },
                 "Towards zero.", "std::trunc(v)");
            Pure("math.fraction", "Fraction", { Num("Value") }, { Num("Value") },
                 "What is left after the whole part.", "v - std::trunc(v)");
            Pure("math.lerp", "Lerp", { Num("A"), Num("B", "1"), Num("Alpha") }, { Num("Value") },
                 "Somewhere between two numbers.", "a + (b - a) * t");
            Pure("math.wrap", "Wrap", { Num("Value"), Num("Min"), Num("Max", "1") },
                 { Num("Value") }, "Held inside a range by wrapping round it.", "a modulo");
            Pure("math.degrees", "To Degrees", { Num("Radians") }, { Num("Degrees") },
                 "Radians as degrees.", "v * 180 / pi");
            Pure("math.radians", "To Radians", { Num("Degrees") }, { Num("Radians") },
                 "Degrees as radians.", "v * pi / 180");
            Pure("math.inRange", "In Range",
                 { Num("Value"), Num("Min"), Num("Max", "1"), Yes("Inclusive", "true") },
                 { Yes("Value") }, "Whether a number is between two others.", "lo <= v && v <= hi");
            Pure("compare.nearly", "Nearly Equal",
                 { Num("A"), Num("B"), Num("Within", "0.0001") }, { Yes("Value") },
                 "Equal to within a tolerance, which is what comparing two computed numbers "
                 "actually means.", "std::abs(a - b) <= eps");
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
            Pure("text.substring", "Substring", { Txt("Text"), Num("First"), Num("Count", "1") },
                 { Txt("Value") }, "A run of characters out of the middle.", "text.substr(…)");
            Pure("text.indexOf", "Index Of", { Txt("Text"), Txt("Part") }, { Num("Index") },
                 "Where a piece is inside another, or -1.", "text.find(part)");
            Pure("text.replace", "Replace", { Txt("Text"), Txt("Find"), Txt("With") },
                 { Txt("Value") }, "Every occurrence swapped.", "a replace loop");
            Pure("text.startsWith", "Starts With", { Txt("Text"), Txt("Part") }, { Yes("Value") },
                 "Whether it begins with this.", "text.starts_with(part)");
            Pure("text.endsWith", "Ends With", { Txt("Text"), Txt("Part") }, { Yes("Value") },
                 "Whether it ends with this.", "text.ends_with(part)");
            Pure("text.pad", "Pad", { Txt("Text"), Num("Width", "2"), Txt("With", "0"),
                                      Yes("Left", "true") },
                 { Txt("Value") }, "Filled out to a width. What a clock's 09 is.", "padding");
            Pure("text.repeat", "Repeat", { Txt("Text"), Num("Times", "2") }, { Txt("Value") },
                 "The same text over and over.", "a repeat loop");
            Pure("text.char", "Character At", { Txt("Text"), Num("Index") }, { Txt("Value") },
                 "One character, or empty past the end.", "text[i]");
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
            Pure("util.reroute", "Reroute Value", { Any("Value") }, { Any("Value") },
                 "A corner to bend a value wire around.", "nothing at all");
            Pure("select.any", "Select", { Yes("Condition"), Any("True"), Any("False") },
                 { Any("Value") }, "One or the other, whatever they are.", "condition ? a : b");
            Pure("sound.playing", "Sound Playing", { Num("Voice") }, { Yes("Value") },
                 "Whether a voice is still going.", "self.SoundPlaying(voice)");
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

    // --- one canvas ---------------------------------------------------------------------------

    BlueprintNode* BlueprintCanvas::Find(u32 id) {
        for (BlueprintNode& node : nodes) if (node.id == id) return &node;
        return nullptr;
    }

    const BlueprintNode* BlueprintCanvas::Find(u32 id) const {
        for (const BlueprintNode& node : nodes) if (node.id == id) return &node;
        return nullptr;
    }

    BlueprintComment* BlueprintCanvas::FindComment(u32 id) {
        for (BlueprintComment& box : comments) if (box.id == id) return &box;
        return nullptr;
    }

    const BlueprintNode* BlueprintCanvas::FindType(std::string_view type) const {
        for (const BlueprintNode& node : nodes) if (node.type == type) return &node;
        return nullptr;
    }

    void BlueprintCanvas::RemoveNode(u32 id) {
        std::erase_if(links, [&](const BlueprintLink& link) {
            return link.from == id || link.to == id;
        });
        std::erase_if(nodes, [&](const BlueprintNode& node) { return node.id == id; });
    }

    void BlueprintCanvas::RemoveLink(u32 id) {
        std::erase_if(links, [&](const BlueprintLink& link) { return link.id == id; });
    }

    void BlueprintCanvas::DisplaceAt(u32 node, std::string_view pin, bool isInput, bool exec) {
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

    const BlueprintLink* BlueprintCanvas::LinkInto(u32 node, std::string_view pin) const {
        for (const BlueprintLink& link : links)
            if (link.to == node && link.toPin == pin) return &link;
        return nullptr;
    }

    const BlueprintLink* BlueprintCanvas::LinkOutOf(u32 node, std::string_view pin) const {
        for (const BlueprintLink& link : links)
            if (link.from == node && link.fromPin == pin) return &link;
        return nullptr;
    }

    std::vector<const BlueprintLink*> BlueprintCanvas::LinksOutOf(u32 node,
                                                                 std::string_view pin) const {
        std::vector<const BlueprintLink*> out;
        for (const BlueprintLink& link : links)
            if (link.from == node && link.fromPin == pin) out.push_back(&link);
        return out;
    }

    // --- the blueprint itself --------------------------------------------------------------------

    u32 Blueprint::MintId() { return m_NextId++; }

    void Blueprint::RecomputeNextId() {
        u32 highest = 0;
        const auto Scan = [&](const BlueprintCanvas& canvas) {
            for (const BlueprintNode& node : canvas.nodes)      highest = std::max(highest, node.id);
            for (const BlueprintLink& link : canvas.links)      highest = std::max(highest, link.id);
            for (const BlueprintComment& box : canvas.comments) highest = std::max(highest, box.id);
        };
        Scan(graph);
        for (const BlueprintFunction& function : functions) Scan(function.body);
        m_NextId = highest + 1;
    }

    u32 Blueprint::AddNode(BlueprintCanvas& into, BlueprintNode node) {
        if (node.id == 0) node.id = MintId();
        else m_NextId = std::max(m_NextId, node.id + 1);
        const u32 id = node.id;
        into.nodes.push_back(std::move(node));
        return id;
    }

    u32 Blueprint::AddLink(BlueprintCanvas& into, BlueprintLink link) {
        if (link.id == 0) link.id = MintId();
        else m_NextId = std::max(m_NextId, link.id + 1);
        const u32 id = link.id;
        into.links.push_back(std::move(link));
        return id;
    }

    BlueprintCanvas* Blueprint::CanvasFor(std::string_view function) {
        if (function.empty()) return &graph;
        BlueprintFunction* found = FindFunction(function);
        return found ? &found->body : nullptr;
    }

    const BlueprintCanvas* Blueprint::CanvasFor(std::string_view function) const {
        if (function.empty()) return &graph;
        const BlueprintFunction* found = FindFunction(function);
        return found ? &found->body : nullptr;
    }

    BlueprintFunction* Blueprint::FindFunction(std::string_view name) {
        for (BlueprintFunction& function : functions)
            if (function.name == name) return &function;
        return nullptr;
    }

    const BlueprintFunction* Blueprint::FindFunction(std::string_view name) const {
        for (const BlueprintFunction& function : functions)
            if (function.name == name) return &function;
        return nullptr;
    }

    void Blueprint::SetFunction(BlueprintFunction function) {
        for (BlueprintFunction& existing : functions)
            if (existing.name == function.name) { existing = std::move(function); return; }
        functions.push_back(std::move(function));
    }

    void Blueprint::RemoveFunction(std::string_view name) {
        std::erase_if(functions, [&](const BlueprintFunction& f) { return f.name == name; });
        // Every call to it goes too. Leaving them would leave a blueprint that does not compile
        // and a red node with nothing to click on to find out why.
        const auto Prune = [&](BlueprintCanvas& canvas) {
            std::vector<u32> gone;
            for (const BlueprintNode& node : canvas.nodes)
                if ((node.type == "func.call" || node.type == "event.call") && node.target == name)
                    gone.push_back(node.id);
            for (const u32 id : gone) canvas.RemoveNode(id);
        };
        Prune(graph);
        for (BlueprintFunction& function : functions) Prune(function.body);
    }

    void Blueprint::RenameFunction(std::string_view from, std::string_view to) {
        for (BlueprintFunction& function : functions)
            if (function.name == from) function.name = to;
        const auto Rename = [&](BlueprintCanvas& canvas) {
            for (BlueprintNode& node : canvas.nodes)
                if ((node.type == "func.call" || node.type == "event.call") && node.target == from)
                    node.target = std::string(to);
        };
        Rename(graph);
        for (BlueprintFunction& function : functions) Rename(function.body);
    }

    std::vector<std::pair<std::string, const BlueprintCanvas*>> Blueprint::Canvases() const {
        std::vector<std::pair<std::string, const BlueprintCanvas*>> out;
        out.emplace_back(std::string(), &graph);
        for (const BlueprintFunction& function : functions)
            out.emplace_back(function.name, &function.body);
        return out;
    }

    const BlueprintVariable* Blueprint::FindVariable(std::string_view name) const {
        for (const BlueprintVariable& variable : variables)
            if (variable.name == name) return &variable;
        return nullptr;
    }

    const BlueprintVariable* Blueprint::FindVariable(std::string_view name,
                                                     std::string_view function) const {
        // A local wins over a blueprint variable of the same name, which is what every language
        // with both does, and is why the two can share a name at all.
        if (!function.empty())
            if (const BlueprintFunction* found = FindFunction(function)) {
                for (const BlueprintVariable& local : found->locals)
                    if (local.name == name) return &local;
                for (const BlueprintParam& param : found->params)
                    if (param.name == name) {
                        // A parameter reads like a local that was handed a value. Returned through
                        // a static so the caller gets a stable pointer without every caller having
                        // to know the difference.
                        static thread_local BlueprintVariable seen;
                        seen = { param.name, param.type, param.defaultValue };
                        return &seen;
                    }
            }
        return FindVariable(name);
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
        const auto Rename = [&](BlueprintCanvas& canvas) {
            for (BlueprintNode& node : canvas.nodes)
                if (node.target == from && (node.type == "var.get" || node.type == "var.set"))
                    node.target = std::string(to);
        };
        Rename(graph);
        for (BlueprintFunction& function : functions) Rename(function.body);
    }

    // --- lists and maps as text -------------------------------------------------------------------

    namespace {
        // A backslash escape over the two characters that separate things, so an element holding a
        // newline or a tab survives the round trip. Nothing else is touched, which keeps the stored
        // form readable in the state bag and in a file.
        std::string Escape(std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (const char c : s) {
                if (c == '\\')      out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c == '\t') out += "\\t";
                else                out += c;
            }
            return out;
        }

        std::string Unescape(std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (std::size_t at = 0; at < s.size(); ++at) {
                if (s[at] != '\\' || at + 1 >= s.size()) { out += s[at]; continue; }
                switch (s[++at]) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case '\\': out += '\\'; break;
                    default:   out += s[at]; break;
                }
            }
            return out;
        }

        std::vector<std::string> SplitLines(std::string_view text) {
            std::vector<std::string> lines;
            if (text.empty()) return lines;
            std::size_t at = 0;
            while (true) {
                const std::size_t nl = text.find('\n', at);
                lines.emplace_back(text.substr(at, nl == std::string_view::npos
                                                   ? std::string_view::npos : nl - at));
                if (nl == std::string_view::npos) break;
                at = nl + 1;
            }
            return lines;
        }
    }

    std::string ListText(const std::vector<std::string>& items) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += '\n';
            out += Escape(items[i]);
        }
        return out;
    }

    std::vector<std::string> ParseListText(std::string_view text) {
        std::vector<std::string> items;
        for (const std::string& line : SplitLines(text)) items.push_back(Unescape(line));
        return items;
    }

    std::string MapText(const std::vector<std::pair<std::string, std::string>>& entries) {
        std::string out;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i) out += '\n';
            out += Escape(entries[i].first);
            out += '\t';
            out += Escape(entries[i].second);
        }
        return out;
    }

    std::vector<std::pair<std::string, std::string>> ParseMapText(std::string_view text) {
        std::vector<std::pair<std::string, std::string>> entries;
        for (const std::string& line : SplitLines(text)) {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) { entries.emplace_back(Unescape(line), std::string()); continue; }
            entries.emplace_back(Unescape(line.substr(0, tab)), Unescape(line.substr(tab + 1)));
        }
        return entries;
    }

    // --- pins, once the blueprint has answered for them -------------------------------------------

    std::string SequencePinName(u32 index) { return "Then " + std::to_string(index); }
    std::string CasePinName(u32 index)     { return "Case " + std::to_string(index); }
    std::string VariadicPinName(std::string_view base, u32 index) {
        return std::string(base) + " " + std::to_string(index);
    }

    namespace {
        // A generated pin name has to be owned somewhere, because a PinSpec holds a view of it.
        // One pool per pattern name, grown on demand — so a Sequence with two hundred outputs is a
        // strange thing to draw but not a thing that is refused.
        const std::string& GeneratedName(std::string_view base, u32 index) {
            // A map and not a vector, because a PinSpec holds a VIEW of the name it is handed and
            // a vector that grows moves every string it holds. A node-based container never does.
            static std::map<std::pair<std::string, u32>, std::string> pool;
            const auto key = std::pair<std::string, u32>(std::string(base), index);
            const auto it = pool.find(key);
            if (it != pool.end()) return it->second;
            return pool.emplace(key, VariadicPinName(base, index)).first->second;
        }

        // A variable node's pins are the variable's type. An unknown variable stays Any rather than
        // guessing: the compiler will say which one is missing, and a guess would hide it behind a
        // type error somewhere else.
        PinType VariablePin(const Blueprint& blueprint, const BlueprintNode& node,
                            std::string_view function) {
            const BlueprintVariable* variable = blueprint.FindVariable(node.target, function);
            return variable ? variable->type : PinType::Any;
        }

        void Resolve(const Blueprint& blueprint, const BlueprintNode& node,
                     std::string_view function, std::vector<PinSpec>& pins) {
            if (node.type != "var.get" && node.type != "var.set") return;
            const PinType type = VariablePin(blueprint, node, function);
            for (PinSpec& pin : pins)
                if (pin.type == PinType::Any) pin.type = type;
        }

        // A signature's pins. Owned in a thread-local, because a PinSpec holds a view and the
        // signature it is a view of outlives the call that asked for it.
        std::vector<PinSpec> Signature(const std::vector<BlueprintParam>& params,
                                       std::vector<std::string>& keep, bool exec,
                                       const char* execName) {
            std::vector<PinSpec> pins;
            keep.clear();
            keep.reserve(params.size());
            for (const BlueprintParam& param : params) keep.push_back(param.name);
            if (exec) pins.push_back({ execName, PinType::Exec, "", false });
            for (std::size_t i = 0; i < params.size(); ++i)
                pins.push_back({ keep[i], params[i].type, "", false });
            return pins;
        }
    }

    std::vector<PinSpec> BlueprintInputs(const Blueprint& blueprint, const BlueprintNode& node,
                                         std::string_view function) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return {};

        // ---- the nodes whose pins are a signature rather than a table row -----------------------
        static thread_local std::vector<std::string> keep;
        if (node.type == "func.return") {
            const BlueprintFunction* owner = blueprint.FindFunction(function);
            if (!owner) return {};
            return Signature(owner->returns, keep, !owner->pure, "In");
        }
        if (node.type == "func.call" || node.type == "event.call") {
            const BlueprintFunction* called = blueprint.FindFunction(node.target);
            if (!called) return type->inputs;
            return Signature(called->params, keep, !called->pure, "In");
        }

        std::vector<PinSpec> pins = type->inputs;
        Resolve(blueprint, node, function, pins);
        if (type->variadicIn && !pins.empty()) {
            // The last declared pin is the pattern; everything before it stays as it is.
            const PinSpec pattern = pins.back();
            pins.pop_back();
            for (u32 i = 0; i < 2u + node.extraPins; ++i)
                pins.push_back({ GeneratedName(pattern.name, i), pattern.type, pattern.literal,
                                 pattern.fixed });
        }
        return pins;
    }

    std::vector<PinSpec> BlueprintOutputs(const Blueprint& blueprint, const BlueprintNode& node,
                                          std::string_view function) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return {};

        static thread_local std::vector<std::string> keep;
        if (node.type == "func.entry") {
            const BlueprintFunction* owner = blueprint.FindFunction(function);
            if (!owner) return {};
            return Signature(owner->params, keep, !owner->pure, "Out");
        }
        if (node.type == "func.call" || node.type == "event.call") {
            const BlueprintFunction* called = blueprint.FindFunction(node.target);
            if (!called) return type->outputs;
            return Signature(called->returns, keep, !called->pure, "Out");
        }

        std::vector<PinSpec> pins = type->outputs;
        Resolve(blueprint, node, function, pins);
        if (type->variadicOut && !pins.empty()) {
            // The pattern is the FIRST output here rather than the last, because what follows it —
            // a switch's Default, a loop's Done — is what the run leads to when it is finished.
            const PinSpec pattern = pins.front();
            std::vector<PinSpec> generated;
            for (u32 i = 0; i < 2u + node.extraPins; ++i)
                generated.push_back({ GeneratedName(pattern.name, i), pattern.type, "", false });
            for (std::size_t i = 1; i < pins.size(); ++i) generated.push_back(pins[i]);
            return generated;
        }
        return pins;
    }

    bool IsPureNode(const Blueprint& blueprint, const BlueprintNode& node) {
        if (node.type == "func.call") {
            const BlueprintFunction* called = blueprint.FindFunction(node.target);
            return called && called->pure;
        }
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        return type && type->pure;
    }

    std::string BlueprintNodeTitle(const Blueprint& blueprint, const BlueprintNode& node) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return node.type.empty() ? "?" : node.type;
        if (node.type == "var.get") return "Get " + (node.target.empty() ? "?" : node.target);
        if (node.type == "var.set") return "Set " + (node.target.empty() ? "?" : node.target);
        if (node.type == "func.call" || node.type == "event.call")
            return node.target.empty() ? std::string("Call ?") : node.target;
        if (node.type == "event.custom") return node.target.empty() ? std::string("Event")
                                                                    : node.target;
        (void)blueprint;
        return std::string(type->title);
    }

    Value ParsePinLiteral(std::string_view text, PinType type) {
        switch (type) {
            case PinType::Bool:   return text == "true" || text == "1";
            case PinType::Number: return ParseNumber(text).value_or(0.0f);
            case PinType::Colour: return ColorFromHex(text).value_or(Color{ 1, 1, 1, 1 });
            case PinType::Text:
            case PinType::List:
            case PinType::Map:    return std::string(text);
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
            case PinType::Text:
            case PinType::List:
            case PinType::Map: {
                const std::string* text = std::get_if<std::string>(&value);
                return text ? *text : std::string();
            }
            case PinType::Exec:
            case PinType::Any:  break;
        }
        return {};
    }

    Value DefaultPinValue(PinType type) {
        switch (type) {
            case PinType::Number: return 0.0f;
            case PinType::Bool:   return false;
            case PinType::Colour: return Color{ 1, 1, 1, 1 };
            case PinType::Exec:
            case PinType::Text:
            case PinType::List:
            case PinType::Map:
            case PinType::Any:    break;
        }
        return std::string();
    }

    const char* CollapseResultText(CollapseResult result) {
        switch (result) {
            case CollapseResult::Ok:          return "collapsed";
            case CollapseResult::Empty:       return "nothing is selected";
            case CollapseResult::HoldsEvent:  return "an event cannot move into a function";
            case CollapseResult::HoldsLatent: return "a Delay cannot be inside a function";
            case CollapseResult::ManyEntries: return "more than one node is reached from outside";
            case CollapseResult::ManyExits:   return "more than one node carries on outside";
            case CollapseResult::NameTaken:   return "there is already one of those";
        }
        return "cannot be collapsed";
    }

    namespace {

        // A pin the boundary crosses, and where the other end of it was.
        struct Crossing {
            u32 node = 0;                       // the node inside the selection
            std::string pin;
            PinType type = PinType::Number;
            std::vector<std::pair<u32, std::string>> outside;   // every end on the other side
        };

        Crossing* FindCrossing(std::vector<Crossing>& list, u32 node, std::string_view pin) {
            for (Crossing& crossing : list)
                if (crossing.node == node && crossing.pin == pin) return &crossing;
            return nullptr;
        }

        PinType PinTypeAt(const std::vector<PinSpec>& pins, std::string_view name) {
            for (const PinSpec& pin : pins)
                if (pin.name == name) return pin.type;
            return PinType::Exec;
        }

        // A parameter is named after the pin it came from, which is what someone reading the call
        // would have called it anyway. Two pins with the same name get a number.
        std::string UniqueParamName(const std::vector<BlueprintParam>& taken, std::string_view want) {
            std::string base(want.empty() ? "value" : want);
            std::string name = base;
            for (u32 n = 2; ; ++n) {
                bool clash = false;
                for (const BlueprintParam& param : taken) clash = clash || param.name == name;
                if (!clash) return name;
                name = base + " " + std::to_string(n);
            }
        }

    }

    CollapseResult CollapseIntoFunction(Blueprint& blueprint, std::string_view canvasName,
                                        const std::vector<u32>& selection, std::string_view name) {
        BlueprintCanvas* canvas = blueprint.CanvasFor(canvasName);
        if (!canvas) return CollapseResult::Empty;
        if (name.empty() || blueprint.FindFunction(name)) return CollapseResult::NameTaken;

        std::vector<u32> inside;
        for (const u32 id : selection)
            if (canvas->Find(id) && std::find(inside.begin(), inside.end(), id) == inside.end())
                inside.push_back(id);
        if (inside.empty()) return CollapseResult::Empty;

        const auto Selected = [&inside](u32 id) {
            return std::find(inside.begin(), inside.end(), id) != inside.end();
        };

        // What cannot move. An event is reached from outside the blueprint entirely, so a function
        // is not where it can live; a latent node suspends the canvas it is on, and a function has
        // to have finished by the time the call returns.
        bool anyStatement = false;
        for (const u32 id : inside) {
            const BlueprintNode* node = canvas->Find(id);
            const BlueprintNodeType* type = FindBlueprintNodeType(node->type);
            if (!type) return CollapseResult::Empty;
            if (type->category == BlueprintCategory::Event) return CollapseResult::HoldsEvent;
            if (node->type == "func.entry" || node->type == "func.return")
                return CollapseResult::HoldsEvent;
            if (type->latent) return CollapseResult::HoldsLatent;
            if (!IsPureNode(blueprint, *node)) anyStatement = true;
        }

        // ---- what crosses the boundary ---------------------------------------------------------
        std::vector<std::pair<u32, std::string>> execIn;    // outside ends that reach in
        u32 entryNode = 0; std::string entryPin;
        u32 exitNode = 0;  std::string exitPin;             // the inside end that carries on out
        std::pair<u32, std::string> exitTo{ 0, {} };
        std::vector<Crossing> dataIn;                       // one per outside source pin
        std::vector<Crossing> dataOut;                      // one per inside source pin

        for (const BlueprintLink& link : canvas->links) {
            const bool fromIn = Selected(link.from);
            const bool toIn   = Selected(link.to);
            if (fromIn == toIn) continue;

            const BlueprintNode* source = canvas->Find(link.from);
            if (!source) continue;
            const PinType type = PinTypeAt(BlueprintOutputs(blueprint, *source, canvasName),
                                           link.fromPin);

            if (!fromIn && toIn) {
                if (type == PinType::Exec) {
                    if (entryNode && (entryNode != link.to || entryPin != link.toPin))
                        return CollapseResult::ManyEntries;
                    entryNode = link.to;
                    entryPin = link.toPin;
                    execIn.push_back({ link.from, link.fromPin });
                } else {
                    // Keyed on the OUTSIDE pin: one value read by three nodes inside is one
                    // parameter, not three.
                    Crossing* crossing = FindCrossing(dataIn, link.from, link.fromPin);
                    if (!crossing) {
                        dataIn.push_back({ link.from, link.fromPin, type, {} });
                        crossing = &dataIn.back();
                    }
                    crossing->outside.push_back({ link.to, link.toPin });
                }
            } else {
                if (type == PinType::Exec) {
                    if (exitNode && (exitNode != link.from || exitPin != link.fromPin))
                        return CollapseResult::ManyExits;
                    exitNode = link.from;
                    exitPin = link.fromPin;
                    exitTo = { link.to, link.toPin };
                } else {
                    Crossing* crossing = FindCrossing(dataOut, link.from, link.fromPin);
                    if (!crossing) {
                        dataOut.push_back({ link.from, link.fromPin, type, {} });
                        crossing = &dataOut.back();
                    }
                    crossing->outside.push_back({ link.to, link.toPin });
                }
            }
        }

        // ---- the function ----------------------------------------------------------------------
        BlueprintFunction function;
        function.name = std::string(name);
        // Nothing but expressions, and nothing wired in or out of an execution pin: that is an
        // expression itself, so it becomes one.
        function.pure = !anyStatement && !entryNode && !exitNode;

        for (Crossing& crossing : dataIn) {
            BlueprintParam param;
            param.name = UniqueParamName(function.params, crossing.pin);
            param.type = crossing.type;
            param.defaultValue = DefaultPinValue(crossing.type);
            crossing.pin = param.name;          // reused below as the entry pin to wire from
            function.params.push_back(std::move(param));
        }
        for (Crossing& crossing : dataOut) {
            BlueprintParam value;
            value.name = UniqueParamName(function.returns, crossing.pin);
            value.type = crossing.type;
            value.defaultValue = DefaultPinValue(crossing.type);
            function.returns.push_back(std::move(value));
        }

        // Where the nodes were, so the new canvas is laid out the way the selection already was and
        // the entry and return sit either side of it.
        f32 left = 0.0f, right = 0.0f, top = 0.0f;
        for (std::size_t i = 0; i < inside.size(); ++i) {
            const Vec2 at = canvas->Find(inside[i])->position;
            if (i == 0) { left = right = at.x; top = at.y; }
            left = std::min(left, at.x);
            right = std::max(right, at.x);
            top = std::min(top, at.y);
        }

        // The nodes themselves move, ids and all: an id is minted from one counter shared by every
        // canvas, so nothing has to be renumbered and every link still names the same node.
        for (const u32 id : inside) {
            function.body.nodes.push_back(*canvas->Find(id));
            // Its own pins may have been wired to something outside; the wire is gone, but the
            // literal that was under it is not, and it is what the pin falls back to.
        }
        for (const BlueprintLink& link : canvas->links)
            if (Selected(link.from) && Selected(link.to)) function.body.links.push_back(link);

        blueprint.SetFunction(std::move(function));
        BlueprintFunction* added = blueprint.FindFunction(name);
        BlueprintCanvas& body = added->body;

        BlueprintNode entry;
        entry.type = "func.entry";
        entry.position = { left - 240.0f, top };
        const u32 entryId = blueprint.AddNode(body, std::move(entry));

        BlueprintNode ret;
        ret.type = "func.return";
        ret.position = { right + 280.0f, top };
        const u32 returnId = blueprint.AddNode(body, std::move(ret));

        if (entryNode) blueprint.AddLink(body, { 0, entryId, "Out", entryNode, entryPin });
        for (std::size_t i = 0; i < dataIn.size(); ++i)
            for (const auto& [node, pin] : dataIn[i].outside)
                blueprint.AddLink(body, { 0, entryId, added->params[i].name, node, pin });
        if (exitNode) blueprint.AddLink(body, { 0, exitNode, exitPin, returnId, "In" });
        for (std::size_t i = 0; i < dataOut.size(); ++i)
            blueprint.AddLink(body, { 0, dataOut[i].node, dataOut[i].pin,
                                      returnId, added->returns[i].name });

        // ---- the call left in their place ------------------------------------------------------
        // Re-found, because adding the function may have moved the vector the canvas lives in when
        // what is being collapsed is itself inside a function.
        canvas = blueprint.CanvasFor(canvasName);
        for (const u32 id : inside) canvas->RemoveNode(id);

        BlueprintNode call;
        call.type = "func.call";
        call.target = std::string(name);
        call.position = { left, top };
        const u32 callId = blueprint.AddNode(*canvas, std::move(call));

        for (const auto& [node, pin] : execIn)
            blueprint.AddLink(*canvas, { 0, node, pin, callId, "In" });
        if (exitNode) blueprint.AddLink(*canvas, { 0, callId, "Out", exitTo.first, exitTo.second });
        for (std::size_t i = 0; i < dataIn.size(); ++i)
            blueprint.AddLink(*canvas, { 0, dataIn[i].node, dataIn[i].pin,
                                         callId, added->params[i].name });
        for (std::size_t i = 0; i < dataOut.size(); ++i)
            for (const auto& [node, pin] : dataOut[i].outside)
                blueprint.AddLink(*canvas, { 0, callId, added->returns[i].name, node, pin });

        return CollapseResult::Ok;
    }

    Value BlueprintLiteral(const Blueprint& blueprint, const BlueprintNode& node, const PinSpec& pin,
                           std::string_view function) {
        const auto it = node.literals.find(std::string(pin.name));
        if (it != node.literals.end() && IsSet(it->second)) return it->second;
        PinType type = pin.type;
        if (type == PinType::Any) type = VariablePin(blueprint, node, function);
        return ParsePinLiteral(pin.literal, type);
    }

}
