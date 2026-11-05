"""Owner-bound DPAPI storage for a pending USB setup; no plaintext files."""
import ctypes as C
from ctypes import wintypes as W
import sys


class Blob(C.Structure):
    _fields_ = [('size', W.DWORD), ('data', C.POINTER(C.c_ubyte))]


def transform(value: bytes, decrypt: bool = False) -> bytes:
    if sys.platform != 'win32':
        raise RuntimeError('Pending credential recovery requires Windows DPAPI')
    crypt, kernel = C.WinDLL('crypt32'), C.WinDLL('kernel32')
    source_buffer = (C.c_ubyte * len(value)).from_buffer_copy(value)
    entropy_value = b'ai-passport-feishu/1'
    entropy_buffer = (C.c_ubyte * len(entropy_value)).from_buffer_copy(entropy_value)
    source, entropy, output = Blob(len(value), source_buffer), Blob(len(entropy_value), entropy_buffer), Blob()
    function = crypt.CryptUnprotectData if decrypt else crypt.CryptProtectData
    function.argtypes = [C.POINTER(Blob), C.c_void_p, C.POINTER(Blob), C.c_void_p,
                         C.c_void_p, W.DWORD, C.POINTER(Blob)]
    function.restype = W.BOOL
    kernel.LocalFree.argtypes = [C.c_void_p]
    if not function(C.byref(source), None, C.byref(entropy), None, None, 1, C.byref(output)):
        raise RuntimeError('Windows DPAPI operation failed')
    try:
        return C.string_at(output.data, output.size)
    finally:
        C.memset(source_buffer, 0, len(value))
        C.memset(output.data, 0, output.size)
        kernel.LocalFree(output.data)
