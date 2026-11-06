#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Render a Markdown file to HTML styled like GitHub and open it in the browser.

Usage: tools/show-md.py [FILE.md] [--out OUT.html] [--no-open]

Rendering is local (markdown-it-py, GFM-like: tables, strikethrough, raw HTML,
auto-linked URLs, GitHub-style heading anchors). Relative images and links
resolve against the Markdown file's directory.  Requires `pip install
markdown-it-py` (or the distro package python3-markdown-it-py).
"""
import argparse
import html
import re
import sys
import tempfile
import webbrowser
from pathlib import Path

try:
    from markdown_it import MarkdownIt
except ImportError:
    sys.exit("show-md.py needs markdown-it-py (pip install markdown-it-py)")

CSS = """
:root{color-scheme:light dark;--fg:#1f2328;--bg:#fff;--muted:#59636e;--border:#d1d9e0;
--code-bg:#f6f8fa;--link:#0969da;--th-bg:#f6f8fa}
@media (prefers-color-scheme:dark){:root{--fg:#f0f6fc;--bg:#0d1117;--muted:#9198a1;
--border:#3d444d;--code-bg:#151b23;--link:#4493f8;--th-bg:#151b23}}
body{margin:0;background:var(--bg);color:var(--fg);
font:16px/1.5 -apple-system,"Segoe UI",Noto Sans,Helvetica,Arial,sans-serif}
.page{max-width:980px;margin:0 auto;padding:32px 16px}
article{border:1px solid var(--border);border-radius:6px;padding:45px;word-wrap:break-word}
@media (max-width:767px){article{padding:15px;border:0}}
a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}
h1,h2,h3,h4,h5,h6{margin:24px 0 16px;font-weight:600;line-height:1.25}
h1{font-size:2em;padding-bottom:.3em;border-bottom:1px solid var(--border)}
h2{font-size:1.5em;padding-bottom:.3em;border-bottom:1px solid var(--border)}
h3{font-size:1.25em}h4{font-size:1em}
article>:first-child{margin-top:0}
p,ul,ol,table,pre,blockquote,details{margin:0 0 16px}
ul,ol{padding-left:2em}li+li{margin-top:.25em}
img{max-width:100%;box-sizing:content-box}
code,tt{font:85% ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
background:color-mix(in srgb,var(--muted) 20%,transparent);padding:.2em .4em;border-radius:6px}
pre{background:var(--code-bg);padding:16px;overflow:auto;border-radius:6px;line-height:1.45}
pre code{background:none;padding:0;font-size:85%}
blockquote{margin-left:0;padding:0 1em;color:var(--muted);border-left:.25em solid var(--border)}
hr{height:.25em;padding:0;margin:24px 0;background:var(--border);border:0}
table{border-collapse:collapse;display:block;max-width:100%;overflow:auto;width:max-content}
table table{display:table}
th,td{padding:6px 13px;border:1px solid var(--border)}
tr:nth-child(2n){background:var(--th-bg)}th{font-weight:600}
table[align=center] tr,table[align=center] td{background:none}
table[align=center] td{border:0}
kbd{font:11px ui-monospace,monospace;padding:3px 5px;border:1px solid var(--border);
border-radius:6px;background:var(--code-bg)}
"""


def slugify(text, seen):
    # GitHub: lowercase, drop everything but word chars, spaces and hyphens,
    # spaces -> hyphens; duplicates get -1, -2 ...
    s = re.sub(r"[^\w\- ]", "", text.strip().lower()).replace(" ", "-")
    n = seen.get(s, 0)
    seen[s] = n + 1
    return s if n == 0 else f"{s}-{n}"


def add_heading_ids(state):
    seen = {}
    toks = state.tokens
    for i, t in enumerate(toks):
        if t.type == "heading_open":
            inline = toks[i + 1]
            text = "".join(c.content for c in inline.children
                           if c.type in ("text", "code_inline"))
            t.attrSet("id", slugify(text, seen))


def render(md_text):
    md = (MarkdownIt("commonmark", {"html": True, "linkify": False})
          .enable(["table", "strikethrough"]))
    md.core.ruler.push("heading_ids", add_heading_ids)
    return md.render(md_text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", nargs="?", default="README.md")
    ap.add_argument("--out", help="write HTML here instead of a temp file")
    ap.add_argument("--no-open", action="store_true", help="don't launch a browser")
    args = ap.parse_args()

    src = Path(args.file).resolve()
    body = render(src.read_text(encoding="utf-8"))
    page = (f'<!doctype html><html><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<base href="{html.escape(src.parent.as_uri())}/">'
            f'<title>{html.escape(src.name)}</title><style>{CSS}</style></head>'
            f'<body><div class="page"><article>{body}</article></div></body></html>')

    if args.out:
        out = Path(args.out).resolve()
    else:
        fd = tempfile.NamedTemporaryFile("w", suffix=".html", prefix="show-md-",
                                         delete=False, encoding="utf-8")
        fd.close()
        out = Path(fd.name)
    out.write_text(page, encoding="utf-8")
    print(out)
    if not args.no_open:
        webbrowser.open(out.as_uri())


if __name__ == "__main__":
    main()
