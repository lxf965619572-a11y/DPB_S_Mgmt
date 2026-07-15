#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模拟BBU服务器 - 用于测试FPGA固件上注功能
不需要真实BBU，通过TCP发送CPRI消息触发固件上注流程
"""

import socket
import struct
import time
import sys
import os

# CPRI消息ID定义
MSG_CHANNEL_SETUP_REQ = 1
MSG_CHANNEL_SETUP_CFG = 2
MSG_CHANNEL_SETUP_ACK = 3
MSG_VERSION_DOWNLOAD_REQ = 21
MSG_VERSION_DOWNLOAD_ACK = 22
MSG_VERSION_ACTIVATE_IND = 31
MSG_VERSION_ACTIVATE_ACK = 32

CPRI_HEADER_LEN = 15

class MockBBUServer:
    def __init__(self, host='0.0.0.0', port=30000):
        self.host = host
        self.port = port
        self.sock = None
        self.client_sock = None
        self.serial_num = 1

    def start(self):
        """启动模拟BBU服务器"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.listen(1)
        print(f"[BBU] Mock BBU Server started on {self.host}:{self.port}")
        print("[BBU] Waiting for PAAU connection...")

    def wait_for_connection(self):
        """等待PAAU连接"""
        self.client_sock, addr = self.sock.accept()
        print(f"[BBU] PAAU connected from {addr}")
        return True

    def encode_cpri_header(self, msg_id, payload_len):
        """编码CPRI消息头（小端序）"""
        msg_length = CPRI_HEADER_LEN + payload_len
        paau_id = 0
        bbu_id = 0
        port_num = 0

        header = struct.pack('<I', msg_id)           # 消息ID (4B, 小端)
        header += struct.pack('<I', msg_length)      # 消息长度 (4B, 小端)
        header += struct.pack('B', paau_id)          # PAAU ID (1B)
        header += struct.pack('B', bbu_id)           # BBU ID (1B)
        header += struct.pack('B', port_num)         # 端口号 (1B)
        header += struct.pack('<I', self.serial_num) # 流水号 (4B, 小端)

        self.serial_num += 1
        return header

    def send_message(self, msg_id, payload=b''):
        """发送CPRI消息"""
        header = self.encode_cpri_header(msg_id, len(payload))
        message = header + payload
        self.client_sock.sendall(message)
        print(f"[BBU] Sent message: msg_id={msg_id}, length={len(message)} bytes")

    def recv_message(self, timeout=5):
        """接收CPRI消息"""
        self.client_sock.settimeout(timeout)
        try:
            # 接收消息头
            header = self.client_sock.recv(CPRI_HEADER_LEN)
            if len(header) < CPRI_HEADER_LEN:
                return None

            # 解析消息头
            msg_id = struct.unpack('<I', header[0:4])[0]
            msg_length = struct.unpack('<I', header[4:8])[0]
            serial_num = struct.unpack('<I', header[11:15])[0]

            # 接收载荷
            payload_len = msg_length - CPRI_HEADER_LEN
            payload = b''
            if payload_len > 0:
                payload = self.client_sock.recv(payload_len)

            print(f"[BBU] Received message: msg_id={msg_id}, length={msg_length}, serial_num={serial_num}")
            return {'msg_id': msg_id, 'payload': payload, 'serial_num': serial_num}

        except socket.timeout:
            print("[BBU] Receive timeout")
            return None

    def handle_channel_setup(self):
        """处理通道建立流程"""
        print("\n=== Step 1: Channel Setup ===")

        # 等待通道建立请求
        msg = self.recv_message(timeout=10)
        if not msg or msg['msg_id'] != MSG_CHANNEL_SETUP_REQ:
            print("[BBU] ERROR: Expected channel setup request")
            return False

        print("[BBU] Received channel setup request")

        # 发送通道建立配置
        # 载荷: 系统时间(8B) + CPU统计周期(4B) + 工作模式(4B)
        current_time = int(time.time())
        cpu_period = 60  # 60秒
        work_mode = 0    # 业务模式

        payload = struct.pack('<Q', current_time)  # 系统时间
        payload += struct.pack('<I', cpu_period)   # CPU统计周期
        payload += struct.pack('<I', work_mode)    # 工作模式

        self.send_message(MSG_CHANNEL_SETUP_CFG, payload)

        # 等待通道建立应答
        msg = self.recv_message()
        if not msg or msg['msg_id'] != MSG_CHANNEL_SETUP_ACK:
            print("[BBU] ERROR: Expected channel setup ack")
            return False

        print("[BBU] Channel setup completed\n")
        return True

    def send_version_download_request(self, version_file):
        """发送版本下载请求"""
        print("\n=== Step 2: Version Download Request ===")

        # 检查版本文件是否存在
        if not os.path.exists(version_file):
            print(f"[BBU] ERROR: Version file not found: {version_file}")
            return False

        file_size = os.path.getsize(version_file)
        version_num = os.path.basename(version_file).replace('.tar.gz', '').replace('antenna_', '')

        print(f"[BBU] Version file: {version_file}")
        print(f"[BBU] Version number: {version_num}")
        print(f"[BBU] File size: {file_size} bytes")

        # 构造载荷
        # FTP服务器地址(32B) + FTP端口(2B) + 文件路径(128B) + 文件名(64B) + 版本号(40B) + 文件大小(4B)
        ftp_server = b'10.10.10.6' + b'\x00' * 22  # 32字节
        ftp_port = struct.pack('<H', 21)
        file_path = b'/versions/' + b'\x00' * 118  # 128字节
        file_name = os.path.basename(version_file).encode() + b'\x00' * (64 - len(os.path.basename(version_file)))
        version_bytes = version_num.encode() + b'\x00' * (40 - len(version_num))
        file_len = struct.pack('<I', file_size)

        payload = ftp_server + ftp_port + file_path + file_name + version_bytes + file_len

        self.send_message(MSG_VERSION_DOWNLOAD_REQ, payload)

        # 等待下载应答
        msg = self.recv_message(timeout=30)
        if not msg or msg['msg_id'] != MSG_VERSION_DOWNLOAD_ACK:
            print("[BBU] ERROR: Expected version download ack")
            return False

        # 解析应答结果
        if len(msg['payload']) >= 1:
            result = struct.unpack('B', msg['payload'][0:1])[0]
            if result == 0:
                print("[BBU] Version download SUCCESS\n")
                return True
            else:
                print(f"[BBU] Version download FAILED (result={result})\n")
                return False

        return False

    def send_version_activate_indication(self, version_num):
        """发送版本激活指示"""
        print("\n=== Step 3: Version Activate Indication ===")

        # 构造载荷: 版本号(40B)
        version_bytes = version_num.encode() + b'\x00' * (40 - len(version_num))
        payload = version_bytes

        self.send_message(MSG_VERSION_ACTIVATE_IND, payload)

        # 等待激活应答
        msg = self.recv_message(timeout=10)
        if not msg or msg['msg_id'] != MSG_VERSION_ACTIVATE_ACK:
            print("[BBU] ERROR: Expected version activate ack")
            return False

        # 解析应答结果
        if len(msg['payload']) >= 1:
            result = struct.unpack('B', msg['payload'][0:1])[0]
            if result == 0:
                print("[BBU] Version activate SUCCESS\n")
                return True
            else:
                print(f"[BBU] Version activate FAILED (result={result})\n")
                return False

        return False

    def close(self):
        """关闭连接"""
        if self.client_sock:
            self.client_sock.close()
        if self.sock:
            self.sock.close()
        print("[BBU] Server closed")


