#pragma once

#include "vae/doc/Document.h"

#include <map>

namespace vae::ui {

    // The palette the standard widgets are built from. Every colour in the library is a token
    // reference, never a literal, so switching the theme or rebranding the whole app is an edit to
    // these and nothing else.
    void InstallDefaultTokens(doc::Document& document);

    struct Library {
        std::map<std::string, Uuid, std::less<>> components;
        Uuid Find(std::string_view name) const;
    };

    // One component per widget, each an ordinary subtree of frames and text. A designer opens any
    // of them, rearranges it and restyles it, and the native behavior keeps working — because it
    // addresses the parts it needs by role, never by position.
    Library BuildStandardLibrary(doc::Document& document);

}
