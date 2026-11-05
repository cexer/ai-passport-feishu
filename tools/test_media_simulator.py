"""JPEG streaming previews, local views, offline cache and fault recovery."""
import argparse
import datetime
import hashlib
import json
from PIL import Image
import os
import shutil
import subprocess
from pathlib import Path
from urllib.request import Request,urlopen

p=argparse.ArgumentParser()
p.add_argument("--simulator",required=True)
p.add_argument("--build",required=True,type=Path)
p.add_argument("--peer-port",type=int,default=18032)
p.add_argument("--only-offline-pan",action="store_true")
p.add_argument("--keep-on-failure",action="store_true")
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
stamp=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
out=root/"build/media-simulator-report"/stamp
out.mkdir(parents=True)
home=root/"build/sim-media-home"/stamp
(home/"config").mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256((a.build/"FoloToy-AI-Passport-full.bin").read_bytes()).hexdigest()
image=root/"build/simulator-images"/digest
image.mkdir(parents=True,exist_ok=True)
for name in ("FoloToy-AI-Passport-full.bin","FoloToy-AI-Passport.elf"):
    shutil.copyfile(a.build/name,image/name)
corpus="media_"+digest[:16]
(home/"config/corpus.toml").write_text("["+corpus+"]\n"+"\n".join(
    kind+" = "+json.dumps((image/name).as_posix()) for kind,name in (
        ("bin","FoloToy-AI-Passport-full.bin"),("elf","FoloToy-AI-Passport.elf"))),encoding="utf-8")
env=dict(os.environ,PASSPORTSIM_HOME=str(home))
instance=None
index=0
checks=[]
screenshot_times={}


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
    # Keep each call inside the host budget even when JPEG/flash work runs
    # slower than real time. A host-budget refusal is never a guest PASS.
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
    advance(300)
    r=cli("screenshot",view="raw",save_as=name)
    filename=Path(r["artifacts"][0]["path"]).name
    found=max((home/"data/artifacts").rglob(filename),key=lambda x:x.stat().st_mtime)
    shutil.copyfile(found,out/(name+".png"))
    screenshot_times[name]=tick(r)

def image_ready(text,rounds=100):
    # Button delivery can fall at the end of then_run. Let the 100 ms UI timer
    # publish its busy state before accepting a footer shared by two views.
    advance(200)
    current=None
    for _ in range(rounds):
        current=view()
        if text in current["text"] and "低清" not in current["text"] and "正在更新预览" not in current["text"]:return current
        advance(500)
    raise AssertionError("Detailed view timeout: "+current["text"])
def network():
    cli("wifi_ap",ssid="feishu-simulator-ap",channel=6,rssi=-40)
    cli("net_http",op="bridge",port=80,host_port=a.peer_port)
def tick(d): return d.get("vt_us",d.get("receipt",{}).get("vt_us",0))

def resources(): return [e for e in events()["events"] if "resource" in e]

def enter_image(expected=None):
    wait("< 模拟测试群")
    press("up");press();wait("消息详情")
    if expected:wait("图片样例 "+expected,rounds=10)
    start=tick(view());press();opened=wait("图片消息",rounds=20);return start,opened

def leave_image():
    press("ok","hold");wait("消息详情")
    press("ok","hold");wait("< 模拟测试群")

def variant(name):
    control(image_variant=name,message_type="image")
    wait("< 模拟测试群")
    advance(9000)
    enter_image(name)

def color(name,position,channel):
    # UI-tree publication precedes SPI display transfer. Observe the actual
    # framebuffer within a bounded deadline, including the final saved PNG.
    # Decode has a 30-second job deadline. A button can be queued while the
    # old framebuffer/footer is still visible, so include that bounded work.
    for attempt in range(45):
        pixels=Image.open(out/(name+".png")).convert("RGB")
        value=pixels.getpixel(position)
        if value[channel]>180 and all(x<70 for i,x in enumerate(value) if i!=channel):return
        advance(500);screen(name)
    raise AssertionError((name,position,value))

