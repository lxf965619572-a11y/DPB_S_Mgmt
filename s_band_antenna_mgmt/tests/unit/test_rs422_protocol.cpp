/**
 * @file test_rs422_protocol.cpp
 * @brief RS-422协议层单元测试示例
 */

#include <gtest/gtest.h>
#include <cstring>
#include <arpa/inet.h>

extern "C" {
#include "rs422_protocol.h"
#include "common.h"
}

// ============ 测试夹具 ============

class RS422ProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 初始化协议层
        ASSERT_EQ(SUCCESS, rs422_protocol_init());
        memset(frame_buf, 0, sizeof(frame_buf));
    }

    void TearDown() override {
        rs422_protocol_cleanup();
    }

    uint8_t frame_buf[1024];
};

// ============ 初始化和清理测试 ============

TEST_F(RS422ProtocolTest, InitSuccess) {
    // 在SetUp中已经初始化,这里测试重复初始化
    rs422_protocol_cleanup();
    int ret = rs422_protocol_init();
    EXPECT_EQ(SUCCESS, ret);
}

TEST_F(RS422ProtocolTest, InitMultipleTimes) {
    // 测试多次初始化不会崩溃
    rs422_protocol_cleanup();
    EXPECT_EQ(SUCCESS, rs422_protocol_init());
    EXPECT_EQ(SUCCESS, rs422_protocol_init());
    EXPECT_EQ(SUCCESS, rs422_protocol_init());
}

TEST_F(RS422ProtocolTest, CleanupWithoutInit) {
    // 测试未初始化就清理不会崩溃
    rs422_protocol_cleanup();
    rs422_protocol_cleanup();
}

// ============ 校验和测试 ============

TEST_F(RS422ProtocolTest, ChecksumCalculation) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint16_t checksum = rs422_checksum(data, sizeof(data));
    // sum = 0x0F, ~sum = 0xFFF0
    EXPECT_EQ(0xFFF0, checksum);
}

TEST_F(RS422ProtocolTest, ChecksumEmpty) {
    uint16_t checksum = rs422_checksum(NULL, 0);
    EXPECT_EQ(0xFFFF, checksum);
}

TEST_F(RS422ProtocolTest, ChecksumSingleByte) {
    uint8_t data[] = {0xAA};
    uint16_t checksum = rs422_checksum(data, sizeof(data));
    // sum = 0xAA, ~sum = 0xFF55
    EXPECT_EQ(0xFF55, checksum);
}

TEST_F(RS422ProtocolTest, ChecksumAllZeros) {
    uint8_t data[100] = {0};
    uint16_t checksum = rs422_checksum(data, sizeof(data));
    // sum = 0, ~sum = 0xFFFF
    EXPECT_EQ(0xFFFF, checksum);
}

TEST_F(RS422ProtocolTest, ChecksumAllOnes) {
    uint8_t data[10];
    memset(data, 0xFF, sizeof(data));
    uint16_t checksum = rs422_checksum(data, sizeof(data));
    // sum = 0xFF * 10 = 0x9F6, ~sum = 0xF609
    EXPECT_EQ(0xF609, checksum);
}

TEST_F(RS422ProtocolTest, ChecksumLargeData) {
    uint8_t data[1000];
    for (int i = 0; i < 1000; i++) {
        data[i] = i % 256;
    }
    uint16_t checksum = rs422_checksum(data, sizeof(data));
    // 验证不会崩溃且返回有效值
    EXPECT_NE(0, checksum);
}

// ============ CRC16测试 ============

TEST_F(RS422ProtocolTest, CRC16Calculation) {
    // 标准测试向量 "123456789"
    uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint16_t crc = rs422_crc16_ccitt_false(data, sizeof(data));
    // 标准结果: 0x29B1
    EXPECT_EQ(0x29B1, crc);
}

TEST_F(RS422ProtocolTest, CRC16Empty) {
    uint16_t crc = rs422_crc16_ccitt_false(NULL, 0);
    // 空数据应返回初始值
    EXPECT_EQ(0xFFFF, crc);
}

TEST_F(RS422ProtocolTest, CRC16SingleByte) {
    uint8_t data[] = {0x00};
    uint16_t crc = rs422_crc16_ccitt_false(data, sizeof(data));
    // 验证单字节CRC计算
    EXPECT_NE(0xFFFF, crc);
}

