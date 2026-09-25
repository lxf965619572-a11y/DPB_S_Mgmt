#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
校准结果查询（IE 308 -> IE 358）的如实回报验证。

用 pty 造伪串口喂带指定 calib_result 的遥测帧，然后发一条状态查询（msg 41，
IE 308），读回 msg 42 里的 IE 358，看回报的值。
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
MSG_PAAU_STATUS_QUERY = 41
MSG_PAAU_STATUS_QUERY_RSP = 42
IE_CALIB_QUERY = 308
IE_CALIB_RESP = 358
PORT = 30005
BUILD = os.path.expanduser('~/paau_build')
BIN = os.path.join(BUILD, 'antenna_mgmt')

CALIB_SUCCESS, CALIB_FAILURE, CALIB_UNAVAILABLE = 0, 1, 2


def build_ie(t, d):
    return struct.pack('<HH', t, 4 + len(d)) + d


def build_msg(mid, serial, payload=b''):
    return HDR.pack(mid, HDR.size + len(payload), 0, 0, 0, serial) + payload


def telemetry(calib_result=0, mode=0):
    data = bytearray(127)
    data[0] = 0x0F
    struct.pack_into('<I', data, 77, mode)      # 相控阵工作模式
    data[85] = calib_result                     # 校准结果（0=成功，其他=失败）
    return (struct.pack('>H', 0xEB90) + struct.pack('>H', 127)
            + bytes(data) + struct.pack('>H', 0x5555))


def run(name, feed_calib, extra_cfg, expect):
    master_fd, slave_fd = pty.openpty()
    slave = os.ttyname(slave_fd)

    conf = os.path.join(BUILD, 'calib.conf')
    cfg = ('BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n'
           'PAAU_ID_CHECK=0\nUART_DEVICE=%s\nLOG_LEVEL=INFO\n' % (PORT, slave))
    for k, v in extra_cfg.items():
        cfg += '%s=%d\n' % (k, v)
    with open(conf, 'w') as f:
        f.write(cfg)

    stop = threading.Event()
    result = {}

    def feeder():
        frame = telemetry(feed_calib if feed_calib is not None else 0)
        while not stop.is_set():
            if feed_calib is not None:
                try:
                    os.write(master_fd, frame)
                except OSError:
                    break
                try:
                    os.set_blocking(master_fd, False)
                    while True:
                        if not os.read(master_fd, 4096):
                            break
                except (BlockingIOError, OSError):
                    pass
            stop.wait(0.2)

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
        conn.settimeout(6)
        time.sleep(1.0)     # 等遥测进入状态缓存
        conn.sendall(build_msg(MSG_PAAU_STATUS_QUERY, 0x5001,
                               build_ie(IE_CALIB_QUERY, b'')))
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
                body = buf[HDR.size:mlen]
                if mid == MSG_PAAU_STATUS_QUERY_RSP:
                    off = 0
                    while off + 4 <= len(body):
                        t, l = struct.unpack_from('<HH', body, off)
                        if l < 4 or off + l > len(body):
                            break
                        if t == IE_CALIB_RESP:
                            result['value'] = body[off + 4]
                        off += l
                buf = buf[mlen:]
            if 'value' in result:
                break
        conn.close()
        srv.close()

    threading.Thread(target=feeder, daemon=True).start()
    threading.Thread(target=mock, daemon=True).start()
    time.sleep(0.4)

    proc = subprocess.Popen([BIN, conf], cwd=BUILD, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1)
    lines = []
    threading.Thread(target=lambda: [lines.append(r.decode('utf-8', 'replace').rstrip())
                                     for r in iter(proc.stdout.readline, b'')],
                     daemon=True).start()
    time.sleep(8)
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
    stop.set()
    os.close(master_fd)

    got = result.get('value')
    ok = (got == expect)
    print('  %-50s 回报=%-4s 期望=%-4s %s'
          % (name, got if got is not None else '无应答', expect, 'PASS' if ok else 'FAIL'))
    if not ok:
        for l in lines:
            if any(k in l for k in ('Calibration', 'calibration')):
                print('      | %s' % l)
    return ok


def main():
    print('===== 校准结果查询如实回报 验证（0=成功 1=失败 2=不可判定）=====\n')
    results = [
        run('遥测 calib_result=0 → 应回报 0(成功)', 0, {}, CALIB_SUCCESS),
        run('遥测 calib_result=1 → 应回报 1(失败)', 1, {}, CALIB_FAILURE),
        run('遥测 calib_result=5 → 应回报 1(失败)', 5, {}, CALIB_FAILURE),
        run('无遥测 → 应回报 2(不可判定)，不伪造成功', None, {}, CALIB_UNAVAILABLE),
        run('兼容开关=0 且遥测=1 → 应回报 0(旧行为)',
            1, {'CALIB_QUERY_REPORT_REAL': 0}, CALIB_SUCCESS),
    ]
    print('\n===== 汇总 =====')
    fails = sum(1 for ok in results if not ok)
    print('%s（失败 %d 项 / 共 %d 项）'
          % ('全部通过' if fails == 0 else '存在失败', fails, len(results)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
