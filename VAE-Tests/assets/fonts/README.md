Test fixtures, not engine assets — nothing outside `VAE-Tests` reads this directory and no export
copies it.

Two faces the vendored JetBrains Mono cannot stand in for, because the tests that use them are about
what happens when a script needs more than a codepoint-to-glyph mapping:

- `NotoSansArabic-Regular.ttf` — cursive joining and right-to-left ordering.
- `NotoSansDevanagari-Regular.ttf` — a matra typed after its consonant and drawn before it.

Both are Noto, under the SIL Open Font License 1.1 (`OFL.txt`).
