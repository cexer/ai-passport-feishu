#!/usr/bin/env python3
"""Exercise real guest UI/workers against simulator_peer.py, never hardware."""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
from urllib.request import Request, urlopen

parser = argparse.ArgumentParser()
parser.add_argument("--simulator", required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--peer-port", type=int, default=18027)
parser.add_argument("--only-auth", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
report_base = root / "build" / "simulator-report"
report_dir = report_base / datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
report_dir.mkdir(parents=True, exist_ok=True)
home = root / "build" / "sim-home"
config = home / "config"
config.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PASSPORTSIM_HOME=str(home))
digest = hashlib.sha256((args.build / "FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image_dir = root / "build" / "simulator-images" / digest
image_dir.mkdir(parents=True, exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin", "FoloToy-AI-Passport.elf"):
    shutil.copyfile(args.build / name, image_dir / name)
corpus_id = "feishu_" + digest[:16]
corpus = "[" + corpus_id + "]\n" + "\n".join(
    f'{kind} = {json.dumps((image_dir / name).as_posix())}' for kind, name in (
        ("bin", "FoloToy-AI-Passport-full.bin"), ("elf", "FoloToy-AI-Passport.elf")))
(config / "corpus.toml").write_text(corpus + "\n", encoding="utf-8")
index = 0
instance = None
checks = []


def call(command, **payload):
    global index
    index += 1
    if instance is not None:
        payload.setdefault("instance", instance)
    request = report_dir / f"{index:03d}-{command}-input.json"
    request.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    process = subprocess.run([args.simulator, command, "--json", "@" + str(request),
                              "--output", "json"], env=env, capture_output=True,
                             encoding="utf-8", timeout=45)
    response = process.stdout.strip() or process.stderr.strip()
    (report_dir / f"{index:03d}-{command}.json").write_text(response, encoding="utf-8")
    result = json.loads(response)
    # CLI exit 10 is documented PASS_WITH_CAVEATS (e.g. unmodeled ADC/RF).
    if process.returncode not in (0, 10) or result.get("code"):
        raise RuntimeError(f'{command}: {result.get("code")}: {result.get("message")}')
    return result


def advance(milliseconds):
    return call("run", **{"for": f"{milliseconds}ms", "wall_budget_ms": 30000})


def view():
    return call("ui")["text"]


def wait_text(text, rounds=15, refresh_air=False):
    for _ in range(rounds):
        if refresh_air:
            call("wifi_ap", ssid="feishu-simulator-ap", channel=6, rssi=-40)
            bridge = call("net_http", op="status")["lan"]["bridge"]
            if not bridge["attached"]:
                call("net_http", op="bridge", port=80, host_port=args.peer_port)
        advance(1000)
        current = view()
        if text in current:
            return current
    raise AssertionError(f"UI did not show {text!r}; last UI: {current}")


def press(button="ok", action="click"):
    call("input", button=button, action=action, then_run="350ms")


def screenshot(name):
    result = call("screenshot", view="raw", save_as=name)
    relative = result["artifacts"][0]["path"]
    matches = list((home / "data" / "artifacts").rglob(Path(relative).name))
    assert matches, relative
    shutil.copyfile(max(matches, key=lambda p: p.stat().st_mtime), report_dir / (name + ".png"))


def control(mode, reset=False):
    request = Request(f"http://127.0.0.1:{args.peer_port}/control",
                      json.dumps({"mode": mode, "reset": reset}).encode(),
                      {"Content-Type": "application/json"})
    with urlopen(request, timeout=5) as response:
        assert json.load(response)["ok"]


def peer_events():
    with urlopen(f"http://127.0.0.1:{args.peer_port}/events", timeout=5) as response:
        return json.load(response)


def fresh_instance():
    global instance
    if instance is not None:
        call("stop")
        instance = None
    instance = call("start", fw=corpus_id, boot="none", label="feishu-integration")["instance"]
    call("wifi_ap", ssid="feishu-simulator-ap", channel=6, rssi=-40)
    call("net_http", op="bridge", port=80, host_port=args.peer_port)


try:
    control("normal", reset=True)
    assert peer_events()["sends"] == 0, "start a fresh peer for each test run"
    if not args.only_auth:
        fresh_instance()
        wait_text("模拟测试群")
        screenshot("01-chats")
        checks.append("cached sign-in and Chinese conversation list")
        press()
        wait_text("中文消息与回复测试")
        screenshot("02-messages")
        press("up")
        press()
        wait_text("中文消息与回复测试")
        screenshot("02a-message-detail")
        press()
        advance(300)
        screenshot("02b-reply-ready")
        call("mic_set", kind="tone", hz=440, amplitude=16000, duration_ms=20000, fs=16000, channels=1)
        press()
        advance(300)
        screenshot("02c-recording")
        advance(800)
        press()
        wait_text("模拟器语音回复")
        screenshot("03-review")
        assert peer_events()["sends"] == 0
        press("down")
        wait_text("中文消息与回复测试")
        assert peer_events()["sends"] == 0
        checks.append("streaming microphone, ASR review and cancel without send")
        # Select the original message again and confirm one reply.
        press("up")
        press()
        press()
        press()
        advance(800)
        press()
        wait_text("模拟器语音回复")
        press()
        wait_text("回复已发送")
        assert peer_events()["sends"] == 1
        screenshot("04-sent")
        checks.append("explicit confirmation submits exactly one reply")
        press("ok", "hold")
        wait_text("飞书 | 会话")
        press("ok", "hold")
        wait_text("重新配置 Wi-Fi")
        screenshot("04a-settings")

        for mode in ("malformed", "permission", "server_error"):
            before = peer_events()
            control(mode)
            fresh_instance()
            wait_text("暂时无法连接飞书")
            screenshot("05-" + mode)
            after = peer_events()
            assert sum(e["path"].endswith("/device_authorization") for e in after["events"]) == sum(
                e["path"].endswith("/device_authorization") for e in before["events"])
            control("normal")
            wait_text("模拟测试群", rounds=20)
            checks.append(mode + " preserves credentials and automatically recovers")

    # A malformed or nested OAuth error must never revoke a real binding.
    for mode in ("oauth_truncated", "oauth_nested", "oauth_partial"):
        before = peer_events()
        control(mode)
        fresh_instance()
        wait_text("暂时无法连接飞书")
        screenshot("oauth-" + mode)
        after = peer_events()
        assert any(e["path"].endswith("/token") for e in after["events"][len(before["events"]):]), mode
        assert sum(e["path"].endswith("/device_authorization") for e in after["events"]) == sum(
            e["path"].endswith("/device_authorization") for e in before["events"]), mode
        control("normal")
        wait_text("模拟测试群", rounds=20)
        checks.append(mode + " preserves binding and normal service recovers")

    control("revoked")
    fresh_instance()
    advance(2000)
    # A guest-triggered restart also resets the emulator's virtual air.
    call("wifi_ap", ssid="feishu-simulator-ap", channel=6, rssi=-40)
    wait_text("绑定飞书", rounds=25, refresh_air=True)
    screenshot("06-reauthorize")
    control("normal")
    wait_text("模拟测试群")
    checks.append("explicit revoked grant reauthorizes the same owner app")
    events = peer_events()
    if not args.only_auth:
        assert events["sends"] == 1
        assert any(e.get("nonzero_pcm") for e in events["events"])
    (report_dir / "peer-events.json").write_text(json.dumps(events, ensure_ascii=False, indent=2), encoding="utf-8")
    summary = {"status": "PASS", "image_sha256": digest, "checks": checks,
               "scope": "Emulator (not hardware); synthetic HTTP peer and Wi-Fi fixture"}
except Exception as error:
    summary = {"status": "FAIL", "image_sha256": digest, "checks": checks,
               "error": str(error)}
    if instance:
        try:
            request=report_dir/"fault-console-input.json"
            request.write_text(json.dumps({"instance":instance,"op":"read","cursor":0,"max_bytes":65536}),encoding="utf-8")
            raw=subprocess.run([args.simulator,"serial","--json","@"+str(request),"--output","json"],
                               env=env,capture_output=True,encoding="utf-8",timeout=15)
            shaped=json.loads(raw.stdout)["serial"]
            lines=shaped["head"]+shaped["tail"]
            lines=[s for s in lines if "PROBE_PANIC" in s or "assert failed" in s or "stack overflow" in s]
            (report_dir/"fault-console.txt").write_text("\n".join(lines),encoding="utf-8")
            print("\n".join(lines),flush=True)
        except Exception as diagnostic_error:
            print("Fault console unavailable: "+str(diagnostic_error),flush=True)
    raise
finally:
    if instance is not None:
        call("stop")
    summary["report_directory"] = str(report_dir.relative_to(root))
    (report_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (report_base / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))
