#pragma once

#include <string>
#include <vector>

namespace vae::script {

    // Every name a script can write, per language. It exists so the editor's completion can offer
    // the real API rather than a list somebody typed once and never revisited — and it lives next
    // to the bindings, with a test that fails when the two drift apart.
    //
    // Lua names are given as they are written: `self:set_text`, `vae.component`, `event.kind`.
    const std::vector<std::string>& LuaApi();
    const std::vector<std::string>& CppApi();

    // Broken out by where they are written, because completion is about what can follow the
    // caret: after `self:` only a method makes sense, after `event.` only a field, and offering
    // the whole API in either position is offering mostly wrong answers.
    //
    // LuaSelfMethods is also what the Lua host actually binds — the test that keeps this honest
    // asks a live component whether each of these is a function on `self`.
    const std::vector<std::string>& LuaSelfMethods();
    const std::vector<std::string>& LuaEventFields();
    const std::vector<std::string>& LuaGlobals();
    // The strings an event's `kind` can be, which get no help from anything else.
    const std::vector<std::string>& LuaEventKinds();

}
