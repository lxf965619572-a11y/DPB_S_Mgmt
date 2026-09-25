#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对指定二进制发一条截断构造的 msg_id，看它如何被处理（用于对照基线）。"""
import socket
import struct
import subprocess
import sys
import threading
import time

HDR = struct.Struct('<IIBBBI')
BIN = sys.argv[1]
PORT = int(sys.argv[2])
MSG_ID = int(sys.argv[3])

proc = subprocess.Popen([BIN, '/tmp/probe.conf'], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, bufsize=1)
lines = []


def pump():
    for raw in iter(proc.stdout.readline, b''):
        lines.append(raw.decode('utf-8', 'replace').rstrip())


threading.Thread(target=pump, daemon=True).start()


def mock():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', PORT))
    srv.listen(1)
    srv.settimeout(8)
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        srv.close()
        return
    payload = bytes([1, 2, 3, 4, 5, 6, 7, 8])
    conn.sendall(HDR.pack(MSG_ID, HDR.size + len(payload), 0, 0, 0, 0x4001) + payload)
    time.sleep(2.5)
    conn.close()
    srv.close()


mt = threading.Thread(target=mock, daemon=True)
mt.start()
time.sleep(5)
proc.terminate()
try:
    proc.wait(timeout=8)
except subprocess.TimeoutExpired:
    proc.kill()
mt.join(timeout=2)

print('--- msg_id=%d 在 %s 下的处理 ---' % (MSG_ID, BIN.split('/')[-1]))
for l in lines:
    if any(k in l for k in ('Dispatching', 'TRANSPARENT', 'ransparent', 'unexpected',
                            'Rejected', 'No handler', 'forwarded to FPGA', 'Forwarding')):
        print('  %s' % l)
