#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
msg_id 白名单 + 透传通道开关 的场景测试。

每个场景：写配置 → 起 PAAU → mock 发一条指定 msg_id 的报文 → 看日志里出现哪个标记，
据此判断报文是被拒绝还是在正常处理。
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

HDR = struct.Struct('<IIBBBI')
PORT = 30002
BUILD = os.path.expanduser('~/paau_build')
BIN = os.path.join(BUILD, 'antenna_mgmt')

IE_TYPE_CELL_CONFIG = 0x5E2


def build_ie(t, d):
    return struct.pack('<HH', t, 4 + len(d)) + d


def build_msg(msg_id, serial, payload=b'', paau_id=0):
    return HDR.pack(msg_id, HDR.size + len(payload), paau_id, 0, 0, serial) + payload


def cell_payload():
    b = struct.pack('<B', 0) + struct.pack('<I', 1) + struct.pack('<H', 1000)
    b += struct.pack('<B', 0) + struct.pack('<B', 16) + struct.pack('<B', 3)
    return build_ie(IE_TYPE_CELL_CONFIG, b)


def run(name, msg_id, extra_cfg, expect_any, expect_none):
    conf = os.path.join(BUILD, 'msgid.conf')
    cfg = ('BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n'
           'PAAU_ID_CHECK=0\nUART_DEVICE=/dev/null\nLOG_LEVEL=INFO\n' % PORT)
    for k, v in extra_cfg.items():
        cfg += '%s=%d\n' % (k, v)
    with open(conf, 'w') as f:
        f.write(cfg)

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
        payload = cell_payload() if msg_id in (195, 196) else b'\x01\x02\x03\x04\x05\x06\x07\x08'
        conn.sendall(build_msg(msg_id, 0x3001, payload))
        time.sleep(2.5)
        conn.close()
        srv.close()

    mt = threading.Thread(target=mock, daemon=True)
    mt.start()
    time.sleep(0.4)

    proc = subprocess.Popen([BIN, conf], cwd=BUILD, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1)
    lines = []

    def pump():
        for raw in iter(proc.stdout.readline, b''):
            lines.append(raw.decode('utf-8', 'replace').rstrip())

    pt = threading.Thread(target=pump, daemon=True)
    pt.start()
    time.sleep(5)
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
    mt.join(timeout=2)

    blob = '\n'.join(lines)
    hit = [m for m in expect_any if m in blob]
    bad = [m for m in expect_none if m in blob]
    ok = bool(hit) and not bad
    print('  %-52s %s' % (name, 'PASS' if ok else 'FAIL'))
    if not ok:
        print('        期望出现之一: %s' % expect_any)
        print('        实际命中: %s' % (hit or '（无）'))
        if bad:
            print('        不应出现但出现了: %s' % bad)
        for l in lines:
            if any(k in l for k in ('msg_id', 'Transparent', 'transparent', 'Dispatching', 'Rejected')):
                print('        | %s' % l)
    return ok


def main():
    print('===== msg_id 白名单 + 透传通道开关 场景测试 =====\n')
    results = []

    print('[白名单]')
    results.append(run('msg_id=195 已登记消息应正常受理', 195, {},
                       ['Dispatching message'], ['Rejected unknown msg_id']))
    results.append(run('msg_id=9999 未登记消息应被拒', 9999, {},
                       ['Rejected unknown msg_id'], ['Dispatching message']))
    # 关键：msg_id 是 32 位，0x10000+221 截断后正好是透传号 221。
    # 修复前会被当成透传受理，修复后必须被拒。
    results.append(run('msg_id=65757(=0x10000+221) 截断构造应被拒', 65757, {},
                       ['Rejected unknown msg_id'], ['TRANSPARENT', 'Dispatching message']))
    results.append(run('msg_id=65536+231 截断构造应被拒', 65536 + 231, {},
                       ['Rejected unknown msg_id'], ['TRANSPARENT']))

    print('\n[透传通道开关]')
    results.append(run('msg_id=231 通道关闭(ENABLE=0)应被拒', 231,
                       {'TRANSPARENT_ENABLE': 0},
                       ['Transparent channel disabled'], ['TRANSPARENT MESSAGE RECEIVED']))
    results.append(run('msg_id=231 通道开启(ENABLE=1)应进入处理', 231,
                       {'TRANSPARENT_ENABLE': 1},
                       ['Failed to parse transparent message',
                        'TRANSPARENT MESSAGE RECEIVED'],
                       ['Transparent channel disabled']))

    print('\n===== 汇总 =====')
    fails = sum(1 for ok in results if not ok)
    print('%s（失败 %d 项 / 共 %d 项）'
          % ('全部通过' if fails == 0 else '存在失败', fails, len(results)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
