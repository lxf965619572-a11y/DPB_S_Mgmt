#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
复位路径验证：FPGA 下发失败须如实返回失败；重启动作异步且可配置；
日志不得再谎报"将在 2 秒后重启"。
"""
import os
import pty
import socket
import struct
import subprocess
import sys
import threading
import time

HDR = struct.Struct('<IIBBBI')
MSG_RESET_IND = 141
IE_RESET_IND = 0x0515
RESET_SOFT, RESET_HARD = 0, 1
PORT = 30006
BUILD = os.path.expanduser('~/paau_build')
BIN = os.path.join(BUILD, 'antenna_mgmt')


def build_msg(mid, serial, payload=b''):
    return HDR.pack(mid, HDR.size + len(payload), 0, 0, 0, serial) + payload


def reset_payload(reset_type):
    data = struct.pack('<I', reset_type)
    return struct.pack('<HH', IE_RESET_IND, 4 + len(data)) + data


def run(name, use_uart, extra_cfg, reset_type, expect_any, expect_none):
    slave = None
    master_fd = None
    if use_uart:
        master_fd, slave_fd = pty.openpty()
        slave = os.ttyname(slave_fd)
    uart_dev = slave if use_uart else '/dev/null'

    conf = os.path.join(BUILD, 'reset.conf')
    cfg = ('BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n'
           'PAAU_ID_CHECK=0\nUART_DEVICE=%s\nLOG_LEVEL=INFO\n' % (PORT, uart_dev))
    for k, v in extra_cfg.items():
        cfg += '%s=%d\n' % (k, v)
    with open(conf, 'w') as f:
        f.write(cfg)

    def mock():
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('127.0.0.1', PORT))
        srv.listen(1)
        srv.settimeout(10)
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            srv.close()
            return
        time.sleep(1.0)
        # 每条消息必须带唯一 serial，避免被当成重发
        conn.sendall(build_msg(MSG_RESET_IND, 0x6001, reset_payload(reset_type)))
        time.sleep(3)
        conn.close()
        srv.close()

    threading.Thread(target=mock, daemon=True).start()
    time.sleep(0.4)

    proc = subprocess.Popen([BIN, conf], cwd=BUILD, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1)
    lines = []
    threading.Thread(target=lambda: [lines.append(r.decode('utf-8', 'replace').rstrip())
                                     for r in iter(proc.stdout.readline, b'')],
                     daemon=True).start()
    time.sleep(6)
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
    if master_fd is not None:
        os.close(master_fd)

    blob = '\n'.join(lines)
    hit = [m for m in expect_any if m in blob]
    bad = [m for m in expect_none if m in blob]
    ok = bool(hit) and not bad
    print('  %-46s %s' % (name, 'PASS' if ok else 'FAIL'))
    if not ok:
        print('        期望出现之一: %s -> 命中 %s' % (expect_any, hit or '（无）'))
        if bad:
            print('        不应出现: %s' % bad)
        for l in lines:
            if 'eset' in l or 'estart' in l or 'eboot' in l:
                print('        | %s' % l)
    return ok


def main():
    print('===== 复位路径验证 =====\n')
    results = []

    # UART 打不开 → fpga_send_phase_restore 必失败 → 必须如实返回失败，且不排重启
    results.append(run(
        'UART 不可用 → 软复位应如实失败、不排重启', False,
        {'RESET_RESTART_SERVICE': 1}, RESET_SOFT,
        expect_any=['reset NOT performed'],
        expect_none=['will restart in 2 seconds', 'Scheduled service restart']))

    results.append(run(
        'UART 不可用 → 硬复位应如实失败', False,
        {'RESET_REBOOT_SYSTEM': 1}, RESET_HARD,
        expect_any=['reset NOT performed'],
        expect_none=['will reboot in 2 seconds', 'Scheduled system reboot']))

    # UART 可用 → 命令下发成功；重启开关为 0 时应明说未重启
    results.append(run(
        'UART 可用 + 重启关闭 → 应明说未重启', True,
        {'RESET_RESTART_SERVICE': 0}, RESET_SOFT,
        expect_any=['Soft reset command sent to FPGA successfully',
                    'Service restart disabled by config'],
        expect_none=['will restart in 2 seconds', 'Scheduled service restart']))

    # UART 可用 + 重启打开 → 应异步排程（不在接收线程里 sleep）
    results.append(run(
        'UART 可用 + 重启打开 → 应异步排程重启', True,
        {'RESET_RESTART_SERVICE': 1}, RESET_SOFT,
        expect_any=['Scheduled service restart in 2 seconds'],
        expect_none=['Service will restart in 2 seconds...']))

    print('\n===== 汇总 =====')
    fails = sum(1 for ok in results if not ok)
    print('%s（失败 %d 项 / 共 %d 项）'
          % ('全部通过' if fails == 0 else '存在失败', fails, len(results)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
