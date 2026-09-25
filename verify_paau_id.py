#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
入站 paau_id 校验接口的场景测试。

每个场景：写一份配置 → 起 PAAU → mock BBU 发一条"建立小区"请求（msg 195，
带指定 paau_id）→ 看是否收到响应（msg 196）。
  收到响应 = 报文被受理；没收到 = 被丢弃。
同时抓取日志里的校验相关行。
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
MSG_NR_CELL_CONFIG = 195
MSG_NR_CELL_CONFIG_RSP = 196
IE_TYPE_CELL_CONFIG = 0x5E2

BUILD = os.path.expanduser('~/paau_build')
BIN = os.path.join(BUILD, 'antenna_mgmt')
PORT = 30001


def build_ie(t, d):
    return struct.pack('<HH', t, 4 + len(d)) + d


def build_msg(msg_id, serial, paau_id, payload=b''):
    return HDR.pack(msg_id, HDR.size + len(payload), paau_id, 0, 0, serial) + payload


def cell_ie(cell_id):
    b = struct.pack('<B', 0) + struct.pack('<I', cell_id) + struct.pack('<H', 1000)
    b += struct.pack('<B', 0) + struct.pack('<B', 16) + struct.pack('<B', 3)
    return b


def run_scenario(name, send_paau_id, check_mode, local_id=0, broadcast=255):
    conf = os.path.join(BUILD, 'paauid.conf')
    with open(conf, 'w') as f:
        f.write('BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\n'
                'PAAU_ID=%d\nPAAU_ID_CHECK=%d\nPAAU_ID_BROADCAST=%d\n'
                'UART_DEVICE=/dev/null\nLOG_LEVEL=INFO\n'
                % (PORT, local_id, check_mode, broadcast))

    got = {}

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
        conn.settimeout(6)
        ie = build_ie(IE_TYPE_CELL_CONFIG, cell_ie(1))
        conn.sendall(build_msg(MSG_NR_CELL_CONFIG, 0x2001, send_paau_id, ie))
        buf = b''
        t0 = time.time()
        while time.time() - t0 < 6:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= HDR.size:
                mid, mlen, _, _, _, _ = HDR.unpack_from(buf, 0)
                if mlen < HDR.size or len(buf) < mlen:
                    break
                if mid == MSG_NR_CELL_CONFIG_RSP:
                    got['ok'] = True
                buf = buf[mlen:]
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
    time.sleep(6)
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
    mt.join(timeout=2)

    accepted = got.get('ok', False)
    warn = [l for l in lines if 'does not match local' in l]
    dropped = [l for l in lines if 'Message DROPPED' in l]
    init = [l for l in lines if 'Inbound paau_id check' in l]

    print('  %-46s 受理=%-5s  WARN=%d  丢弃=%d'
          % (name, '是' if accepted else '否', len(warn), len(dropped)))
    if init:
        m = re.search(r'mode=(\d+).*local=(-?\d+).*broadcast=(-?\d+)', init[0])
        if m:
            print('        生效配置: mode=%s local=%s broadcast=%s'
                  % (m.group(1), m.group(2), m.group(3)))
    return accepted, len(warn), len(dropped)


def main():
    print('===== 入站 paau_id 校验接口场景测试（本地 PAAU_ID=0）=====')
    results = []

    print('\n[模式 0 = 关闭] 不校验，与改造前行为一致')
    a, w, d = run_scenario('paau_id=7, CHECK=0', 7, 0)
    results.append(('CHECK=0 不校验时异己报文应被受理', a and w == 0 and d == 0))

    print('\n[模式 1 = 仅记录（默认）] 不匹配打 WARN，但受理')
    a, w, d = run_scenario('paau_id=7, CHECK=1', 7, 1)
    results.append(('CHECK=1 异己报文应被受理且产生 WARN', a and w >= 1 and d == 0))

    print('\n[模式 2 = 拒绝] 不匹配则丢弃')
    a, w, d = run_scenario('paau_id=7, CHECK=2', 7, 2)
    results.append(('CHECK=2 异己报文应被丢弃', (not a) and d >= 1))

    print('\n[模式 2 + 匹配值] 正常报文必须照常受理（防止误伤）')
    a, w, d = run_scenario('paau_id=0, CHECK=2', 0, 2)
    results.append(('CHECK=2 本机 paau_id 应被受理', a and d == 0))

    print('\n[模式 2 + 广播值] 广播报文应被受理且不告警')
    a, w, d = run_scenario('paau_id=255, CHECK=2, BCAST=255', 255, 2)
    results.append(('CHECK=2 广播值应被受理且无 WARN', a and w == 0 and d == 0))

    print('\n[广播值可关] BCAST=-1 时广播值不再被特殊受理')
    a, w, d = run_scenario('paau_id=255, CHECK=2, BCAST=-1', 255, 2, broadcast=-1)
    results.append(('BCAST=-1 时广播值应被丢弃', (not a) and d >= 1))

    print('\n[本地 ID 可配] PAAU_ID=3 时 paau_id=3 应被受理')
    a, w, d = run_scenario('paau_id=3, CHECK=2, LOCAL=3', 3, 2, local_id=3)
    results.append(('本地 ID 可配为 3', a and d == 0))

    print('\n===== 汇总 =====')
    fails = 0
    for desc, ok in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', desc))
        if not ok:
            fails += 1
    print('\n%s（失败 %d 项）' % ('全部通过' if fails == 0 else '存在失败', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
