# 快速开始指南

## 目录结构

已创建以下目录结构：

```
s_band_antenna_mgmt/
├── CMakeLists.txt              # 根CMake配置
├── src/                        # 源代码
│   ├── rs422_protocol.c
│   └── logger.c
├── include/                    # 头文件
│   ├── rs422_protocol.h
│   ├── common.h
│   └── logger.h
├── tests/                      # 测试代码
│   ├── CMakeLists.txt
│   └── unit/
│       └── test_rs422_protocol.cpp  # 示例测试
├── build/                      # 构建输出
│   ├── debug/
│   ├── release/
│   └── coverage/
├── scripts/                    # 脚本
│   ├── build/
│   ├── test/
│   │   └── quick_test.sh      # 快速测试脚本
│   ├── coverage/
│   └── ci/
└── reports/                    # 报告输出
    ├── html/                   # HTML覆盖率报告
    ├── xml/                    # XML测试报告
    └── json/
```

## 快速运行

### 方法1: 使用快速测试脚本

```bash
cd /d/lixf/StarNet_SC_ANTENNA/s_band_antenna_mgmt
./scripts/test/quick_test.sh
```

### 方法2: 手动执行

```bash
# 1. 创建构建目录
mkdir -p build/coverage
cd build/coverage

# 2. 配置CMake
cmake -DCMAKE_BUILD_TYPE=Coverage ../..

# 3. 编译
make -j$(nproc)

# 4. 运行测试
ctest --output-on-failure --verbose

# 5. 生成覆盖率报告
make coverage

# 6. 查看报告
firefox ../../reports/html/index.html
```

## 预期输出

### 测试输出
```
[==========] Running 13 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 13 tests from RS422ProtocolTest
[ RUN      ] RS422ProtocolTest.ChecksumCalculation
[       OK ] RS422ProtocolTest.ChecksumCalculation (0 ms)
[ RUN      ] RS422ProtocolTest.CRC16Calculation
[       OK ] RS422ProtocolTest.CRC16Calculation (0 ms)
...
[----------] 13 tests from RS422ProtocolTest (5 ms total)
[==========] 13 tests from 1 test suite ran. (5 ms total)
[  PASSED  ] 13 tests.
```

### 覆盖率输出
```
Overall coverage rate:
  lines......: 85.3% (256 of 300 lines)
  functions..: 94.7% (18 of 19 functions)
  branches...: 78.2% (89 of 114 branches)
```

## 查看报告

覆盖率HTML报告位置：
```
reports/html/index.html
```

在浏览器中打开即可查看详细的覆盖率信息。

## 故障排查

如果遇到问题，请检查：

1. Google Test是否已安装
   ```bash
   pkg-config --modversion gtest
   ```

2. lcov是否已安装
   ```bash
   lcov --version
   ```

3. 编译器版本
   ```bash
   gcc --version  # 需要 >= 7.5
   ```

## 下一步

- 添加更多测试用例到 `tests/unit/`
- 实现Mock对象到 `tests/mocks/`
- 添加集成测试到 `tests/integration/`
- 配置CI/CD流水线

---

**创建日期**: 2026-03-16
