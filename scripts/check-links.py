#!/usr/bin/env python3
"""Verify that every relative Markdown link in the repository resolves.

Documentation that points at files which do not exist is worse than no
documentation: it sends a reader looking for something that was never written.
This runs in CI so a renamed file cannot silently break the learning path.

Fenced code blocks are skipped, because `[&](std::size_t i)` is a lambda, not a
link.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

LINK = re.compile(r"\[([^\]]*)\]\(([^)]+)\)")
FENCE = re.compile(r"^\s*```")


def strip_code_blocks(text: str) -> str:
    """Blank out fenced code blocks, preserving line count for error messages."""
    out, inside = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            inside = not inside
            out.append("")
        else:
            out.append("" if inside else line)
    return "\n".join(out)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    targets = sorted(
        set(root.glob("**/*.md")) - set(root.glob("build/**/*.md"))
        - set(root.glob("claude-code/**/*.md"))
    )

    broken: list[str] = []
    checked = 0

    for document in targets:
        body = strip_code_blocks(document.read_text(encoding="utf-8"))
        for match in LINK.finditer(body):
            target = match.group(2).strip()
            if target.startswith(("http://", "https://", "#", "mailto:")):
                continue
            target = target.split("#", 1)[0]
            if not target:
                continue
            checked += 1
            if not (document.parent / target).resolve().exists():
                rel = document.relative_to(root)
                broken.append(f"{rel}: [{match.group(1)}] -> {target}")

    print(f"checked {checked} relative links across {len(targets)} documents")
    if broken:
        print(f"\n{len(broken)} broken:", file=sys.stderr)
        for entry in sorted(set(broken)):
            print(f"  {entry}", file=sys.stderr)
        return 1
    print("all resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
