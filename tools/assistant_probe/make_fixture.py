#!/usr/bin/env python3
"""Package six synthetic command WAVs for the opt-in, muted device suite."""
import argparse
import hashlib
import json
import wave
from pathlib import Path


def read_command(path: Path) -> bytes:
    with wave.open(str(path), "rb") as stream:
        if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate(),
                stream.getcomptype()) != (1, 2, 16000, "NONE"):
            raise ValueError(f"{path.name}: expected 16 kHz mono PCM16 WAV")
        if not 6400 <= stream.getnframes() <= 29 * 16000:
            raise ValueError(f"{path.name}: command must last 0.4 to 29 seconds")
        pcm = stream.readframes(stream.getnframes())
    data = bytes(9600) + pcm + bytes(16000)
    return data + bytes((-len(data)) % 640)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-chats", type=Path, required=True)
    parser.add_argument("--navigate", type=Path, required=True)
    parser.add_argument("--draft", type=Path, required=True)
    parser.add_argument("--open-chat", type=Path, required=True)
    parser.add_argument("--read-latest", type=Path, required=True)
    parser.add_argument("--goodbye", type=Path, required=True)
    args = parser.parse_args()
    paths = [args.list_chats, args.navigate, args.draft, args.open_chat, args.read_latest, args.goodbye]
    commands = [read_command(path) for path in paths]
    if sum(map(len, commands)) > 960 * 1024:
        parser.error("combined fixture exceeds 960 KiB; shorten the commands")
    root = Path(__file__).resolve().parents[2]
    output = root / "build/assistant-autotest"
    output.mkdir(parents=True, exist_ok=True)
    source = ["#pragma once\n/* Synthetic assistant commands; never ship in the product. */\n"]
    rows = []
    for number, (path, data) in enumerate(zip(paths, commands)):
        source.append(f"static const unsigned char assistant_fixture_{number}[]={{\n")
        source.extend(",".join(str(value) for value in data[at:at + 32]) + ",\n"
                      for at in range(0, len(data), 32))
        source.append("};\n")
        rows.append({"source": path.name, "bytes": len(data),
                     "duration_ms": len(data) // 32,
                     "sha256": hashlib.sha256(data).hexdigest()})
    source.append("static const unsigned char *const assistant_fixtures[]={"
                  "assistant_fixture_0,assistant_fixture_1,assistant_fixture_2,assistant_fixture_3,assistant_fixture_4,assistant_fixture_5};\n")
    source.append("static const unsigned assistant_fixture_sizes[]={"
                  "sizeof(assistant_fixture_0),sizeof(assistant_fixture_1),sizeof(assistant_fixture_2),sizeof(assistant_fixture_3),sizeof(assistant_fixture_4),sizeof(assistant_fixture_5)};\n")
    (output / "assistant_fixture.h").write_text("".join(source), encoding="utf-8")
    (output / "fixture.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    # The shared injection driver also declares the ordinary voice suite. On a
    # fresh checkout provide aliases, while preserving any existing voice data.
    voice = root / "build/voice-autotest/voice_autotest_fixture.h"
    if not voice.exists():
        voice.parent.mkdir(parents=True, exist_ok=True)
        voice.write_text(
            '#pragma once\n#ifndef FEISHU_ASSISTANT_AUTOTEST\n'
            '#error "These aliases are only for the assistant diagnostic"\n#endif\n'
            '#include "../assistant-autotest/assistant_fixture.h"\n'
            '#define voice_fixture_one assistant_fixture_0\n'
            '#define voice_fixture_two assistant_fixture_1\n'
            'static const char voice_expected_one[]="assistant fixture";\n'
            'static const char voice_expected_two[]="assistant fixture";\n', encoding="utf-8")
    print(json.dumps({"output": str(output), "fixture_bytes": sum(map(len, commands)),
                      "cases": 8, "silent": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