TEST_F(RS422ProtocolTest, CRC16AllZeros) {
    uint8_t data[100] = {0};
    uint16_t crc = rs422_crc16_ccitt_false(data, sizeof(data));
    // 全零数据的CRC应该是确定值
    EXPECT_NE(0xFFFF, crc);
}

TEST_F(RS422ProtocolTest, CRC16LargeData) {
    uint8_t data[1000];
    for (int i = 0; i < 1000; i++) {
        data[i] = i % 256;
    }
    uint16_t crc = rs422_crc16_ccitt_false(data, sizeof(data));
    // 验证大数据不会崩溃
    EXPECT_NE(0, crc);
}

TEST_F(RS422ProtocolTest, CRC16Consistency) {
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint16_t crc1 = rs422_crc16_ccitt_false(data, sizeof(data));
    uint16_t crc2 = rs422_crc16_ccitt_false(data, sizeof(data));
    // 相同数据应产生相同CRC
    EXPECT_EQ(crc1, crc2);
}

// ============ 帧编码测试 ============

TEST_F(RS422ProtocolTest, EncodeFrameBasic) {
    uint8_t payload[] = {0xAA, 0xBB, 0xCC};

    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload, sizeof(payload),
        frame_buf, sizeof(frame_buf)
    );

    // 验证帧长度: 头(8) + 命令码(2) + 载荷(3) + 校验和(2) = 15
    EXPECT_EQ(15, frame_len);

    // 验证同步字
    uint16_t sync;
    memcpy(&sync, frame_buf, 2);
    EXPECT_EQ(0xEB90, ntohs(sync));
}

TEST_F(RS422ProtocolTest, EncodeFrameNullPayload) {
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_RECONFIG_QUERY,
        NULL, 0,
        frame_buf, sizeof(frame_buf)
    );

    // 验证帧长度: 头(8) + 命令码(2) + 校验和(2) = 12
    EXPECT_EQ(12, frame_len);
}

TEST_F(RS422ProtocolTest, EncodeFrameNullBuffer) {
    uint8_t payload[] = {0x01};
    int ret = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload, sizeof(payload),
        NULL, 0
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, EncodeFrameBufferTooSmall) {
    uint8_t payload[100];
    uint8_t small_buf[10];
    int ret = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload, sizeof(payload),
        small_buf, sizeof(small_buf)
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, EncodeFrameMaxPayload) {
    uint8_t payload[RS422_MAX_PAYLOAD_SIZE - 2];
    memset(payload, 0xAA, sizeof(payload));

    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload, sizeof(payload),
        frame_buf, sizeof(frame_buf)
    );

    // 应该成功
    EXPECT_GT(frame_len, 0);
}

TEST_F(RS422ProtocolTest, EncodeFramePayloadTooLarge) {
    uint8_t payload[RS422_MAX_PAYLOAD_SIZE];
    int ret = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload, sizeof(payload),
        frame_buf, sizeof(frame_buf)
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, EncodeFrameDifferentAPID) {
    uint8_t payload[] = {0x01, 0x02};
    int frame_len = rs422_encode_frame(
        RS422_APID_FILE_DATA,
        RS422_CMD_FILE_DATA,
        payload, sizeof(payload),
        frame_buf, sizeof(frame_buf)
    );
    EXPECT_GT(frame_len, 0);

    // 验证APID被正确编码
    uint16_t apid;
    memcpy(&apid, frame_buf + 2, 2);
    apid = ntohs(apid);
    EXPECT_EQ(RS422_APID_FILE_DATA & 0x07FF, apid);
}

// ============ 帧解码测试 ============

TEST_F(RS422ProtocolTest, DecodeFrameBasic) {
    // 先编码一个帧
    uint8_t payload_in[] = {0x11, 0x22, 0x33};
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload_in, sizeof(payload_in),
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    // 解码帧
    uint16_t apid, cmd_code;
    const uint8_t *payload_out;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload_out, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_APID_CONTROL, apid);
    EXPECT_EQ(RS422_CMD_VERSION_QUERY, cmd_code);
    EXPECT_EQ(sizeof(payload_in), payload_len);
    EXPECT_EQ(0, memcmp(payload_in, payload_out, payload_len));
}

TEST_F(RS422ProtocolTest, DecodeFrameInvalidSync) {
    // 构造错误的同步字
    frame_buf[0] = 0xFF;
    frame_buf[1] = 0xFF;

    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, 20,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_NE(SUCCESS, ret);
}

