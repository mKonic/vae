#!/usr/bin/env python3
"""Drives a running VAE app over AT-SPI, the way a screen reader would.

The accessibility *tree* is unit-tested — VAE-Tests, `a11y` — because building it is pure inspection
of the view tree. What cannot be: whether the D-Bus interfaces that carry it are shaped the way a
client expects, whether pressing a button over the bus really presses it, and whether the app says
what changed. All of that needs a bus and a real app, so it lives here.

    ./bin/<config>/VAE-Player/VAE-Player ~/Documents/VAE/'Counter example'/'Counter example.vaescreen' &
    ./scripts/CheckAccessibility.py Home

The argument is any part of the screen's name. Exit code is the result. Needs PyGObject (Gio), which
every desktop with an accessibility bus already has, and an accessibility bus to be running — a
machine with none is not a failure, and this says so and exits 0.

Run it against a nested compositor rather than the live session; a screen reader pressing buttons in
the app you are using is exactly as disruptive as it sounds.
"""
import sys

try:
    import gi
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib
except (ImportError, ValueError) as problem:
    print(f"accessibility check needs PyGObject: {problem}")
    sys.exit(77)

APP = sys.argv[1] if len(sys.argv) > 1 else "Home"

failures, total = [], [0]


def check(ok, what, detail=""):
    total[0] += 1
    if not ok:
        failures.append(what)
    print(("  ok   " if ok else "  FAIL ") + what + (f"  [{detail}]" if detail else ""))


def wait(ms):
    loop = GLib.MainLoop()
    GLib.timeout_add(ms, lambda: (loop.quit(), False)[1])
    loop.run()


# --- the accessibility bus, which only the session bus knows the address of ----------------------
session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
try:
    address = session.call_sync("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress",
                                None, GLib.VariantType("(s)"), 0, 2000, None).unpack()[0]
except GLib.Error as problem:
    print(f"no accessibility bus on this desktop: {problem.message}")
    sys.exit(77)

bus = Gio.DBusConnection.new_for_address_sync(
    address,
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    None, None)


def call(dest, path, iface, method, args=None, reply=None):
    return bus.call_sync(dest, path, iface, method, args,
                         GLib.VariantType(reply) if reply else None, 0, 2000, None)


def prop(dest, path, iface, name):
    return call(dest, path, "org.freedesktop.DBus.Properties", "Get",
                GLib.Variant("(ss)", (iface, name)), "(v)").unpack()[0]


def children(dest, path):
    return call(dest, path, "org.a11y.atspi.Accessible", "GetChildren", None, "(a(so))").unpack()[0]


def name_of(dest, path):
    return prop(dest, path, "org.a11y.atspi.Accessible", "Name")


def role_of(dest, path):
    return call(dest, path, "org.a11y.atspi.Accessible", "GetRoleName", None, "(s)").unpack()[0]


def interfaces(dest, path):
    return call(dest, path, "org.a11y.atspi.Accessible", "GetInterfaces", None, "(as)").unpack()[0]


# --- find the app the registry was told about ---------------------------------------------------
ROOT = "/org/a11y/atspi/accessible/root"
apps = children("org.a11y.atspi.Registry", ROOT)
target = None
for dest, path in apps:
    try:
        if APP.lower() in name_of(dest, path).lower():
            target = (dest, path)
            break
    except GLib.Error:
        continue

if not target:
    seen = []
    for d, p in apps:
        try:
            seen.append(name_of(d, p))
        except GLib.Error:
            pass
    print(f"no application matching {APP!r} on the accessibility bus; saw {seen}")
    sys.exit(2)

dest, apppath = target
print(f"application: {name_of(dest, apppath)!r} at {dest}")

found = []


def walk(path, depth=0):
    if depth > 12:
        return
    try:
        found.append((path, role_of(dest, path), name_of(dest, path)))
        for _, child in children(dest, path):
            walk(child, depth + 1)
    except GLib.Error:
        pass


walk(apppath)
buttons = [n for n in found if n[1] == "push button"]
labels = [n for n in found if n[1] == "label"]
entries = [n for n in found if n[1] == "entry"]
secrets = [n for n in found if n[1] == "password text"]
print(f"walked {len(found)} nodes: {len(buttons)} buttons, {len(labels)} labels, "
      f"{len(entries)} entries, {len(secrets)} password fields")
check(len(found) > 1, "the app publishes a tree")


# --- Action: what a screen reader offers, and whether doing it does anything --------------------
if buttons:
    path = buttons[0][0]
    check("org.a11y.atspi.Action" in interfaces(dest, path), "a button carries Action")
    count = prop(dest, path, "org.a11y.atspi.Action", "NActions")
    check(count == 1, "and offers exactly one thing to do", count)
    actions = call(dest, path, "org.a11y.atspi.Action", "GetActions", None, "(a(sss))").unpack()[0]
    check([a[0] for a in actions] == ["click"], "which is called click", actions)
    check(call(dest, path, "org.a11y.atspi.Action", "GetName",
               GLib.Variant("(i)", (0,)), "(s)").unpack()[0] == "click",
          "GetName agrees with GetActions")
    check(not call(dest, path, "org.a11y.atspi.Action", "DoAction",
                   GLib.Variant("(i)", (7,)), "(b)").unpack()[0],
          "an action index it does not have is refused")

