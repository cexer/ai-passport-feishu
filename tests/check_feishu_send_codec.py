"""Decode generated requests with Python's independent JSON/UTF-8 implementation."""
import json
import sys
from pathlib import Path

directory = Path(sys.argv[1])
expected = (directory / "send-source.bin").read_bytes()
parts = sorted(directory.glob("send-[0-9][0-9].json"))
assert len(parts) > 1
decoded = []
for index, path in enumerate(parts):
    body = path.read_bytes()
    assert len(body) <= 128 * 1024
    outer = json.loads(body)
    assert outer["uuid"] == f"fake-uuid-{index}"
    assert outer["receive_id"] == "oc_fake_chat" and outer["msg_type"] == "text"
    decoded.append(json.loads(outer["content"])["text"].encode("utf-8"))
assert b"".join(decoded) == expected
reply = json.loads((directory / "send-reply.json").read_bytes())
assert "receive_id" not in reply
assert expected.startswith(json.loads(reply["content"])["text"].encode("utf-8"))
print("send codec independent double-JSON and complete 300 KiB UTF-8 round trip: PASS")
