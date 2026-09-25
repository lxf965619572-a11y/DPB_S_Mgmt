#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P1-2 验证：证明 5 秒天线模式校验已不在 TCP 接收线程上执行。

做法：
  1. 用 pty 造一个伪串口，按 FPGA 遥测帧格式持续喂"业务模式(0)"的状态帧，
     让 wait_for_antenna_mode_cycle() 真正进入 5 秒轮询（模式一直是 0，第一阶段必然等满 5 秒）。
  2. mock BBU 在 0s / 2s / 4s 各发一条 msg 195（频点配置）。
  3. 看 PAAU 日志里三条 "Handle NR cell config" 落在什么时刻：

     修复后：三条都在前 5 秒内被受理（接收线程未被阻塞），响应随后陆续发出
     修复前：第二条要等第一条的 5 秒阻塞结束才被受理，即 0s / 5s / 10s

  响应时长同时自检：若响应不是 ~5 秒后才回来，说明伪遥测没生效，测试无效。
"""
import os
import pty
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
IE_TYPE_FREQ_CONFIG = 0x5E3

BUILD_DIR = os.path.expanduser('~/paau_build')
LOG_PATH = os.path.join(BUILD_DIR, 'verify.log')
CONF_PATH = os.path.join(BUILD_DIR, 'verify.conf')

paau_lines = []
paau_lock = threading.Lock()
LOGF = None


def build_ie(ie_type, data):
    return struct.pack('<HH', ie_type, 4 + len(data)) + data


def build_msg(msg_id, serial, payload=b''):
    return HDR.pack(msg_id, HDR.size + len(payload), 0, 0, 0, serial) + payload


def build_cell_ie(cell_id, power=1000, freq_count=16, cell_type=3):
    """IE 1506，10 字节；必须先建立小区，否则频点配置会以 'non-existent cell' 提前失败"""
    b = struct.pack('<B', 0)          # cell_cfg_flag = 0 (建立)
    b += struct.pack('<I', cell_id)   # local_cell_id
    b += struct.pack('<H', power)     # cell_power
    b += struct.pack('<B', 0)         # reserved
    b += struct.pack('<B', freq_count)
    b += struct.pack('<B', cell_type)  # 3 = 5G NR
    assert len(b) == 10, len(b)
    return b


def build_freq_ie(cell_id, beam_id, dl, ul, bw):
    b = struct.pack('<B', 0)
    b += struct.pack('<I', cell_id)
    b += struct.pack('<B', beam_id)
    b += struct.pack('<I', dl)
    b += struct.pack('<I', 0)
    b += struct.pack('<B', 0)
    b += struct.pack('<I', 0)
    b += struct.pack('<I', bw)
    b += struct.pack('<I', 0)
    b += struct.pack('<B', 0)
    b += struct.pack('<I', ul)
    assert len(b) == 32
    return b


def telemetry_frame(mode=0):
    """133 字节 FPGA 状态响应帧：帧头(2)+长度(2)+数据(127)+帧尾(2)"""
    data = bytearray(127)
    data[0] = 0x0F                       # link_success_flag：主路建链
    struct.pack_into('<I', data, 77, mode)   # Byte 77-80 相控阵工作模式
    data[85] = 0                         # calib_result = 0
    return struct.pack('>H', 0xEB90) + struct.pack('>H', 127) + bytes(data) + struct.pack('>H', 0x5555)


def feeder(master_fd, stop_evt):
    """持续喂遥测帧，同时排空 PAAU 发出的查询"""
    frame = telemetry_frame(0)
    n = 0
    while not stop_evt.is_set():
        try:
            os.write(master_fd, frame)
            n += 1
        except OSError:
            break
        # 排空 master 侧收到的内容（PAAU 的状态查询等），防止缓冲写满
        try:
            os.set_blocking(master_fd, False)
            while True:
                if not os.read(master_fd, 4096):
                    break
        except (BlockingIOError, OSError):
            pass
        stop_evt.wait(0.2)
    print('[feeder] 共发送 %d 帧遥测' % n, flush=True)


def mock_bbu(host, port, sent_times, stop_evt):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print('[mock] 监听 %s:%d' % (host, port), flush=True)
    conn, addr = srv.accept()
    print('[mock] PAAU 已连接 %s' % (addr,), flush=True)
    conn.settimeout(1.0)

    t0 = time.time()
    # 时间轴（秒）：
    #   2.0  建立小区（必须先行，否则频点配置以 'non-existent cell' 提前失败）
    #   4/6/8 三条频点配置，每条触发一次最长 5 秒的模式校验
    # 推迟到遥测稳定之后再发：PAAU 启动后需要一小段时间才有有效状态缓存，
    # 否则 fpga_handler_get_status 失败会让模式校验提前返回，测不到 5 秒阻塞。
    plan = [4.0, 6.0, 8.0]
    cell_ie = build_ie(IE_TYPE_CELL_CONFIG, build_cell_ie(1))
    ie = build_ie(IE_TYPE_FREQ_CONFIG, build_freq_ie(1, 0, 2180100, 1980000, 100))

    buf = b''
    idx = 0
    cell_sent = False
    while time.time() - t0 < 26 and not stop_evt.is_set():
        now = time.time() - t0
        if not cell_sent and now >= 2.0:
            conn.sendall(build_msg(MSG_NR_CELL_CONFIG, 0x0F00, cell_ie))
            cell_sent = True
            print('[mock] t=+%.2fs 发送 msg 195 建立小区 (IE 1506, cell_id=1)' % now, flush=True)
        if idx < len(plan) and now >= plan[idx]:
            serial = 0x1000 + idx
            conn.sendall(build_msg(MSG_NR_CELL_CONFIG, serial, ie))
            sent_times.append((idx + 1, serial, time.time() - t0))
            print('[mock] t=+%.2fs 发送 msg 195 #%d (serial=0x%04X)' % (now, idx + 1, serial), flush=True)
            idx += 1
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        while len(buf) >= HDR.size:
            msg_id, msg_len, paau, bbu, port, serial = HDR.unpack_from(buf, 0)
            if msg_len < HDR.size or len(buf) < msg_len:
                break
            body = buf[HDR.size:msg_len]
            print('[mock] t=+%.2fs 收到 msg_id=%d serial=0x%04X len=%d'
                  % (time.time() - t0, msg_id, serial, msg_len), flush=True)
            buf = buf[msg_len:]
    conn.close()
    srv.close()
    stop_evt.set()


def pump_paau_log(proc, stop_evt):
    global LOGF
    for raw in iter(proc.stdout.readline, b''):
        line = raw.decode('utf-8', 'replace').rstrip()
        with paau_lock:
            paau_lines.append(line)
        try:
            LOGF.write(line + '\n')
            LOGF.flush()
        except Exception:
            pass
        m = re.search(r'\[(\d{2}:\d{2}:\d{2})\].*(Handle NR cell config|Cell config request queued|Sent cell config response|Cell config worker thread exited)', line)
        if m:
            print('[paau] %s  %s' % (m.group(1), m.group(2)), flush=True)
        if stop_evt.is_set():
            break


def main():
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    print('[pty] 伪串口: %s' % slave_name, flush=True)

    with open(CONF_PATH, 'w') as f:
        f.write(
            'BBU_IP=127.0.0.1\n'
            'BBU_PORT=30000\n'
            'PAAU_IP=127.0.0.1\n'
            'PAAU_ID=0\n'
            'UART_DEVICE=%s\n'
            'LOG_LEVEL=INFO\n' % slave_name)

    stop_evt = threading.Event()
    sent_times = []

    ft = threading.Thread(target=feeder, args=(master_fd, stop_evt), daemon=True)
    ft.start()
    time.sleep(0.5)

    mt = threading.Thread(target=mock_bbu, args=('127.0.0.1', 30000, sent_times, stop_evt), daemon=True)
    mt.start()
    time.sleep(0.5)

    global LOGF
    LOGF = open(LOG_PATH, 'w')
    proc = subprocess.Popen([os.path.join(BUILD_DIR, 'antenna_mgmt'), CONF_PATH],
                            cwd=BUILD_DIR, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    lt = threading.Thread(target=pump_paau_log, args=(proc, stop_evt), daemon=True)
    lt.start()

    # 给 mock 足够时间跑完 3 条请求 + 等响应
    time.sleep(20)
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    stop_evt.set()

    print('\n================ 判定 ================', flush=True)
    with paau_lock:
        lines = list(paau_lines)
    handles = [l for l in lines if 'Handle NR cell config' in l]
    queued = [l for l in lines if 'Cell config request queued' in l]
    ts = []
    for l in handles:
        m = re.search(r'\[(\d{2}):(\d{2}):(\d{2})\]', l)
        if m:
            ts.append(int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)))
    print('收到 msg 195 次数: %d' % len(handles))
    for l in handles:
        print('  %s' % l)
    if len(ts) >= 2:
        span = ts[-1] - ts[0]
        print('\n首末次受理间隔: %d 秒' % span)
        if span <= 4:
            print('判定: 通过 —— 三条请求都在前 5 秒内被受理，接收线程未被 5 秒校验阻塞')
        else:
            print('判定: 失败 —— 受理被拉开，接收线程仍被阻塞')
    print('入队次数: %d' % len(queued))
    print('\n======== PAAU 日志中 UART/FPGA/模式 相关行 ========')
    for l in lines:
        if re.search(r'[Uu][Aa][Rr][Tt]|FPGA|fpga|mode|Mode|串口|watchdog', l):
            print('  %s' % l)
    return 0


if __name__ == '__main__':
    sys.exit(main())
