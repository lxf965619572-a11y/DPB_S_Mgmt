#!/bin/bash
# 一键运行单元测试和覆盖率报告

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/coverage"

echo "=== S-Band天线管理 - 单元测试 ==="
echo ""

# 1. 创建构建目录
if [ ! -d "$BUILD_DIR" ]; then
    echo "创建构建目录..."
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# 2. 配置CMake
echo "配置CMake (Coverage模式)..."
cmake -DCMAKE_BUILD_TYPE=Coverage ../..

# 3. 编译
echo ""
echo "编译项目..."
make -j$(nproc)

# 4. 运行测试
echo ""
echo "运行单元测试..."
ctest --output-on-failure

# 5. 生成覆盖率报告
echo ""
echo "生成覆盖率报告..."
make coverage

echo ""
echo "=== 完成! ==="
echo ""
echo "覆盖率报告: $PROJECT_ROOT/build/coverage/reports/html/index.html"
echo ""
echo "在浏览器中打开查看:"
echo "  firefox $PROJECT_ROOT/build/coverage/reports/html/index.html"
echo "  或"
echo "  xdg-open $PROJECT_ROOT/build/coverage/reports/html/index.html"
