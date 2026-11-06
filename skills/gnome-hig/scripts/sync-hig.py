#!/usr/bin/env python3
"""Regenerate skills/gnome-hig/references/ from a developer.gnome.org-hig checkout.

The GNOME Human Interface Guidelines are written in reStructuredText and built
with Sphinx. This script converts each content page to GitHub-flavoured
Markdown with pandoc so agents can read the guidelines without a Sphinx build.

Usage:
    scripts/sync-hig.py [--hig-dir PATH] [--out PATH]

    --hig-dir  path to the upstream checkout
               (default: $HIG_DIR, then ~/Documents/Dev/developer.gnome.org-hig)
    --out      output directory (default: <skill>/references)

Requires pandoc on PATH. Upstream: https://gitlab.gnome.org/Teams/Websites/developer.gnome.org-hig
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

UPSTREAM_SITE = "https://developer.gnome.org/hig/"
UPSTREAM_REPO = "https://gitlab.gnome.org/Teams/Websites/developer.gnome.org-hig"

# Landing pages are tiled tables of contents; INDEX.md replaces them.
SKIP_PAGES = {
    "index",
    "guidelines",
    "patterns",
    "patterns/containers",
    "patterns/controls",
    "patterns/feedback",
    "patterns/nav",
    "reference",
}

# Section order and headings for INDEX.md.
SECTIONS = [
    ("", "Principles & resources"),
    ("guidelines", "Guidelines"),
    ("patterns/containers", "Patterns: containers"),
    ("patterns/nav", "Patterns: navigation"),
    ("patterns/controls", "Patterns: controls"),
    ("patterns/feedback", "Patterns: feedback"),
    ("reference", "Reference"),
]

HEADING_UNDERLINE = re.compile(r"^(=+|-+|~+|\^+|\"+|\++|\*+)\s*$")


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def load_palette(conf_py: Path) -> dict[str, tuple[int, int, int]]:
    """Pull hig_palette_colors_rgb out of conf.py without importing it."""
    tree = ast.parse(conf_py.read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "hig_palette_colors_rgb":
                    return ast.literal_eval(node.value)
    die(f"hig_palette_colors_rgb not found in {conf_py}")
    return {}


def github_slug(text: str) -> str:
    text = text.strip().lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"\s+", "-", text)


def page_title(lines: list[str]) -> str:
    for i in range(len(lines) - 1):
        if lines[i].strip() and HEADING_UNDERLINE.match(lines[i + 1]) and not lines[i].startswith(".."):
            return lines[i].strip()
    return ""


def collect_labels(source: Path, pages: list[str]) -> dict[str, tuple[str, str]]:
    """Map `.. _label:` targets to (page, heading slug)."""
    labels: dict[str, tuple[str, str]] = {}
    for page in pages:
        lines = (source / f"{page}.rst").read_text().splitlines()
        for i, line in enumerate(lines):
            m = re.match(r"^\.\. _([A-Za-z0-9-]+):\s*$", line)
            if not m:
                continue
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            heading = lines[j].strip() if j + 1 < len(lines) and HEADING_UNDERLINE.match(lines[j + 1]) else ""
            labels[m.group(1)] = (page, github_slug(heading))
    return labels


def resolve_doc(target: str, page: str) -> str:
    """Resolve a :doc: target (absolute or relative to the page) to a page key."""
    if target.startswith("/"):
        return target.lstrip("/")
    return os.path.normpath(os.path.join(os.path.dirname(page), target))


def rel_link(from_page: str, to_page: str, anchor: str = "") -> str:
    if to_page in SKIP_PAGES:
        # Landing pages are not converted; send readers to the matching INDEX.md section.
        section = next((heading for prefix, heading in SECTIONS if prefix == to_page), "")
        to_file, anchor = "INDEX", github_slug(section)
    else:
        to_file = to_page
    rel = os.path.relpath(f"{to_file}.md", os.path.dirname(from_page) or ".")
    return f"{rel}#{anchor}" if anchor else rel


def preprocess(text: str, page: str, titles: dict[str, str], labels: dict[str, tuple[str, str]],
               palette: dict[str, tuple[int, int, int]]) -> str:
    lines = text.splitlines()
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^(\s*)\.\. (image|toctree|cssclass)::\s*(.*)$", line)
        if m:
            indent, directive, arg = m.groups()
            # Swallow the directive's indented option/body block.
            block: list[str] = []
            j = i + 1
            while j < len(lines) and (not lines[j].strip() or len(lines[j]) - len(lines[j].lstrip()) > len(indent)):
                block.append(lines[j])
                j += 1
            if directive == "image" and "only-dark" not in " ".join(block):
                name = os.path.basename(arg.strip())
                out.append(f"{indent}Figure: `{name} <{UPSTREAM_SITE}_images/{name}>`__")
                out.append("")
            i = j
            continue
        out.append(line)
        i += 1
    text = "\n".join(out) + "\n"

    def doc_role(m: re.Match) -> str:
        label, target = m.group(1), m.group(2)
        key = resolve_doc(target, page)
        if key not in titles:
            print(f"warning: {page}: unresolved :doc: target {target!r}", file=sys.stderr)
            return label
        return f"`{label} <{rel_link(page, key)}>`__"

    def doc_role_bare(m: re.Match) -> str:
        key = resolve_doc(m.group(1), page)
        if key not in titles:
            print(f"warning: {page}: unresolved :doc: target {m.group(1)!r}", file=sys.stderr)
            return m.group(1)
        return f"`{titles[key]} <{rel_link(page, key)}>`__"

    def ref_role(m: re.Match) -> str:
        label, name = m.group(1), m.group(2)
        if name not in labels:
            print(f"warning: {page}: unresolved :ref: {name!r}", file=sys.stderr)
            return label
        target, anchor = labels[name]
        return f"`{label} <{rel_link(page, target, anchor)}>`__"

    def ref_role_bare(m: re.Match) -> str:
        name = m.group(1)
        if name not in labels:
            print(f"warning: {page}: unresolved :ref: {name!r}", file=sys.stderr)
            return name
        target, anchor = labels[name]
        return f"`{anchor.replace('-', ' ').title()} <{rel_link(page, target, anchor)}>`__"

    text = re.sub(r":doc:`([^`<]+?)\s*<([^>`]+)>`", doc_role, text)
    text = re.sub(r":doc:`([^`<]+)`", doc_role_bare, text)
    text = re.sub(r":ref:`([^`<]+?)\s*<([^>`]+)>`", ref_role, text)
    text = re.sub(r":ref:`([^`<]+)`", ref_role_bare, text)

    def palette_role(m: re.Match) -> str:
        kind, name = m.group(1), m.group(2)
        rgb = palette.get(name)
        if rgb is None:
            print(f"warning: {page}: unknown palette colour {name!r}", file=sys.stderr)
            return name
        if kind == "hex":
            return "``#%02x%02x%02x``" % rgb
        if kind == "rgb":
            return "``rgb(%d, %d, %d)``" % rgb
        return "—"

    text = re.sub(r":palette-(hex|rgb|svg):`([^`]+)`", palette_role, text)
    return text


def pandoc(rst: str) -> str:
    proc = subprocess.run(
        ["pandoc", "--from=rst", "--to=gfm", "--wrap=none"],
        input=rst, text=True, capture_output=True,
    )
    if proc.returncode != 0:
        die(f"pandoc failed: {proc.stderr}")
    if proc.stderr.strip():
        print(proc.stderr.rstrip(), file=sys.stderr)
    # RST default-role text (`like this`) comes out as a title-ref span; plain emphasis reads better.
    md = re.sub(r'<span class="title-ref">(.*?)</span>', r"*\1*", proc.stdout)
    # pandoc marks hard line breaks with trailing double spaces, which trips `git diff --check`.
    return "\n".join(line.rstrip() for line in md.splitlines()) + "\n"


def git(hig_dir: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(hig_dir), *args], text=True, capture_output=True, check=True).stdout.strip()


def main() -> None:
    skill_dir = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hig-dir", type=Path,
                    default=Path(os.environ.get("HIG_DIR", "~/Documents/Dev/developer.gnome.org-hig")).expanduser())
    ap.add_argument("--out", type=Path, default=skill_dir / "references")
    args = ap.parse_args()

    if shutil.which("pandoc") is None:
        die("pandoc not found on PATH")
    source = args.hig_dir / "source"
    if not (source / "conf.py").is_file():
        die(f"{args.hig_dir} does not look like a developer.gnome.org-hig checkout (no source/conf.py)")

    pages = sorted(
        str(p.relative_to(source).with_suffix("")) for p in source.rglob("*.rst")
        if not any(part.startswith("_") for part in p.relative_to(source).parts)
    )
    titles = {page: page_title((source / f"{page}.rst").read_text().splitlines()) for page in pages}
    labels = collect_labels(source, pages)
    palette = load_palette(source / "conf.py")

    commit = git(args.hig_dir, "rev-parse", "HEAD")
    date = git(args.hig_dir, "log", "-1", "--format=%cd", "--date=short")

    out_dir = args.out
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    written: list[str] = []
    for page in pages:
        if page in SKIP_PAGES:
            continue
        rst = (source / f"{page}.rst").read_text()
        md = pandoc(preprocess(rst, page, titles, labels, palette))
        header = (
            f"<!-- Generated by scripts/sync-hig.py from {UPSTREAM_REPO} "
            f"({commit[:12]}, {date}); source/{page}.rst. Do not edit by hand. -->\n"
            f"> Upstream: {UPSTREAM_SITE}{page}.html\n\n"
        )
        dest = out_dir / f"{page}.md"
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(header + md)
        written.append(page)

    index = [
        "# GNOME HIG reference index",
        "",
        f"Converted from {UPSTREAM_REPO} at commit `{commit[:12]}` ({date}).",
        f"Each page links back to its upstream URL under {UPSTREAM_SITE} for citations.",
        "Regenerate with `scripts/sync-hig.py`.",
        "",
    ]
    for prefix, heading in SECTIONS:
        members = [p for p in written if os.path.dirname(p) == prefix]
        if not members:
            continue
        index.append(f"## {heading}")
        index.append("")
        for p in members:
            index.append(f"- [{titles[p]}]({p}.md)")
        index.append("")
    (out_dir / "INDEX.md").write_text("\n".join(index))
    (out_dir / "UPSTREAM.json").write_text(json.dumps(
        {"repo": UPSTREAM_REPO, "site": UPSTREAM_SITE, "commit": commit, "date": date, "pages": written},
        indent=2) + "\n")
    print(f"wrote {len(written)} pages to {out_dir} (upstream {commit[:12]}, {date})")


if __name__ == "__main__":
    main()