TEST_F(RS422ProtocolTest, DecodeFrameNullBuffer) {
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        NULL, 100,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, DecodeFrameTooShort) {
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, 5,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, DecodeFrameInvalidChecksum) {
    // 先编码一个有效帧
    uint8_t payload_in[] = {0x11, 0x22};
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload_in, sizeof(payload_in),
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    // 破坏校验和
    frame_buf[frame_len - 1] ^= 0xFF;

    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_NE(SUCCESS, ret);
}

TEST_F(RS422ProtocolTest, DecodeFrameEmptyPayload) {
    // 编码无载荷帧
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_RECONFIG_QUERY,
        NULL, 0,
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(0, payload_len);
}

TEST_F(RS422ProtocolTest, DecodeFrameNullOutputParams) {
    // 编码一个帧
    uint8_t payload_in[] = {0x01};
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload_in, sizeof(payload_in),
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    // 测试NULL输出参数
    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        NULL, NULL,
        NULL, NULL
    );

    // 应该成功,只是不返回值
    EXPECT_EQ(SUCCESS, ret);
}

// ============ 命令构造测试 ============

TEST_F(RS422ProtocolTest, BuildReconfigStartValid) {
    int frame_len = rs422_build_reconfig_start(
        RS422_FILE_TYPE_DEFAULT,
        0x00,
        frame_buf, sizeof(frame_buf)
    );

    // 验证帧长度: 头(8) + 命令码(2) + 载荷(2) + 校验和(2) = 14
    EXPECT_EQ(14, frame_len);

    // 解码验证
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_CMD_RECONFIG_START, cmd_code);
    EXPECT_EQ(2, payload_len);
}

TEST_F(RS422ProtocolTest, BuildReconfigStartBoundary) {
    // 测试边界文件类型
    int frame_len = rs422_build_reconfig_start(
        RS422_FILE_TYPE_MIN,
        0xFF,
        frame_buf, sizeof(frame_buf)
    );
    EXPECT_GT(frame_len, 0);
}

TEST_F(RS422ProtocolTest, BuildReconfigStartNullBuffer) {
    int ret = rs422_build_reconfig_start(
        RS422_FILE_TYPE_DEFAULT,
        0x00,
        NULL, 0
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildReconfigQueryValid) {
    int frame_len = rs422_build_reconfig_query(
        frame_buf, sizeof(frame_buf)
    );

    // 验证帧长度: 头(8) + 命令码(2) + 校验和(2) = 12
    EXPECT_EQ(12, frame_len);
}

TEST_F(RS422ProtocolTest, BuildReconfigQueryNullBuffer) {
    int ret = rs422_build_reconfig_query(NULL, 0);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildReconfigQuerySmallBuffer) {
    uint8_t small_buf[5];
    int ret = rs422_build_reconfig_query(small_buf, sizeof(small_buf));
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildVersionQueryValid) {
    int frame_len = rs422_build_version_query(
        frame_buf, sizeof(frame_buf)
    );

    EXPECT_EQ(12, frame_len);

    // 解码验证
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_CMD_VERSION_QUERY, cmd_code);
}

TEST_F(RS422ProtocolTest, BuildVersionQueryNullBuffer) {
    int ret = rs422_build_version_query(NULL, 0);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildVersionQuerySmallBuffer) {
    uint8_t small_buf[8];
    int ret = rs422_build_version_query(small_buf, sizeof(small_buf));
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildTransferStartCommand) {
    rs422_cmd_transfer_start_t params;
    params.device_id = 0x1D;
    params.file_type = 0xFF;
    params.file_sub_type = 0x00;
    params.segment_info = htons((3 << 14) | 100);
    params.file_length = htonl(102000);
    params.last_segment_len = htonl(1000);
    params.file_checksum = htons(0x1234);

    int frame_len = rs422_build_transfer_start(
        &params,
        frame_buf, sizeof(frame_buf)
    );

    // 验证帧长度: 头(8) + 命令码(2) + 载荷(15) + 校验和(2) = 27
    EXPECT_EQ(27, frame_len);
}

