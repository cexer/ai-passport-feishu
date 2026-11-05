"""Verify long stored text, UTF-8 paging, tail access and offline read position."""
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
out=root/"build/full-text-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-full-text-home"
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="fulltext_"+digest[:16]
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
    body="开头_120K\n"+"长"*40000+"\nEND_120K_MARK"
    total=len(body.encode("utf-8"));pages=(total+255)//256
    control(reset=True,message_text=body)
    instance=cli("start",fw=corpus,boot="none",label="full-text-acceptance")["instance"]
    network()
    wait("模拟测试群",rounds=30)
    press()
    wait("开头_120K",rounds=120,step=1000)
    press("up");press()
    wait("消息详情",rounds=25)
    wait(f"1/{pages}页",rounds=25)
    screen("01-first-page")
    first=cli("inspect",what=["heap","tasks"])
    for page in range(1,4):
        press("down")
        wait(f"{page+1}/{pages}页",rounds=15,step=250)
    advance(2500)
    screen("02-reading-position")
    checks.append({"case":"120KB decoded body is paged beyond the former preview","text_bytes":total,"pages":pages})
    press("reset",reset_kind="usb_rts")
    wait("模拟测试群",rounds=20)
    press();wait("开头_120K",rounds=15)
    press("up");press()
    wait(f"4/{pages}页",rounds=20)
    screen("03-offline-resumed-page")
    checks.append({"case":"offline reboot restores the saved text page","page":4})
    # A long hold jumps ten byte pages, without reading the intermediate body.
    for batch in range((pages+9)//10):
        current=view()["text"]
        if f"{pages}/{pages}页" in current:break
        press("down","hold")
        if batch%10==0:print(f"full-text fast paging batch {batch}",flush=True)
    wait(f"{pages}/{pages}页",rounds=20)
    wait("END_120K_MARK")
    press("down")
    screen("04-offline-tail")
    checks.append({"case":"the final marker is readable without AP or HTTP bridge"})
    cli("inspect",what=["heap","tasks"])
    assert events()["sends"]==0
    summary={"status":"PASS","image_sha256":digest,"checks":checks,
             "scope":"Emulator only; synthetic peer; no physical/TLS acceptance"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    control(hold_messages=False)
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance:cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
