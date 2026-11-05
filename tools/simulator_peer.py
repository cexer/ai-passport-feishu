#!/usr/bin/env python3
"""Loopback-only Feishu fixture. All identities, tokens and messages are fake."""
import argparse
import base64
import http.server
import json
import threading
import hashlib
import socket
import struct
from pathlib import Path
from urllib.parse import urlsplit,parse_qs

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, default=18026)
parser.add_argument("--events", type=Path, required=True)
args = parser.parse_args()
lock = threading.Lock()
chats_allowed=threading.Event()
chats_allowed.set()
messages_allowed=threading.Event()
messages_allowed.set()
asr_allowed=threading.Event()
asr_allowed.set()
resources_allowed=threading.Event()
resources_allowed.set()
events = []
state = {"mode": "normal", "sent": {}, "message_text": "中文消息与回复测试",
         "asr_text": "模拟器语音回复", "send_fault":"normal", "echo_send":False,"asr_fault":"normal"}
state.update(message_type="text",image_variant="quadrants",resource_fault="normal",mentions=[],mentions_before=False,chat_variant="normal",chat_pages=0,page_fault="normal",message_pages=0,message_page_fault="normal",message_meta_before=False,send_result_id="om_simulator_sent")
state.update(registration="normal",registration_polls=0,registration_begins=0,device_grant="normal")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, data, status=200, content_type="application/json"):
        raw = data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def dispatch(self):
        path = urlsplit(self.path).path
        message_get=self.command=="GET" and (path.endswith("/messages") or
            ("/im/v1/messages/" in path and "/resources/" not in path))
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.headers.get('Content-Type','').startswith('application/x-www-form-urlencoded'):
            body={key:values[0] for key,values in parse_qs(raw.decode(),keep_blank_values=True).items()}
        else:body = json.loads(raw or b"{}")
        if path == "/control":
            with lock:
                if body.get("reset"):
                    events.clear()
                    state["sent"].clear()
                    state["message_text"] = "中文消息与回复测试"
                    state["asr_text"] = "模拟器语音回复"
                    state["send_fault"]="normal"
                    state["echo_send"]=False
                    state["asr_fault"]="normal"
                    chats_allowed.set()
                    messages_allowed.set()
                    asr_allowed.set()
                    resources_allowed.set()
                    state.update(message_type="text",image_variant="quadrants",resource_fault="normal",mentions=[],mentions_before=False,chat_variant="normal",chat_pages=0,page_fault="normal",message_pages=0,message_page_fault="normal",message_meta_before=False,send_result_id="om_simulator_sent")
                if "hold_chats" in body:
                    if body["hold_chats"]:chats_allowed.clear()
                    else:chats_allowed.set()
                if "hold_messages" in body:
                    if body["hold_messages"]: messages_allowed.clear()
                    else: messages_allowed.set()
                if "hold_asr" in body:
                    if body["hold_asr"]: asr_allowed.clear()
                    else: asr_allowed.set()
                if "hold_resources" in body:
                    if body["hold_resources"]:resources_allowed.clear()
                    else:resources_allowed.set()
                for field in ("message_text", "asr_text", "send_fault", "echo_send", "asr_fault","message_type","image_variant","resource_fault","mentions","mentions_before","chat_variant","chat_pages","page_fault","message_pages","message_page_fault","message_meta_before","send_result_id"):
                    if field in body:
                        state[field] = body[field]
                state["mode"] = body.get("mode", "normal")
                for field in ("registration","device_grant"):
                    if field in body:state[field]=body[field]
                if body.get('reset_registration'):
                    state.update(registration_polls=0,registration_begins=0)
            return self.reply({"ok": True})
        if path == "/events":
            with lock:
                combined="".join(state["sent"].values()).encode("utf-8")
                return self.reply({"events": events, "sends": len(state["sent"]),
                    "sent_bytes":len(combined),"sent_sha256":hashlib.sha256(combined).hexdigest()})
        event = {"path": path, "method": self.command}
        if path.endswith('/registration'):
            event['action']=body.get('action')
        if path.endswith('/device_authorization') or path.endswith('/token'):
            event['client_id']=body.get('client_id')
        with lock:
            mode = state["mode"]
            events.append(event)
            args.events.parent.mkdir(parents=True, exist_ok=True)
            args.events.write_text(json.dumps(events, ensure_ascii=False, indent=2), encoding="utf-8")
        if path.endswith("/chats") and not chats_allowed.is_set():
            with lock:event["chats_held"]=True
            if not chats_allowed.wait(30):return self.reply({"code":500},503)
        if message_get:event["message_page_token"]=parse_qs(urlsplit(self.path).query).get("page_token",[""])[0]
        if message_get and not messages_allowed.is_set():
            with lock: event["held"]=True
            if not messages_allowed.wait(30): return self.reply({"code":500},503)
        if path.endswith("/chats") or message_get:
            if mode == "permission":
                return self.reply({"code": 99991672}, 403)
            if mode == "malformed":
                return self.reply(b'{"data":')
            if mode == "server_error":
                return self.reply({"code": 500}, 503)
        if path.endswith("/user_info") or path.endswith("/chats") or message_get:
            if mode in ("revoked","oauth_truncated","oauth_nested","oauth_partial"):
                return self.reply({"code": 99991668}, 401)
        if path.endswith('/registration'):
            kind=state['registration']
            if body.get('action')=='begin':
                state['registration_begins']+=1
                if kind=='server_error':return self.reply({'error':'unavailable'},503)
                return self.reply({'device_code':'synthetic+/=&','verification_uri_complete':
                    'https://open.feishu.cn/page/launcher?user_code=SIMULATOR-ONLY',
                    'expires_in':12,'interval':2})
            assert body.get('device_code')=='synthetic+/=&'
            state['registration_polls']+=1
            if kind=='pending':return self.reply({'error':'authorization_pending'},400)
            if kind=='deny':return self.reply({'error':'access_denied'},400)
            if kind=='expire_once' and state['registration_begins']==1:return self.reply({'error':'expired_token'},400)
            if kind=='slow_once' and state['registration_polls']==1:return self.reply({'error':'slow_down'},400)
            if kind=='malformed_once' and state['registration_polls']==1:return self.reply(b'{"client_secret":"partial"')
            return self.reply({'client_id':'cli_registered_fixture','client_secret':'synthetic-not-a-secret'})
        if path.endswith("/token"):
            if 'device_code' in body:
                if state['device_grant']=='pending':return self.reply({'error':'authorization_pending'},400)
                if state['device_grant']=='deny':return self.reply({'error':'access_denied'},400)
            if mode=="oauth_truncated":return self.reply(b'{"error":"invalid_grant"',400)
            if mode=="oauth_nested":return self.reply({"debug":{"error":"invalid_grant"},"error":"invalid_scope"},400)
            if mode=="oauth_partial":return self.reply(b'{"access_token":"partial-new-token",',500)
            if mode == "revoked" and "refresh_token" in body:
                with lock:
                    state["mode"] = "pending"
                return self.reply({"error": "invalid_grant"}, 400)
            if mode == "pending" and "device_code" in body:
                return self.reply({"error": "authorization_pending"}, 400)
            data = {"access_token": "simulator-access", "refresh_token": "simulator-refresh",
                    "token_type": "Bearer", "expires_in": 7200}
            result = data
        elif path.endswith("/device_authorization"):
            if state['device_grant']=='invalid_client':return self.reply({'error':'invalid_client'},400)
            result = {"device_code": "simulator-device-code",
                      "verification_uri_complete": "https://example.invalid/simulator-only",
                      "expires_in": 30, "interval": 2}
        elif path.endswith("/tenant_access_token/internal"):
            result = {"code": 0, "tenant_access_token": "simulator-tenant", "expire": 7200}
        else:
            if path.endswith("/user_info"):
                data = {"open_id": "ou_simulator_self"}
            elif path.endswith("/chats"):
                data = {"items": [
                    {"chat_id": "oc_simulator_a", "name": "模拟测试群", "chat_mode": "group"},
                    {"chat_id": "oc_simulator_b", "name": "第二个会话", "chat_mode": "p2p"},
                ], "has_more": False}
                if state["chat_variant"]=="only_b":data["items"]=data["items"][1:]
                if state["chat_pages"]:
                    query=parse_qs(urlsplit(self.path).query)
                    token=query.get("page_token",[""])[0]
                    if token and not (token.startswith("cursor+") and token.endswith("/=")):
                        return self.reply({"code":400},400)
                    offset=int(token[7:-2]) if token else 0
                    event["page_offset"]=offset
                    size=min(int(query.get("page_size",[10])[0]),10)
                    total=int(state["chat_pages"])*10
                    data={"items":[{"chat_id":f"oc_page_{i+1:05d}","name":f"分页群{i+1:05d}","chat_mode":"group"}
                        for i in range(offset,min(offset+size,total))],"has_more":offset+size<total}
                    if data["has_more"]:data["page_token"]=f"cursor+{offset+size}/="
                    if state["page_fault"]=="cycle":data.update(has_more=True,page_token=token)
                    if state["page_fault"]=="missing":data.pop("page_token",None)
                    if state["page_fault"]=="oversized":data.update(has_more=True,page_token="X"*513)
            elif message_get:
                image=state["message_type"]=="image"
                content={"image_key":"img_fixture_"+state["image_variant"]} if image else {"text":state["message_text"]}
                data = {"items": [{"message_id": ("om_simulator_image" if image else "om_simulator_text") if path.endswith("/messages") else path.rsplit("/",1)[-1], "msg_type": "image" if image else "text",
                    "create_time": "1700000001000", "deleted": False,
                    "sender": {"id": "ou_simulator_other", "sender_name": "图片样例 "+state["image_variant"] if image else "测试用户"},
                    "body": {"content": json.dumps(content, ensure_ascii=False)},
                }], "has_more": False}
                message=data["items"][0]
                if state["mentions_before"]:
                    body=message.pop("body")
                    message["mentions"]=state["mentions"]
                    message["body"]=body
                else:message["mentions"]=state["mentions"]
                if state["message_pages"]:
                    query=parse_qs(urlsplit(self.path).query);token=query.get("page_token",[""])[0]
                    total=int(state["message_pages"])*8
                    def history_item(rank):
                        text=f"历史消息{rank:04d}"
                        if rank==17:text+="\n"+"长"*1000+"\nHISTORY_TAIL_0017"
                        return {"message_id":f"om_hist_{rank:04d}","msg_type":"text","create_time":str(1700000000000+rank*1000),
                            "sender":{"id":"ou_simulator_other","sender_name":"历史用户"},
                            "body":{"content":json.dumps({"text":text},ensure_ascii=False)}}
                    if path.endswith("/messages"):
                        if token and not (token.startswith("message+") and token.endswith("/=")):
                            return self.reply({"code":400},400)
                        offset=int(token[8:-2]) if token else 0;size=min(int(query.get("page_size",[8])[0]),8)
                        event["message_offset"]=offset
                        rows=[history_item(i) for i in range(total-offset,max(0,total-offset-size),-1)]
                        more=offset+size<total;meta={"has_more":more}
                        if more:meta["page_token"]=f"message+{offset+size}/="
                        fault=state["message_page_fault"]
                        if fault=="cycle":meta.update(has_more=True,page_token=token)
                        if fault=="missing":meta.pop("page_token",None)
                        if fault=="oversized":meta.update(has_more=True,page_token="X"*513)
                        if fault=="nonboolean":meta["has_more"]="false"
                        if fault=="overflow":rows.append(history_item(max(1,total-offset-size)))
                        data=dict(meta,items=rows) if state["message_meta_before"] else dict(items=rows,**meta)
                    else:
                        identity=path.rsplit("/",1)[-1]
                        rank=int(identity.removeprefix("om_hist_")) if identity.startswith("om_hist_") else total
                        data={"items":[history_item(rank)]}
            elif "/resources/" in path:
                resource=path.rsplit("/",1)[-1]
                if not resource.startswith("img_fixture_"):return self.reply({"code":404},404)
                variant=resource.removeprefix("img_fixture_");event["resource"]=variant
                if not resources_allowed.is_set():
                    event["resource_held"]=True
                    if not resources_allowed.wait(30):return self.reply({"code":500},503)
                if state["resource_fault"]=="permission":return self.reply({"code":99991672},403)
                if state["resource_fault"]=="server_error":return self.reply({"code":500},503)
                fixtures=Path(__file__).resolve().parents[1]/"tests/fixtures/jpeg"
                source=fixtures/("progressive.jpg" if variant=="progressive" else "gray.jpg" if variant=="gray" else "quadrants.jpg")
                raw=source.read_bytes()
                if variant=="large":raw=raw[:2]+(b"\xff\xef"+struct.pack(">H",65535)+bytes(65533))*6+raw[2:]
                if variant=="truncated":raw=raw[:-2]
                if variant=="huge":
                    changed=bytearray(raw);at=changed.index(b"\xff\xc0");struct.pack_into(">HH",changed,at+5,5000,5000);raw=bytes(changed)
                if variant=="png":raw=b"\x89PNG\r\n\x1a\n"+bytes(100)
                event["resource_bytes"]=len(raw)
                return self.reply(raw,content_type="image/jpeg")
            elif path.endswith("/stream_recognize"):
                pcm = base64.b64decode(body["speech"]["speech"])
                event.update(action=body["config"]["action"], pcm_bytes=len(pcm),
                             nonzero_pcm=any(pcm))
                if not asr_allowed.is_set():
                    event["asr_held"]=True
                    if not asr_allowed.wait(30):return self.reply({"code":500},503)
                if body["config"]["action"]==2 and state["asr_fault"]=="late_error":
                    return self.reply({"data":{"recognition_text":state["asr_text"]},"code":999})
                data = {"recognition_text": state["asr_text"]}
            elif path.endswith("/reply") or (path.endswith("/messages") and self.command == "POST"):
                assert body["msg_type"] == "text" and body["uuid"]
                text=json.loads(body["content"])["text"]
                event.update(send_attempt=True,target=body.get("receive_id"),uuid=body["uuid"],request_bytes=int(self.headers.get("Content-Length",0)))
                if state["send_fault"]=="reject_once":
                    state["send_fault"]="normal";event["rejected"]=True
                    return self.reply({"code":230025},400)
                with lock:
                    state["sent"].setdefault(body["uuid"],text)
                event.update(send=True, uuid=body["uuid"],
                             text_bytes=len(text.encode("utf-8")))
                if state["send_fault"]=="lose_ack_once":
                    state["send_fault"]="normal";event["ack_lost"]=True
                    self.close_connection=True
                    self.connection.shutdown(socket.SHUT_RDWR);self.connection.close()
                    return
                data = {"message_id": state["send_result_id"]}
                if state["echo_send"]:data["body"]={"content":body["content"]}
            else:
                return self.reply({"code": 404}, 404)
            result = {"code": 0, "data": data}
        if message_get and path.endswith("/messages"):
            if state["message_page_fault"]=="latecode":result["code"]=999
            if state["message_page_fault"]=="missingcode":result.pop("code",None)
            if state["message_page_fault"]=="trailing":return self.reply(json.dumps(result).encode()+b"trailing")
        if path.endswith("/chats"):
            if state["page_fault"]=="latecode":result["code"]=999
            if state["page_fault"]=="missingcode":result.pop("code",None)
            if state["page_fault"]=="nonboolean":result["data"]["has_more"]="false"
            if state["page_fault"]=="trailing":return self.reply(json.dumps(result).encode()+b"trailing")
        event["response_bytes"] = len(json.dumps(result, ensure_ascii=False).encode("utf-8"))
        with lock:
            args.events.write_text(json.dumps(events, ensure_ascii=False, indent=2), encoding="utf-8")
        self.reply(result)

    do_GET = dispatch
    do_POST = dispatch


server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
print("Synthetic Feishu peer ready on loopback", flush=True)
server.serve_forever()
