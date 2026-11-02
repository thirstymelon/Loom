#!/usr/bin/env python3
import html.parser
import pathlib
import re
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
docs = root / "docs"

class Page(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []
        self.ids = set()
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            if attrs["id"] in self.ids:
                raise AssertionError(f"duplicate id {attrs['id']}")
            self.ids.add(attrs["id"])
        key = "href" if tag in ("a", "link") else "src" if tag in ("script", "img") else None
        if key and attrs.get(key):
            self.links.append(attrs[key])

pages = sorted(docs.rglob("*.html"))
if not pages:
    raise SystemExit(f"no documentation pages found in {docs}")

page_data = {}
for page in pages:
    parser = Page()
    parser.feed(page.read_text(encoding="utf-8"))
    page_data[page] = parser

for page, parser in page_data.items():
    for href in parser.links:
        if href.startswith(("http://", "https://", "mailto:", "data:", "javascript:")):
            continue
        path_part, _, fragment = href.partition("#")
        target = (page.parent / path_part).resolve() if path_part else page
        if not target.exists():
            raise SystemExit(f"Broken documentation link/asset in {page.relative_to(root)}: {href}")
        if fragment and target.suffix == ".html":
            target_parser = page_data.get(target)
            if target_parser is None:
                target_parser = Page()
                target_parser.feed(target.read_text(encoding="utf-8"))
            if fragment not in target_parser.ids:
                raise SystemExit(f"Broken documentation anchor in {page.relative_to(root)}: {href}")

required_pages = [
    "index.html",
    "getting-started.html",
    "developer-overview.html",
    "embedded-rp2040.html",
    "reference.html",
    "tutorials/index.html",
    "tutorials/beginner-guide.html",
    "styles.css"
]
for required in required_pages:
    if not (docs / required).is_file():
        raise SystemExit(f"missing required documentation page or asset: {required}")

inline_scripts = []
for page in pages:
    page_text = page.read_text(encoding="utf-8")
    if re.search(r"<script\b[^>]*\bsrc\s*=", page_text, flags=re.IGNORECASE):
        raise SystemExit(f"documentation scripts must be inline in HTML: {page.relative_to(root)}")
    inline_scripts.extend(re.findall(r"<script\b[^>]*>(.*?)</script\s*>", page_text, flags=re.IGNORECASE | re.DOTALL))

search_script = "\n".join(inline_scripts)
for feature in ["ArrowDown", "ArrowUp", "Escape", "navigator.clipboard", "IDX", "localStorage"]:
    if feature not in search_script:
        raise SystemExit(f"documentation UI is missing expected script behavior: {feature}")

css = (docs / "styles.css").read_text(encoding="utf-8")
for feature in [":root", "--a:", ".btn", ".card", "table", "@media"]:
    if feature not in css:
        raise SystemExit(f"documentation styling is missing expected style: {feature}")

code_blocks = []
for p in pages:
    text = p.read_text(encoding="utf-8")
    for block in re.findall(r"<pre><code>(.*?)</code></pre>", text, flags=re.DOTALL):
        code = block.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
        if "Module " in code and "Block " in code:
            code_blocks.append((p.name, code))

if not code_blocks:
    raise SystemExit("no complete Loom example programs found in documentation")

sdk_dirs = []
for cand in [root / "sdk" / "targets" / "rp2040" / "bsp", root / "sdk" / "targets" / "rp2040", pathlib.Path("/usr/local/share/loom/sdk/targets/rp2040/bsp"), pathlib.Path("/usr/local/share/loom/sdk/targets/rp2040")]:
    if cand.is_dir():
        sdk_dirs.extend(["-I", str(cand.resolve())])

with tempfile.TemporaryDirectory(prefix="loom-doc-examples-") as temp:
    tmppath = pathlib.Path(temp)
    for idx, (fname, code) in enumerate(code_blocks):
        src = tmppath / f"example_{idx}.loom"
        src.write_text(code, encoding="utf-8")
        output = tmppath / f"example_{idx}.ll"
        cmd = [str(compiler), str(src), str(output)]
        if "Import GPIO" in code or "Import System" in code:
            cmd.extend(["--baremetal", *sdk_dirs])
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"documentation example in {fname} does not compile:\n{code}\n{result.stderr}")

print(f"documentation checks passed: {len(pages)} pages, internal links/assets, UI hooks, and {len(code_blocks)} compiled Loom examples")