def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("Usage: python3 mock_bbu_server.py <version_file.tar.gz>")
        print("Example: python3 mock_bbu_server.py antenna_v1.0.1.tar.gz")
        sys.exit(1)

    version_file = sys.argv[1]

    # 创建模拟BBU服务器
    server = MockBBUServer(host='0.0.0.0', port=30000)

    try:
        # 启动服务器
        server.start()

        # 等待PAAU连接
        if not server.wait_for_connection():
            return

        # 等待一下，让PAAU完成初始化
        time.sleep(2)

        # Step 1: 通道建立
        if not server.handle_channel_setup():
            print("[BBU] Channel setup failed, exiting")
            return

        # Step 2: 版本下载请求
        version_num = os.path.basename(version_file).replace('.tar.gz', '').replace('antenna_', '')
        if not server.send_version_download_request(version_file):
            print("[BBU] Version download failed, exiting")
            return

        # 等待PAAU下载和校验
        print("[BBU] Waiting for PAAU to download and verify...")
        time.sleep(5)

        # Step 3: 版本激活
        if not server.send_version_activate_indication(version_num):
            print("[BBU] Version activate failed, exiting")
            return

        print("\n=== Test Completed Successfully ===")
        print("PAAU should restart with new version now")

        # 保持连接，等待PAAU重启后重新连接
        print("\n[BBU] Waiting for PAAU to reconnect after restart...")
        time.sleep(60)

    except KeyboardInterrupt:
        print("\n[BBU] Interrupted by user")
    except Exception as e:
        print(f"[BBU] Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        server.close()


if __name__ == '__main__':
    main()