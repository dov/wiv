import re, sys
import gi
gi.require_version('Gdk', '3.0')
from gi.repository import Gdk

# giv resolves color names via gdk_rgba_parse() (see color_parse() in
# giv-parser.cc, which calls gdk_rgba_parse for anything not of the form
# "r,g,b" or "r,g,b,a"). GDK's parser uses CSS3/SVG named colors, which are
# NOT the same table as the X11 rgb.txt file for a handful of names (most
# famously "green": X11 rgb.txt has (0,255,0), but CSS/SVG "green" is
# (0,128,0) - X11's (0,255,0) is CSS "lime"). Other historical X11/CSS
# mismatches include "maroon", "purple", and "gray"/"grey" percent shades.
#
# To stay byte-for-byte compatible with giv's actual runtime behavior, we
# seed the table from the X11R6 rgb.txt name list (for full name coverage,
# including things like "gray37" that aren't part of the small CSS named
# set) but then re-resolve every name through gdk_rgba_parse() - the same
# function giv calls - and prefer that result whenever it succeeds. Only
# names gdk_rgba_parse() can't parse at all fall back to the raw X11 rgb.txt
# value.

seen = {}
order = []
with open('/usr/share/emacs/30.2/etc/rgb.txt') as f:
    for line in f:
        line = line.rstrip('\n')
        if not line or line.startswith('#') or line.startswith('!'):
            continue
        m = re.match(r'\s*(\d+)\s+(\d+)\s+(\d+)\s+(.+?)\s*$', line)
        if not m:
            continue
        r,g,b,name = m.groups()
        key = name.lower().replace(' ', '')
        if key in seen:
            continue  # first definition wins (matches X11 convention)
        seen[key] = (int(r),int(g),int(b))
        order.append(key)

print(len(order), "colors from X11 rgb.txt", file=sys.stderr)

num_overridden = 0
for key in order:
    rgba = Gdk.RGBA()
    if rgba.parse(key):
        r = round(rgba.red * 255)
        g = round(rgba.green * 255)
        b = round(rgba.blue * 255)
        if (r,g,b) != seen[key]:
            num_overridden += 1
        seen[key] = (r,g,b)
    # else: gdk_rgba_parse() couldn't resolve this name either - keep the
    # X11 rgb.txt fallback value already in seen[key].

print(num_overridden, "entries overridden by gdk_rgba_parse to match giv",
      file=sys.stderr)
for check in ["midnightblue","pink3","purple3","orange","gray","purple","green","blue","red","black","white","none","maroon"]:
    print(check, seen.get(check), file=sys.stderr)

with open('/home/dov/git/wiv/src/GivColorTable.inc', 'w') as out:
    out.write("// Auto-generated color name table matching giv's actual runtime color\n")
    out.write("// resolution: giv parses names via GDK's gdk_rgba_parse() (see\n")
    out.write("// color_parse() in giv-parser.cc), which uses CSS3/SVG named colors -\n")
    out.write("// NOT the same table as the historical X11 rgb.txt (they disagree on a\n")
    out.write("// handful of names, e.g. \"green\" is (0,255,0) in X11 rgb.txt but\n")
    out.write("// (0,128,0) per CSS/SVG and GDK). The name list/coverage comes from the\n")
    out.write("// X11R6 rgb.txt file (X Consortium, 1994, as shipped with GNU Emacs), but\n")
    out.write("// every value is re-resolved through gdk_rgba_parse() - the same call giv\n")
    out.write("// makes - so this table matches giv byte-for-byte wherever GDK can parse\n")
    out.write("// the name; only names GDK can't parse fall back to the raw X11 value.\n")
    out.write("// Generator: tools/gen_color_table.py.\n")
    out.write("static const GivNamedColor g_givColorTable[] = {\n")
    for key in order:
        r,g,b = seen[key]
        out.write(f'  {{"{key}", {r}, {g}, {b}}},\n')
    out.write("};\n")
    out.write(f"static const size_t g_givColorTableSize = {len(order)};\n")