if labels:
    check("org.a11y.atspi.Action" not in interfaces(dest, labels[0][0]),
          "a label carries no Action — there is nothing to do to it")
    check("org.a11y.atspi.Text" in interfaces(dest, labels[0][0]),
          "but it does carry Text, so a long one can be read a line at a time")

# The one that matters: pressing it has to actually press it. A counter is the app that says so.
counters = [n for n in labels if n[2].strip().lstrip("-").isdigit()]
add = next((b for b in buttons if "add" in b[2].lower() or "+" in b[2]), None)
if counters and add:
    counter = counters[0][0]
    before = int(name_of(dest, counter))

    seen_events = []
    bus.signal_subscribe(dest, "org.a11y.atspi.Event.Object", None, None, None, 0,
                         lambda c, s, p, i, m, params: seen_events.append((m, params.unpack()[0])))

    check(call(dest, add[0], "org.a11y.atspi.Action", "DoAction",
               GLib.Variant("(i)", (0,)), "(b)").unpack()[0], "DoAction is accepted")
    wait(1200)
    after = int(name_of(dest, counter))
    check(after == before + 1, "and the app really did it", f"{before} -> {after}")

    for _ in range(2):
        call(dest, add[0], "org.a11y.atspi.Action", "DoAction", GLib.Variant("(i)", (0,)), "(b)")
        wait(400)
    check(int(name_of(dest, counter)) == before + 3, "three presses, three increments",
          name_of(dest, counter))

    kinds = {k for k in seen_events}
    print("  events:", sorted(kinds))
    check(any(s == "PropertyChange" and d == "accessible-name" for s, d in kinds),
          "what changed was announced, rather than left to be noticed")
else:
    print("  (no counter on this screen — run against 'Counter example' to check that a press "
          "reaches the app)")


# --- Text: what an entry holds, and where the caret is ------------------------------------------
if entries:
    path = entries[0][0]
    check("org.a11y.atspi.Text" in interfaces(dest, path), "an entry carries Text")
    check(call(dest, path, "org.a11y.atspi.Component", "GrabFocus", None, "(b)").unpack()[0],
          "and can be focused from the bus")

    text = call(dest, path, "org.a11y.atspi.Text", "GetText",
                GLib.Variant("(ii)", (0, -1)), "(s)").unpack()[0]
    count = prop(dest, path, "org.a11y.atspi.Text", "CharacterCount")
    check(count == len(text), "CharacterCount matches the text it returns", (count, text))

    if count > 1:
        check(call(dest, path, "org.a11y.atspi.Text", "SetCaretOffset",
                   GLib.Variant("(i)", (2,)), "(b)").unpack()[0], "SetCaretOffset is accepted")
        wait(600)
        now = call(dest, path, "org.a11y.atspi.Text", "GetCaretOffset", None, "(i)").unpack()[0]
        check(now == 2, "and the caret really moved there", now)

        word = call(dest, path, "org.a11y.atspi.Text", "GetStringAtOffset",
                    GLib.Variant("(iu)", (0, 1)), "(sii)").unpack()
        check(word[0] and word[0] in text, "a word can be read out of it", word)
        check(call(dest, path, "org.a11y.atspi.Text", "GetCharacterAtOffset",
                   GLib.Variant("(i)", (0,)), "(i)").unpack()[0] == ord(text[0]),
              "and a character by offset")
        extents = call(dest, path, "org.a11y.atspi.Text", "GetCharacterExtents",
                       GLib.Variant("(iu)", (0, 0)), "(iiii)").unpack()
        check(extents[2] > 0 and extents[3] > 0,
              "a character has extents, so a reader can box what it reads", extents)
    else:
        print("  (the entry is empty — type into it first to check the caret)")

if secrets:
    path = secrets[0][0]
    shown = call(dest, path, "org.a11y.atspi.Text", "GetText",
                 GLib.Variant("(ii)", (0, -1)), "(s)").unpack()[0]
    count = prop(dest, path, "org.a11y.atspi.Text", "CharacterCount")
    if count > 0:
        # The tree goes onto a bus any process on the session can read. "What is in that field" is
        # the one question a password field exists to refuse.
        check(shown and set(shown) <= {"*"}, "a password field hands out only asterisks", repr(shown))
        check(len(shown) == count, "one per character, so its length is still readable", count)
    else:
        print("  (the password field is empty — type into it first to check that it is masked)")

print()
if failures:
    print(f"{total[0] - len(failures)} passed, {len(failures)} FAILED")
    for problem in failures:
        print("  -", problem)
    sys.exit(1)
print(f"{total[0]} checks passed")
