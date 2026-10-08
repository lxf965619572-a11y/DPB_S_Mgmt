#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""遥测日志(0xEB93)端到端验证。

用 pty 伪造 FPGA 串口，跑真实二进制，不需要真实 FPGA / BBU / FTP，也不需要 socat。

覆盖的断言：
  A. 轮询线程按周期下发 0xA1/0xB1/0xB2，且封装与每秒状态查询同构（各 1 字节载荷 0x00）
  B. 回复的 0xEB93 帧被【逐字节】原样落盘到遥测日志
  C. 落盘的每一帧结构合法（帧头 / 来源 / 长度 / 帧尾），且不同帧不粘连
  D. 0xEB90 状态帧路径未被分帧改造破坏（靠 1097 告警被触发来证明，见下）
  E. 按大小轮转生效，且轮转后新文件继续正常追加
  F. 帧头跨两次 write 断开时仍能拼回（扫描循环 remaining==1 的修复）
  G. 帧前灌乱码后仍能重新同步，不丢后续帧
  H. 二进制安全：载荷含 0x00 / 0x0A / EB93 / EB90 / 5555 也不被破坏，且日志里无文本污染
  I. TELEMETRY_LOG_POLL_SEC 真的可配置：改配置值会改变实际下发间隔

D 的证明方式：把 ALARM_LINK_GRACE_SEC 设为 0（关闭宽限期），并让伪造的 0xEB90 状态帧
里 link_success_flag 的 bit0-3 全为 0。这样只要状态帧被正确解析并喂给 alarm_periodic_check，
就会打出 4 条 "Alarm triggered: ... (code=1097, sub=0..3)"。没有这些日志就说明
0xEB90 分帧路径坏了 —— 而它承载着全部告警判断。
"""
import argparse
import os
import pty
import re
import shutil
import struct
import subprocess
import sys
import termios
import time
import tty

HDR = bytes([0xEB, 0x90])
TEL = bytes([0xEB, 0x93])
TAIL = bytes([0x55, 0x55])

MSG_STATUS_QUERY = 0xFF
POLL_LEN = {0xA1: 127, 0xB1: 146, 0xB2: 146}   # 来源/命令码 -> 载荷长度
POLL_ORDER = [0xA1, 0xB1, 0xB2]

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


# ---------------------------------------------------------------- 组帧

def downlink_cmd(msg_id, payload=b"\x00"):
    """程序发出的下行帧：帧头(2) + msg_id(1) + 长度(1) + 载荷 + 帧尾(2)"""
    return HDR + bytes([msg_id, len(payload)]) + payload + TAIL


def make_payload(n, tag):
    """确定性载荷。刻意埋入会被误认成帧头/帧尾的字节序列，用来验证分帧与二进制安全。"""
    p = bytearray(n)
    for i in range(n):
        p[i] = (tag * 31 + i * 7) & 0xFF
    p[0:4] = bytes([0xEB, 0x93, 0xA1, 0x7F])   # 一个完整的"假帧头"
    p[10:12] = TAIL                            # 假帧尾
    p[20:22] = HDR                             # 假 0xEB90 帧头
    p[30] = 0x00                               # NUL，证明按二进制而非文本处理
    p[31] = 0x0A                               # 换行，同上
    return bytes(p)


def tel_frame(src, seq):
    """FPGA 回复的遥测日志帧：帧头(2) + 来源(1) + 长度(1) + 数据(N) + 帧尾(2)"""
    n = POLL_LEN[src]
    return TEL + bytes([src, n]) + make_payload(n, seq) + TAIL


def status_frame(link_flag=0x00):
    """0xEB90 状态帧：帧头(2) + 长度(2,大端=0x007F) + 数据(127) + 帧尾(2)。
    注意长度字段是 2 字节，这是 0xEB90 与 0xEB93 的关键差异。"""
    d = bytearray(127)
    d[0] = link_flag                       # link_success_flag
    d[77:81] = struct.pack(">I", 0)        # phased_array_work_mode = 0 (业务)
    return HDR + bytes([0x00, 0x7F]) + bytes(d) + TAIL


def parse_downlink(buf):
    """切出缓冲区里所有完整的下行帧，返回 ([(msg_id, payload)], 已消费字节数)"""
    out = []
    i = 0
    while True:
        j = buf.find(HDR, i)
        if j < 0 or j + 4 > len(buf):
            break
        total = 6 + buf[j + 3]
        if j + total > len(buf):
            break
        if buf[j + total - 2:j + total] != TAIL:
            i = j + 1
            continue
        out.append((buf[j + 2], buf[j + 4:j + 4 + buf[j + 3]]))
        i = j + total
    return out, i


# ---------------------------------------------------------------- 测试台

class Harness:
    def __init__(self, binary, workdir, poll_sec=1, max_size=0, grace=0,
                 backups=3, split_hdr=False, garbage=0, quiet_status=False):
        self.binary = binary
        self.workdir = workdir
        self.poll_sec = poll_sec
        self.max_size = max_size
        self.grace = grace
        self.backups = backups
        self.split_hdr = split_hdr
        self.garbage = garbage
        self.quiet_status = quiet_status
        self.proc = None
        self.master = None
        self.slave = None
        self.rx = b""
        self.sent_frames = []      # 按序记录伪造 FPGA 发出去的遥测帧
        self.seen_polls = []       # 收到的轮询命令（去重后的原始消息）
        self.tel_path = os.path.join(workdir, "logs", "telemetry.bin")
        self.text_path = os.path.join(workdir, "logs", "text.log")
        self.stdout = os.path.join(workdir, "stdout.log")

    def start(self):
        os.makedirs(os.path.join(self.workdir, "logs"), exist_ok=True)
        self.master, self.slave = pty.openpty()
        # 立刻把 pty 置为 raw：程序自己也会设，但在它 open 之前若处于 canonical 模式，
        # 二进制帧可能被行规程改写/回显
        tty.setraw(self.slave)
        dev = os.ttyname(self.slave)

        cfg = os.path.join(self.workdir, "test.conf")
        with open(cfg, "w") as f:
            f.write(
                "BBU_IP=127.0.0.1\nBBU_PORT=39999\nPAAU_IP=127.0.0.1\nPAAU_ID=0\n"
                "UART_DEVICE=%s\nLOG_LEVEL=DEBUG\nLOG_FILE=%s\n"
                "LOG_ROTATE_ON_STARTUP=0\nLOG_MAX_SIZE=10485760\nLOG_MAX_BACKUPS=2\n"
                "TELEMETRY_LOG_FILE=%s\nTELEMETRY_LOG_MAX_SIZE=%d\n"
                "TELEMETRY_LOG_MAX_BACKUPS=%d\nTELEMETRY_LOG_POLL_SEC=%d\n"
                "TELEMETRY_LOG_POLL_GAP_MS=20\nALARM_LINK_GRACE_SEC=%d\n"
                "PAAU_ID_CHECK=0\nFTP_SERVER=127.0.0.1\nFTP_PORT=21\n"
                % (dev, self.text_path, self.tel_path, self.max_size,
                   self.backups, self.poll_sec, self.grace))

        print("  [info] pty=%s 启动 %s" % (dev, self.binary))
        self.out_f = open(self.stdout, "wb")
        self.proc = subprocess.Popen([self.binary, cfg], cwd=self.workdir,
                                     stdout=self.out_f, stderr=subprocess.STDOUT)
        time.sleep(0.4)

    def serve(self, seconds, stop_after_frames=None):
        """扮演 FPGA：读下行帧并回复。"""
        deadline = time.time() + seconds
        seq = 0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                break
            try:
                import select
                r, _, _ = select.select([self.master], [], [], 0.1)
            except (OSError, ValueError):
                break
            if not r:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                break
            if not chunk:
                continue
            self.rx += chunk

            msgs, used = parse_downlink(self.rx)
            self.rx = self.rx[used:]

            for msg_id, payload in msgs:
                if msg_id == MSG_STATUS_QUERY:
                    if not self.quiet_status:
                        os.write(self.master, status_frame(0x00))
                    continue
                if msg_id in POLL_LEN:
                    # 带时间戳：t_interval 要靠它量真实轮询周期
                    self.seen_polls.append((time.time(), msg_id, payload))
                    if stop_after_frames is not None and seq >= stop_after_frames:
                        continue
                    seq += 1
                    frame = tel_frame(msg_id, seq)
                    self.sent_frames.append(frame)
                    if self.garbage and seq == 1:
                        os.write(self.master, bytes([0x11, 0x22, 0x33]) * 4)
                    if self.split_hdr and seq == 2 and len(frame) >= 2:
                        # 把帧头首字节单独写一次，强制它跨 read 边界（专打 remaining==1）
                        os.write(self.master, frame[:1])
                        time.sleep(0.05)
                        os.write(self.master, frame[1:])
                    else:
                        os.write(self.master, frame)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                print("  [warn] 进程未在 10s 内退出，SIGKILL")
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.exit_code = self.proc.returncode if self.proc else None
        if self.out_f:
            self.out_f.close()
        for fd in (self.master, self.slave):
            try:
                if fd is not None:
                    os.close(fd)
            except OSError:
                pass

    def read_tel(self, path=None):
        p = path or self.tel_path
        if not os.path.exists(p):
            return None
        with open(p, "rb") as f:
            return f.read()

    def read_text_log(self):
        if not os.path.exists(self.text_path):
            return ""
        with open(self.text_path, "rb") as f:
            return f.read().decode("utf-8", "replace")


def parse_tel_stream(data):
    """按 0xEB93 规则切开遥测日志，返回帧列表；遇到不合法立即返回已解析部分与错误"""
    frames = []
    i = 0
    while i < len(data):
        if data[i:i + 2] != TEL:
            return frames, "偏移 %d 处不是帧头 0xEB93: %s" % (i, data[i:i + 8].hex())
        if i + 4 > len(data):
            return frames, "偏移 %d 处不足 4 字节，无法读长度" % i
        src, n = data[i + 2], data[i + 3]
        total = 6 + n
        if i + total > len(data):
            return frames, "偏移 %d 处帧不完整(需 %d, 剩 %d)" % (i, total, len(data) - i)
        if data[i + total - 2:i + total] != TAIL:
            return frames, "偏移 %d 处帧尾错误: %s" % (i, data[i + total - 2:i + total].hex())
        frames.append((src, n, data[i:i + total]))
        i += total
    return frames, None


# ---------------------------------------------------------------- 各测试项

def t_basic(tmp, binary):
    print("\n[A-C,H] 基本流程：轮询 -> 回帧 -> 原样落盘，且 0xEB90 状态帧路径未回归")
    h = Harness(binary, os.path.join(tmp, "basic"), poll_sec=1, grace=0)
    h.start()
    h.serve(4.5)
    h.stop()

    polls = [m for _, m, _ in h.seen_polls]
    check(polls == POLL_ORDER + POLL_ORDER or
          polls[:3] == POLL_ORDER,
          "轮询命令按 A1/B1/B2 顺序下发", "实际=%s" % polls[:6])
    check(all(p == b"\x00" for _, _, p in h.seen_polls) and h.seen_polls,
          "轮询载荷为 1 字节 0x00（与状态查询同构）",
          "实际=%s" % [p.hex() for _, _, p in h.seen_polls][:6])
    check(len(h.sent_frames) >= 3, "伪造 FPGA 发出了遥测帧", "n=%d" % len(h.sent_frames))

    data = h.read_tel()
    if not check(data is not None, "遥测日志文件已创建", h.tel_path):
        return
    expect = b"".join(h.sent_frames)
    check(data == expect, "落盘内容与发出的帧逐字节一致",
          "落盘 %d 字节 / 期望 %d 字节" % (len(data), len(expect)))
    frames, err = parse_tel_stream(data)
    check(err is None, "落盘流可完整切成合法帧", str(err))
    check(len(frames) == len(h.sent_frames),
          "帧数与发出的一致", "%d vs %d" % (len(frames), len(h.sent_frames)))
    check(all(src in POLL_LEN and n == POLL_LEN[src] for src, n, _ in frames),
          "每帧来源合法且长度等于协议值",
          str([(hex(s), n) for s, n, _ in frames][:6]))
    check(b"\xEB\x90" in data and b"\x00" in data and b"\x0A" in data,
          "载荷里的 EB90/NUL/换行等字节未被破坏（二进制安全）")

    log = h.read_text_log()
    n1097 = len(re.findall(r"Alarm triggered:.*code=1097", log))
    check(n1097 >= 4, "0xEB90 状态帧仍被解析并触发 1097 告警（分帧改造未回归）",
          "匹配到 %d 条；宽限期=%d" % (n1097, h.grace))
    check("link_grace=0s" in log, "配置里的 ALARM_LINK_GRACE_SEC=0 已生效")
    check("Telemetry log initialized" in log, "遥测日志模块已初始化")


def t_rotation(tmp, binary):
    print("\n[E] 按大小轮转：单文件上限压到几百字节，确认出现 .1/.2 且新文件继续可切帧")
    h = Harness(binary, os.path.join(tmp, "rot"), poll_sec=1, max_size=400,
                backups=2, grace=0)
    h.start()
    h.serve(6.0)
    h.stop()

    d = os.path.dirname(h.tel_path)
    base = os.path.basename(h.tel_path)
    files = sorted(f for f in os.listdir(d) if f.startswith(base))
    check(len(files) >= 2, "产生了轮转备份", "实际=%s" % files)
    for f in files:
        data = h.read_tel(os.path.join(d, f))
        frames, err = parse_tel_stream(data)
        check(err is None and len(frames) > 0,
              "轮转文件 %s 仍是完整的帧流（未从中间截断）" % f,
              "err=%s n=%d" % (err, len(frames)))
    data = h.read_tel()
    frames, err = parse_tel_stream(data)
    check(err is None and len(frames) > 0, "轮转后当前文件继续正常追加", "err=%s" % err)


def t_split_and_garbage(tmp, binary):
    print("\n[F,G] 帧头跨 write 断开 + 帧前灌乱码：仍能拼回/重新同步，不丢后续帧")
    h = Harness(binary, os.path.join(tmp, "split"), poll_sec=1, max_size=0,
                grace=0, split_hdr=True, garbage=True)
    h.start()
    h.serve(6.0)
    h.stop()

    data = h.read_tel()
    if not check(data is not None, "遥测日志已创建"):
        return
    frames, err = parse_tel_stream(data)
    check(err is None, "含跨 read 半帧头与乱码的流仍可完整切帧", str(err))
    expect = b"".join(h.sent_frames)
    check(data == expect, "逐字节一致（跨 read 的半帧头被正确拼回，乱码被丢弃）",
          "落盘 %d / 期望 %d" % (len(data), len(expect)))
    check(len(frames) == len(h.sent_frames),
          "帧数一致（没有因为乱码丢掉后续帧）",
          "%d vs %d" % (len(frames), len(h.sent_frames)))


def t_interval(tmp, binary):
    print("\n[I] 轮询周期可配置：改 TELEMETRY_LOG_POLL_SEC 应改变实际下发间隔")
    observed = {}
    for want in (2, 4):
        h = Harness(binary, os.path.join(tmp, "int%d" % want),
                    poll_sec=want, grace=0)
        h.start()
        h.serve(want * 3 + 1)
        h.stop()

        # 每轮的第一条都是 A1，用相邻两次 A1 的时间差量周期
        a1 = [t for t, m, _ in h.seen_polls if m == 0xA1]
        if not check(len(a1) >= 2, "配置 %ds 时至少观察到两轮" % want,
                     "只看到 %d 条 A1" % len(a1)):
            continue
        gaps = [b - a for a, b in zip(a1, a1[1:])]
        avg = sum(gaps) / len(gaps)
        observed[want] = avg
        # 容差 0.8s：本测试台的 select 粒度是 0.1s，且每轮三条命令之间还有 gap
        check(abs(avg - want) <= 0.8,
              "配置 %ds 时实测周期 ≈ %ds" % (want, want),
              "实测=%.2fs（各次 %s）" % (avg, ["%.2f" % g for g in gaps]))

    if len(observed) == 2:
        check(observed[2] < observed[4],
              "2s 与 4s 的实测间隔确实不同 —— 配置真的生效，不是写死的",
              "2s→%.2fs, 4s→%.2fs" % (observed[2], observed[4]))


def t_status_regression_strict(tmp, binary):
    print("\n[D] 0xEB90 回归对照：link_success_flag=0x0F 时不应有 1097 告警")
    h = Harness(binary, os.path.join(tmp, "status"), poll_sec=60, grace=0)
    h.start()
    # 用一个 bit0-3 全 1 的状态帧替换默认的 0x00
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if h.proc.poll() is not None:
            break
        import select
        r, _, _ = select.select([h.master], [], [], 0.1)
        if not r:
            continue
        try:
            chunk = os.read(h.master, 4096)
        except OSError:
            break
        h.rx += chunk
        msgs, used = parse_downlink(h.rx)
        h.rx = h.rx[used:]
        for msg_id, _ in msgs:
            if msg_id == MSG_STATUS_QUERY:
                os.write(h.master, status_frame(0x0F))
    h.stop()

    log = h.read_text_log()
    n1097 = len(re.findall(r"Alarm triggered:.*code=1097", log))
    check(n1097 == 0, "链路位 bit0-3 全 1 时不报 1097（分帧与告警判据方向正确）",
          "匹配到 %d 条" % n1097)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./antenna_mgmt")
    ap.add_argument("--workdir", default="/tmp/tel_verify")
    ap.add_argument("--only", default=None, help="只跑某一项: basic/rot/split/status")
    args = ap.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("找不到二进制: %s（先在 s_band_antenna_mgmt 下 make）" % binary)
        return 2
    tmp = os.path.abspath(args.workdir)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp, exist_ok=True)
    print("二进制: %s\n工作目录: %s" % (binary, tmp))

    tests = {"basic": t_basic, "rot": t_rotation, "split": t_split_and_garbage,
             "status": t_status_regression_strict, "interval": t_interval}
    for name, fn in tests.items():
        if args.only and args.only != name:
            continue
        try:
            fn(tmp, binary)
        except Exception as e:          # noqa: BLE001
            print("  [FAIL] %s 抛异常: %r" % (name, e))
            FAILURES.append("%s:exception" % name)

    print("\n" + "=" * 60)
    if FAILURES:
        print("结果: 失败 %d / 共 %d 项检查" % (len(FAILURES), CHECKS))
        for f in FAILURES:
            print("  - %s" % f)
        return 1
    print("结果: 全部通过（%d 项检查）" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
