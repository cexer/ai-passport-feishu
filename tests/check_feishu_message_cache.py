"""Independent JSON/Unicode oracle for the actual parser/store/cache pipeline."""
import json
import re
import subprocess
import sys
from pathlib import Path

executable=str(Path(sys.argv[1]).resolve())
directory=Path(sys.argv[2]).resolve()
directory.mkdir(parents=True)
names={f"@_user_{i}":f"人员{i:02d}😀" for i in range(1,33)}
raw="界"+"a"*1019+(" ".join(names)+" @_user_1000 @_user_1_extra\n")*20+"末尾标记"
def normalized(text,mapping):
    return re.sub(r"@[A-Za-z0-9_]+",lambda match:"@"+mapping[match.group()] if match.group() in mapping else match.group(),text)
def item(content,mentions,before=False,kind="text"):
    message={"message_id":"om_case","msg_type":kind,"sender":{"id":"ou_self"}}
    if before:message["mentions"]=mentions
    message["body"]={"content":json.dumps(content,ensure_ascii=False,separators=(",",":"))}
    if not before:message["mentions"]=mentions
    return message
mentions=[{"key":key,"name":value} for key,value in names.items()]
cases=[]
for before in [False,True]:
    cases.append(("before" if before else "after",{"code":0,"data":{"items":[item({"text":raw},mentions,before)]}},normalized(raw,names),"mapped","none"))
post={"en_us":{"title":"English","content":[[{"tag":"text","text":"EN @_user_1"}]]},
      "zh_cn":{"title":"中文","content":[[{"tag":"text","text":"ZH @_user_1 / @_user_10"}]]}}
for before in [False,True]:
    cases.append(("postb" if before else "posta",{"data":{"items":[item(post,mentions,before,"post")]},"code":0},normalized("中文\nZH @_user_1 / @_user_10\n",names),"mapped","none"))
for name in ["00123","名"*42+"ab"]:
    cases.append(("name"+str(len(cases)),{"code":0,"data":{"items":[item({"text":"@_user_1"},[{"key":"_user_1","name":name}],True)]}},"@"+name,"mapped","none"))
for label,invalid in [("longname",[{"key":"@_user_1","name":"x"*129}]),
                      ("conflict",[{"key":"@_user_1","name":"A"},{"key":"@_user_1","name":"B"}])]:
    cases.append((label,{"code":0,"data":{"items":[item({"text":raw},invalid)]}},raw,"literal","none"))
valid={"code":0,"data":{"items":[item({"text":raw},mentions)]}}
cases.append(("metaio",valid,raw,"literal","metadata"))
cases.append(("bodyio",valid,"","unavailable","body"))
cases.append(("cancel",valid,"","cancel","none"))
cases.append(("scope",valid,"","scope","none"))
cases.append(("late",{"data":valid["data"],"code":99999},"","envelope","none"))
cases.append(("nocode",{"data":valid["data"]},"","envelope","none"))
for label,document,expected,mode,fault in cases:
    for fragment in [1,37,1024]:
        out=directory/(label+str(fragment))
        out.mkdir()
        source=out/"input.json";reference=out/"expected.txt"
        source.write_text(json.dumps(document,ensure_ascii=False,separators=(",",":")),encoding="utf-8")
        reference.write_bytes(expected.encode("utf-8"))
        result=subprocess.run([executable,str(out),str(source),str(reference),mode,fault,str(fragment)],capture_output=True,text=True)
        if result.returncode:
            raise RuntimeError(f"{label}/{fragment} exit={result.returncode}: {result.stdout}\n{result.stderr}")
print("message cache: 42 JSON order/fragment cases, >8 mentions, locales, fallback, late errors, scope/cancel and protected drafts PASS")
