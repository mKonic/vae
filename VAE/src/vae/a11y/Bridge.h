#pragma once

#include "vae/a11y/Accessibility.h"

#include <string>
#include <string_view>

namespace vae::a11y {

    // Carries the accessibility tree to whatever the desktop uses to read a screen out loud.
    //
    // On Linux that is AT-SPI, which is a D-Bus protocol: the app exports an object per accessible
    // node on a bus of its own, and a screen reader walks it. The tree this publishes is built and
    // tested separately (Accessibility.h) precisely so that the part with the design decisions in
    // it does not depend on having a screen reader attached.
    //
    // Null from Create() is the normal answer on a machine with no accessibility bus running,
    // which is most of them until somebody turns a screen reader on.
    class Bridge {
    public:
        virtual ~Bridge() = default;

        // Null when this build has no bridge compiled in, or when the desktop has no bus.
        static Scope<Bridge> Create();

        virtual bool Connect(std::string_view applicationName) = 0;
        virtual bool Connected() const = 0;
        virtual void Shutdown() = 0;

        // The whole tree, whenever it has changed. Cheap when nothing did: publishing compares
        // against what was last sent and says nothing when the answer is the same, because a
        // screen reader that is told everything changed every frame reads the screen out again
        // every frame.
        virtual void Publish(const Tree& tree) = 0;

        // Called from the app's own loop, because a bus connection has to be pumped and doing it
        // on a thread of its own would mean locking the tree against the frame that rebuilds it.
        virtual void Pump() = 0;

        // What the bridge is doing, for the app to log or show. Never a lie: "not connected" says
        // why rather than staying silent.
        virtual const std::string& Status() const = 0;
    };

}
