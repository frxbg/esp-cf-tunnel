"""Embed small, dependency-free browser assets in flash (no runtime filesystem)."""
from pathlib import Path
import sys

source, target = map(Path, sys.argv[1:])
assets = [("index.html", "/", "text/html; charset=utf-8"),
          ("style.css", "/style.css", "text/css; charset=utf-8"),
          ("app.js", "/app.js", "text/javascript; charset=utf-8"),
          ("sha256.js", "/sha256.js", "text/javascript; charset=utf-8"),
          ("favicon.svg", "/favicon.svg", "image/svg+xml")]
lines = ['#include "monitor.h"']
entries = []
for i, (name, uri, mime) in enumerate(assets):
    data = (source / name).read_bytes()
    if len(data) > 49152:
        raise SystemExit(f"Asset exceeds the 48 KiB budget: {name}")
    lines.append(f"static const uint8_t asset_{i}[] = {{")
    lines.extend(",".join(str(b) for b in data[n:n+24]) + "," for n in range(0, len(data), 24))
    lines.append("};")
    entries.append(f'{{"{uri}","{mime}",asset_{i},sizeof(asset_{i})}}')
lines.append("const monitor_asset monitor_assets[] = {" + ",".join(entries) + "};")
lines.append("const size_t monitor_asset_count = sizeof(monitor_assets)/sizeof(monitor_assets[0]);")
target.write_text("\n".join(lines) + "\n", encoding="utf-8")
