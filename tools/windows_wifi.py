"""Read the connected owner's Windows WLAN profile in memory for USB setup.

No shell commands, profile export files, keys in arguments, or profile logging.
"""
import ctypes as C
import sys
import xml.etree.ElementTree as ET
from ctypes import wintypes as W


def _read_wifi(requested_profile: str | None = None) -> tuple[str, str]:
    if sys.platform != 'win32':
        raise RuntimeError('Automatic Wi-Fi setup requires Windows')
    wlan = C.WinDLL('wlanapi')
    wlan.WlanFreeMemory.argtypes = [C.c_void_p]
    wlan.WlanOpenHandle.argtypes = [W.DWORD, C.c_void_p, C.POINTER(W.DWORD), C.POINTER(W.HANDLE)]
    wlan.WlanEnumInterfaces.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(C.c_void_p)]
    wlan.WlanQueryInterface.argtypes = [W.HANDLE, C.c_void_p, C.c_int, C.c_void_p,
                                      C.POINTER(W.DWORD), C.POINTER(C.c_void_p), C.c_void_p]
    wlan.WlanGetProfile.argtypes = [W.HANDLE, C.c_void_p, W.LPCWSTR, C.c_void_p,
                                   C.POINTER(C.c_void_p), C.POINTER(W.DWORD), C.POINTER(W.DWORD)]
    wlan.WlanCloseHandle.argtypes = [W.HANDLE, C.c_void_p]
    handle, version = W.HANDLE(), W.DWORD()
    if wlan.WlanOpenHandle(2, None, C.byref(version), C.byref(handle)):
        raise RuntimeError('Cannot open the Windows WLAN service')
    interfaces = C.c_void_p()
    try:
        if wlan.WlanEnumInterfaces(handle, None, C.byref(interfaces)):
            raise RuntimeError('Cannot enumerate Windows WLAN interfaces')
        count = C.cast(interfaces, C.POINTER(W.DWORD))[0]
        for index in range(count):
            # WLAN_INTERFACE_INFO: GUID(16), WCHAR description[256], DWORD state.
            interface = interfaces.value + 8 + index * 532
            if requested_profile is None and W.DWORD.from_address(interface + 528).value != 1:
                continue
            if requested_profile is not None:
                profile, ssid = requested_profile, None
            else:
                size, connection = W.DWORD(), C.c_void_p()
                if wlan.WlanQueryInterface(handle, interface, 7, None, C.byref(size), C.byref(connection), None):
                    continue
                try:
                    if size.value < 556:
                        raise RuntimeError('Windows returned incomplete Wi-Fi connection data')
                    profile = C.wstring_at(connection.value + 8, 256).split('\0', 1)[0]
                    length = W.DWORD.from_address(connection.value + 520).value
                    if not 1 <= length <= 32:
                        raise RuntimeError('Windows returned an invalid SSID length')
                    ssid = C.string_at(connection.value + 524, length).decode('utf-8')
                finally:
                    wlan.WlanFreeMemory(connection)
            xml, flags, access = C.c_void_p(), W.DWORD(4), W.DWORD()
            if wlan.WlanGetProfile(handle, interface, profile, None, C.byref(xml), C.byref(flags), C.byref(access)):
                if requested_profile is not None:
                    continue
                raise RuntimeError('Windows did not allow reading the connected Wi-Fi profile')
            try:
                root = ET.fromstring(C.wstring_at(xml))
                if ssid is None:
                    ssid = root.findtext('.//{*}SSID/{*}name')
                    if not ssid:
                        encoded = root.findtext('.//{*}SSID/{*}hex')
                        ssid = bytes.fromhex(encoded or '').decode('utf-8')
                    if not ssid:
                        raise RuntimeError('Requested Wi-Fi profile has no SSID')
                key = root.find('.//{*}sharedKey')
                if key is None:
                    auth = root.findtext('.//{*}authentication')
                    if auth != 'open':
                        raise RuntimeError('Enterprise Wi-Fi is not supported for automatic setup')
                    return ssid, ''
                if key.findtext('{*}protected') != 'false':
                    raise RuntimeError('Windows did not provide a plaintext Wi-Fi key')
                return ssid, key.findtext('{*}keyMaterial', '')
            finally:
                wlan.WlanFreeMemory(xml)
        raise RuntimeError('Requested Wi-Fi profile is unavailable' if requested_profile else
                           'No connected Windows Wi-Fi interface; a network must be supplied separately')
    finally:
        if interfaces:
            wlan.WlanFreeMemory(interfaces)
        wlan.WlanCloseHandle(handle, None)


def current_wifi() -> tuple[str, str]:
    return _read_wifi()


def wifi_profile(profile_name: str) -> tuple[str, str]:
    if not profile_name or '\0' in profile_name:
        raise ValueError('A Windows Wi-Fi profile name is required')
    return _read_wifi(profile_name)
