"""Uploads the build's ELF to Vittil, so the crashes of this build are decoded.

Run by the build after the link (CMakeLists.txt), with the ELF, the build's
sdkconfig.json and a stamp file:

    python elf_upload.py <app.elf> <config/sdkconfig.json> <stamp>

The project key and the server come from sdkconfig.json; the key is never
printed. On success it prints one line and writes the stamp. Anything else (no
network, the server down, a refused key) prints nothing and removes the stamp,
so the next build tries again; it never fails the build.
"""

import gzip
import hashlib
import json
import os
import ssl
import sys
import urllib.request

TIMEOUT_S = 10


def upload(elf_path, config_path, stamp_path):
    with open(config_path, encoding="utf-8") as f:
        config = json.load(f)
    key = config.get("VITTIL_PROJECT_KEY") or ""
    server = (config.get("VITTIL_SERVER_URL") or "").rstrip("/")
    if not key or not server.startswith(("https://", "http://")):
        return False
    with open(elf_path, "rb") as f:
        elf = f.read()
    # the device reports the start of the ELF's SHA-256 (esp_app_desc), this many characters
    build_id = hashlib.sha256(elf).hexdigest()[:int(config.get("APP_RETRIEVE_LEN_ELF_SHA") or 9)]
    req = urllib.request.Request(
        server + "/api/firmware/" + build_id + "/elf",
        data=gzip.compress(elf, 6),
        method="PUT",
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/octet-stream",
            "Content-Encoding": "gzip",
        },
    )
    context = None
    if server.startswith("https://"):
        try:
            import certifi  # some Pythons (macOS) have no CA certificates of their own
            context = ssl.create_default_context(cafile=certifi.where())
        except ImportError:
            context = ssl.create_default_context()
    with urllib.request.urlopen(req, timeout=TIMEOUT_S, context=context) as resp:
        answer = json.loads(resp.read())
    # the server names the build it stored; any other 2xx (a proxy's page) is not an upload
    if not isinstance(answer, dict) or answer.get("build_id") != build_id:
        return False
    with open(stamp_path, "w", encoding="utf-8") as f:
        f.write(build_id + "\n")
    print("vittil: ELF " + build_id + " uploaded")
    return True


def main(argv):
    try:
        done = upload(argv[1], argv[2], argv[3])
    except Exception:
        done = False
    if not done:
        # a missing stamp makes the build run this step again; an old one would not
        try:
            os.remove(argv[3])
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