try:
    control(reset=True,message_type="image",image_variant="large" if a.only_offline_pan else "quadrants")
    instance=cli("start",fw=corpus,boot="none",label="media-acceptance")["instance"]
    network();wait("模拟测试群",rounds=30)
    press();wait("< 模拟测试群")
    if a.only_offline_pan:
        enter_image("large");wait("完整预览",rounds=100,step=500)
    else:
        enter_image("quadrants");wait("完整预览",rounds=100,step=500)
        screen("01-overview")
        color("01-overview",(66,110),0);color("01-overview",(174,110),1);color("01-overview",(66,190),2)
        checks.append({"case":"large-dimension JPEG displays correctly fitted without a whole-frame bitmap"})
        before=len(resources())
        zoom_start=tick(press("up","hold"));quick=wait("低清",rounds=20,step=100)
        assert tick(quick)-zoom_start<3000000,("quick zoom UI latency",tick(quick)-zoom_start)
        image_ready("局部 2x")
        screen("02-zoom")
        pan_start=tick(press("up"));wait("低清",rounds=20,step=100)
        screen("03-quick-pan");color("03-quick-pan",(66,170),0)
        assert screenshot_times["03-quick-pan"]-pan_start<4000000,("quick mapped framebuffer latency",screenshot_times["03-quick-pan"]-pan_start)
        checks.append({"case":"cached overview supplies a bounded low-resolution pan before detailed decode finishes",
                       "framebuffer_latency_us":screenshot_times["03-quick-pan"]-pan_start})
        image_ready("局部 2x");screen("03-vertical-pan")
        color("03-vertical-pan",(66,170),0)
        press();wait("图片操作")
        for _ in range(3):press("down")
        press();wait("上左 下右")
        press("up");image_ready("上左 下右");screen("04-horizontal-pan")
        color("04-horizontal-pan",(174,110),0)
        assert len(resources())==before,"small retained original should support local decode without HTTP"
        checks.append({"case":"zoom and both pan axes use a retained bounded original without another request"})
        cli("inspect",what=["heap","tasks"])
    
        press("reset",reset_kind="usb_rts")
        wait("模拟测试群",rounds=20,step=500);press();wait("< 模拟测试群")
        before=len(resources());start,opened=enter_image()
        ready=opened if "完整预览" in opened["text"] else wait("完整预览",rounds=10,step=250)
        assert tick(ready)-start<2000000,("cached preview latency",tick(ready)-start)
        assert len(resources())==before
        screen("05-offline-cached-preview")
        checks.append({"case":"offline reboot displays cached preview directly","latency_us":tick(ready)-start})
        leave_image();network()
        advance(30000)
        variant("large");wait("完整预览",rounds=100,step=500)
    screen("06-large-source-preview")
    assert resources()[-1]["resource_bytes"]>300*1024
    color("06-large-source-preview",(66,110),0)
    checks.append({"case":"original over 300 KiB streams to a bounded preview","source_bytes":resources()[-1]["resource_bytes"]})
    cli("input",button="reset",action="click",reset_kind="usb_rts",then_run="350ms")
    wait("模拟测试群",rounds=20,step=500);press();wait("< 模拟测试群")
    enter_image();wait("完整预览",rounds=15,step=250)
    before=len(resources());press("up","hold");wait("低清",rounds=20,step=250)
    press("up");screen("06-offline-low-resolution-pan");color("06-offline-low-resolution-pan",(66,170),0)
    assert len(resources())==before
    checks.append({"case":"offline pan remains viewable from overview when the large original was not retained"})
    leave_image();network();advance(30000)
    if not a.only_offline_pan:
        for name,notice in (("progressive","编码不支持"),("truncated","图片暂不可读"),("huge","图片或缓存过大"),("png","编码不支持")):
            variant(name);wait(notice,rounds=100,step=500);screen("fault-"+name);leave_image()
        variant("quadrants");wait("完整预览",rounds=100);screen("07-recovered-supported-image")
        checks.append({"case":"unsupported/progressive/truncated/oversized resources recover to a normal supported image"})
        leave_image();control(resource_fault="permission")
        variant("permission");wait("图片暂不可读",rounds=70)
        assert not any(e["path"].endswith("/device_authorization") for e in events()["events"])
        control(resource_fault="normal")
        press();wait("图片操作")
        for _ in range(4):press("down")
        press();wait("完整预览",rounds=100)
        checks.append({"case":"HTTP 403 preserves binding and a later image retry recovers"})
        leave_image()
        control(message_type="text")
        wait("中文消息与回复测试",rounds=30,step=1000)
        # Composer remains selected after re-entering the message list explicitly.
        press("ok","hold");wait("飞书 | 会话");press();wait("< 模拟测试群")
        press();wait("按确定后开始录音")
        cli("mic_set",kind="tone",hz=440,amplitude=16000,duration_ms=2000,fs=16000,channels=1)
        press();wait("正在录音");advance(600);press();wait("模拟器语音回复",rounds=60)
        press();wait("消息已发送",rounds=60)
        assert events()["sends"]==1
        checks.append({"case":"normal text reading, recording and explicit send still work after image faults"})
        cli("inspect",what=["heap","tasks"])
    summary={"status":"PASS","suite":"offline_pan" if a.only_offline_pan else "full","image_sha256":digest,"checks":checks,
             "scope":"Emulator only; synthetic JPEG/HTTP/ASR, no real TLS/physical display acceptance"}
