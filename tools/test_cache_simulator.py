"""Cache-first UI, persistent offline boot and refresh isolation on a fake peer."""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
from urllib.request import Request,urlopen

p=argparse.ArgumentParser()
p.add_argument("--simulator",required=True)
p.add_argument("--build",required=True,type=Path)
p.add_argument("--peer-port",type=int,default=18032)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
stamp=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
out=root/"build/cache-simulator-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-cache-home"
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="cache_"+digest[:16]
(home/"config/corpus.toml").write_text("["+corpus+"]\n"+"\n".join(
    kind+" = "+json.dumps((image/name).as_posix()) for kind,name in (
        ("bin","FoloToy-AI-Passport-full.bin"),("elf","FoloToy-AI-Passport.elf"))),encoding="utf-8")
env=dict(os.environ,PASSPORTSIM_HOME=str(home))
instance=None
index=0
checks=[]


def cli(command,**payload):
    global index
    index+=1
    if instance: payload.setdefault("instance",instance)
    request=out/f"{index:03d}-{command}-input.json"
    request.write_text(json.dumps(payload,ensure_ascii=False),encoding="utf-8")
    r=subprocess.run([a.simulator,command,"--json","@"+str(request),"--output","json"],
                     env=env,capture_output=True,encoding="utf-8",timeout=45)
    d=json.loads(r.stdout or r.stderr)
    (out/f"{index:03d}-{command}.json").write_text(json.dumps(d,ensure_ascii=False,indent=2),encoding="utf-8")
    if r.returncode not in (0,10) or d.get("code"):
        raise RuntimeError(f"{command}: {d.get('code')}: {d.get('message')}")
    return d


def control(**fields):
    fields.setdefault("mode","normal")
    req=Request(f"http://127.0.0.1:{a.peer_port}/control",json.dumps(fields).encode(),{"Content-Type":"application/json"})
    with urlopen(req,timeout=5) as r: assert json.load(r)["ok"]


def events():
    with urlopen(f"http://127.0.0.1:{a.peer_port}/events",timeout=5) as r: return json.load(r)


def advance(ms=1000):
    result=None
    while ms:
        step=min(ms,1000)
        result=cli("run",**{"for":f"{step}ms","wall_budget_ms":30000})
        ms-=step
    return result
def press(button="ok",action="click",**kw): return cli("input",button=button,action=action,then_run="350ms",**kw)
def view(): return cli("ui")
def wait(text,rounds=15,step=500):
    current=None
    for _ in range(rounds):
        current=view()
        if text in current["text"]: return current
        advance(step)
    raise AssertionError("UI timeout for "+text+": "+current["text"])
def screen(name):
    r=cli("screenshot",view="raw",save_as=name)
    filename=Path(r["artifacts"][0]["path"]).name
    found=max((home/"data/artifacts").rglob(filename),key=lambda x:x.stat().st_mtime)
    shutil.copyfile(found,out/(name+".png"))
def network():
    cli("wifi_ap",ssid="feishu-simulator-ap",channel=6,rssi=-40)
    cli("net_http",op="bridge",port=80,host_port=a.peer_port)
def tick(d): return d.get("vt_us",d.get("receipt",{}).get("vt_us",0))


try:
    control(reset=True)
    instance=cli("start",fw=corpus,boot="none",label="cache-acceptance")["instance"]
    network()
    wait("模拟测试群")
    press()
    wait("中文消息与回复测试")
    screen("01-populated")
    cli("inspect",what=["heap","tasks"])
    press("ok","hold")
    wait("飞书 | 会话")
    control(mode="server_error")
    before=tick(view())
    press()
    after=wait("中文消息与回复测试",rounds=4,step=250)
    assert tick(after)-before<2000000,"cache hit waited for a network timeout"
    screen("02-cache-with-server-error")
    checks.append({"case":"warm cache during HTTP 503","latency_us":tick(after)-before})
    # Same flash, fresh boot; no AP or bridge is restored until after cache reads.
    press("reset",reset_kind="usb_rts")
    wait("模拟测试群",rounds=10,step=500)
    screen("03-offline-cached-chats")
    before=tick(view())
    press()
    after=wait("中文消息与回复测试",rounds=4,step=250)
    assert tick(after)-before<2000000,"offline cache waited for Wi-Fi"
    screen("04-offline-cached-messages")
    checks.append({"case":"persistent cache after reboot without AP","message_latency_us":tick(after)-before})
    press()
    wait("发送消息")
    press()
    wait("离线缓存可读",rounds=4,step=250)
    assert not any(e["path"].endswith("/stream_recognize") for e in events()["events"])
    press("ok","hold")
    wait("中文消息与回复测试")
    checks.append({"case":"offline recording is rejected immediately and never queued for later"})
    control(message_text="联网恢复后的消息")
    network()
    wait("联网恢复后的消息",rounds=45,step=1000)
    screen("05-reconnected")
    checks.append({"case":"same guest resumes refresh after network returns"})
    # Hold one actual request until the reader enters the detail screen.
    control(message_text="后台刷新产生的新内容",hold_messages=True)
    for _ in range(12):
        advance(1000)
        if any(e.get("held") for e in events()["events"]): break
    else: raise AssertionError("no held refresh request")
    press("up")
    press()
    wait("消息详情")
    assert "联网恢复后的消息" in view()["text"]
    control(message_text="后台刷新产生的新内容",hold_messages=False)
    advance(1500)
    current=view()["text"]
    assert "消息详情" in current and "联网恢复后的消息" in current
    assert "后台刷新产生的新内容" not in current
    screen("06-reading-version-preserved")
    checks.append({"case":"in-flight refresh preserves the active detail version"})
    press("ok","hold")
    wait("后台刷新产生的新内容",rounds=25,step=1000)
    cli("inspect",what=["heap","tasks"])
    assert events()["sends"]==0
    summary={"status":"PASS","image_sha256":digest,"checks":checks,
             "scope":"Emulator only; synthetic HTTP; no real OAuth/TLS/hardware acceptance"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    control(hold_messages=False)
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance: cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
