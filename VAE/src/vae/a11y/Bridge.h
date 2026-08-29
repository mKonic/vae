#pragma once

#include "vae/a11y/Accessibility.h"

#include <string>
#include <string_view>

namespace vae::a11y {

    // The other direction. Reading the tree is answered from a snapshot, which is what makes it
    // safe to do from a bus callback; *acting* has to reach the live widget, and only the app knows
    // how to get there. Every call runs on the app's own thread, from inside Bridge::Pump().
    //
    // Nodes are named by their index in the tree that was last published, which is the only name
    // the thing on the other end has.
    class Actor {
    public:
        virtual ~Actor() = default;

        // Perform an action on a node. False when it could not be done, which the bus reports back
        // rather than swallowing — a screen reader that says "done" about nothing is worse than
        // one that says it cannot.
        virtual bool Do(u32 node, Action action) = 0;

        // Move the caret in a text field, in characters. `end` equal to `start` clears the
        // selection, which is what setting a caret means.
        virtual bool SetCaret(u32 node, u32 start, u32 end) = 0;

        // Give a node the keyboard focus, which is how a screen reader moves through an app.
        virtual bool Focus(u32 node) = 0;
    };

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

        // Who to ask when a screen reader wants something done. Null — the default — makes every
        // action fail politely, which is the right answer for anything publishing a tree it does
        // not own, and the state a bridge is in before the app has finished starting.
        virtual void SetActor(Actor* actor) = 0;

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
