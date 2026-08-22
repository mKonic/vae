#pragma once

#include "vae/core/Event.h"
#include "vae/ui/ViewTree.h"

namespace vae::ui {

    class UiHost;

    // Same set as vae::Cursor, and deliberately the same order: the desktop backend converts with
    // a cast rather than a switch that can drift.
    enum class CursorShape : u8 { Arrow, Hand, IBeam, ResizeH, ResizeV, Crosshair, NotAllowed,
                                  ResizeNWSE, ResizeNESW, ResizeAll };

    // What a widget reports upward. Polled rather than delivered through callbacks: the headless
    // interaction tests, the Studio inspector and the script layer then all read one stream, and a
    // widget cannot reenter the tree it is being dispatched from.
    enum class ActionKind : u8 {
        Clicked, ValueChanged, TextChanged, Submitted, SelectionChanged,
        Opened, Closed, Dismissed, Navigated, Scrolled,
    };

    const char* ActionName(ActionKind kind);

    struct Action {
        ActionKind kind = ActionKind::Clicked;
        Uuid source = Uuid::Invalid();
        Uuid instance = Uuid::Invalid();
        std::string name;              // the node's name, so a test can address it readably
        doc::Value value;
    };

    struct WidgetContext {
        ViewTree& tree;
        UiHost& host;
        u32 view = ViewTree::kInvalid;

        ViewTree::View& Self() const { return tree.At(view); }
        WidgetId Id() const { return { Self().sourceId, Self().instanceId }; }
        const Rect& Bounds() const { return tree.Bounds(view); }
        doc::Value Prop(doc::Prop prop) const { return tree.ResolvedProp(view, prop); }
        f32  Number(doc::Prop prop, f32 fallback = 0.0f) const { return tree.Number(view, prop, fallback); }
        bool Flag(doc::Prop prop, bool fallback = false) const { return tree.Flag(view, prop, fallback); }
        std::string Str(doc::Prop prop, std::string fallback = {}) const {
            return tree.Str(view, prop, std::move(fallback));
        }
        void Set(doc::Prop prop, doc::Value value) const { tree.SetViewProp(view, prop, std::move(value)); }
        void SetState(StateBit bit, bool on) const { tree.SetState(view, bit, on); }
        bool Enabled() const { return tree.IsEnabled(view); }
    };

    // The native half of a widget. It owns interaction, never appearance: everything it changes is
    // either a state flag the document restyles on, or a document property.
    class Behavior {
    public:
        virtual ~Behavior() = default;
        virtual Role Kind() const = 0;

        // Can this widget hold keyboard focus? A disabled one never does, which the host enforces.
        virtual bool Focusable() const { return true; }

        // Mirror the document into state flags. Runs after every build, so a checkbox loaded from
        // disk already looks checked before anything is clicked.
        virtual void Sync(WidgetContext&) {}

        // Position the parts this widget owns — a slider's knob, a switch's knob, a scrollbar's
        // thumb — now that layout has run and their boxes are real. Doing this in Sync instead
        // reads every bound as zero, because Sync runs before the solver.
        virtual void Arrange(WidgetContext&) {}

        // True stops the event bubbling to the parent.
        virtual bool OnEvent(WidgetContext&, const Event&) { return false; }
        virtual void OnTick(WidgetContext&, f32 dt) { (void)dt; }

        // Decoration a behavior owns rather than the document: the caret, a virtualized list's
        // rows, a scrollbar thumb. Runs after the view's own painting.
        virtual void OnPaint(const WidgetContext&, PaintContext&) const {}

        // What the pointer looks like over this widget. Asked of the nearest behavior at or above
        // whatever the pointer is actually over, so hovering a button's label is hovering the
        // button — a part is not a thing you point at separately.
        virtual CursorShape CursorOver(const WidgetContext& context) const {
            return context.Enabled() ? CursorShape::Arrow : CursorShape::NotAllowed;
        }

        virtual void OnFocusLost(WidgetContext&) {}
        virtual void OnCaptureLost(WidgetContext&) {}
    };

    Scope<Behavior> MakeBehavior(Role role);

}