TEST_F(RS422ProtocolTest, BuildTransferStartNullParams) {
    int ret = rs422_build_transfer_start(
        NULL,
        frame_buf, sizeof(frame_buf)
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildTransferStartNullBuffer) {
    rs422_cmd_transfer_start_t params;
    params.device_id = 0x1D;
    params.file_type = 0xFF;
    params.file_sub_type = 0x00;
    params.segment_info = 0;
    params.file_length = 1000;
    params.last_segment_len = 1000;
    params.file_checksum = 0x1234;

    int ret = rs422_build_transfer_start(&params, NULL, 0);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildTransferEndValid) {
    int frame_len = rs422_build_transfer_end(
        frame_buf, sizeof(frame_buf)
    );

    EXPECT_EQ(12, frame_len);

    // 解码验证
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_CMD_TRANSFER_END, cmd_code);
}

TEST_F(RS422ProtocolTest, BuildTransferEndNullBuffer) {
    int ret = rs422_build_transfer_end(NULL, 0);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildTransferEndSmallBuffer) {
    uint8_t small_buf[5];
    int ret = rs422_build_transfer_end(small_buf, sizeof(small_buf));
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildTransferAbortValid) {
    int frame_len = rs422_build_transfer_abort(
        frame_buf, sizeof(frame_buf)
    );

    EXPECT_EQ(12, frame_len);

    // 解码验证
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_CMD_TRANSFER_ABORT, cmd_code);
}

