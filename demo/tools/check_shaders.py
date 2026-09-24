"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds twelve pieces of GLSL and so does `source/Shaders.cpp`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. The whole claim of these pages is that they run the plugin's own
shader rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `dltest` drives the real plugin class and has no idea this
page exists, and `tools/check-shaders.sh` puts the plugin's shaders through
glslc (via `dltest --dump-shaders`) and never looks at the JS copy.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment updated on one side and not the other is exactly
the drift worth catching, because the comments in these shaders carry the
derivation: why the noise is drawn in the carrier's frame, why a link sample is
centred on its own instant, why the Slow clamp's history is computed.

The link pass is not one literal in the C++. `kLinkShader` is the
`DOWNLINK_LINK_LIBRARY` macro followed by a second raw string holding its
`main`, which the compiler joins. So the page carries the two pieces as
`LINK_LIBRARY` and `LINK_MAIN`, each compared here, and this also checks that
the page joins them the way C++ does and in no other way. The harness's probe
(`kLinkProbeShader`, the same library with another `main`) is not carried: the
page does not run it, and neither does the plugin outside `dltest`.

The one transformation is a decode, not a normalisation. Two comments in the
link library quote an identifier with backticks -- `turn`, `cached`, `at` --
and a backtick cannot appear raw inside a JavaScript template literal, so
`plugin.js` escapes it as \\`. This unescapes that and *rejects any other
backslash on the JS side*; there are none anywhere in the C++, so a second
escape could only be somebody hiding a difference. A `${` would interpolate in
a template literal, so that is rejected too.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `resolve`, `HostClock`, `MakeRowTable`,
`NoiseTaps`, the emphasis networks and every conversion in plugin.js are a hand
translation of Downlink.cpp, Link.cpp and Controls.h, and only a reader can tell
whether they still agree. When you change one of those, change it here too --
and remember that a wrong tap or a wrong conversion shows up on the page as a
picture that is subtly noisier or cleaner than it should be, which nobody will
notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CPP = "source/Shaders.cpp"

# JS constant, and how to find the same text in the C++.
#
# Twelve pieces for eleven passes' worth of GLSL: the vertex, the ten fragment
# shaders the plugin renders with, with the link split into the macro and its
# main as the C++ has it.
SHADERS = [
    ("VERTEX", "kVertexShader"),
    ("RESAMPLE", "kResampleShader"),
    ("ENCODE", "kEncodeShader"),
    ("PREEMPH", "kPreemphShader"),
    ("LINK_LIBRARY", "#define DOWNLINK_LINK_LIBRARY"),
    ("LINK_MAIN", "kLinkShader"),
    ("DETECT", "kDetectShader"),
    ("DEEMPH", "kDeemphShader"),
    ("PORCH", "kPorchShader"),
    ("CLAMP", "kClampShader"),
    ("DECODE", "kDecodeShader"),
    ("OUTPUT", "kOutputShader"),
]

# How the page assembles the link pass. Exactly this line, and it must appear.
LINK_JOIN = "const LINK = LINK_LIBRARY + LINK_MAIN;"


def from_cpp(source, symbol):
    if symbol.startswith("#define "):
        name = symbol.split()[1]
        pattern = r"^#define " + name + r' R"\((.*?)\)"$'
    elif symbol == "kLinkShader":
        pattern = r'const char\* const kLinkShader = DOWNLINK_LINK_LIBRARY R"\((.*?)\)";'
    else:
        pattern = r"const char\* const " + symbol + r' = R"\((.*?)\)";'
    match = re.search(pattern, source, re.S | re.M)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r"^const " + name + r" = `(.*?)`;$", source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    if "${" in body:
        upto = body[: body.index("${")]
        return None, f"template interpolation, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, CPP)) as handle:
        cpp = handle.read()
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {CPP}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<14} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {CPP}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break
        else:
            print("        the lines agree; the difference is a trailing newline")

    # The join. C++ glues the macro's literal to the main's; the page has to do
    # that and nothing more, or it is running a link shader nobody compared.
    joins = [line for line in js.splitlines() if re.match(r"^const LINK\b", line)]
    if joins == [LINK_JOIN]:
        print(f"ok    LINK           is LINK_LIBRARY + LINK_MAIN, as kLinkShader is")
    else:
        problems += 1
        print(f"FAIL  the link pass is not assembled as `{LINK_JOIN}` (found {joins or 'nothing'})")

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shader pieces are identical to the plugin's, and the link is joined as C++ joins it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
