#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""基带失联关发射 端到端验证。

mock BBU(CPRI over TCP) + 假 FPGA(pty)，跑真实二进制，断言：
  A. 通道建立 + 基带下发频点后，天线收到"打开发射"（0x0B 载荷=1）
  B. TCP 断开后，天线收到"关闭发射"（0x0B 载荷=0）—— 这是本次要验的核心行为
  C. 关发射只发一次（发→关跳变去重），不会因重连抖动反复下发
  D. 日志里能看出失联原因

前置条件说明：要让发射被打开，必须
  1. 回一条 MSG_CHANNEL_SETUP_CFG(2)，否则"允许开发射"的闸门不开（见
     cell_config_notify_bbu_link 的设计）；
  2. 先建小区再配频点，因为 apply_freq 要靠 local_cell_id 找到小区槽位，
     找不到就不会把 sent_to_antenna 置真，也就不会开射。
"""
import argparse
import os
import pty
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import time
import tty

HDR = struct.Struct("<IIBBBI")

MSG_CHANNEL_SETUP_REQ = 1
MSG_CHANNEL_SETUP_CFG = 2
MSG_NR_CELL_CONFIG = 195
MSG_NR_CELL_CONFIG_RSP = 196
IE_TYPE_CELL_CONFIG = 0x5E2
IE_TYPE_FREQ_CONFIG = 0x5E3

FPGA_HDR = bytes([0xEB, 0x90])
FPGA_TAIL = bytes([0x55, 0x55])
MSG_STATUS_QUERY = 0xFF
FPGA_MSG_TX_CONTROL = 0x0B

BBU_PORT = 40002
FAILURES = []
CHECKS = 0


def check(ok, label, detail=""):
    global CHECKS
    CHECKS += 1
    print("  [%s] %s  %s" % ("PASS" if ok else "FAIL", label, detail if not ok else ""))
    if not ok:
        FAILURES.append(label)
    return ok


def build_ie(ie_type, data):
    return struct.pack("<HH", ie_type, 4 + len(data)) + data


def build_msg(msg_id, serial, payload=b""):
    return HDR.pack(msg_id, HDR.size + len(payload), 0, 0, 0, serial) + payload


def build_cell_ie(cell_id, cell_type=3):
    """cell_config_ie_t，10 字节：flag(1)+cell_id(4)+power(2)+reserved(1)+freq_count(1)+type(1)"""
    b = struct.pack("<B", 0)             # cell_cfg_flag = 0 (建立)
    b += struct.pack("<I", cell_id)
    b += struct.pack("<H", 20000)        # cell_power (1/256 dBm)
    b += struct.pack("<B", 0)            # reserved
    b += struct.pack("<B", 1)            # freq_count
    b += struct.pack("<B", cell_type)
    assert len(b) == 10, len(b)
    return b


def build_freq_ie(cell_id, beam_id, dl, ul, bw):
    """与 mock_bbu_test.py / cell_config_parse_freq_ie 逐字节对齐，32 字节"""
    b = struct.pack("<B", 0)             # freq_cfg_flag = 0 (建立)
    b += struct.pack("<I", cell_id)
    b += struct.pack("<B", beam_id)
    b += struct.pack("<I", dl)
    b += struct.pack("<I", 0)            # reserved1
    b += struct.pack("<B", 0)            # special_subframe
    b += struct.pack("<I", 0)            # sys_subframe_num
    b += struct.pack("<I", bw)
    b += struct.pack("<I", 0)            # ul_dl_config
    b += struct.pack("<B", 0)            # reserved2
    b += struct.pack("<I", ul)
    assert len(b) == 32, len(b)
    return b


def status_frame(link_flag=0x0F, work_mode=0):
    """0xEB90 状态帧。

    work_mode 必须是 0(业务)：cell_config_apply_freq 在 mode 不是业务/频谱时会直接
    拒收频点（"Antenna in mode %u (not business/spectrum), reject freq config"），
    频点只存本地不下发，发射也就不会打开 —— 别为了省时间改成 2(待机)。

    代价是 wait_for_antenna_mode_cycle 会走满 5 秒（模式始终不变，超时后按"可能天线
    已经配置好了"返回 SUCCESS），所以从下发频点到看到开射大约要 5~6 秒。"""
    d = bytearray(127)
    d[0] = link_flag
    d[77:81] = struct.pack(">I", work_mode)
    return FPGA_HDR + bytes([0x00, 0x7F]) + bytes(d) + FPGA_TAIL


