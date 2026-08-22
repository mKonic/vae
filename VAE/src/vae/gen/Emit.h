#pragma once

#include "vae/doc/Document.h"

#include <filesystem>
#include <string>

namespace vae::gen {

    // Document → C++ that rebuilds it, against `vae::doc::Builder`.
    //
    // The escape hatch, and the thing that keeps the format from being a black box: whatever the
    // designer drew, there is a readable C++ file that says the same thing, and it compiles against
    // the same public API a hand-written app would use.
    struct Options {
        std::string function = "BuildDocument";   // what the emitted entry point is called
        std::string appName  = "App";             // the generated project's name
        bool comments = true;                     // name each section after the thing it builds
        // The script the exported app loads, as a filename beside the binary. Empty for a project
        // with no logic — the screens still draw, they just do not do anything.
        std::string script;
        // Where the project's images are, so the export can take them with it. An exported app
        // that cannot find its own pictures is not an app.
        std::filesystem::path assetRoot;
    };

    // Just the builder function, header included. What the golden tests compare.
    std::string EmitDocument(const doc::Document& document, const Options& options = {});

    // A whole project: the builder, a main that runs it, and a premake file that builds it against
    // the engine it was exported from. Returns false and fills `error` if anything could not be
    // written; nothing is half-written on failure that was not there already.
    bool EmitProject(const doc::Document& document, const std::filesystem::path& directory,
                     const Options& options = {}, std::string* error = nullptr);

}