except Exception as error:
    summary={"status":"FAIL","image_sha256":digest,"checks":checks,"error":str(error)}
    raise
finally:
    diagnostic_errors=[]
    try:
        control(hold_resources=False)
        (out/"peer-events.json").write_text(json.dumps(events(),ensure_ascii=False,indent=2),encoding="utf-8")
    except Exception as error:
        diagnostic_errors.append("peer cleanup: "+type(error).__name__)
    if instance:
        try:
            request=out/"preview-console-input.json"
            cursor=0;lines=[]
            for window in range(256):
                request.write_text(json.dumps({"instance":instance,"op":"read","cursor":cursor,"max_bytes":1024}),encoding="utf-8")
                raw=subprocess.run([a.simulator,"serial","--json","@"+str(request),"--output","json"],env=env,capture_output=True,encoding="utf-8",timeout=15)
                data=json.loads(raw.stdout);shaped=data["serial"]
                lines.extend(line for line in shaped["head"]+shaped["tail"] if "preview ready:" in line or "preview request:" in line)
                next_cursor=data.get("next_cursor",shaped.get("next_cursor"))
                if not window:print("Console cursor fields:",list(data),list(shaped),flush=True)
                if next_cursor is None or next_cursor<=cursor:break
                cursor=next_cursor
            (out/"preview-console.txt").write_text("\n".join(lines),encoding="utf-8")
        except Exception as error:
            diagnostic_errors.append("filtered console: "+type(error).__name__)
        if a.keep_on_failure and summary["status"]=="FAIL":
            (out/"retained-instance.json").write_text(json.dumps({"instance":instance,"home":str(home)}),encoding="utf-8")
        else:
            try:cli("stop")
            except Exception as error:diagnostic_errors.append("stop: "+type(error).__name__)
    cleanup_failed=bool(diagnostic_errors) and summary["status"]=="PASS"
    if diagnostic_errors:
        summary["diagnostic_errors"]=diagnostic_errors
        if cleanup_failed:summary.update(status="FAIL",error="Acceptance cleanup incomplete")
    (out/"summary.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(summary,report_directory=str(out)),ensure_ascii=False),flush=True)
    if cleanup_failed:raise RuntimeError("Acceptance cleanup incomplete")
