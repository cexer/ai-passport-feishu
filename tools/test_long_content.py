#!/usr/bin/env python3
"""Measure bounded-content behavior in the existing synthetic guest fixture."""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from urllib.request import Request, urlopen

parser = argparse.ArgumentParser()
parser.add_argument("--simulator", required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--peer-port", type=int, default=18030)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
home = root / "build/sim-long-home"
(home / "config").mkdir(parents=True, exist_ok=True)
output = root / "build/long-content-report" / datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
output.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PASSPORTSIM_HOME=str(home))
digest = hashlib.sha256((args.build / "FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image = root / "build/simulator-images" / digest
image.mkdir(parents=True, exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin", "FoloToy-AI-Passport.elf"):
    shutil.copyfile(args.build / name, image / name)
corpus_id = "feishu_long_" + digest[:16]
with (home / "config/corpus.toml").open("a", encoding="utf-8") as stream:
    stream.write("\n[" + corpus_id + "]\n")
    for kind, name in (("bin", "FoloToy-AI-Passport-full.bin"), ("elf", "FoloToy-AI-Passport.elf")):
        stream.write(kind + " = " + json.dumps((image / name).as_posix()) + "\n")
instance = None
index = 0
results = []


def cli(command, optional=False, **payload):
    global index
    index += 1
    if instance:
        payload.setdefault("instance", instance)
    request = output / f"{index:03d}-{command}-input.json"
    request.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    process = subprocess.run([args.simulator, command, "--json", "@" + str(request), "--output", "json"],
                             env=env, capture_output=True, encoding="utf-8", timeout=45)
    response = process.stdout.strip() or process.stderr.strip()
    (output / f"{index:03d}-{command}.json").write_text(response, encoding="utf-8")
    data = json.loads(response)
    if not optional and (process.returncode not in (0, 10) or data.get("code")):
        raise RuntimeError(f'{command}: {data.get("code")}: {data.get("message")}')
    return data


def control(**fields):
    fields.setdefault("mode", "normal")
    request = Request(f"http://127.0.0.1:{args.peer_port}/control", json.dumps(fields).encode(),
                      {"Content-Type": "application/json"})
    with urlopen(request, timeout=5) as response:
        assert json.load(response)["ok"]


def advance(ms=1000):
    cli("run", **{"for": f"{ms}ms", "wall_budget_ms": 30000})


def wait_any(*texts):
    for _ in range(15):
        advance()
        current = cli("ui")["text"]
        if any(text in current for text in texts):
            return current
    raise AssertionError("UI timeout: " + current)


def press(button="ok", action="click"):
    cli("input", button=button, action=action, then_run="350ms")


def screen(name):
    result = cli("screenshot", view="raw", save_as=name)
    relative = result["artifacts"][0]["path"]
    matches = list((home / "data/artifacts").rglob(Path(relative).name))
    shutil.copyfile(max(matches, key=lambda p: p.stat().st_mtime), output / (name + ".png"))


def fresh():
    global instance
    if instance:
        cli("stop")
        instance = None
    control(message_text="中文消息与回复测试", asr_text="模拟器语音回复")
    instance = cli("start", fw=corpus_id, boot="none", label="feishu-content-boundaries")["instance"]
    cli("wifi_ap", ssid="feishu-simulator-ap", channel=6, rssi=-40)
    cli("net_http", op="bridge", port=80, host_port=args.peer_port)
    wait_any("模拟测试群")


def max_glyphs(text, glyph):
    return max((part.count(glyph) for part in re.findall(r'label "(.*?)" \[', text)), default=0)


try:
    control(reset=True)
    for count in (4000, 10000, 40000):
        fresh()
        before = cli("inspect", optional=True, what=["heap"])
        control(message_text="长" * count)
        press()
        current = wait_any("长长长", "无法加载消息")
        outcome = "bounded-error" if "无法加载消息" in current else "truncated-preview"
        if outcome == "truncated-preview":
            press("up")
            press()
            current = wait_any("消息详情")
            # PassportSim's UI walker itself bounds label bytes. The C host
            # test verifies the exact model limit; this checks live rendering.
            assert max_glyphs(current, "长") >= 60
        after = cli("inspect", optional=True, what=["heap"])
        screen(f"receive-{count * 3}-bytes")
        # Recover on the same guest, with the existing credentials and queues.
        control(message_text="中文消息与回复测试")
        press("ok", "hold")
        press("ok", "hold")
        wait_any("飞书 | 会话")
        press()
        wait_any("中文消息与回复测试")
        results.append({"receive_text_bytes": count * 3, "outcome": outcome,
                        "same_guest_recovered": True,
                        "heap_before": before, "heap_after": after})
    for count in (1000, 4000):
        fresh()
        control(asr_text="语" * count)
        press()
        wait_any("中文消息与回复测试")
        press("up")
        press()
        press()
        cli("mic_set", kind="tone", hz=440, amplitude=16000, duration_ms=10000, fs=16000, channels=1)
        press()
        advance(800)
        # An oversized first ASR response can fail before the user stops.
        # Do not turn that error-page observation into a new message click.
        if "语音识别失败" not in cli("ui")["text"]:
            press()
        current = wait_any("语语语", "语音识别失败")
        screen(f"asr-{count * 3}-bytes")
        if count == 1000:
            assert max_glyphs(current, "语") >= 60
            press()
            wait_any("回复已发送")
            with urlopen(f"http://127.0.0.1:{args.peer_port}/events", timeout=5) as response:
                events = json.load(response)
            sends = [event for event in events["events"] if event.get("send")]
            assert sends[-1]["text_bytes"] == 510
            outcome = "silently-truncated-to-510-bytes-and-sent"
        else:
            assert "语音识别失败" in current
            outcome = "bounded-error"
        results.append({"asr_text_bytes": count * 3, "outcome": outcome})
    summary = {"status": "PASS", "scope": "Emulator, not hardware; synthetic HTTP boundary cases",
               "image_sha256": digest, "results": results}
except Exception as error:
    summary = {"status": "FAIL", "error": str(error), "results": results}
    raise
finally:
    if instance:
        cli("stop")
    summary["report_directory"] = str(output.relative_to(root))
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (root / "build/long-content-report/summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in summary.items() if key != "results"}, ensure_ascii=False))
