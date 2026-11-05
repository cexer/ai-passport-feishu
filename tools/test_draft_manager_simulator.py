"""Global protected-draft navigation, immutable targets and fault recovery."""
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
out=root/"build/draft-manager-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-draft-manager-home"/stamp
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="draft_manager_"+digest[:16]
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

def held(field): return any(e.get(field) for e in events()["events"])

try:
    control(reset=True,asr_text="原始保留草稿")
    instance=cli("start",fw=corpus,boot="none",label="draft-manager-acceptance")["instance"]
    network();wait("模拟测试群",rounds=30)
    press();wait("< 模拟测试群");wait("中文消息与回复测试")
    press();wait("按确定后开始录音")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press();wait("正在录音");advance(600);press();wait("原始保留草稿",rounds=50)
    press("ok","hold");wait("< 模拟测试群")
    control(chat_variant="only_b")
    press("ok","hold");wait("飞书 | 会话");advance(15000)
    assert "模拟测试群" not in view()["text"]
    press("ok","hold");wait("全部待发送草稿")
    press("down");press("down");press();wait("全部待发送草稿")
    wait("待确认");screen("01-hidden-chat-draft")
    press();wait("审阅草稿");wait("原始保留草稿");wait("目标：oc_simulator_a")
    checks.append({"case":"global manager opens a protected draft whose destination is absent from current chats"})
    screen("02-immutable-destination")
    press("ok","hold");wait("全部待发送草稿")
    cli("input",button="reset",action="click",reset_kind="usb_rts",then_run="350ms")
    wait("飞书 | 会话",rounds=25);press("ok","hold");wait("全部待发送草稿")
    press("down");press("down");press();wait("待确认")
    press();wait("原始保留草稿");assert events()["sends"]==0
    checks.append({"case":"offline reboot preserves and reopens the global draft without sending"})
    # Return online with a different visible chat; rerecording must keep the saved target.
    press("up");wait("离线缓存可读")
    press("ok","hold");wait("全部待发送草稿");press();wait("原始保留草稿")
    checks.append({"case":"offline rerecord is rejected and returns to the intact global manager"})
    network();advance(30000);control(asr_fault="late_error")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press("up");wait("正在录音");advance(600);press();wait("全部待发送草稿",rounds=60)
    press();wait("原始保留草稿")
    checks.append({"case":"failed rerecord returns to manager and preserves the original protected draft"})
    control(asr_fault="normal",asr_text="全局新草稿")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press("up");wait("正在录音");advance(600);press();wait("全局新草稿",rounds=60)
    wait("目标：oc_simulator_a");press();wait("没有待发送草稿",rounds=60)
    ev=events();sends=[e for e in ev["events"] if e.get("send")]
    assert ev["sends"]==1 and len(sends)==1 and sends[0]["target"]=="oc_simulator_a"
    checks.append({"case":"rerecord and explicit send keep the original target and remove only its completed draft"})
    screen("03-sent-manager-empty")
    press("ok","hold");wait("飞书 | 会话");press();wait("< 第二个会话")
    press();wait("按确定后开始录音")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press();wait("正在录音");advance(600);press();wait("全局新草稿",rounds=60)
    press("down");wait("草稿已取消");wait("< 第二个会话")
    assert events()["sends"]==1
    checks.append({"case":"ordinary compose and cancel recover on the visible chat after manager completion"})
    cli("inspect",what=["heap","tasks"])
    summary={"status":"PASS","image_sha256":digest,"checks":checks,"scope":"Emulator only; synthetic HTTP/ASR; no physical audio or real sends"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance:cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
