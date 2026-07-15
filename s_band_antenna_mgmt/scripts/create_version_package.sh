#!/bin/bash
#
# 创建版本升级包脚本
# 用于BBU侧打包可执行文件和校验和
#

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
VERSION=""
BIN_FILE=""
OUTPUT_DIR="."

# 显示帮助
show_help() {
    echo "Usage: $0 -v <version> -b <bin_file> [-o <output_dir>]"
    echo ""
    echo "Options:"
    echo "  -v <version>      Version string (e.g., v1.0.1)"
    echo "  -b <bin_file>     Path to executable binary file"
    echo "  -o <output_dir>   Output directory (default: current directory)"
    echo "  -h                Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 -v v1.0.1 -b ./antenna_mgmt -o /tmp/packages"
    exit 0
}

# 解析参数
while getopts "v:b:o:h" opt; do
    case $opt in
        v) VERSION="$OPTARG" ;;
        b) BIN_FILE="$OPTARG" ;;
        o) OUTPUT_DIR="$OPTARG" ;;
        h) show_help ;;
        *) show_help ;;
    esac
done

# 检查必需参数
if [ -z "$VERSION" ] || [ -z "$BIN_FILE" ]; then
    echo -e "${RED}Error: Version and bin file are required${NC}"
    show_help
fi

# 检查bin文件
if [ ! -f "$BIN_FILE" ]; then
    echo -e "${RED}Error: Binary file not found: $BIN_FILE${NC}"
    exit 1
fi

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

# 临时工作目录
WORK_DIR=$(mktemp -d)
trap "rm -rf $WORK_DIR" EXIT

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Version Package Creator${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "${GREEN}Version:${NC}     $VERSION"
echo -e "${GREEN}Binary:${NC}      $BIN_FILE"
echo -e "${GREEN}Output:${NC}      $OUTPUT_DIR"
echo ""

# 复制bin文件到工作目录
echo -e "${YELLOW}[1/5]${NC} Copying binary file..."
cp "$BIN_FILE" "$WORK_DIR/antenna_mgmt.bin"
chmod +x "$WORK_DIR/antenna_mgmt.bin"

# 获取文件信息
FILE_SIZE=$(stat -c%s "$WORK_DIR/antenna_mgmt.bin" 2>/dev/null || stat -f%z "$WORK_DIR/antenna_mgmt.bin")
echo "      File size: $FILE_SIZE bytes"

# 计算SHA-256校验和
echo -e "${YELLOW}[2/5]${NC} Calculating SHA-256 checksum..."
if command -v sha256sum >/dev/null 2>&1; then
    CHECKSUM=$(sha256sum "$WORK_DIR/antenna_mgmt.bin" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    CHECKSUM=$(shasum -a 256 "$WORK_DIR/antenna_mgmt.bin" | awk '{print $1}')
else
    echo -e "${RED}Error: sha256sum or shasum not found${NC}"
    exit 1
fi

echo "      Checksum: $CHECKSUM"

# 创建checksum.txt
echo -e "${YELLOW}[3/5]${NC} Creating checksum.txt..."
cat > "$WORK_DIR/checksum.txt" <<EOF
$CHECKSUM
EOF

echo "      Created: checksum.txt"

# 创建tar.gz包
OUTPUT_FILE="$OUTPUT_DIR/antenna_${VERSION}.tar.gz"
echo -e "${YELLOW}[4/5]${NC} Creating tar.gz package..."
cd "$WORK_DIR"
tar -czf "$OUTPUT_FILE" antenna_mgmt.bin checksum.txt
cd - > /dev/null

PACKAGE_SIZE=$(stat -c%s "$OUTPUT_FILE" 2>/dev/null || stat -f%z "$OUTPUT_FILE")
echo "      Package: $OUTPUT_FILE"
echo "      Size: $PACKAGE_SIZE bytes"

# 验证包
echo -e "${YELLOW}[5/5]${NC} Verifying package..."
VERIFY_DIR=$(mktemp -d)
tar -xzf "$OUTPUT_FILE" -C "$VERIFY_DIR"

VERIFY_CHECKSUM=$(cat "$VERIFY_DIR/checksum.txt")
if command -v sha256sum >/dev/null 2>&1; then
    VERIFY_ACTUAL=$(sha256sum "$VERIFY_DIR/antenna_mgmt.bin" | awk '{print $1}')
else
    VERIFY_ACTUAL=$(shasum -a 256 "$VERIFY_DIR/antenna_mgmt.bin" | awk '{print $1}')
fi

rm -rf "$VERIFY_DIR"

if [ "$VERIFY_CHECKSUM" == "$VERIFY_ACTUAL" ]; then
    echo -e "      ${GREEN}✓ Verification PASSED${NC}"
else
    echo -e "      ${RED}✗ Verification FAILED${NC}"
    exit 1
fi

# 生成上传信息
echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Package created successfully!${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo "Package Information:"
echo "  File:     $OUTPUT_FILE"
echo "  Size:     $PACKAGE_SIZE bytes"
echo "  Checksum: $CHECKSUM"
echo ""
echo "Upload to FTP server:"
echo "  ftp <server>"
echo "  > cd /versions"
echo "  > put $OUTPUT_FILE"
echo ""
echo "CPRI Message Parameters (MsgID: 21):"
echo "  file_path:   /versions/"
echo "  file_name:   antenna_${VERSION}.tar.gz"
echo "  file_ver:    $VERSION"
echo "  file_len:    $PACKAGE_SIZE"
echo ""
