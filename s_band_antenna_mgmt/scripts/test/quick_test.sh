#!/bin/bash
# 快速测试脚本

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/coverage"

# 颜色
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo -e "${GREEN}=== Quick Test & Coverage Demo ===${NC}"
echo "Project: ${PROJECT_ROOT}"
echo ""

# 1. 创建构建目录
echo -e "${GREEN}Step 1: Creating build directory...${NC}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# 2. 运行CMake
echo -e "${GREEN}Step 2: Running CMake...${NC}"
cmake -DCMAKE_BUILD_TYPE=Coverage "${PROJECT_ROOT}"

# 3. 编译
echo -e "${GREEN}Step 3: Building...${NC}"
make -j$(nproc)

# 4. 运行测试
echo -e "${GREEN}Step 4: Running tests...${NC}"
ctest --output-on-failure --verbose

# 5. 生成覆盖率
echo -e "${GREEN}Step 5: Generating coverage report...${NC}"
make coverage

# 6. 显示结果
echo ""
echo -e "${GREEN}=== Test Complete! ===${NC}"
echo "Coverage report: ${PROJECT_ROOT}/reports/html/index.html"
echo ""
echo "To view the report, run:"
echo "  firefox ${PROJECT_ROOT}/reports/html/index.html"
echo "  # or"
echo "  xdg-open ${PROJECT_ROOT}/reports/html/index.html"
