#pragma once

#include "EditorState.h"

#include <string_view>

namespace vae {

    // A worked example, built in code rather than shipped as a file so it cannot drift out of date
    // with the format. It exists to answer the question a blank canvas cannot: what does a project
    // with logic in it actually look like?
    //
    // A counter, twice over — two copies of one component, each keeping its own count, each built
    // from the library's Button. That is the whole of the composition story on one screen.
    void BuildCounterExample(EditorState& state);
    // The same project with its logic drawn rather than written: no script file, a blueprint on the
    // Counter component.
    void BuildCounterBlueprint(EditorState& state);
    // The click the Counter's buttons make, generated into the project's assets folder and
    // registered under the name the script plays it by. Generated rather than shipped: a 4 KB
    // binary in the repo is a thing nobody can read in a diff, and the twenty lines that make one
    // say exactly what it sounds like.
    void WriteExampleClick(EditorState& state, const std::filesystem::path& folder);

    // The other half of what a project is: more than one screen, and the relations between them.
    // A list that opens a detail, a detail that goes back, and an alert presented over whichever is
    // showing — wired with `goTo` where no logic is needed and with a script where some is.
    void BuildScreensExample(EditorState& state);
    std::string_view ScreensExampleLua();
    std::string_view ScreensExampleCpp();

    // The third thing a project is: one that talks to something. A screen that has to be four
    // screens — waiting, empty, broken, and the answer — plus a table nobody hand-built a row for,
    // plus a connection the server speaks down first.
    void BuildFeedExample(EditorState& state);
    std::string_view FeedExampleLua();
    std::string_view FeedExampleCpp();

    // The script that drives it, in either language. Written next to the project when the example
    // is opened, so Play works without the author typing a line.
    std::string_view CounterExampleLua();
    std::string_view CounterExampleCpp();

}
