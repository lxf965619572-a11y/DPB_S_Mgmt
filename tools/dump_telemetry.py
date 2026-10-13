#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""遥测日志查看器：把 logs/telemetry.bin 解码成可读文本。

背景：telemetry.bin 是【纯二进制帧流】，不是文本 —— 帧内容里会有 0x00、0x0A
以及帧尾 0x55 0x55。用 cat / tail / grep / 任何按行聚合的工具看都是乱码，
用 od/xxd 看又是一整片十六进制、难以对帧。本工具按协议切成帧后逐帧打印，
每帧一行摘要 + 十六进制/ASCII 转储，便于"把收到的内容给人看"。

帧结构（0xEB93）：
    帧头(2)=EB93 | 来源(1) | 长度(1) | 数据(N) | 帧尾(2)=5555     共 6+N 字节
    来源 0xA1 -> N=127 (V7A)
    来源 0xB1 -> N=146 (V7B1)
    来源 0xB2 -> N=146 (V7B2)

用法：
    python3 tools/dump_telemetry.py logs/telemetry.bin
    python3 tools/dump_telemetry.py logs/telemetry.bin --limit 3      # 只看前 3 帧
    python3 tools/dump_telemetry.py logs/telemetry.bin --ascii        # 附 ASCII 列
    python3 tools/dump_telemetry.py logs/telemetry.bin --summary      # 只出统计
    python3 tools/dump_telemetry.py logs/telemetry.bin --hex-stream > frames.txt

