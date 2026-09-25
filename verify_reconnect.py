#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
C-2 验证：反复断线重连，检查接收线程能否被正确收掉、进程能否干净退出。

修复前：每次重连都直接覆盖 client->recv_thread，旧 tid 永久丢失、永不 join，
多次重连后旧线程可能在本客户端销毁后仍访问 client。
"""
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

HDR = struct.Struct('<IIBBBI')
PORT = 30007
BUILD = os.path.expanduser('~/paau_build')
BIN = os.path.join(BUILD, 'antenna_mgmt')
ROUNDS = 6


def main():
    conf = os.path.join(BUILD, 'reconn.conf')
    with open(conf, 'w') as f:
        f.write('BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n'
                'PAAU_ID_CHECK=0\nUART_DEVICE=/dev/null\nLOG_LEVEL=INFO\n' % PORT)

    stop = threading.Event()
    accepted = []

    def server():
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('127.0.0.1', PORT))
        srv.listen(8)
        srv.settimeout(20)
        while not stop.is_set() and len(accepted) < ROUNDS:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                break
            accepted.append(time.time())
            time.sleep(1.0)          # 让 PAAU 完成 on_connected 等
            conn.close()             # 主动断开 → PAAU 侧 recv 返回 0 → 断线
            time.sleep(1.2)          # 等 PAAU 重连
        srv.close()

    threading.Thread(target=server, daemon=True).start()

    proc = subprocess.Popen([BIN, conf], cwd=BUILD, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1)
    lines = []
    threading.Thread(target=lambda: [lines.append(r.decode('utf-8', 'replace').rstrip())
                                     for r in iter(proc.stdout.readline, b'')],
                     daemon=True).start()

    time.sleep(1.0 + ROUNDS * 2.4 + 3)
    stop.set()

    # 触发优雅退出，走完整清理链
    proc.terminate()
    try:
        rc = proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = -9

    blob = '\n'.join(lines)
    connects = len(re.findall(r'Connected to BBU server successfully', blob))
    recv_exits = len(re.findall(r'Recv thread exiting', blob))
    crashed = bool(re.search(r'Segmentation|core dumped|double free|corrupted', blob))
    destroyed = 'Message handler destroyed' in blob or 'Log upload module destroyed' in blob

    print('===== 重连压力测试 =====')
    print('  服务端断开次数        : %d' % len(accepted))
    print('  PAAU 重连成功次数     : %d' % connects)
    print('  接收线程退出次数      : %d' % recv_exits)
    print('  退出码                : %d' % rc)
    print('  崩溃迹象              : %s' % ('有' if crashed else '无'))
    print('  清理链走完            : %s' % ('是' if destroyed else '否'))

    ok = (connects >= ROUNDS - 1 and not crashed and rc != -9 and destroyed)
    if not ok:
        print('\n--- 可疑日志 ---')
        for l in lines[-40:]:
            print('  | %s' % l)
    print('\n%s' % ('通过：多次重连后仍能干净退出，无崩溃迹象' if ok else '失败'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
