#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include "vae/base/Utf8.h"
#include "vae/text/FontDB.h"

#include <algorithm>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        bool IsWordChar(u32 codepoint) {
            return codepoint == '_' || codepoint > 0x7F
                || (codepoint >= '0' && codepoint <= '9')
                || (codepoint >= 'A' && codepoint <= 'Z')
                || (codepoint >= 'a' && codepoint <= 'z');
        }

        std::size_t PrevBoundary(std::string_view text, std::size_t index) {
            if (index == 0) return 0;
            --index;
            while (index > 0 && (static_cast<u8>(text[index]) & 0xC0) == 0x80) --index;
            return index;
        }

        std::size_t NextBoundary(std::string_view text, std::size_t index) {
            if (index >= text.size()) return text.size();
            Utf8Next(text, index);
            return index;
        }

        u32 CodepointAt(std::string_view text, std::size_t index) {
            if (index >= text.size()) return 0;
            return Utf8Next(text, index);
        }

        std::size_t PrevWord(std::string_view text, std::size_t index) {
            while (index > 0 && !IsWordChar(CodepointAt(text, PrevBoundary(text, index))))
                index = PrevBoundary(text, index);
            while (index > 0 && IsWordChar(CodepointAt(text, PrevBoundary(text, index))))
                index = PrevBoundary(text, index);
            return index;
        }

        std::size_t NextWord(std::string_view text, std::size_t index) {
            while (index < text.size() && IsWordChar(CodepointAt(text, index)))
                index = NextBoundary(text, index);
            while (index < text.size() && !IsWordChar(CodepointAt(text, index)))
                index = NextBoundary(text, index);
            return index;
        }

        std::size_t CodepointIndexOf(std::string_view text, std::size_t byteOffset) {
            std::size_t index = 0, count = 0;
            while (index < text.size() && index < byteOffset) { Utf8Next(text, index); ++count; }
            return count;
        }

        std::size_t ByteOffsetOf(std::string_view text, std::size_t codepointIndex) {
            std::size_t index = 0, count = 0;
            while (index < text.size() && count < codepointIndex) { Utf8Next(text, index); ++count; }
            return index;
        }

        std::size_t LineStart(std::string_view text, std::size_t index) {
            const std::size_t found = text.rfind('\n', index == 0 ? 0 : index - 1);
            return (found == std::string_view::npos || index == 0) ? 0 : found + 1;
        }

        std::size_t LineEnd(std::string_view text, std::size_t index) {
            const std::size_t found = text.find('\n', index);
            return found == std::string_view::npos ? text.size() : found;
        }

        // A single-line editor over a UTF-8 string, plus the multi-line variant, plus a caret that
        // has to survive the view tree being rebuilt underneath it. The caret lives on the host,
        // keyed by node, for exactly that reason.
        class TextInputBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::TextInput; }

            // The one place the pointer means "put the caret here", so it says so.
            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::IBeam : CursorShape::NotAllowed;
            }

            void Sync(WidgetContext& context) override {
                const std::string value = context.Str(doc::Prop::Text);
                TextEditState& edit = State(context);
                edit.caret = std::min(edit.caret, value.size());
                edit.anchor = std::min(edit.anchor, value.size());
                Mirror(context, value);
            }

            void OnTick(WidgetContext& context, f32 dt) override {
                if (context.host.Focused() != context.view) return;
                State(context).blink += dt;
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                switch (event.type) {
                    case EventType::MouseButtonPressed:  return OnPress(context, event);
                    case EventType::MouseMoved:          return OnDrag(context, event);
                    case EventType::MouseButtonReleased: return true;
                    case EventType::KeyPressed:          return OnKey(context, event);
                    case EventType::TextInput:           return OnChar(context, event);
                    default: return false;
                }
            }

            void OnPaint(const WidgetContext& context, PaintContext& paint) const override {
                if (!paint.list) return;
                const auto* edit = context.host.FindEditState(context.Id());
                if (!edit) return;

                const u32 label = LabelOf(context.tree, context.view);
                if (label == ViewTree::kInvalid) return;
                const std::string display = Display(context);
                const text::TextStyle style = context.tree.StyleFor(label);
                if (!style.font) return;

                const Rect box = context.tree.Bounds(label);
                const auto layout = text::TextLayout::Layout(display, style, box.size.x,
                                                             WrapOf(context, label));
                const text::FontMetrics metrics = style.font->Metrics(style.size);

                // One rect per line, not one rect for the whole range. A selection that spans a
                // wrap is the normal case the moment text is allowed to be more than one line, and
                // a single rect draws it as a band across everything in between.
                if (edit->HasSelection()) {
                    const std::size_t begin = DisplayOffset(context, display, edit->Begin());
                    const std::size_t end   = DisplayOffset(context, display, edit->End());
                    for (const auto& line : layout.lines) {
                        f32 from = 0.0f, to = 0.0f;
                        bool any = false;
                        for (u32 i = line.firstGlyph; i < line.firstGlyph + line.glyphCount; ++i) {
                            const auto& glyph = layout.glyphs[i];
                            if (glyph.byteOffset < begin || glyph.byteOffset >= end) continue;
                            from = any ? std::min(from, glyph.pen.x) : glyph.pen.x;
                            to   = any ? std::max(to, glyph.pen.x + glyph.advance)
                                       : glyph.pen.x + glyph.advance;
                            any = true;
                        }
                        if (!any) continue;
                        const Rect selection{ { box.pos.x + from,
                                                box.pos.y + line.baselineY + metrics.ascent },
                                              { std::max(to - from, 1.0f), metrics.LineHeight() } };
                        paint.list->AddRect(selection,
                                            draw::Paint::Solid({ 0.35f, 0.55f, 0.95f, 0.45f }));
                    }
                }

                // The caret blinks only while focused, and a blink phase that restarts on every
                // keystroke is what makes fast typing readable.
                if (context.host.Focused() != context.view) return;
                if (std::fmod(edit->blink, 1.0f) >= 0.5f) return;
                const Vec2 spot = CaretSpot(context, layout, metrics, display, edit->caret);
                const Rect caret{ { box.pos.x + spot.x, box.pos.y + spot.y },
                                  { 1.5f, metrics.LineHeight() } };
                paint.list->AddRect(caret, draw::Paint::Solid(
                    context.tree.Resolved(label).Colour(doc::Prop::TextColor, { 1, 1, 1, 1 })));
            }

            void OnFocusLost(WidgetContext& context) override {
                TextEditState& edit = State(context);
                edit.anchor = edit.caret;
            }

        private:
            static TextEditState& State(const WidgetContext& context) {
                return context.host.EditState(context.Id());
            }

            // What the user sees, which is not what the field holds when it is a password field or
            // when it is empty and showing its placeholder.
            static std::string Display(const WidgetContext& context) {
                const std::string value = context.Str(doc::Prop::Text);
                if (!context.Flag(doc::Prop::Password)) return value;
                std::string masked;
                for (std::size_t i = 0, count = Utf8Length(value); i < count; ++i)
                    Utf8Append(masked, 0x2022);   // BULLET
                return masked;
            }

            // A password field's glyphs do not line up with the value's bytes, so an offset into
            // the value is converted through codepoint index rather than assumed to share offsets.
            static std::size_t DisplayOffset(const WidgetContext& context, std::string_view display,
                                             std::size_t byteOffset) {
                if (!context.Flag(doc::Prop::Password)) return byteOffset;
                return ByteOffsetOf(display, CodepointIndexOf(context.Str(doc::Prop::Text), byteOffset));
            }

            // Where the caret sits: an x, and the top of the line it is on. Reading only the x is
            // what pins a multi-line field's caret to the first line for ever.
            static Vec2 CaretSpot(const WidgetContext& context, const text::TextLayoutResult& layout,
                                  const text::FontMetrics& metrics, std::string_view display,
                                  std::size_t byteOffset) {
                const std::size_t target = DisplayOffset(context, display, byteOffset);
                for (const auto& glyph : layout.glyphs)
                    if (glyph.byteOffset >= target)
                        return { glyph.pen.x, glyph.pen.y + metrics.ascent };
                if (layout.glyphs.empty()) return { 0.0f, 0.0f };
                const auto& last = layout.glyphs.back();
                return { last.pen.x + last.advance, last.pen.y + metrics.ascent };
            }

            // Visual navigation needs the laid-out lines rather than the string: a wrapped
            // paragraph has more lines than it has newlines, so Up from the second visual line of
            // one paragraph must land on the first, not on the paragraph before it. A password
            // field has neither wrapping nor newlines, and its glyphs do not line up with its
            // bytes, so it keeps the string answers.
            bool Laid(const WidgetContext& context, text::TextLayoutResult& out) const {
                if (context.Flag(doc::Prop::Password)) return false;
                const u32 label = LabelOf(context.tree, context.view);
                if (label == ViewTree::kInvalid) return false;
                const text::TextStyle style = context.tree.StyleFor(label);
                if (!style.font) return false;
                out = text::TextLayout::Layout(Display(context), style,
                                               context.tree.Bounds(label).size.x,
                                               WrapOf(context, label));
                return !out.lines.empty();
            }

            static u32 LineOf(const text::TextLayoutResult& layout, std::size_t offset) {
                u32 line = 0;
                for (const auto& glyph : layout.glyphs) {
                    if (glyph.byteOffset >= offset) return glyph.line;
                    line = glyph.line;
                }
                return line;
            }

            std::size_t StepLine(const WidgetContext& context, std::size_t caret, int delta) const {
                const std::string value = context.Str(doc::Prop::Text);
                text::TextLayoutResult layout;
                if (!Laid(context, layout))
                    return delta < 0 ? std::size_t(0) : value.size();

                const auto target = static_cast<i32>(LineOf(layout, caret)) + delta;
                // Off either end is the document's start or end, which is what every editor does.
                if (target < 0) return 0;
                if (target >= static_cast<i32>(layout.lines.size())) return value.size();

                const text::TextStyle style = context.tree.StyleFor(LabelOf(context.tree, context.view));
                const text::FontMetrics metrics = style.font->Metrics(style.size);
                const Vec2 spot = CaretSpot(context, layout, metrics, value, caret);
                return text::TextLayout::HitTest(
                    layout, { spot.x, layout.lines[static_cast<std::size_t>(target)].baselineY });
            }

            std::size_t VisualStart(const WidgetContext& context, std::size_t caret) const {
                const std::string value = context.Str(doc::Prop::Text);
                text::TextLayoutResult layout;
                if (!Laid(context, layout) || layout.lines.size() < 2)
                    return LineStart(value, caret);
                const text::TextLine& line = layout.lines[LineOf(layout, caret)];
                if (line.glyphCount == 0) return LineStart(value, caret);
                return layout.glyphs[line.firstGlyph].byteOffset;
            }

            std::size_t VisualEnd(const WidgetContext& context, std::size_t caret) const {
                const std::string value = context.Str(doc::Prop::Text);
                text::TextLayoutResult layout;
                if (!Laid(context, layout) || layout.lines.size() < 2)
                    return LineEnd(value, caret);
                const u32 index = LineOf(layout, caret);
                if (index + 1 >= layout.lines.size()) return value.size();
                const text::TextLine& next = layout.lines[index + 1];
                if (next.glyphCount == 0) return LineEnd(value, caret);
                // Up to, not past: the caret sits before the first glyph of the next line.
                return layout.glyphs[next.firstGlyph].byteOffset;
            }

            // Wrapping is the label's own decision, and the caret has to agree with it. Laying the
            // caret out against a different wrap than the text was painted with puts it somewhere
            // the character it belongs to is not.
            static text::WrapMode WrapOf(const WidgetContext& context, u32 label) {
                const std::string name = context.tree.Str(label, doc::Prop::TextWrap, "word");
                if (name == "none") return text::WrapMode::None;
                if (name == "char") return text::WrapMode::Char;
                return text::WrapMode::Word;
            }

            void Mirror(WidgetContext& context, const std::string& value) {
                const u32 label = LabelOf(context.tree, context.view);
                if (label == ViewTree::kInvalid) return;

                if (value.empty()) {
                    const std::string placeholder = context.Str(doc::Prop::Placeholder);
                    context.tree.At(label).props.Set(doc::Prop::Text, doc::Value{ placeholder });
                    // Placeholders read as hints, not content — dimming is the one visual decision
                    // the behavior makes, and a designer overrides it with a "placeholder" style.
                    if (!placeholder.empty()) {
                        doc::PropBag& props = context.tree.At(label).props;
                        Color colour = props.Colour(doc::Prop::TextColor, { 1, 1, 1, 1 });
                        colour.a *= 0.45f;
                        props.Set(doc::Prop::TextColor, doc::Value{ colour });
                    }
                    return;
                }
                context.tree.At(label).props.Set(doc::Prop::Text,
                                                 doc::Value{ context.Flag(doc::Prop::Password)
                                                             ? Display(context) : value });
            }

            std::size_t OffsetAt(WidgetContext& context, Vec2 point) const {
                const u32 label = LabelOf(context.tree, context.view);
                if (label == ViewTree::kInvalid) return 0;
                const std::string display = Display(context);
                const text::TextStyle style = context.tree.StyleFor(label);
                if (!style.font) return 0;

                const Rect box = context.tree.Bounds(label);
                const auto layout = text::TextLayout::Layout(display, style, box.size.x,
                                                             WrapOf(context, label));
                const std::size_t offset = text::TextLayout::HitTest(layout, point - box.pos);
                if (!context.Flag(doc::Prop::Password)) return offset;
                return ByteOffsetOf(context.Str(doc::Prop::Text), CodepointIndexOf(display, offset));
            }

            bool OnPress(WidgetContext& context, const Event& event) {
                if (event.button.button != Mouse::Left) return false;
                TextEditState& edit = State(context);
                edit.caret = edit.anchor = OffsetAt(context, PointOf(event));
                edit.blink = 0.0f;
                context.host.Capture(context.view);
                return true;
            }

            bool OnDrag(WidgetContext& context, const Event& event) {
                if (context.host.Captured() != context.view) return false;
                State(context).caret = OffsetAt(context, PointOf(event));
                return true;
            }

            bool OnChar(WidgetContext& context, const Event& event) {
                if (context.Flag(doc::Prop::ReadOnly)) return true;
                if (event.text.codepoint < 0x20 || event.text.codepoint == 0x7F) return true;
                std::string inserted;
                Utf8Append(inserted, event.text.codepoint);
                Insert(context, inserted);
                return true;
            }

            bool OnKey(WidgetContext& context, const Event& event) {
                const bool shift = (event.mods & Mod::Shift) != 0;
                const bool control = (event.mods & Mod::Control) != 0;
                const bool readOnly = context.Flag(doc::Prop::ReadOnly);
                const std::string value = context.Str(doc::Prop::Text);
                TextEditState& edit = State(context);
                edit.blink = 0.0f;

                switch (event.key.code) {
                    case Key::Left:
                        MoveTo(context, control ? PrevWord(value, edit.caret)
                                                : PrevBoundary(value, edit.caret), shift);
                        return true;
                    case Key::Right:
                        MoveTo(context, control ? NextWord(value, edit.caret)
                                                : NextBoundary(value, edit.caret), shift);
                        return true;
                    case Key::Up:
                        if (!context.Flag(doc::Prop::Multiline)) return false;
                        MoveTo(context, StepLine(context, edit.caret, -1), shift);
                        return true;
                    case Key::Down:
                        if (!context.Flag(doc::Prop::Multiline)) return false;
                        MoveTo(context, StepLine(context, edit.caret, 1), shift);
                        return true;
                    case Key::Home: MoveTo(context, VisualStart(context, edit.caret), shift); return true;
                    case Key::End:  MoveTo(context, VisualEnd(context, edit.caret), shift);   return true;

                    case Key::Backspace:
                        if (readOnly) return true;
                        if (!edit.HasSelection() && edit.caret > 0)
                            edit.anchor = control ? PrevWord(value, edit.caret)
                                                  : PrevBoundary(value, edit.caret);
                        Replace(context, {});
                        return true;

                    case Key::Delete:
                        if (readOnly) return true;
                        if (!edit.HasSelection() && edit.caret < value.size())
                            edit.anchor = control ? NextWord(value, edit.caret)
                                                  : NextBoundary(value, edit.caret);
                        Replace(context, {});
                        return true;

                    case Key::Enter:
                        if (context.Flag(doc::Prop::Multiline)) {
                            if (!readOnly) Insert(context, "\n");
                        } else {
                            Fire(context, ActionKind::Submitted, doc::Value{ value });
                        }
                        return true;

                    case Key::A:
                        if (!control) return false;
                        edit.anchor = 0;
                        edit.caret = value.size();
                        return true;

                    case Key::C:
                        if (!control) return false;
                        if (edit.HasSelection())
                            context.host.GetClipboard().SetText(
                                value.substr(edit.Begin(), edit.End() - edit.Begin()));
                        return true;

                    case Key::X:
                        if (!control) return false;
                        if (edit.HasSelection()) {
                            context.host.GetClipboard().SetText(
                                value.substr(edit.Begin(), edit.End() - edit.Begin()));
                            if (!readOnly) Replace(context, {});
                        }
                        return true;

                    case Key::V: {
                        if (!control || readOnly) return control;
                        std::string pasted = context.host.GetClipboard().GetText();
                        if (!context.Flag(doc::Prop::Multiline))
                            std::erase(pasted, '\n');
                        Insert(context, pasted);
                        return true;
                    }

                    default: return false;
                }
            }

            void MoveTo(WidgetContext& context, std::size_t offset, bool extend) {
                TextEditState& edit = State(context);
                edit.caret = offset;
                if (!extend) edit.anchor = offset;
            }

            void Insert(WidgetContext& context, const std::string& text) {
                Replace(context, text);
            }

            void Replace(WidgetContext& context, const std::string& insertion) {
                std::string value = context.Str(doc::Prop::Text);
                TextEditState& edit = State(context);
                const std::size_t begin = std::min(edit.Begin(), value.size());
                const std::size_t end = std::min(edit.End(), value.size());

                std::string next = value.substr(0, begin) + insertion + value.substr(end);
                const auto maxLength = static_cast<std::size_t>(context.Number(doc::Prop::MaxLength, 0.0f));
                if (maxLength > 0 && Utf8Length(next) > maxLength) {
                    // Truncate rather than reject: a paste that is one character too long should
                    // land, not vanish.
                    next.resize(ByteOffsetOf(next, maxLength));
                    if (begin + insertion.size() > next.size()) {
                        edit.caret = edit.anchor = next.size();
                        Commit(context, next);
                        return;
                    }
                }

                edit.caret = edit.anchor = begin + insertion.size();
                edit.blink = 0.0f;
                if (next == value) return;
                Commit(context, next);
            }

            void Commit(WidgetContext& context, const std::string& value) {
                context.Set(doc::Prop::Text, doc::Value{ value });
                Mirror(context, value);
                Fire(context, ActionKind::TextChanged, doc::Value{ value });
            }
        };

    }

    Scope<Behavior> MakeTextInput() { return CreateScope<TextInputBehavior>(); }

}
