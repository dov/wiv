import re, sys

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

print(len(order), "colors", file=sys.stderr)
for check in ["midnightblue","pink3","purple3","orange","gray","purple","green","blue","red","black","white","none"]:
    print(check, seen.get(check), file=sys.stderr)

with open('/home/dov/git/wiv/src/GivColorTable.inc', 'w') as out:
    out.write("// Auto-generated from the authentic X11R6 rgb.txt (X Consortium, 1994),\n")
    out.write("// as shipped with GNU Emacs (xc/programs/rgb/rgb.txt). Public/MIT-style\n")
    out.write("// license per the X Consortium copyright notice in that file.\n")
    out.write("// Generator: tools/gen_color_table.py (see scratchpad history in README).\n")
    out.write("static const GivNamedColor g_givColorTable[] = {\n")
    for key in order:
        r,g,b = seen[key]
        out.write(f'  {{"{key}", {r}, {g}, {b}}},\n')
    out.write("};\n")
    out.write(f"static const size_t g_givColorTableSize = {len(order)};\n")
