"""Read priority and cancellation while actual HTTP requests are held."""
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
out=root/"build/message-page-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-message-page-home"/stamp
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="message_pages_"+digest[:16]
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
    control(reset=True,message_pages=10)
    instance=cli("start",fw=corpus,boot="none",label="message-page-acceptance")["instance"]
    network();wait("模拟测试群",rounds=40);press();wait("历史消息0080",rounds=60);wait("1页")
    advance(1500);first=cli("inspect",what=["heap","tasks"])
    for fault in ["cycle","missing","oversized","nonboolean","latecode","missingcode","trailing","overflow"]:
        control(message_page_fault=fault)
        press("up","hold");wait("历史暂不可读",rounds=60);wait("1页");wait("历史消息0080")
    control(message_page_fault="normal",mode="permission")
    press("up","hold");wait("历史暂不可读",rounds=60);wait("历史消息0080")
    checks.append({"case":"invalid page metadata/envelopes, excessive item count and HTTP 403 retain the displayed messages"})
    control(hold_messages=True);press("up","hold")
    for _ in range(25):
        if any(e.get("held") and e.get("message_page_token")=="message+8/=" for e in events()["events"]):break
        advance(500)
    else:raise AssertionError("older-page request was not held")
    before=tick(view());press("ok","hold");cancelled=wait("已停止翻页",rounds=12,step=250)
    assert tick(cancelled)-before<3000000
    wait("历史消息0080");control(hold_messages=False);advance(1500)
    checks.append({"case":"cancelling an in-flight history request retains the current message page"})
    for number in range(2,11):
        control(message_meta_before=number%2==0)
        press("up","hold");wait(f"{number}页",rounds=60);wait(f"历史消息{88-number*8:04d}")
        if number%2==0:print(f"message page {number} passed",flush=True)
    last=cli("inspect",what=["heap","tasks"])
    assert last["heap"]["total_free_bytes"]>=first["heap"]["total_free_bytes"]-12288
    screen("01-oldest-messages")
    checks.append({"case":"80 messages traverse ten bounded windows with both metadata orders",
        "initial_free":first["heap"]["total_free_bytes"],"final_free":last["heap"]["total_free_bytes"]})
    before=len([e for e in events()["events"] if "message_offset" in e])
    press("down","hold");wait("9页");press("down","hold");wait("8页")
    assert len([e for e in events()["events"] if "message_offset" in e])==before
    for _ in range(7):press("up")
    wait("历史消息0017");press();wait("消息详情");wait("历史消息0017")
    body="历史消息0017\n"+"长"*1000+"\nHISTORY_TAIL_0017";pages=(len(body.encode())+255)//256
    wait(f"1/{pages}页")
    press("down","hold");press("down","hold");wait(f"{pages}/{pages}页");wait("HISTORY_TAIL_0017")
    advance(2500);screen("02-history-body-tail")
    checks.append({"case":"cached newer pages and full stored text on an older page remain readable"})
    cli("input",button="reset",action="click",reset_kind="usb_rts",then_run="350ms")
    wait("模拟测试群",rounds=30);press();wait("历史消息0080",rounds=30)
    for number in range(2,9):press("up","hold");wait(f"{number}页",rounds=35)
    for _ in range(7):press("up")
    press();wait(f"{pages}/{pages}页",rounds=35);wait("HISTORY_TAIL_0017")
    screen("03-offline-history-body")
    checks.append({"case":"offline reboot restores cached history navigation and the long body's reading position"})
    press("ok","hold");wait("8页");advance(45000);wait("8页")
    checks.append({"case":"offline connection retries preserve the displayed history page"})
    network();advance(30000);press("ok","hold");wait("飞书 | 会话");press();wait("< 模拟测试群")
    press();wait("按确定后开始录音")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press();wait("正在录音");advance(600);press();wait("模拟器语音回复",rounds=60)
    press("down");wait("草稿已取消");assert events()["sends"]==0
    checks.append({"case":"normal record/review/cancel recovers after history errors and offline reads"})
    summary={"status":"PASS","image_sha256":digest,"checks":checks,"scope":"Emulator only; synthetic paginated HTTP and ASR; no physical audio or real messages"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    control(hold_messages=False)
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance:cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
