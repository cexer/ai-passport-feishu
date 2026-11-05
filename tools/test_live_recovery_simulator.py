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
out=root/"build/live-recovery-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-live-recovery-home"/stamp
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="live_recovery_"+digest[:16]
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
    control(reset=True,asr_text="SIM_CORRUPT_CAPTURE",hold_asr=True)
    instance=cli("start",fw=corpus,boot="none",label="live-recovery-acceptance")["instance"]
    network();advance(12000);wait("模拟测试群",rounds=60)
    press();wait("中文消息与回复测试",rounds=45);press();wait("按确定后开始录音")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=4000,fs=16000,channels=1)
    press();wait("正在录音")
    for _ in range(25):
        if any(e.get("asr_held") for e in events()["events"]):break
        advance(250)
    else:raise AssertionError("ASR did not reach the held response")
    control(hold_asr=False)
    wait("飞书 | 会话",rounds=45,step=250)
    assert events()["sends"]==0
    advance(100);assert "feishu_capture" not in json.dumps(cli("inspect",what=["tasks"]))
    advance(4500) # Let unused synthetic-tone chunks expire; real capture has ended.
    checks.append({"case":"storage corruption during active capture drains the old capture task and restores home without sending"})
    control(asr_text="恢复后的确认发送",send_result_id="om_simulator_recover_trigger")
    press();wait("< 模拟测试群");press();wait("按确定后开始录音")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press();wait("正在录音");advance(600);press();wait("恢复后的确认发送",rounds=60)
    press();wait("飞书 | 会话",rounds=60,step=250)
    assert events()["sends"]==1
    control(send_result_id="om_simulator_sent");advance(12000)
    assert events()["sends"]==1
    screen("01-after-in-flight-send-fault")
    checks.append({"case":"corruption after a submitted send never overwrites a fresh draft or automatically retries the submitted UUID"})
    cli("input",button="reset",action="click",reset_kind="usb_rts",then_run="350ms")
    wait("模拟测试群",rounds=35);assert events()["sends"]==1
    network();advance(25000);press();wait("< 模拟测试群");press();wait("按确定后开始录音")
    cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
    press();wait("正在录音");advance(600);press();wait("恢复后的确认发送",rounds=60)
    press("down");wait("草稿已取消");assert events()["sends"]==1
    checks.append({"case":"reboot and subsequent normal read/record/review/cancel recover after both active-operation faults"})
    cli("inspect",what=["heap","tasks"])
    summary={"status":"PASS","image_sha256":digest,"checks":checks,"scope":"Emulator fault fixture; real application recovery branches, synthetic HTTP/ASR and a 500ms fault-completion delay; no physical audio or real sends"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    control(hold_asr=False)
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance:cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
