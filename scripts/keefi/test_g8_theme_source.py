#!/usr/bin/env python3
"""D1 source contract: display colours and font sizes live in theme roles."""
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
# Explicit exceptions: colour definitions/style generation and bundled font roles.
# Qt transparent/NoPen/NoBrush express absence of paint, not a theme colour.
ALLOWED = {"KeefiTheme.cpp", "KeefiFonts.cpp"}
PATTERNS = {
    "hex_colour": re.compile(r"#[0-9a-fA-F]{3,8}\b"),
    "numeric_colour": re.compile(r"(?:QColor(?:\s+\w+)?\s*\(|QColor::from(?:Rgb|Rgba|Hsv|Hsl)\w*\s*\()\s*\d"),
    "named_colour": re.compile(r"Qt::(?:white|black|red|green|blue|cyan|magenta|yellow|gray|darkGray|lightGray|darkRed|darkGreen|darkBlue|darkCyan|darkMagenta|darkYellow)\b"),
    "font_size_call": re.compile(r"\.(?:setPixelSize|setPointSizeF?)\s*\("),
    "font_size_css": re.compile(r"(?:font-size\s*:\s*|font\s*:\s*(?:bold\s+)?)[0-9]+(?:\.[0-9]+)?(?:px|pt)"),
    "font_constructor_size": re.compile(r'QFont\s*\(\s*"[^"\n]+"\s*,\s*[0-9]'),
}


def violations(root=ROOT):
    root = Path(root)
    directory = root / "fincept-qt/src/keefi"
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("Missing KEEFI theme inspection directory")
    targets = sorted(path for path in directory.rglob("*")
                     if path.is_file() and not path.is_symlink()
                     and path.suffix in {".h", ".cpp"} and path.name not in ALLOWED)
    if not targets:
        raise ValueError("Empty KEEFI theme inspection target set")
    found = []
    for path in targets:
        for line_number, line in enumerate(path.read_text().splitlines(), 1):
            for kind, pattern in PATTERNS.items():
                if pattern.search(line):
                    found.append({"file": str(path.relative_to(root)), "line": line_number, "kind": kind})
    return found


if __name__ == "__main__":
    found = violations()
    result = {"allowed_definition_files": sorted(ALLOWED), "violations": found, "passed": not found}
    if "--json" in sys.argv:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        for item in found:
            print(f"{item['file']}:{item['line']}: {item['kind']}")
        print(f"D1 theme source contract: {len(found)} violations")
    raise SystemExit(bool(found))