def parse_downlink(buf):
    out = []
    i = 0
    while True:
        j = buf.find(FPGA_HDR, i)
        if j < 0 or j + 4 > len(buf):
            break
        total = 6 + buf[j + 3]
        if j + total > len(buf):
            break
        if buf[j + total - 2:j + total] != FPGA_TAIL:
            i = j + 1
            continue
        out.append((buf[j + 2], buf[j + 4:j + 4 + buf[j + 3]]))
        i += total
    return out, i


def parse_cpri(buf):
    out = []
    i = 0
    while i + HDR.size <= len(buf):
        msg_id, msg_len, paau, bbu, port, serial = HDR.unpack_from(buf, i)
        if msg_len < HDR.size or msg_len > 65536:
            i += 1
            continue
        if i + msg_len > len(buf):
            break
        out.append((msg_id, serial, buf[i + HDR.size:i + msg_len]))
        i += msg_len
    return out, i


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./antenna_mgmt")
    ap.add_argument("--workdir", default="/tmp/tx_verify")
    args = ap.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("找不到二进制: %s" % binary)
        return 2
    tmp = os.path.abspath(args.workdir)
    shutil.rmtree(tmp, ignore_errors=True)
    logs = os.path.join(tmp, "logs")
    os.makedirs(logs, exist_ok=True)
    print("二进制: %s\n工作目录: %s" % (binary, tmp))

    master, slave = pty.openpty()
    tty.setraw(slave)
    dev = os.ttyname(slave)

    cfg = os.path.join(tmp, "test.conf")
    with open(cfg, "w") as f:
        f.write(
            "BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n"
            "UART_DEVICE=%s\nLOG_LEVEL=DEBUG\nLOG_FILE=%s\n"
            "LOG_ROTATE_ON_STARTUP=0\nRECONNECT_INTERVAL_MS=30000\n"
            "TELEMETRY_LOG_FILE=%s\nTELEMETRY_LOG_POLL_SEC=600\n"
            "ALARM_LINK_GRACE_SEC=600\nPAAU_ID_CHECK=0\n"
            % (BBU_PORT, dev, os.path.join(logs, "text.log"),
               os.path.join(logs, "telemetry.bin")))

    out_f = open(os.path.join(tmp, "stdout.log"), "wb")
    proc = subprocess.Popen([binary, cfg], cwd=tmp, stdout=out_f, stderr=subprocess.STDOUT)
    srv = None
    try:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", BBU_PORT))
        srv.listen(2)
        srv.settimeout(20)
        print("\n[环境] pty=%s  BBU监听=%d" % (dev, BBU_PORT))

        conn, _ = srv.accept()
        conn.settimeout(0.3)
        print("[环境] PAAU 已连上")

        rx_fpga = b""
        rx_bbu = b""
        tx_events = []          # (时间, 载荷) —— 0x0B 帧按顺序记录
        sent_cell = False
        got_rsp = False
        disconnected = False
        t0 = time.time()

        while time.time() - t0 < 40:
            # conn 关闭后不能再进 select（fd 已失效，会抛 ValueError），断开后就只看 UART
            rlist = [master] if disconnected else [master, conn]
            r, _, _ = select.select(rlist, [], [], 0.1)

            if master in r:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    chunk = b""
                rx_fpga += chunk
                msgs, used = parse_downlink(rx_fpga)
                rx_fpga = rx_fpga[used:]
                for msg_id, payload in msgs:
                    if msg_id == MSG_STATUS_QUERY:
                        os.write(master, status_frame())
                    elif msg_id == FPGA_MSG_TX_CONTROL:
                        tx_events.append((time.time() - t0, payload[0] if payload else None))
                        print("[环境] t=+%.2fs 收到发射控制 0x0B 载荷=%s"
                              % (tx_events[-1][0], payload[0] if payload else None))

            if not disconnected and conn in r:
                try:
                    chunk = conn.recv(65536)
                except socket.timeout:
                    chunk = b""
                if chunk:
                    rx_bbu += chunk
                    msgs, used = parse_cpri(rx_bbu)
                    rx_bbu = rx_bbu[used:]
                    for msg_id, serial, payload in msgs:
                        if msg_id == MSG_CHANNEL_SETUP_REQ:
                            # 回通道建立配置，闸门才开
                            conn.sendall(build_msg(MSG_CHANNEL_SETUP_CFG, serial))
                            print("[环境] 已回 MSG_CHANNEL_SETUP_CFG(2)")
                        elif msg_id == MSG_NR_CELL_CONFIG_RSP:
                            got_rsp = True
                            print("[环境] 收到小区配置响应 196")

            # 通道建立后，下发"建小区 + 配频点"
            if not sent_cell and time.time() - t0 > 1.5:
                ie = build_ie(IE_TYPE_CELL_CONFIG, build_cell_ie(1))
                ie += build_ie(IE_TYPE_FREQ_CONFIG, build_freq_ie(1, 0, 2180100, 1980000, 100))
                conn.sendall(build_msg(MSG_NR_CELL_CONFIG, 0x2001, ie))
                print("[环境] 已下发 msg195（小区IE + 频点IE）")
                sent_cell = True

            # 拿到响应并确认已开射之后，模拟基带失联
            if got_rsp and not disconnected and tx_events and tx_events[-1][1] == 1:
                time.sleep(0.5)
                print("[环境] 模拟基带失联：关闭 TCP 连接")
                conn.close()
                disconnected = True
                t_disc = time.time()

            if disconnected and time.time() - t_disc > 3:
                break

        # ---------- 断言 ----------
        print("\n[A] 建立通道 + 下发频点后开射")
        on_events = [e for e in tx_events if e[1] == 1]
        check(len(on_events) >= 1, "天线收到了打开发射(0x0B=1)",
              "事件序列=%s" % tx_events)
        check(got_rsp, "收到小区配置响应 196（频点配置被受理）")

        print("\n[B] TCP 断开后关发射")
        off_events = [e for e in tx_events if e[1] == 0]
        check(len(off_events) >= 1, "天线收到了关闭发射(0x0B=0)",
              "事件序列=%s" % tx_events)
        if on_events and off_events:
            check(off_events[0][0] > on_events[0][0], "关发射发生在开发射之后",
                  "on=%.2fs off=%.2fs" % (on_events[0][0], off_events[0][0]))
            check(off_events[0][0] - t_disc < 2.5,
                  "断开后 2.5 秒内即下发关发射",
                  "延迟=%.2fs" % (off_events[0][0] - t_disc))

        print("\n[C] 去重：关发射只发一次")
        check(len(off_events) == 1, "关发射正好下发 1 次（发→关跳变去重）",
              "实际 %d 次: %s" % (len(off_events), off_events))

        print("\n[D] 日志")
        log = open(os.path.join(logs, "text.log"), "rb").read().decode("utf-8", "replace")
        check("BBU link lost (TCP disconnected from BBU)" in log,
              "日志记录了失联原因", "")
        check("TX disabled (BBU link lost)" in log, "日志记录了关闭发射", "")
        check("TX enabled (at least one cell has freqs)" in log,
              "日志记录了开射及其理由", "")
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        out_f.close()
        if srv:
            srv.close()
        for fd in (master, slave):
            try:
                os.close(fd)
            except OSError:
                pass

    print("\n" + "=" * 60)
    if FAILURES:
        print("结果: 失败 %d / 共 %d 项检查" % (len(FAILURES), CHECKS))
        for f in FAILURES:
            print("  - %s" % f)
        print("\n--- 程序日志尾部 ---")
        print(open(os.path.join(logs, "text.log"), "rb").read()
              .decode("utf-8", "replace")[-2500:])
        return 1
    print("结果: 全部通过（%d 项检查）" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
