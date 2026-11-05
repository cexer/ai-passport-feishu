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
out=root/"build/page-retry-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-page-retry-home"/stamp
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="page_retry_"+digest[:16]
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
    control(reset=True,chat_pages=3)
    instance=cli("start",fw=corpus,boot="none",label="page-retry-acceptance")["instance"]
    network();wait("分页群00001",rounds=40)
    press("down","hold");wait("2页",rounds=45);wait("分页群00011")
    cli("input",button="reset",action="click",reset_kind="usb_rts",then_run="350ms")
    wait("1页",rounds=30);press("down","hold");wait("2页");wait("分页群00011")
    advance(45000);wait("2页");wait("分页群00011")
    press("up","hold");wait("1页");press("down","hold");wait("2页")
    assert events()["sends"]==0
    checks.append({"case":"offline connection retries retain the displayed second page, its cursor and cached navigation"})
    summary={"status":"PASS","image_sha256":digest,"checks":checks,"scope":"Emulator only; synthetic page data; negative controls may intentionally fail"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    if instance:cli("stop")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
