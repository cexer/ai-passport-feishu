"""Run an isolated synthetic storage probe; never opens a device port."""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--simulator", required=True)
p.add_argument("--build", type=Path, required=True)
p.add_argument("--steps", type=int, default=80)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
out = root / "build/storage-probe-report" / stamp
out.mkdir(parents=True)
home = root / "build/storage-probe-sim-home"
(home / "config").mkdir(parents=True, exist_ok=True)
digest = hashlib.sha256((a.build / "FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image = root / "build/simulator-images" / digest
image.mkdir(parents=True, exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin", "FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build / name, image / name)
corpus = "storage_" + digest[:16]
config = home / "config/corpus.toml"
text = config.read_text() if config.exists() else ""
if "[" + corpus + "]" not in text:
    with config.open("a", encoding="utf-8") as f:
        f.write("\n[" + corpus + "]\n")
        for kind, name in (("bin", "FoloToy-AI-Passport-full.bin"), ("elf", "FoloToy-AI-Passport.elf")):
            f.write(kind + " = " + json.dumps((image / name).as_posix()) + "\n")
env = dict(os.environ, PASSPORTSIM_HOME=str(home))
instance = None
index = 0


def cli(command, **payload):
    global index
    index += 1
    if instance:
        payload.setdefault("instance", instance)
    request = out / f"{index:03d}-{command}-input.json"
    request.write_text(json.dumps(payload), encoding="utf-8")
    r = subprocess.run([a.simulator, command, "--json", "@" + str(request), "--output", "json"],
                       env=env, capture_output=True, encoding="utf-8", timeout=45)
    data = json.loads(r.stdout or r.stderr)
    # Never retain raw identity-bearing console output.
    if command != "serial":
        (out / f"{index:03d}-{command}.json").write_text(json.dumps(data, indent=2), encoding="utf-8")
    if r.returncode not in (0, 10) or data.get("code"):
        raise RuntimeError(f"{command}: {data.get('code')}: {data.get('message')}")
    return data


lines = []
cursor = 0
try:
    instance = cli("start", fw=corpus, boot="none", label="storage-qualification")["instance"]
    for step in range(a.steps):
        cli("run", **{"for": "1s", "wall_budget_ms": 30000})
        data = cli("serial", op="read", cursor=cursor, max_bytes=65536)
        cursor = data["next_cursor"]
        serial = data["serial"]
        shaped = serial.get("head", []) + serial.get("tail", [])
        for line in shaped:
            if not isinstance(line, str):
                raise TypeError("Unexpected serial line schema: " + str(type(line)))
            if "STORE_PROBE" in line or "STORAGE_PROBE" in line or "FLASHDB_PROBE" in line:
                lines.append(line)
                print(line, flush=True)
        if any("STORAGE_PROBE_DONE" in s for s in lines):
            break
    else:
        raise AssertionError("Probe did not complete within virtual-time budget")
    (out / "summary.json").write_text(json.dumps({"image_sha256": digest, "lines": lines}, indent=2), encoding="utf-8")
    if any(" rc=" in s and " rc=0 " not in s for s in lines):
        raise AssertionError("Storage workload reported a failure")
    print(out, flush=True)
finally:
    (out / "probe-lines.txt").write_text("\n".join(lines), encoding="utf-8")
    if instance:
        cli("stop")
