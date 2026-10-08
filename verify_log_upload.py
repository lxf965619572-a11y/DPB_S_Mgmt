#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""日志上传（打包）端到端验证。

mock BBU(CPRI over TCP) + 假 FPGA(pty) + 本地 FTP 服务端，跑真实二进制，断言：
  A. 收到 MsgID 131 后立刻回 ACK(132/IE1205) result=0，不等上传完成
  B. 上传完成后回【一条】结果指示(133/IE1211) result=0
  C. IE 1211 里的 file_name 是 PAAU<MMDD>.tgz（<=15 字符，未被截断）
  D. FTP 服务端上确实收到该压缩包
  E. 压缩包解开后含两个文件：文本日志 + 遥测日志
  F. 包内遥测日志是完整的帧流（整帧边界，未被截断）
  G. 上传完成后本地临时目录不残留快照与压缩包
  H. 上传期间再来一条 131 会被 ACK=1 拒绝

需要 pyftpdlib（pip3 install --user --break-system-packages pyftpdlib）。
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
import tarfile
import threading
import time
import tty
from datetime import datetime

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer

# ---- CPRI ----
MSG_LOG_UPLOAD_REQ = 131
MSG_LOG_UPLOAD_ACK = 132
MSG_LOG_UPLOAD_RESULT = 133
IE_LOG_UPLOAD_REQ = 0x04B1
IE_LOG_UPLOAD_ACK = 0x04B5
IE_LOG_UPLOAD_RESULT = 0x04BB
LOG_UPLOAD_SUCCESS = 0

CPRI = struct.Struct("<IIBBBI")

# ---- FPGA ----
FPGA_HDR = bytes([0xEB, 0x90])
FPGA_TEL = bytes([0xEB, 0x93])
FPGA_TAIL = bytes([0x55, 0x55])
MSG_STATUS_QUERY = 0xFF
POLL_LEN = {0xA1: 127, 0xB1: 146, 0xB2: 146}

BBU_PORT = 40001
FTP_PORT = 2121

# 刻意用与默认值不同的文件名：归档内的成员名应当跟随这两个配置项，
# 而不是写死成 antenna_mgmt.log / telemetry.bin。若有人改回写死，下面的断言会失败。
TEXT_BASENAME = "paau_text.log"
TEL_BASENAME = "paau_tel.bin"

FAILURES = []
CHECKS = 0


def check(ok, label, detail=""):
    global CHECKS
    CHECKS += 1
    if ok:
        print("  [PASS] %s" % label)
    else:
        print("  [FAIL] %s  %s" % (label, detail))
        FAILURES.append(label)
    return ok


def build_ie(ie_type, data):
    """IE 长度字段包含 IE 头自身（见 cpri_ie_header_t 注释）"""
    return struct.pack("<HH", ie_type, 4 + len(data)) + data


def build_msg(msg_id, serial, payload=b""):
    return CPRI.pack(msg_id, CPRI.size + len(payload), 0, 0, 0, serial) + payload


def parse_cpri(buf):
    """切出所有完整 CPRI 消息，返回 ([(msg_id, serial, payload)], 已消费长度)"""
    out = []
    i = 0
    while i + CPRI.size <= len(buf):
        msg_id, msg_len, paau, bbu, port, serial = CPRI.unpack_from(buf, i)
        if msg_len < CPRI.size or msg_len > 65536:
            i += 1
            continue
        if i + msg_len > len(buf):
            break
        out.append((msg_id, serial, buf[i + CPRI.size:i + msg_len]))
        i += msg_len
    return out, i


def tel_frame(src, seq):
    n = POLL_LEN[src]
    p = bytearray(n)
    for k in range(n):
        p[k] = (seq * 31 + k * 7) & 0xFF
    p[0:4] = bytes([0xEB, 0x93, 0xA1, 0x7F])
    p[10:12] = FPGA_TAIL
    return FPGA_TEL + bytes([src, n]) + bytes(p) + FPGA_TAIL


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


def parse_tel_stream(data):
    frames = []
    i = 0
    while i < len(data):
        if data[i:i + 2] != FPGA_TEL:
            return frames, "偏移 %d 处不是 0xEB93 帧头" % i
        n = data[i + 3]
        total = 6 + n
        if i + total > len(data):
            return frames, "偏移 %d 处帧不完整" % i
        if data[i + total - 2:i + total] != FPGA_TAIL:
            return frames, "偏移 %d 处帧尾错误" % i
        frames.append(data[i:i + total])
        i += total
    return frames, None


class FtpServer:
    def __init__(self, root):
        self.root = root
        os.makedirs(root, exist_ok=True)
        authorizer = DummyAuthorizer()
        authorizer.add_anonymous(root, perm="elradfmwMT")
        handler = FTPHandler
        handler.authorizer = authorizer
        handler.log_prefix = "[ftp] "
        self.server = FTPServer(("127.0.0.1", FTP_PORT), handler)
        self.t = threading.Thread(target=self.server.serve_forever,
                                  kwargs={"timeout": None, "blocking": True},
                                  daemon=True)

    def start(self):
        self.t.start()

    def stop(self):
        try:
            self.server.close_all()
        except Exception:
            pass


