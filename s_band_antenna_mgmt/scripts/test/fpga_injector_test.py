#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FPGA固件上注测试工具
直接通过RS-422串口测试FPGA固件上注功能，不需要BBU
"""

import serial
import struct
import time
import sys
import os

# RS-422命令码定义
CMD_TRANSFER_START = 0x0155
CMD_TRANSFER_START_ACK = 0x015A
CMD_FILE_DATA = 0x0180
CMD_DATA_ACK = 0x018A
CMD_TRANSFER_END = 0x01AA
CMD_TRANSFER_END_ACK = 0x01BB
CMD_RECONFIG_START = 0x01AF
CMD_RECONFIG_QUERY = 0x01C5
CMD_RECONFIG_ACK = 0x01CA

# APID定义
APID_CONTROL = 0x03A0
APID_DATA = 0x03AF

# 分段大小
FIRST_SEGMENT_SIZE = 1000
OTHER_SEGMENT_SIZE = 1002

class FPGAFirmwareInjector:
    def __init__(self, port='/dev/ttyAMA1', baudrate=921600):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.seq_counter = 0

    def open(self):
        """打开串口"""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=5
            )
            print(f"[FPGA] Opened serial port: {self.port} @ {self.baudrate}")
            return True
        except Exception as e:
            print(f"[FPGA] Failed to open serial port: {e}")
            return False

    def close(self):
        """关闭串口"""
        if self.ser:
            self.ser.close()
            print("[FPGA] Serial port closed")

    def rs422_checksum(self, data):
        """计算RS-422校验和（单字节累加求和取反）"""
        sum_val = sum(data) & 0xFFFFFFFF
        checksum = (~sum_val) & 0xFFFF
        return checksum

    def crc16_ccitt_false(self, data):
        """计算CRC16-CCITT-FALSE"""
        crc = 0xFFFF
        poly = 0x1021

        for byte in data:
            crc ^= (byte << 8)
            for _ in range(8):
                if crc & 0x8000:
                    crc = ((crc << 1) ^ poly) & 0xFFFF
                else:
                    crc = (crc << 1) & 0xFFFF

        return crc

    def build_rs422_frame(self, apid, group_flags, cmd_code, payload):
        """构造RS-422帧"""
        frame = bytearray()

        # 1. 标识符 (0xEB90, Big-Endian)
        frame += struct.pack('>H', 0xEB90)

        # 2. 包标识
        packet_id = (0b000 << 13) | (0b0 << 12) | (0b0 << 11) | (apid & 0x07FF)
        frame += struct.pack('>H', packet_id)

        # 3. 包序列控制
        seq_ctrl = (group_flags << 14) | (self.seq_counter & 0x3FFF)
        self.seq_counter += 1
        frame += struct.pack('>H', seq_ctrl)

        # 4. 数据域长度
        data_len = 2 + len(payload) - 1  # 命令码(2B) + 载荷 - 1
        frame += struct.pack('>H', data_len)

        # 5. 命令码
        frame += struct.pack('>H', cmd_code)

        # 6. 载荷
        frame += payload

        # 7. 校验和（从字节偏移2开始）
        checksum = self.rs422_checksum(frame[2:])
        frame += struct.pack('>H', checksum)

        return bytes(frame)

    def send_frame(self, frame):
        """发送帧"""
        self.ser.write(frame)
        print(f"[FPGA] Sent {len(frame)} bytes")

    def recv_frame(self, timeout=5):
        """接收帧"""
        self.ser.timeout = timeout
        try:
            # 读取帧头
            sync = self.ser.read(2)
            if len(sync) < 2:
                return None

            sync_word = struct.unpack('>H', sync)[0]
            if sync_word != 0xEB90:
                print(f"[FPGA] Invalid sync word: 0x{sync_word:04X}")
                return None

            # 读取包标识、包序列控制、数据域长度
            header = self.ser.read(6)
            if len(header) < 6:
                return None

            data_len = struct.unpack('>H', header[4:6])[0]

            # 读取命令码和载荷
            data = self.ser.read(2 + data_len + 1)  # 命令码(2B) + 载荷 + 校验和(2B)
            if len(data) < 2 + data_len + 1:
                return None

            cmd_code = struct.unpack('>H', data[0:2])[0]
            payload = data[2:2+data_len+1-2]  # 去掉命令码和校验和

            print(f"[FPGA] Received frame: cmd=0x{cmd_code:04X}, payload_len={len(payload)}")
            return {'cmd_code': cmd_code, 'payload': payload}

        except Exception as e:
            print(f"[FPGA] Receive error: {e}")
            return None

    def send_transfer_start(self, file_path):
        """发送传输开始命令"""
        print("\n=== Step 1: Transfer Start ===")

        # 读取文件
        with open(file_path, 'rb') as f:
            file_data = f.read()

        file_size = len(file_data)
        file_crc16 = self.crc16_ccitt_false(file_data)

        # 计算分段信息
        if file_size <= FIRST_SEGMENT_SIZE:
            total_segments = 1
            last_seg_len = file_size
        else:
            remaining = file_size - FIRST_SEGMENT_SIZE
            other_segments = (remaining + OTHER_SEGMENT_SIZE - 1) // OTHER_SEGMENT_SIZE
            total_segments = 1 + other_segments
            last_seg_len = remaining - (other_segments - 1) * OTHER_SEGMENT_SIZE

        print(f"[FPGA] File: {file_path}")
        print(f"[FPGA] Size: {file_size} bytes")
        print(f"[FPGA] CRC16: 0x{file_crc16:04X}")
        print(f"[FPGA] Total segments: {total_segments}")
        print(f"[FPGA] Last segment length: {last_seg_len}")

        # 构造载荷
        payload = bytearray()
        payload.append((APID_CONTROL >> 4) & 0x7F)  # device_id
        payload.append(0xFF)  # file_type
        payload.append(0x00)  # file_sub_type
        payload += struct.pack('>H', (3 << 14) | total_segments)  # segment_info
        payload += struct.pack('>I', file_size)  # file_length
        payload += struct.pack('>I', last_seg_len)  # last_segment_len
        payload += struct.pack('>H', file_crc16)  # file_checksum

        # 发送帧
        frame = self.build_rs422_frame(APID_CONTROL, 0b11, CMD_TRANSFER_START, bytes(payload))
        self.send_frame(frame)

        # 等待应答
        print("[FPGA] Waiting for transfer start ack...")
        max_retries = 60  # 最多等待5分钟
        for i in range(max_retries):
            resp = self.recv_frame(timeout=5)
            if resp and resp['cmd_code'] == CMD_TRANSFER_START_ACK:
                result = resp['payload'][0] if len(resp['payload']) > 0 else 0xFF
                if result == 0x00:
                    print("[FPGA] Transfer start ACK: READY (0x00)")
                    return file_data, total_segments
                elif result == 0x11:
                    print(f"[FPGA] Transfer start ACK: PREPARING (0x11), retry {i+1}/{max_retries}")
                    time.sleep(1)
                    continue
                else:
                    print(f"[FPGA] Transfer start ACK: REJECTED (0x{result:02X})")
                    return None, 0

        print("[FPGA] Transfer start timeout")
        return None, 0

    def send_file_data(self, file_data, total_segments):
        """发送文件数据"""
        print("\n=== Step 2: Transfer File Data ===")

        offset = 0
        for seg_num in range(total_segments):
            # 确定本段数据大小
            if seg_num == 0:
                seg_size = min(FIRST_SEGMENT_SIZE, len(file_data) - offset)
            else:
                seg_size = min(OTHER_SEGMENT_SIZE, len(file_data) - offset)

            seg_data = file_data[offset:offset+seg_size]

            # 确定分组标志
            if total_segments == 1:
                group_flags = 0b11  # 单帧
            elif seg_num == 0:
                group_flags = 0b01  # 首段
            elif seg_num == total_segments - 1:
                group_flags = 0b10  # 尾段
            else:
                group_flags = 0b00  # 中间段

            # 构造载荷
            payload = struct.pack('>H', seg_num) + seg_data

            # 发送帧（最多重试3次）
            retry_count = 0
            while retry_count < 3:
                frame = self.build_rs422_frame(APID_DATA, group_flags, CMD_FILE_DATA, payload)
                self.send_frame(frame)

                print(f"[FPGA] Sent segment {seg_num}/{total_segments-1} ({seg_size} bytes)")

                # 等待应答
                resp = self.recv_frame(timeout=5)
                if resp and resp['cmd_code'] == CMD_DATA_ACK:
                    result = resp['payload'][0] if len(resp['payload']) > 0 else 0xFF
                    if result == 0x00:
                        print(f"[FPGA] Segment {seg_num} ACK: SUCCESS")
                        break
                    else:
                        print(f"[FPGA] Segment {seg_num} ACK: FAILED (0x{result:02X}), retry {retry_count+1}/3")
                        retry_count += 1
                else:
                    print(f"[FPGA] Segment {seg_num} no ACK, retry {retry_count+1}/3")
                    retry_count += 1

            if retry_count >= 3:
                print(f"[FPGA] Segment {seg_num} failed after 3 retries")
                return False

            offset += seg_size

        print(f"[FPGA] All {total_segments} segments sent successfully")
        return True

    def send_transfer_end(self):
        """发送传输结束命令"""
        print("\n=== Step 3: Transfer End ===")

        # 发送帧（无载荷）
        frame = self.build_rs422_frame(APID_CONTROL, 0b11, CMD_TRANSFER_END, b'')
        self.send_frame(frame)

        # 等待应答
        print("[FPGA] Waiting for transfer end ack...")
        resp = self.recv_frame(timeout=10)
        if resp and resp['cmd_code'] == CMD_TRANSFER_END_ACK:
            result = resp['payload'][0] if len(resp['payload']) > 0 else 0xFF
            if result == 0x00:
                print("[FPGA] Transfer end ACK: CRC OK (0x00)")
                return True
            else:
                print(f"[FPGA] Transfer end ACK: CRC FAILED (0x{result:02X})")
                return False

        print("[FPGA] Transfer end timeout")
        return False

    def send_reconfig_start(self):
        """发送重构启动命令"""
        print("\n=== Step 4: Reconfig Start ===")

        # 构造载荷
        payload = struct.pack('BB', 0xFF, 0x00)  # file_type, file_sub_type

        # 发送帧
        frame = self.build_rs422_frame(APID_CONTROL, 0b11, CMD_RECONFIG_START, payload)
        self.send_frame(frame)
        print("[FPGA] Sent reconfig start command")

    def query_reconfig_status(self):
        """查询重构状态"""
        print("\n=== Step 5: Query Reconfig Status ===")

        max_retries = 120  # 最多等待10分钟
        for i in range(max_retries):
            # 发送查询命令
            frame = self.build_rs422_frame(APID_CONTROL, 0b11, CMD_RECONFIG_QUERY, b'')
            self.send_frame(frame)

            # 等待应答
            resp = self.recv_frame(timeout=5)
            if resp and resp['cmd_code'] == CMD_RECONFIG_ACK:
                result = resp['payload'][0] if len(resp['payload']) > 0 else 0xFF
                if result == 0x00:
                    print("[FPGA] Reconfig status: SUCCESS (0x00)")
                    return True
                elif result == 0x11:
                    print(f"[FPGA] Reconfig status: IN_PROGRESS (0x11), retry {i+1}/{max_retries}")
                    time.sleep(5)
                    continue
                elif result == 0x22:
                    print(f"[FPGA] Reconfig status: NOT_STARTED (0x22), retry {i+1}/{max_retries}")
                    time.sleep(5)
                    continue
                else:
                    print(f"[FPGA] Reconfig status: FAILED (0x{result:02X})")
                    return False

        print("[FPGA] Reconfig query timeout")
        return False

    def inject_firmware(self, firmware_file):
        """执行完整的固件上注流程"""
        print("=" * 60)
        print("FPGA Firmware Injection Test")
        print("=" * 60)

        # Step 1: 传输开始
        file_data, total_segments = self.send_transfer_start(firmware_file)
        if not file_data:
            print("\n[FPGA] Firmware injection FAILED at transfer start")
            return False

        # Step 2: 传输文件数据
        if not self.send_file_data(file_data, total_segments):
            print("\n[FPGA] Firmware injection FAILED at data transfer")
            return False

        # Step 3: 传输结束
        if not self.send_transfer_end():
            print("\n[FPGA] Firmware injection FAILED at transfer end")
            return False

        # Step 4: 重构启动
        self.send_reconfig_start()

        # Step 5: 查询重构状态
        if not self.query_reconfig_status():
            print("\n[FPGA] Firmware injection FAILED at reconfig")
            return False

        print("\n" + "=" * 60)
        print("FPGA Firmware Injection SUCCESS!")
        print("=" * 60)
        return True


def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("Usage: python3 fpga_injector_test.py <firmware.bin> [serial_port]")
        print("Example: python3 fpga_injector_test.py fpga_firmware.bin /dev/ttyAMA1")
        sys.exit(1)

    firmware_file = sys.argv[1]
    serial_port = sys.argv[2] if len(sys.argv) > 2 else '/dev/ttyAMA1'

    if not os.path.exists(firmware_file):
        print(f"Error: Firmware file not found: {firmware_file}")
        sys.exit(1)

    # 创建固件注入器
    injector = FPGAFirmwareInjector(port=serial_port, baudrate=921600)

    try:
        # 打开串口
        if not injector.open():
            sys.exit(1)

        # 执行固件上注
        success = injector.inject_firmware(firmware_file)

        sys.exit(0 if success else 1)

    except KeyboardInterrupt:
        print("\n[FPGA] Interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"[FPGA] Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        injector.close()


if __name__ == '__main__':
    main()
