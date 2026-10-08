#!/bin/bash
#
# 验证版本升级包脚本
# 用于验证tar.gz包的完整性
#

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 显示帮助
show_help() {
    echo "Usage: $0 <package.tar.gz>"
    echo ""
    echo "Verifies the integrity of a version package by:"
    echo "  1. Extracting the package"
    echo "  2. Reading the expected checksum from checksum.txt"
    echo "  3. Calculating the actual checksum of antenna_mgmt.bin"
    echo "  4. Comparing the two checksums"
    echo ""
    echo "Example:"
    echo "  $0 antenna_v1.0.1.tar.gz"
    exit 0
}

# 检查参数
if [ $# -eq 0 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    show_help
fi

PACKAGE="$1"

# 检查文件
if [ ! -f "$PACKAGE" ]; then
    echo -e "${RED}Error: Package not found: $PACKAGE${NC}"
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Version Package Verifier${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "${GREEN}Package:${NC} $PACKAGE"
echo ""

# 创建临时目录
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# 解压
echo -e "${YELLOW}[1/4]${NC} Extracting package..."
tar -xzf "$PACKAGE" -C "$TEMP_DIR" 2>&1

# 检查文件
if [ ! -f "$TEMP_DIR/antenna_mgmt.bin" ]; then
    echo -e "${RED}Error: antenna_mgmt.bin not found in package${NC}"
    exit 1
fi

if [ ! -f "$TEMP_DIR/checksum.txt" ]; then
    echo -e "${RED}Error: checksum.txt not found in package${NC}"
    exit 1
fi

echo "      ✓ Extracted successfully"

# 读取预期校验和
echo -e "${YELLOW}[2/4]${NC} Reading expected checksum..."
EXPECTED=$(cat "$TEMP_DIR/checksum.txt" | head -1 | awk '{print $1}')

if [ -z "$EXPECTED" ]; then
    echo -e "${RED}Error: Failed to read checksum from checksum.txt${NC}"
    exit 1
fi

echo "      Expected: $EXPECTED"

# 计算实际校验和
echo -e "${YELLOW}[3/4]${NC} Calculating actual checksum..."
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL=$(sha256sum "$TEMP_DIR/antenna_mgmt.bin" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    ACTUAL=$(shasum -a 256 "$TEMP_DIR/antenna_mgmt.bin" | awk '{print $1}')
else
    echo -e "${RED}Error: sha256sum or shasum not found${NC}"
    exit 1
fi

echo "      Actual:   $ACTUAL"

# 比对
echo -e "${YELLOW}[4/4]${NC} Comparing checksums..."

# 转换为小写进行比较
EXPECTED_LOWER=$(echo "$EXPECTED" | tr '[:upper:]' '[:lower:]')
ACTUAL_LOWER=$(echo "$ACTUAL" | tr '[:upper:]' '[:lower:]')

if [ "$EXPECTED_LOWER" == "$ACTUAL_LOWER" ]; then
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${GREEN}✓ Checksum verification PASSED${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    # 显示文件信息
    FILE_SIZE=$(stat -c%s "$TEMP_DIR/antenna_mgmt.bin" 2>/dev/null || stat -f%z "$TEMP_DIR/antenna_mgmt.bin")
    echo "Binary file information:"
    echo "  Size:     $FILE_SIZE bytes"
    echo "  Checksum: $ACTUAL"

    # 检查是否可执行
    if [ -x "$TEMP_DIR/antenna_mgmt.bin" ]; then
        echo "  Executable: Yes"
    else
        echo "  Executable: No (will be set during installation)"
    fi

    echo ""
    echo "This package is ready for deployment."
    exit 0
else
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${RED}✗ Checksum verification FAILED${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    echo "Expected: $EXPECTED"
    echo "Actual:   $ACTUAL"
    echo ""
    echo -e "${RED}WARNING: This package may be corrupted or tampered!${NC}"
    echo -e "${RED}DO NOT deploy this package!${NC}"
    exit 1
fi