TEST_F(RS422ProtocolTest, BuildTransferAbortNullBuffer) {
    int ret = rs422_build_transfer_abort(NULL, 0);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildTransferAbortSmallBuffer) {
    uint8_t small_buf[8];
    int ret = rs422_build_transfer_abort(small_buf, sizeof(small_buf));
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

// ============ 文件数据帧构造测试 ============

TEST_F(RS422ProtocolTest, BuildFileDataSingleSegment) {
    uint8_t data[100];
    memset(data, 0xAA, sizeof(data));

    int frame_len = rs422_build_file_data(
        0, data, sizeof(data),
        frame_buf, sizeof(frame_buf),
        1  // 单段
    );

    EXPECT_GT(frame_len, 0);

    // 验证帧长度
    uint32_t expected_len = RS422_HEADER_SIZE + 2 + sizeof(data) + RS422_CRC_SIZE;
    EXPECT_EQ(expected_len, frame_len);
}

TEST_F(RS422ProtocolTest, BuildFileDataFirstSegment) {
    uint8_t data[100];
    memset(data, 0xBB, sizeof(data));

    int frame_len = rs422_build_file_data(
        0, data, sizeof(data),
        frame_buf, sizeof(frame_buf),
        10  // 总共10段,这是第0段(首段)
    );

    EXPECT_GT(frame_len, 0);
}

TEST_F(RS422ProtocolTest, BuildFileDataMiddleSegment) {
    uint8_t data[100];
    memset(data, 0xCC, sizeof(data));

    int frame_len = rs422_build_file_data(
        5, data, sizeof(data),
        frame_buf, sizeof(frame_buf),
        10  // 总共10段,这是第5段(中间段)
    );

    EXPECT_GT(frame_len, 0);
}

TEST_F(RS422ProtocolTest, BuildFileDataLastSegment) {
    uint8_t data[100];
    memset(data, 0xDD, sizeof(data));

    int frame_len = rs422_build_file_data(
        9, data, sizeof(data),
        frame_buf, sizeof(frame_buf),
        10  // 总共10段,这是第9段(尾段)
    );

    EXPECT_GT(frame_len, 0);
}

TEST_F(RS422ProtocolTest, BuildFileDataNullData) {
    int ret = rs422_build_file_data(
        0, NULL, 100,
        frame_buf, sizeof(frame_buf),
        1
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildFileDataZeroLength) {
    uint8_t data[1];
    int ret = rs422_build_file_data(
        0, data, 0,
        frame_buf, sizeof(frame_buf),
        1
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildFileDataNullBuffer) {
    uint8_t data[100];
    int ret = rs422_build_file_data(
        0, data, sizeof(data),
        NULL, 0,
        1
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildFileDataBufferTooSmall) {
    uint8_t data[100];
    uint8_t small_buf[50];
    int ret = rs422_build_file_data(
        0, data, sizeof(data),
        small_buf, sizeof(small_buf),
        1
    );
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, BuildFileDataMaxSegmentNumber) {
    uint8_t data[10];
    int frame_len = rs422_build_file_data(
        0x3FFF, data, sizeof(data),
        frame_buf, sizeof(frame_buf),
        0x4000
    );
    EXPECT_GT(frame_len, 0);
}

// ============ 应答解析测试 ============

TEST_F(RS422ProtocolTest, ParseReconfigAckSuccess) {
    uint8_t payload[] = {RS422_RECONFIG_SUCCESS};
    uint8_t result;

    int ret = rs422_parse_reconfig_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_RECONFIG_SUCCESS, result);
}

TEST_F(RS422ProtocolTest, ParseReconfigAckInProgress) {
    uint8_t payload[] = {RS422_RECONFIG_IN_PROGRESS};
    uint8_t result;

    int ret = rs422_parse_reconfig_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_RECONFIG_IN_PROGRESS, result);
}

TEST_F(RS422ProtocolTest, ParseReconfigAckFailed) {
    uint8_t payload[] = {RS422_RECONFIG_FAILED};
    uint8_t result;

    int ret = rs422_parse_reconfig_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_RECONFIG_FAILED, result);
}

TEST_F(RS422ProtocolTest, ParseReconfigAckNullPayload) {
    uint8_t result;
    int ret = rs422_parse_reconfig_ack(NULL, 1, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseReconfigAckNullResult) {
    uint8_t payload[] = {RS422_RECONFIG_SUCCESS};
    int ret = rs422_parse_reconfig_ack(payload, sizeof(payload), NULL);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseReconfigAckInvalidLength) {
    uint8_t result;
    int ret = rs422_parse_reconfig_ack(NULL, 0, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseVersionAckValid) {
    uint8_t payload[32];
    strcpy((char*)payload, "v1.2.3");
    char version[32];

    int ret = rs422_parse_version_ack(
        payload, sizeof(payload),
        version
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_STREQ("v1.2.3", version);
}

TEST_F(RS422ProtocolTest, ParseVersionAckMaxLength) {
    uint8_t payload[32];
    memset(payload, 'A', 31);
    payload[31] = '\0';
    char version[32];

    int ret = rs422_parse_version_ack(
        payload, sizeof(payload),
        version
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ('\0', version[31]);  // 确保null终止
}

TEST_F(RS422ProtocolTest, ParseVersionAckEmpty) {
    uint8_t payload[32] = {0};
    char version[32];

    int ret = rs422_parse_version_ack(
        payload, sizeof(payload),
        version
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_STREQ("", version);
}

TEST_F(RS422ProtocolTest, ParseVersionAckNullPayload) {
    char version[32];
    int ret = rs422_parse_version_ack(NULL, 32, version);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseVersionAckNullVersion) {
    uint8_t payload[32];
    int ret = rs422_parse_version_ack(payload, sizeof(payload), NULL);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseVersionAckInvalidLength) {
    uint8_t payload[10];
    char version[32];
    int ret = rs422_parse_version_ack(payload, sizeof(payload), version);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAck) {
    uint8_t payload[] = {RS422_TRANSFER_READY};
    uint8_t result;

    int ret = rs422_parse_transfer_start_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_READY, result);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAckPreparing) {
    uint8_t payload[] = {RS422_TRANSFER_PREPARING};
    uint8_t result;

    int ret = rs422_parse_transfer_start_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_PREPARING, result);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAckRejected) {
    uint8_t payload[] = {RS422_TRANSFER_REJECTED};
    uint8_t result;

    int ret = rs422_parse_transfer_start_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_REJECTED, result);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAckNullPayload) {
    uint8_t result;
    int ret = rs422_parse_transfer_start_ack(NULL, 1, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAckNullResult) {
    uint8_t payload[] = {RS422_TRANSFER_READY};
    int ret = rs422_parse_transfer_start_ack(payload, sizeof(payload), NULL);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAckInvalidLength) {
    uint8_t result;
    int ret = rs422_parse_transfer_start_ack(NULL, 0, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseDataAckSuccess) {
    uint8_t payload[] = {RS422_DATA_RECEIVED_OK};
    uint8_t result;

    int ret = rs422_parse_data_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_DATA_RECEIVED_OK, result);
}

TEST_F(RS422ProtocolTest, ParseDataAckError) {
    uint8_t payload[] = {RS422_DATA_RECEIVED_ERROR};
    uint8_t result;

    int ret = rs422_parse_data_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_DATA_RECEIVED_ERROR, result);
}

TEST_F(RS422ProtocolTest, ParseDataAckNullPayload) {
    uint8_t result;
    int ret = rs422_parse_data_ack(NULL, 1, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseDataAckNullResult) {
    uint8_t payload[] = {RS422_DATA_RECEIVED_OK};
    int ret = rs422_parse_data_ack(payload, sizeof(payload), NULL);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseDataAckInvalidLength) {
    uint8_t result;
    int ret = rs422_parse_data_ack(NULL, 0, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseTransferEndAckSuccess) {
    uint8_t payload[] = {RS422_TRANSFER_COMPLETE_OK};
    uint8_t result;

    int ret = rs422_parse_transfer_end_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_COMPLETE_OK, result);
}

TEST_F(RS422ProtocolTest, ParseTransferEndAckError) {
    uint8_t payload[] = {RS422_TRANSFER_COMPLETE_ERROR};
    uint8_t result;

    int ret = rs422_parse_transfer_end_ack(
        payload, sizeof(payload),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_COMPLETE_ERROR, result);
}

TEST_F(RS422ProtocolTest, ParseTransferEndAckNullPayload) {
    uint8_t result;
    int ret = rs422_parse_transfer_end_ack(NULL, 1, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseTransferEndAckNullResult) {
    uint8_t payload[] = {RS422_TRANSFER_COMPLETE_OK};
    int ret = rs422_parse_transfer_end_ack(payload, sizeof(payload), NULL);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(RS422ProtocolTest, ParseTransferEndAckInvalidLength) {
    uint8_t result;
    int ret = rs422_parse_transfer_end_ack(NULL, 0, &result);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

// ============ 辅助函数测试 ============

TEST_F(RS422ProtocolTest, GetCommandName) {
    const char* name = rs422_get_cmd_name(RS422_CMD_TRANSFER_START);
    EXPECT_NE(nullptr, name);
    EXPECT_GT(strlen(name), 0);
}

TEST_F(RS422ProtocolTest, GetCommandNameAllCommands) {
    // 测试所有已知命令码
    EXPECT_STREQ("重构启动(1-1)", rs422_get_cmd_name(RS422_CMD_RECONFIG_START));
    EXPECT_STREQ("重构查询(1-2)", rs422_get_cmd_name(RS422_CMD_RECONFIG_QUERY));
    EXPECT_STREQ("重构应答(1-3)", rs422_get_cmd_name(RS422_CMD_RECONFIG_ACK));
    EXPECT_STREQ("版本查询(1-4)", rs422_get_cmd_name(RS422_CMD_VERSION_QUERY));
    EXPECT_STREQ("版本应答(1-5)", rs422_get_cmd_name(RS422_CMD_VERSION_ACK));
    EXPECT_STREQ("传输开始(3-3)", rs422_get_cmd_name(RS422_CMD_TRANSFER_START));
    EXPECT_STREQ("传输开始应答(3-4)", rs422_get_cmd_name(RS422_CMD_TRANSFER_START_ACK));
    EXPECT_STREQ("文件数据(3-5)", rs422_get_cmd_name(RS422_CMD_FILE_DATA));
    EXPECT_STREQ("数据应答(3-6)", rs422_get_cmd_name(RS422_CMD_DATA_ACK));
    EXPECT_STREQ("传输结束(3-7)", rs422_get_cmd_name(RS422_CMD_TRANSFER_END));
    EXPECT_STREQ("传输结束应答(3-8)", rs422_get_cmd_name(RS422_CMD_TRANSFER_END_ACK));
    EXPECT_STREQ("传输中止(3-9)", rs422_get_cmd_name(RS422_CMD_TRANSFER_ABORT));
    EXPECT_STREQ("中止应答(3-10)", rs422_get_cmd_name(RS422_CMD_ABORT_ACK));
}

TEST_F(RS422ProtocolTest, GetCommandNameUnknown) {
    const char* name = rs422_get_cmd_name(0xFFFF);
    EXPECT_STREQ("未知命令", name);
}

TEST_F(RS422ProtocolTest, GetResultDescription) {
    const char* desc = rs422_get_result_desc(
        RS422_CMD_TRANSFER_START_ACK,
        RS422_TRANSFER_READY
    );
    EXPECT_NE(nullptr, desc);
    EXPECT_GT(strlen(desc), 0);
}

TEST_F(RS422ProtocolTest, GetResultDescTransferStartAck) {
    EXPECT_STREQ("正常,可以开始文件传输",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_START_ACK, 0x00));
    EXPECT_STREQ("准备中,FPGA正在擦除",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_START_ACK, 0x11));
    EXPECT_STREQ("异常,不能开始文件传输",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_START_ACK, 0xFF));
    EXPECT_STREQ("未知结果",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_START_ACK, 0x99));
}

TEST_F(RS422ProtocolTest, GetResultDescReconfigAck) {
    EXPECT_STREQ("重构成功",
                 rs422_get_result_desc(RS422_CMD_RECONFIG_ACK, 0x00));
    EXPECT_STREQ("重构中",
                 rs422_get_result_desc(RS422_CMD_RECONFIG_ACK, 0x11));
    EXPECT_STREQ("重构失败",
                 rs422_get_result_desc(RS422_CMD_RECONFIG_ACK, 0xFF));
    EXPECT_STREQ("未知结果",
                 rs422_get_result_desc(RS422_CMD_RECONFIG_ACK, 0x22));
}

TEST_F(RS422ProtocolTest, GetResultDescTransferEndAck) {
    EXPECT_STREQ("文件接收正常",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_END_ACK, 0x00));
    EXPECT_STREQ("文件传输异常",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_END_ACK, 0x11));
    EXPECT_STREQ("未知结果",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_END_ACK, 0x22));
}