注意：本文件【只读】日志，不改动任何现场数据。
"""
import argparse
import os
import sys
from collections import Counter

HDR = bytes([0xEB, 0x93])
TAIL = bytes([0x55, 0x55])
# 来源 -> 协议规定长度（与 PAAU 侧 fpga_telemetry_payload_len 一致）
SRC_LEN = {0xA1: 127, 0xB1: 146, 0xB2: 146}
SRC_NAME = {0xA1: "V7A", 0xB1: "V7B1", 0xB2: "V7B2"}


def parse(data):
    """按 0xEB93 切帧。返回 (frames, errors)。
    frames: [(offset, src, data_len, frame_bytes)]
    errors: [(offset, 说明)] —— 不合法处只丢 1 字节重新同步，与 PAAU 接收侧同策略"""
    frames, errors = [], []
    i, n = 0, len(data)
    while i < n:
        if data[i:i + 2] != HDR:
            # 与接收侧一致：找不到帧头就丢 1 字节继续找（保留可能被截断的半个帧头）
            if data[i] == HDR[0] and i + 1 >= n:
                errors.append((i, "文件末尾为孤立的半个帧头 0xEB，可能写入被截断"))
                break
            errors.append((i, "非帧头字节 0x%02X" % data[i]))
            i += 1
            continue
        if i + 4 > n:
            errors.append((i, "帧头后不足 4 字节，读不到长度"))
            break
        src, dlen = data[i + 2], data[i + 3]
        total = 6 + dlen
        if i + total > n:
            errors.append((i, "帧不完整：需 %d 字节，仅剩 %d（文件可能在写入中被读取）"
                           % (total, n - i)))
            break
        if data[i + total - 2:i + total] != TAIL:
            errors.append((i, "帧尾错误：%s（期望 5555）"
                           % data[i + total - 2:i + total].hex()))
            i += 1
            continue
        # 长度是否与协议值相符（PAAU 接收侧会因此拒帧，这里只标注）
        expect = SRC_LEN.get(src)
        if expect is None:
            errors.append((i, "来源 0x%02X 不在白名单 {A1,B1,B2}" % src))
        elif expect != dlen:
            errors.append((i, "来源 0x%02X 长度 %d 与协议值 %d 不符" % (src, dlen, expect)))
        frames.append((i, src, dlen, data[i:i + total]))
        i += total
    return frames, errors


def hexdump(b, ascii_col=True, width=16, indent="      "):
    out = []
    for off in range(0, len(b), width):
        chunk = b[off:off + width]
        hexpart = " ".join("%02X" % c for c in chunk)
        # 补对齐：16 列，每列 2 位 + 1 空格
        hexpart = hexpart.ljust(width * 3 - 1)
        if ascii_col:
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            out.append("%s%04X  %s  |%s|" % (indent, off, hexpart, asc))
        else:
            out.append("%s%04X  %s" % (indent, off, hexpart))
    return "\n".join(out)


def dump_from_package(args):
    """从日志上传的 .tgz 里取出遥测成员并解码。

    包里有两个成员：文本日志 + 遥测日志（成员名 = 各配置路径的 basename）。
    这里挑出"能按 0xEB93 切出帧"的那个，避免依赖成员名恰好叫 telemetry.bin
    （两者 basename 撞名时，遥测成员会被加 .tel 后缀）。"""
    import tarfile
    pkg = args.from_package
    if not os.path.exists(pkg):
        print("找不到压缩包：%s" % pkg, file=sys.stderr)
        return 2

    with tarfile.open(pkg, "r:gz") as tf:
        members = [m for m in tf.getmembers() if m.isfile()]
        if not members:
            print("压缩包里没有文件成员", file=sys.stderr)
            return 2
        print("压缩包    : %s" % os.path.abspath(pkg))
        print("成员      : %s" % ", ".join("%s (%d 字节)" % (m.name, m.size) for m in members))
        print("")

        chosen_name, chosen_data = None, None
        for m in members:
            blob = tf.extractfile(m).read()
            if blob[:2] == HDR:
                chosen_name, chosen_data = m.name, blob
                break
        if chosen_data is None:
            print("包里没有任何成员以 0xEB93 帧头开头 —— 说明这次上传时还没有收到",
                  file=sys.stderr)
            print("FPGA 遥测帧（遥测日志为空时上传会降级为只打包文本日志）。", file=sys.stderr)
            return 1
        print("选中遥测成员: %s" % chosen_name)
        print("")

    return _decode_and_report(args, chosen_name, chosen_data)


def _decode_and_report(args, label, data):
    size = len(data)
    print("=" * 68)
    print("来源      : %s（包内成员）" % label)
    print("大小      : %d 字节" % size)
    frames, errors = parse(data)
    print("帧数      : %d" % len(frames))
    if errors:
        print("异常      : %d 处（见末尾）" % len(errors))
    print("=" * 68)
    _print_frames(args, frames, size)
    _report_errors(errors)
    return 0


def _print_frames(args, frames, size):
    if not frames:
        return
    by_src = Counter(f[1] for f in frames)
    print("按来源统计：")
    for src in sorted(by_src):
        print("  0x%02X %-5s : %d 帧" % (src, SRC_NAME.get(src, "未知"), by_src[src]))
    print("")

    if args.summary or args.hex_stream:
        return
    show = len(frames) if args.limit <= 0 else min(args.limit, len(frames))
    print("逐帧转储（显示前 %d / 共 %d 帧）：" % (show, len(frames)))
    print("")
    for idx in range(show):
        off, src, dlen, fbytes = frames[idx]
        print("--- 帧 #%d  偏移=%d  来源=0x%02X(%s)  数据长度=%d  总长=%d ---"
              % (idx + 1, off, src, SRC_NAME.get(src, "未知"), dlen, 6 + dlen))
        payload = fbytes if args.dump_payload else fbytes[4:4 + dlen]
        print("  %s：" % ("整帧" if args.dump_payload else "数据段(偏移4起)"))
        print(hexdump(payload, ascii_col=args.ascii))
        print("")
    if show < len(frames):
        print("（其余 %d 帧未显示，--limit 0 可全部显示）" % (len(frames) - show))
        print("")


def main():
    ap = argparse.ArgumentParser(description="解码遥测日志 telemetry.bin 为可读文本")
    ap.add_argument("path", nargs="?", default="logs/telemetry.bin",
                    help="遥测日志文件（默认 logs/telemetry.bin）")
    ap.add_argument("--limit", type=int, default=0,
                    help="最多显示多少帧的完整转储（0=全部；帧很多时建议配合 --summary）")
    ap.add_argument("--summary", action="store_true", help="只输出统计，不逐帧转储")
    ap.add_argument("--ascii", action="store_true", help="十六进制转储附带 ASCII 列")
    ap.add_argument("--dump-payload", action="store_true",
                    help="转储时含帧头帧尾（默认只转储数据段）")
    ap.add_argument("--hex-stream", action="store_true",
                    help="每帧一行输出纯十六进制（便于与 FPGA 侧记录做 diff）")
    ap.add_argument("--from-package", metavar="TGZ",
                    help="改为从日志上传的压缩包(.tgz)里取遥测成员并解码，"
                         "例如: --from-package PAAU1013.tgz")
    args = ap.parse_args()

    if args.from_package:
        return dump_from_package(args)

    if not os.path.exists(args.path):
        print("找不到文件：%s" % args.path, file=sys.stderr)
        print("提示：路径相对于进程的工作目录。systemd 下通常是", file=sys.stderr)
        print("      WorkingDirectory=/opt/vendor/current，即 /opt/vendor/current/logs/telemetry.bin",
              file=sys.stderr)
        return 2

    size = os.path.getsize(args.path)
    with open(args.path, "rb") as f:
        data = f.read()

    print("=" * 68)
    print("文件      : %s" % os.path.abspath(args.path))
    print("大小      : %d 字节" % size)
    if size == 0:
        print("")
        print("【空文件】还没有收到任何 FPGA 遥测帧。逐项排查：")
        print("  1) UART 是否打开： grep 'UART RS-422设备已打开' logs/antenna_mgmt.log")
        print("     若只有 'UART not open'，说明串口没起来（/dev/ttyAMA0 是否存在/权限）")
        print("  2) 轮询是否在下发：grep 'Telemetry poll' logs/antenna_mgmt.log")
        print("     若见 'Telemetry poll skipped: UART not open'，是第 1 条的问题")
        print("  3) FPGA 是否在回帧：见下（先确认 1、2 都正常再查这条）")
        print("=" * 68)
        return 1

    frames, errors = parse(data)
    print("帧数      : %d" % len(frames))
    if errors:
        print("异常      : %d 处（见末尾）" % len(errors))
    print("=" * 68)

    if args.hex_stream:
        for (off, src, dlen, fbytes) in frames:
            print(fbytes.hex())
        if errors:
            print("# %d 处异常未包含在上面的帧流中：" % len(errors), file=sys.stderr)
            for (off, msg) in errors[:20]:
                print("#   偏移 %d: %s" % (off, msg), file=sys.stderr)
        return 0

    if frames:
        print("首帧偏移  : %d" % frames[0][0])
        last = frames[-1]
        print("末帧偏移  : %d （末帧结束于 %d，文件 %d 字节）"
              % (last[0], last[0] + 6 + last[2], size))
        print("")
    _print_frames(args, frames, size)
    _report_errors(errors)
    return 0


def _report_errors(errors):
    if not errors:
        return
    print("异常明细（按接收侧同策略只丢 1 字节重新同步，故不影响后续帧）：")
    for (off, msg) in errors[:40]:
        print("  偏移 %-8d %s" % (off, msg))
    if len(errors) > 40:
        print("  ...（共 %d 处，只列前 40）" % len(errors))
    print("")
    print("判读：")
    print("  * '帧不完整' 多出现在文件末尾 —— 读取时正在写入，属正常，稍后重读即可")
    print("  * '非帧头字节' / '帧尾错误' 成片出现 —— 说明串口上有噪声或波特率/校验位不符")
    print("  * '长度与协议值不符' —— FPGA 侧载荷长度变了，需与 FPGA 对协议")


if __name__ == "__main__":
    sys.exit(main())