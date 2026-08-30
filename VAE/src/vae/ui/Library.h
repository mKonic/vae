#pragma once

#include "vae/doc/Document.h"
#include "vae/doc/Serializer.h"

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

    // The same catalog, as something a document can name instead of copying. This is what keeps a
    // .vae file down to the app the designer actually drew: the 53 components and ~480 nodes
    // above are compiled into the binary, so a file says "vae.std" and gets them rebuilt on load.
    //
    // The pairing with BuildStandardLibrary is exact — Install calls it — so a component that
    // still matches what it builds is written as a reference, and one the designer edited is
    // written out in full and wins over the built one when the file is read back.
    const doc::LibrarySource& StandardLibrary();

}