TEST_F(RS422ProtocolTest, GetResultDescDataAck) {
    EXPECT_STREQ("接收正常",
                 rs422_get_result_desc(RS422_CMD_DATA_ACK, 0x00));
    EXPECT_STREQ("接收异常",
                 rs422_get_result_desc(RS422_CMD_DATA_ACK, 0xFF));
}

TEST_F(RS422ProtocolTest, GetResultDescGeneric) {
    // 测试通用结果码(非特定命令)
    EXPECT_STREQ("成功",
                 rs422_get_result_desc(0x9999, 0x00));
    EXPECT_STREQ("失败",
                 rs422_get_result_desc(0x9999, 0xFF));
    EXPECT_STREQ("未知结果",
                 rs422_get_result_desc(0x9999, 0x55));
}

// ============ 边界和未覆盖分支测试 ============

TEST_F(RS422ProtocolTest, EncodeFramePayloadNotNullButZeroLength) {
    // 测试 payload 不为 NULL 但 payload_len 为 0 的情况
    // 这会触发 rs422_encode_frame 第104行的分支3
    uint8_t dummy_payload[1] = {0x00};
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        dummy_payload, 0,  // payload 不为 NULL 但长度为 0
        frame_buf, sizeof(frame_buf)
    );

    // 应该成功,因为 payload_len 为 0 时不会复制载荷
    EXPECT_EQ(12, frame_len);
}

