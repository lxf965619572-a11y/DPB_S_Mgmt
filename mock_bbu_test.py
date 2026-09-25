#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mock BBU：验证 P1-2 异步化改造后的 msg 195 -> 196 往返链路。

PAAU 是 TCP 客户端，主动连 BBU 的 BBU_IP:BBU_PORT。
本脚本在该地址上监听，构造一条合法的频点配置请求（msg 195）发给 PAAU，
然后等待并打印 PAAU 回的响应（msg 196），验证：
  1. 接收线程能正确入队
  2. worker 线程能构造并发出响应
  3. 响应内容（IE 1517）正确
"""
import socket
import struct
import sys
import time

HDR = struct.Struct('<IIBBBI')          # msg_id, msg_length, paau_id, bbu_id, port_num, serial_num
assert HDR.size == 15, HDR.size

MSG_NR_CELL_CONFIG = 195
MSG_NR_CELL_CONFIG_RSP = 196
IE_TYPE_FREQ_CONFIG = 0x5E3             # 1507
IE_TYPE_FREQ_CONFIG_RESP = 0x5ED        # 1517


def build_ie(ie_type, data):
    return struct.pack('<HH', ie_type, 4 + len(data)) + data


def build_msg(msg_id, serial, payload=b''):
    return HDR.pack(msg_id, HDR.size + len(payload), 0, 0, 0, serial) + payload


def build_freq_ie(cell_id, beam_id, dl, ul, bw):
    """与 cell_config_parse_freq_ie 的手工解析偏移逐字节对齐（32 字节）"""
    b = struct.pack('<B', 0)            # freq_cfg_flag = 0 (建立)
    b += struct.pack('<I', cell_id)     # local_cell_id
    b += struct.pack('<B', beam_id)     # beam_id
    b += struct.pack('<I', dl)          # dl_center_freq (kHz)
    b += struct.pack('<I', 0)           # reserved1
    b += struct.pack('<B', 0)           # special_subframe
    b += struct.pack('<I', 0)           # sys_subframe_num
    b += struct.pack('<I', bw)          # beam_bandwidth
    b += struct.pack('<I', 0)           # ul_dl_config
    b += struct.pack('<B', 0)           # reserved2
    b += struct.pack('<I', ul)          # ul_center_freq (kHz)
    assert len(b) == 32, len(b)
    return b


def parse_msgs(buf):
    """按 15 字节头切分粘包"""
    out = []
    off = 0
    while off + HDR.size <= len(buf):
        msg_id, msg_len, paau, bbu, port, serial = HDR.unpack_from(buf, off)
        if msg_len < HDR.size or off + msg_len > len(buf):
            break
        out.append((msg_id, serial, msg_len, buf[off + HDR.size:off + msg_len]))
        off += msg_len
    return out, buf[off:]


def main():
    host, port = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 30000

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print("[mock] 监听 %s:%d，等待 PAAU 连接..." % (host, port), flush=True)

    conn, addr = srv.accept()
    print("[mock] PAAU 已连接: %s" % (addr,), flush=True)
    conn.settimeout(30)

    # 发一条频点配置请求
    ie = build_ie(IE_TYPE_FREQ_CONFIG, build_freq_ie(1, 0, 2180100, 1980000, 100))
    req = build_msg(MSG_NR_CELL_CONFIG, 0x1234, ie)
    t0 = time.time()
    conn.sendall(req)
    print("[mock] t=+0.000s 已发送 msg 195 (serial=0x1234, payload=%d 字节)" % len(ie), flush=True)

    # 等待响应
    buf = b''
    got = False
    while time.time() - t0 < 12:
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            print("[mock] 超时，未再收到数据", flush=True)
            break
        if not chunk:
            print("[mock] 对端关闭", flush=True)
            break
        buf += chunk
        msgs, buf = parse_msgs(buf)
        for msg_id, serial, msg_len, payload in msgs:
            dt = time.time() - t0
            print("[mock] t=+%.3fs 收到 msg_id=%d serial=0x%04X len=%d" % (dt, msg_id, serial, msg_len), flush=True)
            if msg_id == MSG_NR_CELL_CONFIG_RSP:
                off = 0
                while off + 4 <= len(payload):
                    ie_type, ie_len = struct.unpack_from('<HH', payload, off)
                    if ie_len < 4 or off + ie_len > len(payload):
                        break
                    d = payload[off + 4:off + ie_len]
                    if ie_type == IE_TYPE_FREQ_CONFIG_RESP and len(d) >= 9:
                        cid, bid = struct.unpack_from('<IB', d, 0)
                        result = struct.unpack_from('<I', d, 5)[0]
                        print("[mock]   └─ IE 1517: cell_id=%d beam=%d result=%d %s"
                              % (cid, bid, result, "(成功)" if result == 0 else "(失败)"), flush=True)
                        got = True
                    else:
                        print("[mock]   └─ IE %d len=%d" % (ie_type, ie_len), flush=True)
                    off += ie_len
        if got:
            break

    print("[mock] 结果: %s" % ("通过 —— PAAU 异步返回了 msg 196" if got else "失败 —— 未收到 msg 196"), flush=True)
    conn.close()
    srv.close()
    return 0 if got else 1


if __name__ == '__main__':
    sys.exit(main())
