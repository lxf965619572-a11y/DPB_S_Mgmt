# 在D2000/Ubuntu 18.04上运行测试

## 前提条件

本测试框架需要在Linux环境(D2000/Ubuntu 18.04)上运行。当前Windows环境仅用于代码编辑。

## 部署到D2000目标机

### 1. 拷贝项目到目标机

```bash
# 在Windows上打包
cd D:\lixf\StarNet_SC_ANTENNA
tar -czf s_band_antenna_mgmt.tar.gz s_band_antenna_mgmt/

# 拷贝到D2000目标机
scp s_band_antenna_mgmt.tar.gz user@d2000-target:/tmp/

# 在D2000上解压
ssh user@d2000-target
cd /tmp
tar -xzf s_band_antenna_mgmt.tar.gz
cd s_band_antenna_mgmt
```

### 2. 安装依赖(如果还没安装)

```bash
# 更新软件源
sudo apt-get update

# 安装编译工具
sudo apt-get install -y build-essential gcc g++ make

# 安装Google Test
sudo apt-get install -y libgtest-dev
cd /usr/src/gtest
sudo cmake CMakeLists.txt
sudo make
sudo cp *.a /usr/lib

# 安装覆盖率工具
sudo apt-get install -y lcov gcovr
```

### 3. 编译并运行测试

```bash
cd /tmp/s_band_antenna_mgmt

# 方法1: 使用Makefile
make test

# 方法2: 手动编译
# 编译源文件
gcc -Wall -Wextra -I./include -I./src --coverage -fprofile-arcs -ftest-coverage \
    -c src/rs422_protocol.c -o build/coverage/obj/rs422_protocol.o

gcc -Wall -Wextra -I./include -I./src --coverage -fprofile-arcs -ftest-coverage \
    -c src/logger.c -o build/coverage/obj/logger.o

# 编译测试文件
g++ -std=c++14 -Wall -Wextra -I./include -I./src --coverage -fprofile-arcs -ftest-coverage \
    -c tests/unit/test_rs422_protocol.cpp -o build/coverage/obj/test_rs422_protocol.o

# 链接
g++ --coverage build/coverage/obj/*.o -o build/coverage/bin/test_rs422_protocol \
    -lgtest -lgtest_main -lpthread

# 运行测试
./build/coverage/bin/test_rs422_protocol
```

### 4. 生成覆盖率报告

```bash
# 使用Makefile
make coverage

# 或手动生成
lcov --directory build/coverage --capture --output-file build/coverage/coverage.info --rc lcov_branch_coverage=1
lcov --remove build/coverage/coverage.info '/usr/*' '*/tests/*' --output-file build/coverage/coverage_filtered.info --rc lcov_branch_coverage=1
genhtml build/coverage/coverage_filtered.info --output-directory reports/html --branch-coverage
```

### 5. 查看报告

```bash
# 在D2000上查看
firefox reports/html/index.html

# 或拷贝回Windows查看
scp -r user@d2000-target:/tmp/s_band_antenna_mgmt/reports/html /d/lixf/StarNet_SC_ANTENNA/test_reports/
# 然后在Windows上用浏览器打开 test_reports/index.html
```

## 预期输出

### 测试运行输出

```
[==========] Running 13 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 13 tests from RS422ProtocolTest
[ RUN      ] RS422ProtocolTest.ChecksumCalculation
[       OK ] RS422ProtocolTest.ChecksumCalculation (0 ms)
[ RUN      ] RS422ProtocolTest.ChecksumEmpty
[       OK ] RS422ProtocolTest.ChecksumEmpty (0 ms)
[ RUN      ] RS422ProtocolTest.CRC16Calculation
[       OK ] RS422ProtocolTest.CRC16Calculation (0 ms)
[ RUN      ] RS422ProtocolTest.EncodeFrameBasic
[       OK ] RS422ProtocolTest.EncodeFrameBasic (0 ms)
[ RUN      ] RS422ProtocolTest.EncodeFrameNullPayload
[       OK ] RS422ProtocolTest.EncodeFrameNullPayload (0 ms)
[ RUN      ] RS422ProtocolTest.DecodeFrameBasic
[       OK ] RS422ProtocolTest.DecodeFrameBasic (1 ms)
[ RUN      ] RS422ProtocolTest.DecodeFrameInvalidSync
[       OK ] RS422ProtocolTest.DecodeFrameInvalidSync (0 ms)
[ RUN      ] RS422ProtocolTest.BuildTransferStartCommand
[       OK ] RS422ProtocolTest.BuildTransferStartCommand (0 ms)
[ RUN      ] RS422ProtocolTest.ParseTransferStartAck
[       OK ] RS422ProtocolTest.ParseTransferStartAck (0 ms)
[ RUN      ] RS422ProtocolTest.GetCommandName
[       OK ] RS422ProtocolTest.GetCommandName (0 ms)
[ RUN      ] RS422ProtocolTest.GetResultDescription
[       OK ] RS422ProtocolTest.GetResultDescription (0 ms)
[----------] 13 tests from RS422ProtocolTest (2 ms total)

[----------] Global test environment tear-down
[==========] 13 tests from 1 test suite ran. (2 ms total)
[  PASSED  ] 13 tests.
```

### 覆盖率输出

```
Overall coverage rate:
  lines......: 85.3% (256 of 300 lines)
  functions..: 94.7% (18 of 19 functions)
  branches...: 78.2% (89 of 114 branches)
```

## 故障排查

### 问题1: Google Test找不到

```bash
# 检查是否安装
dpkg -l | grep gtest

# 如果没有，重新安装
sudo apt-get install -y libgtest-dev
cd /usr/src/gtest
sudo cmake CMakeLists.txt
sudo make
sudo cp *.a /usr/lib
sudo ldconfig
```

### 问题2: 链接错误

```bash
# 确保链接顺序正确
g++ test.o lib.o -o test -lgtest -lgtest_main -lpthread

# 检查库路径
ldconfig -p | grep gtest
```

### 问题3: 覆盖率数据不生成

```bash
# 确保编译时添加了覆盖率标志
gcc --coverage -fprofile-arcs -ftest-coverage source.c

# 运行测试后检查.gcda文件
find . -name "*.gcda"

# 如果没有，检查权限
chmod 777 build/coverage/
```

## 下一步

1. 在D2000上成功运行测试
2. 查看覆盖率报告
3. 添加更多测试用例
4. 集成到CI/CD流水线

---

**文档版本**: v1.0
**创建日期**: 2026-03-16