TEST_F(RS422ProtocolTest, DecodeFrameLengthMismatch) {
    // 测试帧长度不匹配的情况
    // 这会触发 rs422_decode_frame 第157-160行

    // 先编码一个正常帧
    uint8_t payload_in[] = {0x11, 0x22, 0x33};
    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload_in, sizeof(payload_in),
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    // 修改帧中的数据长度字段,使其与实际帧长度不匹配
    // 数据长度字段在偏移6-7位置
    uint16_t fake_data_len = htons(100);  // 设置一个很大的值
    memcpy(frame_buf + 6, &fake_data_len, 2);

    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    // 应该返回错误
    EXPECT_EQ(ERROR_GENERAL, ret);
}

TEST_F(RS422ProtocolTest, ParseReconfigAckPayloadLengthExact) {
    // 测试 payload_len 正好等于 sizeof 的情况
    // 这会触发第211行的分支3
    uint8_t payload[10];  // 大于 sizeof(rs422_cmd_reconfig_ack_t)
    payload[0] = RS422_RECONFIG_SUCCESS;
    uint8_t result;

    // 传入正好等于结构体大小的长度
    int ret = rs422_parse_reconfig_ack(
        payload, sizeof(rs422_cmd_reconfig_ack_t),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_RECONFIG_SUCCESS, result);
}

TEST_F(RS422ProtocolTest, ParseTransferStartAckPayloadLengthExact) {
    // 测试 payload_len 正好等于 sizeof 的情况
    // 这会触发第262行的分支3
    uint8_t payload[10];
    payload[0] = RS422_TRANSFER_READY;
    uint8_t result;

    int ret = rs422_parse_transfer_start_ack(
        payload, sizeof(rs422_cmd_transfer_start_ack_t),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_READY, result);
}

