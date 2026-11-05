"""Retain matching physical benchmark results and debug artifacts before rebuilding."""
import hashlib
import json
import shutil
from pathlib import Path

root=Path(__file__).resolve().parents[1]
build=root/"build/media-probe"
result=root/"build/device-checkpoint/media-probe.json"
summary=json.loads(result.read_text(encoding="utf-8"))
digest=hashlib.sha256((build/"FoloToy-AI-Passport.bin").read_bytes()).hexdigest()
assert digest==summary["probe_app_sha256"],"Result and application do not match"
out=root/"build/device-probe-report"/digest
out.mkdir(parents=True,exist_ok=False)
for name in ("FoloToy-AI-Passport.bin","FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf","FoloToy-AI-Passport.map"):
    shutil.copyfile(build/name,out/name)
for name in ("media-probe.json","media-probe-sanitized.log"):
    shutil.copyfile(root/"build/device-checkpoint"/name,out/name)
stage=root/"build/media-probe-source"
shutil.copyfile(stage/"sdkconfig",out/"sdkconfig")
files={}
for directory in ("main","components"):
    for file in (stage/directory).rglob("*"):
        if file.is_file():files[file.relative_to(stage).as_posix()]=hashlib.sha256(file.read_bytes()).hexdigest()
(out/"source-manifest.json").write_text(json.dumps(files,indent=2),encoding="utf-8")
print(out)
