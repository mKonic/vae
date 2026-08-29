#include "Test.h"

#include "vae/a11y/TextBoundary.h"

#include <string>

using namespace vae;
using namespace vae::a11y;

// How a screen reader walks a piece of text: a word, a line, a sentence at a time.
//
// This is the part of a bridge that used to be unreachable — it lived inside the D-Bus callbacks,
// where checking it meant attaching a screen reader and listening. What it answers is literally
// what gets read out loud, and every one of these cases is a way for that to be wrong while the
// app looks perfect on screen.

namespace {

    std::string At(std::string_view text, i32 offset, Granularity granularity) {
        const Characters chars(text);
        return std::string(chars.Slice(RangeFor(chars, offset, granularity).start,
                                       RangeFor(chars, offset, granularity).end));
    }

    std::string At(std::string_view text, i32 offset, Boundary boundary) {
        const Characters chars(text);
        const Range range = RangeFor(chars, offset, boundary);
        return std::string(chars.Slice(range.start, range.end));
    }

}

TEST(a11y_text, characters_are_counted_the_way_atspi_counts_them) {
    // Not bytes. A field with anything but ASCII in it reports the wrong place in itself
    // otherwise, and "the wrong place" means a caret announced inside a character.
    const Characters chars("héllo wörld");
    CHECK_EQ(chars.Count(), 11);
    CHECK_EQ(chars.CodeAt(1), 0xE9u);                 // é, one character and two bytes
    CHECK_EQ(chars.ByteAt(2), std::size_t{ 3 });
    CHECK_EQ(std::string(chars.Slice(0, 5)), std::string("héllo"));
    // Past either end is clamped rather than read: a client is allowed to ask for nonsense.
    CHECK_EQ(chars.CodeAt(-1), 0u);
    CHECK_EQ(chars.CodeAt(99), 0u);
    CHECK_EQ(std::string(chars.Slice(9, 200)), std::string("ld"));
}

TEST(a11y_text, a_word_is_the_run_of_non_spaces_around_the_offset) {
    CHECK_EQ(At("the quick brown", 5, Granularity::Word), std::string("quick"));
    CHECK_EQ(At("the quick brown", 0, Granularity::Word), std::string("the"));
    CHECK_EQ(At("the quick brown", 14, Granularity::Word), std::string("brown"));
    // The gap between two words is not a word. A reader told it is one announces nothing
    // between every pair of them.
    CHECK_EQ(At("the quick brown", 3, Granularity::Word), std::string(""));
}

TEST(a11y_text, the_old_word_boundary_keeps_the_spaces_after_it) {
    // WORD_START runs to the start of the next word, so the spaces belong to the word before
    // them; WORD_END runs back to the end of the previous one. Both are the older API, and
    // screen readers still call it.
    CHECK_EQ(At("the quick brown", 4, Boundary::WordStart), std::string("quick "));
    CHECK_EQ(At("the quick brown", 4, Boundary::WordEnd), std::string(" quick"));
}

TEST(a11y_text, a_line_stops_at_the_newline) {
    const std::string text = "first line\nsecond line";
    CHECK_EQ(At(text, 3, Granularity::Line), std::string("first line"));
    CHECK_EQ(At(text, 15, Granularity::Line), std::string("second line"));
    // A paragraph is a line here, because VAE's text has no other structure to find and
    // inventing one would be inventing an answer.
    CHECK_EQ(At(text, 3, Granularity::Paragraph), std::string("first line"));
    // LINE_START takes the newline with it, which is what makes walking forwards land on the
    // next line rather than on the break between them.
    CHECK_EQ(At(text, 3, Boundary::LineStart), std::string("first line\n"));
}

TEST(a11y_text, a_sentence_is_a_sentence_and_not_the_whole_field) {
    const std::string text = "Save the file. Then close it. Done!";
    CHECK_EQ(At(text, 2, Granularity::Sentence), std::string("Save the file."));
    CHECK_EQ(At(text, 20, Granularity::Sentence), std::string("Then close it."));
    CHECK_EQ(At(text, 31, Granularity::Sentence), std::string("Done!"));
    // The offset at the very end is the last sentence, not nothing.
    CHECK_EQ(At(text, static_cast<i32>(text.size()), Granularity::Sentence), std::string("Done!"));
}

TEST(a11y_text, a_full_stop_that_is_not_the_end_of_a_sentence_does_not_end_one) {
    // A decimal point, and an abbreviation followed by a lowercase word. Splitting on every
    // period turns one sentence into three, and a reader walking by sentence then reads
    // fragments — which is worse than the field being read out in one go.
    CHECK_EQ(At("It costs 1.50 today.", 0, Granularity::Sentence),
             std::string("It costs 1.50 today."));
    CHECK_EQ(At("Use tabs, e.g. two of them. Not spaces.", 0, Granularity::Sentence),
             std::string("Use tabs, e.g. two of them."));
}

TEST(a11y_text, a_sentence_takes_the_quotation_mark_that_closes_it) {
    const std::string text = "He said \"go.\" She went.";
    CHECK_EQ(At(text, 0, Granularity::Sentence), std::string("He said \"go.\""));
    CHECK_EQ(At(text, 15, Granularity::Sentence), std::string("She went."));
}

TEST(a11y_text, a_line_break_ends_a_sentence_whether_or_not_it_was_punctuated) {
    const std::string text = "Shopping list\nEggs and milk";
    CHECK_EQ(At(text, 2, Granularity::Sentence), std::string("Shopping list"));
    CHECK_EQ(At(text, 20, Granularity::Sentence), std::string("Eggs and milk"));
}

TEST(a11y_text, text_with_no_terminator_in_it_is_one_sentence) {
    // The common case for a text field, and the one the old answer — the whole of the text —
    // got right by accident.
    CHECK_EQ(At("someone@example.com", 4, Granularity::Sentence),
             std::string("someone@example.com"));
    CHECK_EQ(At("", 0, Granularity::Sentence), std::string(""));
    CHECK_EQ(At("   ", 1, Granularity::Sentence), std::string("   "));
}

TEST(a11y_text, the_sentence_boundary_variants_take_the_space_between_them) {
    const std::string text = "One. Two.";
    CHECK_EQ(At(text, 0, Boundary::SentenceStart), std::string("One. "));
    CHECK_EQ(At(text, 6, Boundary::SentenceEnd), std::string(" Two."));
}

TEST(a11y_text, walking_by_sentence_covers_the_text_exactly_once) {
    // What a reader does with the arrow keys: ask for the range here, then for the one after
    // it. Ranges that overlap read a word twice; ranges with a gap lose one.
    const std::string source = "Alpha beta. Gamma delta! Epsilon?";
    const Characters chars(source);
    i32 at = 0;
    std::string rebuilt;
    u32 guard = 0;
    while (at < chars.Count() && guard++ < 16) {
        const Range range = RangeFor(chars, at, Boundary::SentenceStart);
        CHECK(range.end > at || range.start > at);
        rebuilt += std::string(chars.Slice(range.start, range.end));
        at = range.end;
    }
    CHECK_EQ(rebuilt, source);
}
