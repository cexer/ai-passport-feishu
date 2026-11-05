#!/usr/bin/env python3
"""Provision an owner's Feishu application over physical USB serial."""

from __future__ import annotations

import argparse
import base64
import getpass
import json
import re
import sys
import time

PROTOCOL = "FAP-FEISHU/1"
APP_ID_PATTERN = re.compile(r"^cli_[A-Za-z0-9]+$")


def build_wifi_frame(ssid: str, password: str) -> bytes:
    if not 1 <= len(ssid.encode('utf-8')) <= 32 or '\0' in ssid:
        raise ValueError('Wi-Fi SSID must contain 1-32 UTF-8 bytes')
    if len(password.encode('utf-8')) > 64 or '\0' in password:
        raise ValueError('Wi-Fi password exceeds 64 UTF-8 bytes')
    return encode_frame('FAP-WIFI/1', {'ssid': ssid, 'password': password})


def encode_frame(protocol: str, payload: dict) -> bytes:
    encoded = json.dumps(payload, ensure_ascii=True, separators=(',', ':')).encode()
    if len(encoded) > 512:
        raise ValueError('Provisioning payload exceeds the device limit')
    return protocol.encode('ascii') + b' ' + base64.b64encode(encoded) + b'\n'


def build_frame(app_id: str, app_secret: str) -> bytes:
    if not APP_ID_PATTERN.fullmatch(app_id) or len(app_id) >= 64:
        raise ValueError("App ID must be a Feishu cli_ application ID")
    if not app_secret or len(app_secret) >= 128:
        raise ValueError("App Secret is empty or too long")
    if '\0' in app_secret or len(app_secret.encode('utf-8')) >= 128:
        raise ValueError('App Secret contains invalid bytes')
    return encode_frame(PROTOCOL, {"app_id": app_id, "app_secret": app_secret})


def choose_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from exc
    ports = [item.device for item in list_ports.comports()]
    likely = [item for item in ports if "usb" in item.lower() or "acm" in item.lower()]
    choices = likely or ports
    if len(choices) != 1:
        rendered = ", ".join(choices) if choices else "none found"
        raise RuntimeError(f"specify --port; detected ports: {rendered}")
    return choices[0]


def provision(port_name: str, frame: bytes, timeout: float, wait_auth: bool = False) -> str | None:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from exc
    deadline = time.monotonic() + timeout
    protocol = frame.split(b' ', 1)[0].decode('ascii')
    port = serial.Serial(port=None, baudrate=115200, timeout=0.25, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = port_name
    with port:
        # Opening ESP32-C3 USB Serial/JTAG resets the chip on some hosts. Wait
        # until the application has installed its receiver before sending any
        # secret material; writes during ROM boot can otherwise block or vanish.
        ready = False
        port.write(b'FAP-SETUP/1 PING\n')
        ready_deadline = min(deadline, time.monotonic() + 8.0)
        while time.monotonic() < ready_deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line == f"{protocol} READY":
                ready = True
                break
            if line in ('FAP-WIFI/1 READY', 'FAP-FEISHU/1 READY'):
                raise RuntimeError('device is waiting for a different setup phase')
        if not ready:
            raise RuntimeError('device did not confirm the requested setup phase')
        try:
            port.write(frame)
        except serial.SerialTimeoutException as exc:
            raise RuntimeError("USB write timed out after the device became ready") from exc
        accepted = False
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line == f"{protocol} OK":
                accepted = True
                if not wait_auth:
                    return None
            if accepted and line.startswith('FAP-SETUP/1 AUTH '):
                url = line[len('FAP-SETUP/1 AUTH '):]
                from urllib.parse import urlsplit
                parts = urlsplit(url)
                if parts.scheme != 'https' or parts.hostname not in ('accounts.feishu.cn', 'open.feishu.cn'):
                    raise RuntimeError('device returned an unsupported authorization host')
                return url
            if line == f"{protocol} ERROR":
                raise RuntimeError("device rejected the application credentials")
    raise TimeoutError("device did not confirm provisioning; keep it on the private-app setup screen")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write your own Feishu App ID/Secret to AI Passport over USB."
    )
    parser.add_argument("--app-id", help="owner's Feishu App ID")
    parser.add_argument('--json-stdin', action='store_true', help='Read SDK app_id/app_secret JSON from stdin; never echo credentials')
    parser.add_argument('--protect-stdin', action='store_true', help='DPAPI-protect stdin JSON for a pending local USB setup')
    parser.add_argument('--encrypted-file', help='Resume from a Windows-owner-bound DPAPI credential file')
    parser.add_argument('--wifi-profile', help='Use an explicitly selected saved Windows Wi-Fi profile')
    parser.add_argument('--wifi-current', action='store_true', help='Use the connected Windows Wi-Fi profile; never print its key')
    parser.add_argument('--wait-auth-url', action='store_true', help='Return the device user-authorization URL as JSON')
    parser.add_argument("--port", help="USB serial port; auto-detected when unique")
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()
    secret = ''
    try:
        if sum(bool(value) for value in (args.app_id, args.json_stdin,
               args.encrypted_file, args.protect_stdin, args.wifi_current, args.wifi_profile)) != 1:
            raise ValueError('Choose exactly one credential source or setup action')
        if args.protect_stdin:
            from windows_secrets import transform
            raw = sys.stdin.buffer.read(513)
            if len(raw) > 512:
                raise ValueError('SDK credential payload is too large')
            data = json.loads(raw)
            build_frame(data['app_id'], data['app_secret'])
            print(base64.b64encode(transform(raw)).decode('ascii'))
            return 0
        if args.wifi_current or args.wifi_profile:
            if args.json_stdin or args.app_id or args.wait_auth_url:
                raise ValueError('Wi-Fi and app setup must be separate phases')
            from windows_wifi import current_wifi, wifi_profile
            ssid, secret = wifi_profile(args.wifi_profile) if args.wifi_profile else current_wifi()
            frame = build_wifi_frame(ssid, secret)
        elif args.json_stdin or args.encrypted_file:
            if args.encrypted_file:
                from windows_secrets import transform
                with open(args.encrypted_file, 'rb') as pending:
                    encrypted = pending.read(8193)
                if len(encrypted) > 8192:
                    raise ValueError('Encrypted setup file is too large')
                raw = transform(base64.b64decode(encrypted, validate=True), decrypt=True)
            else:
                raw = sys.stdin.buffer.read(513)
            if len(raw) > 512:
                raise ValueError('SDK credential payload is too large')
            data = json.loads(raw)
            frame = build_frame(data['app_id'], data['app_secret'])
            data.clear()
            raw = b''
        else:
            if not args.app_id:
                raise ValueError('Specify --app-id, --json-stdin, or --wifi-current')
            secret = getpass.getpass("Feishu App Secret (input hidden): ")
            frame = build_frame(args.app_id.strip(), secret)
        secret = ""
        url = provision(choose_port(args.port), frame, args.timeout, args.wait_auth_url)
        frame = b''
        if url:
            print(json.dumps({'stage': 'authorize_user', 'url': url}))
            return 0
    except (RuntimeError, TimeoutError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        secret = ""
    print('Wi-Fi saved over USB.' if args.wifi_current or args.wifi_profile else
          'Feishu application saved. Complete the authorization QR on the device.')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
