"""Persistent drafts, complete long sends and ambiguous-ACK recovery on a fake peer."""
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
out=root/"build/outbox-simulator-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-outbox-home"/stamp
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="outbox_"+digest[:16]
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

def attempts(): return [e for e in events()["events"] if e.get("send_attempt")]

def record(text):
    control(asr_text=text)
    press();wait("按确定后开始录音")
    # Finish any previous synthetic tone timeline before submitting a new one.
    advance(2100)
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press();wait("正在录音",rounds=30,step=250)
    advance(600);press()

def reopen_draft_offline():
    press("reset",reset_kind="usb_rts")
    wait("模拟测试群",rounds=20,step=500)
    press();wait("中文消息与回复测试",rounds=10)
    press();wait("确认发送",rounds=25)

def reconnect():
    before=len(events()["events"]);network()
    for _ in range(65):
        advance(1000)
        if any(e["path"].endswith("/user_info") for e in events()["events"][before:]):
            advance(3000);return
    raise AssertionError("normal authentication did not recover after offline boot")

try:
    control(reset=True)
    instance=cli("start",fw=corpus,boot="none",label="outbox-acceptance")["instance"]
    network();wait("模拟测试群",rounds=30)
    press();wait("中文消息与回复测试")
    text="LONG_ASR_BEGIN\n"+"长"*60000+"\nLONG_ASR_TAIL"
    record(text)
    pages=(len(text.encode())+255)//256
    wait("LONG_ASR_BEGIN",rounds=150,step=1000);wait(f"1/{pages}页",rounds=30)
    assert events()["sends"]==0
    screen("01-long-draft")
    checks.append({"case":"complete long ASR saved without a send","bytes":len(text.encode()),"pages":pages})
    reopen_draft_offline();wait("LONG_ASR_BEGIN");wait(f"1/{pages}页")
    for _ in range((pages+9)//10):
        if f"{pages}/{pages}页" in view()["text"]:break
        press("down","hold")
    wait("LONG_ASR_TAIL",rounds=25);screen("02-offline-restored-tail")
    assert events()["sends"]==0
    checks.append({"case":"saved draft survives offline reboot and its final page is readable"})
    reconnect();control(echo_send=True)
    press();wait("确认处理草稿");screen("03-long-send-actions")
    press();wait("消息已发送",rounds=160,step=1000)
    result=events();assert result["sent_bytes"]==len(text.encode())
    assert result["sent_sha256"]==hashlib.sha256(text.encode()).hexdigest()
    assert result["sends"]==2 and len(attempts())==2
    assert all(e["request_bytes"]<=128*1024 for e in attempts())
    screen("04-entire-text-sent")
    checks.append({"case":"explicit confirmation sends all bytes in two bounded requests, with large echoed ACKs","bytes":result["sent_bytes"]})
    cli("inspect",what=["heap","tasks"])

    control(send_fault="lose_ack_once",echo_send=False)
    short="UNCERTAIN_ACK_TEXT"
    record(short);wait(short,rounds=45)
    press();wait("发送结果未确认",rounds=60,step=1000)
    assert len(attempts())==3 and events()["sends"]==3
    advance(12000);assert len(attempts())==3
    reopen_draft_offline();wait(short)
    assert len(attempts())==3
    reconnect()
    press();wait("再次确定可能重复");assert len(attempts())==3
    screen("05-uncertain-retry-warning")
    press();wait("消息已发送",rounds=45,step=1000)
    assert len(attempts())==4 and events()["sends"]==3
    assert attempts()[-1]["uuid"]==attempts()[-2]["uuid"]
    checks.append({"case":"lost ACK is never replayed automatically, including after reboot; explicit warned retry keeps UUID"})

    control(send_fault="reject_once")
    record("REJECT_THEN_RECOVER");wait("REJECT_THEN_RECOVER",rounds=45)
    press();wait("发送未完成",rounds=45)
    assert attempts()[-1].get("rejected") and events()["sends"]==3
    rejected_uuid=attempts()[-1]["uuid"]
    press();wait("消息已发送",rounds=45)
    assert events()["sends"]==4 and attempts()[-1]["uuid"]==rejected_uuid
    checks.append({"case":"explicit API rejection preserves draft and a later confirmation recovers normal send"})

    control(asr_fault="late_error")
    record("LATE_ASR_ERROR_"+"故障"*10000)
    wait("识别或保存失败",rounds=140,step=1000)
    before=len(attempts());assert events()["sends"]==4
    control(asr_fault="normal")
    record("NORMAL_AFTER_ASR_FAILURE");wait("NORMAL_AFTER_ASR_FAILURE",rounds=45)
    press("down");wait("草稿已取消")
    assert len(attempts())==before and events()["sends"]==4
    checks.append({"case":"late ASR error aborts staged full text; subsequent recording/review/cancel restores main flow"})
    cli("inspect",what=["heap","tasks"])
    summary={"status":"PASS","image_sha256":digest,"checks":checks,
             "scope":"Emulator only; synthetic HTTP/ASR, no real TLS or physical device"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance:cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)

