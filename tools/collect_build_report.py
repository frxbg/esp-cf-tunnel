"""Report compiled smoke binaries only; never infer runtime/tunnel overhead."""
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
rows = []
for build in sorted(root.glob("build-idf-*")):
    binary = build / "cf_protocol_smoke.bin"
    if not binary.exists():
        continue
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    sdk = (build / "sdkconfig").read_text(encoding="utf-8")
    data = binary.read_bytes()
    rows.append({"build": build.name, "target": description["target"],
                 "idf_version": description["git_revision"], "binary_bytes": len(data),
                 "sha256": hashlib.sha256(data).hexdigest(),
                 "psram_enabled": "\nCONFIG_SPIRAM=y\n" in sdk,
                 "transport_enabled": "\nCONFIG_CF_TUNNEL_ESP_TRANSPORT=y\n" in sdk,
                 "hardware_tested": False, "live_cloudflare_tested": False})
report = {"scope": "protocol_smoke with transport linked; no live network or runtime memory baseline", "builds": rows}
content = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
if len(sys.argv) == 2:
    pathlib.Path(sys.argv[1]).write_text(content, encoding="utf-8", newline="\n")
else:
    print(content, end="")