TEST_F(RS422ProtocolTest, ParseDataAckPayloadLengthExact) {
    // 测试 payload_len 正好等于 sizeof 的情况
    // 这会触发第342行的分支3
    uint8_t payload[10];
    payload[0] = RS422_DATA_RECEIVED_OK;
    uint8_t result;

    int ret = rs422_parse_data_ack(
        payload, sizeof(rs422_cmd_data_ack_t),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_DATA_RECEIVED_OK, result);
}

TEST_F(RS422ProtocolTest, ParseTransferEndAckPayloadLengthExact) {
    // 测试 payload_len 正好等于 sizeof 的情况
    // 这会触发第359行的分支3
    uint8_t payload[10];
    payload[0] = RS422_TRANSFER_COMPLETE_OK;
    uint8_t result;

    int ret = rs422_parse_transfer_end_ack(
        payload, sizeof(rs422_cmd_transfer_end_ack_t),
        &result
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_TRANSFER_COMPLETE_OK, result);
}

TEST_F(RS422ProtocolTest, GetResultDescDataAckUnknownResult) {
    // 测试 RS422_CMD_DATA_ACK 的未知结果码
    // 这会触发第432-435行的 default 分支
    const char* desc = rs422_get_result_desc(
        RS422_CMD_DATA_ACK,
        0x55  // 未知的结果码
    );

    EXPECT_STREQ("未知结果", desc);
}

// ============ 集成测试 ============

TEST_F(RS422ProtocolTest, EncodeDecodeRoundTrip) {
    // 测试编码-解码往返
    uint8_t payload_in[] = {0x11, 0x22, 0x33, 0x44, 0x55};

    int frame_len = rs422_encode_frame(
        RS422_APID_CONTROL,
        RS422_CMD_VERSION_QUERY,
        payload_in, sizeof(payload_in),
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    uint16_t apid, cmd_code;
    const uint8_t *payload_out;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload_out, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_APID_CONTROL, apid);
    EXPECT_EQ(RS422_CMD_VERSION_QUERY, cmd_code);
    EXPECT_EQ(sizeof(payload_in), payload_len);
    EXPECT_EQ(0, memcmp(payload_in, payload_out, payload_len));
}

TEST_F(RS422ProtocolTest, BuildParseReconfigWorkflow) {
    // 测试重构启动命令的完整工作流
    int frame_len = rs422_build_reconfig_start(
        RS422_FILE_TYPE_DEFAULT,
        0x00,
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    // 解码帧
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_CMD_RECONFIG_START, cmd_code);
    EXPECT_EQ(2, payload_len);
    EXPECT_EQ(RS422_FILE_TYPE_DEFAULT, payload[0]);
    EXPECT_EQ(0x00, payload[1]);
}

TEST_F(RS422ProtocolTest, BuildParseVersionWorkflow) {
    // 测试版本查询的完整工作流
    int frame_len = rs422_build_version_query(
        frame_buf, sizeof(frame_buf)
    );
    ASSERT_GT(frame_len, 0);

    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    int ret = rs422_decode_frame(
        frame_buf, frame_len,
        &apid, &cmd_code,
        &payload, &payload_len
    );

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(RS422_CMD_VERSION_QUERY, cmd_code);
    EXPECT_EQ(0, payload_len);
}

TEST_F(RS422ProtocolTest, MultipleFramesSequence) {
    // 测试连续编码多个帧
    uint8_t buf1[100], buf2[100], buf3[100];

    int len1 = rs422_build_reconfig_start(0xFF, 0x00, buf1, sizeof(buf1));
    int len2 = rs422_build_version_query(buf2, sizeof(buf2));
    int len3 = rs422_build_transfer_end(buf3, sizeof(buf3));

    EXPECT_GT(len1, 0);
    EXPECT_GT(len2, 0);
    EXPECT_GT(len3, 0);

    // 验证每个帧都可以独立解码
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;

    EXPECT_EQ(SUCCESS, rs422_decode_frame(buf1, len1, &apid, &cmd_code, &payload, &payload_len));
    EXPECT_EQ(RS422_CMD_RECONFIG_START, cmd_code);

    EXPECT_EQ(SUCCESS, rs422_decode_frame(buf2, len2, &apid, &cmd_code, &payload, &payload_len));
    EXPECT_EQ(RS422_CMD_VERSION_QUERY, cmd_code);

    EXPECT_EQ(SUCCESS, rs422_decode_frame(buf3, len3, &apid, &cmd_code, &payload, &payload_len));
    EXPECT_EQ(RS422_CMD_TRANSFER_END, cmd_code);
}

// ============ 主函数 ============

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
