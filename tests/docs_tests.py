#!/usr/bin/env python3
import html.parser
import ast
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
    "rp2040-reference.html",
    "reference.html",
    "tutorials/index.html",
    "tutorials/beginner-guide.html",
    "styles.css"
]
for required in required_pages:
    if not (docs / required).is_file():
        raise SystemExit(f"missing required documentation page or asset: {required}")

api_text = (docs / "rp2040-reference.html").read_text(encoding="utf-8")
if re.search(r"<h2\b(?![^>]*\bid=)[^>]*>", api_text, flags=re.IGNORECASE):
    raise SystemExit("RP2040 BSP section index requires an id on every section heading")
if re.search(r"(?:RP2040\.)?Check\(\s*(?:RP2040\.)?Check\(", api_text):
    raise SystemExit("RP2040 BSP examples contain a duplicated Check wrapper")
fixture_tree = ast.parse((root / "tests/rp2040_peripherals_test.py").read_text(encoding="utf-8"))
fixture_assignment = next(
    n for n in fixture_tree.body
    if isinstance(n, ast.Assign)
    and any(isinstance(t, ast.Name) and t.id == "modules" for t in n.targets)
)
fixtures = {name.lower(): code.strip() for name, code in ast.literal_eval(fixture_assignment.value)}
fixtures["rp2040"] = fixtures["rp2040_umbrella"]
for module in sorted((root / "sdk/targets/rp2040/bsp").glob("*.loom")):
    if module.stem.endswith("_internal") or not re.fullmatch(r"[A-Za-z0-9_]+", module.stem):
        continue
    source = module.read_text(encoding="utf-8")
    section_match = re.search(
        r'<section class="bsp-module" id="mod-' + re.escape(module.stem.lower()) + r'">(.*?)</section>',
        api_text,
        flags=re.DOTALL,
    )
    if not section_match:
        raise SystemExit(f"RP2040 reference is missing module section: {module.stem}")
    section = section_match.group(1)
    fixture = fixtures.get(module.stem.lower())
    if fixture is None:
        raise SystemExit(f"missing RP2040 compile fixture for module: {module.stem}")
    full_example = re.search(r"<pre><code>(.*?)</code></pre>", section, flags=re.DOTALL)
    if not full_example or html.unescape(full_example.group(1)).strip() != fixture:
        raise SystemExit(f"RP2040 walkthrough does not match its module fixture: {module.stem}")
    fixture_lines = fixture.splitlines()
    for line_index, line in enumerate(fixture_lines):
        result_name = "Ok" if re.match(r"^\s*Ok\s*=", line) else "Status" if module.stem.lower() in {"i2c", "spi"} and re.match(r"^\s*Status\s*=", line) else None
        if result_name:
            next_line = fixture_lines[line_index + 1].strip() if line_index + 1 < len(fixture_lines) else ""
            if not next_line.startswith(f"If ({result_name} "):
                raise SystemExit(f"unchecked result in {module.stem} walkthrough: {line.strip()}")
    for function in re.findall(r"^Block\s+(\w+)(?:\s+Takes)?", source, flags=re.MULTILINE):
        if "_Internal_" in function:
            continue
        if function != "Global" and function + "(" not in api_text:
            raise SystemExit(f"RP2040 reference is missing public function: {module.stem}.{function}")
        if function == "Global":
            continue
        entry = re.search(
            r"<summary><code>" + re.escape(function) + r"\(.*?</details>",
            api_text,
            flags=re.DOTALL,
        )
        sample = re.search(r'<pre class="raw"><code>(.*?)</code></pre>', entry.group(0), flags=re.DOTALL) if entry else None
        if not sample or function + "(" not in html.unescape(sample.group(1)):
            raise SystemExit(f"RP2040 reference is missing a function example: {module.stem}.{function}")
        if function == "Global":
            continue
        block_match = re.search(
            r"^Block\s+" + re.escape(function) + r"(?:\s+Takes)?\s*$(.*?)(?=^End\s+" + re.escape(function) + r"\s*$)",
            source,
            flags=re.MULTILINE | re.DOTALL,
        )
        body = block_match.group(1) if block_match else ""
        header = re.split(r"^\s*(?:Declares|Implements)\s*$", body, maxsplit=1, flags=re.MULTILINE)[0]
        args = []
        result_type = None
        in_produces = False
        for line in header.splitlines():
            produced = re.match(r"^\s*Produces\s+(.+?)\s*$", line)
            if produced:
                result_type = produced.group(1)
                in_produces = True
                continue
            parameter = re.match(r"^\s*(\w+)\s*:\s*(.+?)\s*$", line)
            if parameter and not in_produces:
                args.append(f"{parameter.group(1)}: {parameter.group(2)}")
        expected_signature = function + "(" + ", ".join(args) + ")"
        if result_type:
            expected_signature += " → " + result_type
        if expected_signature not in html.unescape(section):
            raise SystemExit(f"RP2040 reference signature is stale: {module.stem}.{function}; expected {expected_signature}")

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
for feature in [":root", "--green:", ".hero", ".btn", ".card", "table", "@media"]:
    if feature not in css:
        raise SystemExit(f"documentation styling is missing expected style: {feature}")

code_blocks = []
for p in pages:
    text = p.read_text(encoding="utf-8")
    for block in re.findall(r"<pre(?:\s[^>]*)?><code>(.*?)</code></pre>", text, flags=re.DOTALL):
        code = block.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
        lines = code.splitlines()
        for line_index, line in enumerate(lines):
            result_name = "Ok" if re.match(r"^\s*Ok\s*=", line) else "Status" if p.name == "rp2040-reference.html" and re.match(r"^\s*Status\s*=", line) else None
            if result_name:
                next_index = line_index + 1
                while next_index < len(lines) and not lines[next_index].strip():
                    next_index += 1
                if next_index >= len(lines) or not lines[next_index].strip().startswith(f"If ({result_name} "):
                    raise SystemExit(f"unchecked result in {p.relative_to(root)}: {line.strip()}")
        if "Module " in code and "Block " in code:
            code_blocks.append((p.name, code))

if not code_blocks:
    raise SystemExit("no complete Loom example programs found in documentation")

sdk_dirs = []
for cand in [root / "sdk" / "targets" / "rp2040" / "bsp", root / "sdk" / "targets" / "rp2040", pathlib.Path("/usr/local/share/loom/sdk/targets/rp2040/bsp"), pathlib.Path("/usr/local/share/loom/sdk/targets/rp2040")]:
    if cand.is_dir():
        sdk_dirs.extend(["-I", str(cand.resolve())])
for library_dir in [root / "libraries" / "ssd1306", root / "libraries" / "mpu6050"]:
    if library_dir.is_dir():
        sdk_dirs.extend(["-I", str(library_dir.resolve())])

with tempfile.TemporaryDirectory(prefix="loom-doc-examples-") as temp:
    tmppath = pathlib.Path(temp)
    for idx, (fname, code) in enumerate(code_blocks):
        src = tmppath / f"example_{idx}.loom"
        src.write_text(code, encoding="utf-8")
        output = tmppath / f"example_{idx}.ll"
        cmd = [str(compiler), str(src), str(output)]
        if any(f"Import {module}" in code for module in ("GPIO", "System", "RP2040", "I2C", "SPI", "SSD1306", "MPU6050")) or fname == "rp2040-reference.html":
            cmd.extend(["--baremetal", *sdk_dirs])
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"documentation example in {fname} does not compile:\n{code}\n{result.stderr}")

print(f"documentation checks passed: {len(pages)} pages, internal links/assets, UI hooks, and {len(code_blocks)} compiled Loom examples")
