"""Minimal OutputDebugString (DBWIN) listener — DebugView clone.

Captures OutputDebugString output from all processes (wxLogTrace on
Windows GUI builds goes here, not stderr). Prints "pid: message" lines
to stdout until killed.

Usage: python ods_listen.py [pid_filter]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import struct

k32 = ctypes.windll.kernel32

# 64-bit: default restype (c_int) truncates handles/pointers
k32.CreateEventW.restype = wt.HANDLE
k32.CreateFileMappingW.restype = wt.HANDLE
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]

INFINITE = 0xFFFFFFFF
PAGE_READWRITE = 0x04
FILE_MAP_READ = 0x0004
EVENT_ALL_ACCESS = 0x1F0003

pid_filter = int(sys.argv[1]) if len(sys.argv) > 1 else None

buffer_ready = k32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
data_ready = k32.CreateEventW(None, False, False, "DBWIN_DATA_READY")

if not buffer_ready or not data_ready:
    sys.exit("failed to create DBWIN events (another DebugView running?)")

mapping = k32.CreateFileMappingW(wt.HANDLE(-1), None, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER")

if not mapping:
    sys.exit("failed to create DBWIN_BUFFER mapping")

view = k32.MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096)

if not view:
    sys.exit("failed to map DBWIN_BUFFER view")

print("listening for OutputDebugString...", flush=True)
k32.SetEvent(buffer_ready)

while True:
    ret = k32.WaitForSingleObject(data_ready, 1000)

    if ret == 0:  # WAIT_OBJECT_0
        raw = ctypes.string_at(view, 4096)
        pid = struct.unpack("<I", raw[:4])[0]
        msg = raw[4:].split(b"\x00", 1)[0].decode("utf-8", "replace").rstrip()

        if msg and (pid_filter is None or pid == pid_filter):
            print(f"{pid}: {msg}", flush=True)

        k32.SetEvent(buffer_ready)