def wait_port(host, port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./antenna_mgmt")
    ap.add_argument("--workdir", default="/tmp/upload_verify")
    args = ap.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("找不到二进制: %s" % binary)
        return 2
    tmp = os.path.abspath(args.workdir)
    shutil.rmtree(tmp, ignore_errors=True)
    logs = os.path.join(tmp, "logs")
    ftproot = os.path.join(tmp, "ftproot")
    os.makedirs(logs, exist_ok=True)
    print("二进制: %s\n工作目录: %s" % (binary, tmp))

    ftp = FtpServer(ftproot)
    ftp.start()

    # ---------- 假 FPGA 串口 ----------
    master, slave = pty.openpty()
    tty.setraw(slave)
    dev = os.ttyname(slave)

    cfg = os.path.join(tmp, "test.conf")
    with open(cfg, "w") as f:
        f.write(
            "BBU_IP=127.0.0.1\nBBU_PORT=%d\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n"
            "UART_DEVICE=%s\nLOG_LEVEL=DEBUG\nLOG_FILE=%s\n"
            "LOG_ROTATE_ON_STARTUP=0\n"
            "TELEMETRY_LOG_FILE=%s\nTELEMETRY_LOG_MAX_SIZE=10485760\n"
            "TELEMETRY_LOG_MAX_BACKUPS=3\nTELEMETRY_LOG_POLL_SEC=1\n"
            "TELEMETRY_LOG_POLL_GAP_MS=20\nALARM_LINK_GRACE_SEC=10\n"
            "PAAU_ID_CHECK=0\nFTP_SERVER=127.0.0.1\nFTP_PORT=%d\n"
            % (BBU_PORT, dev, os.path.join(logs, TEXT_BASENAME),
               os.path.join(logs, TEL_BASENAME), FTP_PORT))

    out_f = open(os.path.join(tmp, "stdout.log"), "wb")
    proc = subprocess.Popen([binary, cfg], cwd=tmp, stdout=out_f,
                            stderr=subprocess.STDOUT)

    exit_code = None
    try:
        # ---------- 建 BBU 侧监听 ----------
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", BBU_PORT))
        srv.listen(2)
        srv.settimeout(20)
        print("\n[环境] FTP=127.0.0.1:%d  pty=%s  BBU监听=%d" % (FTP_PORT, dev, BBU_PORT))

        conn, _ = srv.accept()
        conn.settimeout(0.5)
        print("[环境] PAAU 已连上 BBU 端口")

        # ---------- 一边扮 FPGA 回遥测，一边等 TCP ----------
        rx_fpga = b""
        rx_bbu = b""
        sent_frames = []
        seq = 0
        t0 = time.time()
        stage = "fill"        # fill -> request
        acks = {}             # serial_num -> ACK 的 result
        results = {}          # serial_num -> IE1211 payload
        ack_time = None
        req_time = None

        while time.time() - t0 < 60:
            r, _, _ = select.select([master, conn], [], [], 0.1)

            if master in r:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    chunk = b""
                rx_fpga += chunk
                msgs, used = parse_downlink(rx_fpga)
                rx_fpga = rx_fpga[used:]
                for msg_id, _ in msgs:
                    if msg_id == MSG_STATUS_QUERY:
                        # 状态帧：link 位给 0x0F，避免触发 1097 干扰日志判读
                        d = bytearray(127)
                        d[0] = 0x0F
                        os.write(master, FPGA_HDR + bytes([0x00, 0x7F]) + bytes(d) + FPGA_TAIL)
                    elif msg_id in POLL_LEN and stage == "fill":
                        seq += 1
                        fr = tel_frame(msg_id, seq)
                        sent_frames.append(fr)
                        os.write(master, fr)

            if conn in r:
                try:
                    chunk = conn.recv(65536)
                except socket.timeout:
                    chunk = b""
                if chunk:
                    rx_bbu += chunk
                    msgs, used = parse_cpri(rx_bbu)
                    rx_bbu = rx_bbu[used:]
                    for msg_id, serial, payload in msgs:
                        if msg_id == MSG_LOG_UPLOAD_ACK:
                            acks[serial] = payload[4]
                            if serial == 0x1234 and req_time:
                                ack_time = time.time() - req_time
                        elif msg_id == MSG_LOG_UPLOAD_RESULT:
                            results[serial] = payload

            # 填够 3 帧遥测后发上传请求
            if stage == "fill" and len(sent_frames) >= 3:
                time.sleep(0.3)
                store = b"upload"
                ie = build_ie(IE_LOG_UPLOAD_REQ, store + b"\x00" * (200 - len(store)))
                req_time = time.time()
                conn.sendall(build_msg(MSG_LOG_UPLOAD_REQ, 0x1234, ie))
                # 紧接着再发一条：第一次上传此刻必然还在进行（cp/tar/curl 每一步都要 fork
                # 进程，量级在几十毫秒），所以这一条应当被单任务互斥拒绝。
                # 不要在这里 sleep —— 睡过头第一次就做完了，测不到互斥。
                conn.sendall(build_msg(MSG_LOG_UPLOAD_REQ, 0x1235, ie))
                print("[环境] 已连续发出两条 MsgID 131 (serial=0x1234 / 0x1235)")
                stage = "request"

            if 0x1234 in results:
                break

        # ---------- 断言 ----------
        print("\n[A-C] 报文应答")
        check(0x1234 in acks, "收到 ACK(132/IE1205)")
        check(acks.get(0x1234) == 0, "第一条 131 的 ACK result=0（接受）",
              "实际=%s" % acks.get(0x1234))
        check(ack_time is not None and ack_time < 2.0,
              "ACK 在上传完成前就回了（未阻塞在打包上）",
              "耗时=%s" % ack_time)
        check(0x1235 in acks, "第二条 131 收到了应答")
        check(acks.get(0x1235) == 1, "上传进行中的第二条 131 被拒绝(ACK result=1)",
              "实际=%s" % acks.get(0x1235))

        result_payload = results.get(0x1234)
        check(result_payload is not None, "收到结果指示(133/IE1211)")
        if result_payload is None:
            raise SystemExit("未收到结果指示，后续断言跳过")
        check(len(results) == 1, "只回了一条结果指示（两次请求只受理一次）",
              "实际收到 %d 条" % len(results))
        ie_type, ie_len = struct.unpack_from("<HH", result_payload, 0)
        res = result_payload[4]
        store_path = result_payload[5:205].split(b"\x00")[0].decode()
        file_name = result_payload[205:221].split(b"\x00")[0].decode()
        print("  [info] IE1211: type=0x%04X len=%d result=%d store=%s file=%s"
              % (ie_type, ie_len, res, store_path, file_name))
        check(res == LOG_UPLOAD_SUCCESS, "结果 result=0（上传成功）", "实际=%d" % res)
        expect_name = "PAAU%02d%02d.tgz" % (datetime.now().month, datetime.now().day)
        check(file_name == expect_name, "file_name = %s" % expect_name,
              "实际=%s" % file_name)
        check(len(file_name) <= 15, "file_name 未超 IE 的 16 字节字段",
              "len=%d" % len(file_name))

        print("\n[D-F] FTP 上的压缩包")
        pkg = os.path.join(ftproot, "upload", expect_name)
        check(os.path.exists(pkg), "FTP 服务端收到了 %s" % expect_name, pkg)
        if os.path.exists(pkg):
            size = os.path.getsize(pkg)
            check(size > 0, "压缩包非空", "%d 字节" % size)
            try:
                with tarfile.open(pkg, "r:gz") as tf:
                    names = sorted(tf.getnames())
                    print("  [info] 包内成员: %s" % names)
                    check(names == sorted([TEXT_BASENAME, TEL_BASENAME]),
                          "包内成员名跟随 LOG_FILE / TELEMETRY_LOG_FILE 的 basename",
                          "实际=%s, 期望=%s" % (names, sorted([TEXT_BASENAME, TEL_BASENAME])))
                    tel = tf.extractfile(TEL_BASENAME).read()
                    txt = tf.extractfile(TEXT_BASENAME).read()
                check(len(txt) > 0, "包内文本日志非空", "%d 字节" % len(txt))
                frames, err = parse_tel_stream(tel)
                check(err is None and len(frames) >= 3,
                      "包内遥测日志是完整帧流（整帧边界，未截断）",
                      "err=%s n=%d" % (err, len(frames)))
                check(tel == b"".join(sent_frames),
                      "包内遥测日志与发出的帧逐字节一致",
                      "%d vs %d" % (len(tel), sum(len(x) for x in sent_frames)))
            except tarfile.TarError as e:
                check(False, "压缩包可解且是 gzip 格式", repr(e))

        print("\n[G] 临时目录清理")
        tmpdir = os.path.join(tmp, "logs", ".upload_tmp")
        leftovers = os.listdir(tmpdir) if os.path.isdir(tmpdir) else []
        check(leftovers == [], "本地临时目录无残留", str(leftovers))

        log = open(os.path.join(logs, TEXT_BASENAME), "rb").read().decode("utf-8", "replace")
        check("Uploading log package" in log, "日志里有打包上传记录")
        check("Telemetry log snapshot created" in log, "遥测日志快照已创建")
    finally:
        try:
            proc.terminate()
            exit_code = proc.wait(timeout=10)
        except Exception:
            proc.kill()
        out_f.close()
        try:
            srv.close()
        except Exception:
            pass
        ftp.stop()
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
        print(open(os.path.join(logs, TEXT_BASENAME), "rb").read().decode("utf-8", "replace")[-3000:])
        return 1
    print("结果: 全部通过（%d 项检查）" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
