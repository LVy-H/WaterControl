"""Generate build-local credentials from environment variables; never print secrets."""

import json
import os
from pathlib import Path

Import("env")

names = {
    "BLYNK_TEMPLATE_ID": "WATER_BLYNK_TEMPLATE_ID",
    "BLYNK_TEMPLATE_NAME": "WATER_BLYNK_TEMPLATE_NAME",
    "BLYNK_AUTH_TOKEN": "WATER_BLYNK_AUTH_TOKEN",
    "WATER_WIFI_SSID": "WATER_WIFI_SSID",
    "WATER_WIFI_PASSWORD": "WATER_WIFI_PASSWORD",
}
missing = [name for name in names.values() if name not in os.environ]
if missing:
    raise RuntimeError("Cloud build requires environment variables: " + ", ".join(missing))
for name in ("WATER_BLYNK_TEMPLATE_ID", "WATER_BLYNK_TEMPLATE_NAME", "WATER_BLYNK_AUTH_TOKEN", "WATER_WIFI_SSID"):
    if not os.environ[name]:
        raise RuntimeError("Cloud build requires a nonempty " + name)
build_dir = Path(env.subst("$BUILD_DIR"))
build_dir.mkdir(parents=True, exist_ok=True)
header = build_dir / "water_cloud_credentials.hpp"
content = "#pragma once\n" + "".join(
    "#define " + macro + " " + json.dumps(os.environ[name], ensure_ascii=False) + "\n"
    for macro, name in names.items()
)
if not header.exists() or header.read_text(encoding="utf-8") != content:
    header.write_text(content, encoding="utf-8")
header.chmod(0o600)
env.Append(CPPPATH=[str(build_dir)])
