# 版本打包和校验脚本使用说明

## 概述

本目录包含用于创建和验证版本升级包的脚本工具。

## 脚本列表

1. **create_version_package.sh** - 创建版本升级包
2. **verify_package.sh** - 验证版本升级包完整性
3. **antenna-mgmt.service** - systemd服务文件
4. **antenna-mgmt-launcher.sh** - 服务启动脚本
5. **restart-after-activate.sh** - 激活后重启脚本

## 使用流程

### 1. 创建版本包（BBU侧）

```bash
# 基本用法
./create_version_package.sh -v v1.0.1 -b /path/to/antenna_mgmt

# 指定输出目录
./create_version_package.sh -v v1.0.1 -b ./antenna_mgmt -o /tmp/packages

# 查看帮助
./create_version_package.sh -h
```

**输出示例**：
```
========================================
  Version Package Creator
========================================

Version:     v1.0.1
Binary:      ./antenna_mgmt
Output:      .

[1/5] Copying binary file...
      File size: 1048576 bytes
[2/5] Calculating SHA-256 checksum...
      Checksum: a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890
[3/5] Creating checksum.txt...
      Created: checksum.txt
[4/5] Creating tar.gz package...
      Package: ./antenna_v1.0.1.tar.gz
      Size: 524288 bytes
[5/5] Verifying package...
      ✓ Verification PASSED

========================================
Package created successfully!
========================================

Package Information:
  File:     ./antenna_v1.0.1.tar.gz
  Size:     524288 bytes
  Checksum: a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890

Upload to FTP server:
  ftp <server>
  > cd /versions
  > put antenna_v1.0.1.tar.gz

CPRI Message Parameters (MsgID: 21):
  file_path:   /versions/
  file_name:   antenna_v1.0.1.tar.gz
  file_ver:    v1.0.1
  file_len:    524288
```

### 2. 验证版本包

```bash
# 验证包的完整性
./verify_package.sh antenna_v1.0.1.tar.gz
```

**成功输出**：
```
========================================
  Version Package Verifier
========================================

Package: antenna_v1.0.1.tar.gz

[1/4] Extracting package...
      ✓ Extracted successfully
[2/4] Reading expected checksum...
      Expected: a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890
[3/4] Calculating actual checksum...
      Actual:   a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890
[4/4] Comparing checksums...

========================================
✓ Checksum verification PASSED
========================================

Binary file information:
  Size:     1048576 bytes
  Checksum: a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890
  Executable: Yes

This package is ready for deployment.
```

**失败输出**：
```
========================================
✗ Checksum verification FAILED
========================================

Expected: a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890
Actual:   ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff

WARNING: This package may be corrupted or tampered!
DO NOT deploy this package!
```

### 3. 上传到FTP服务器

```bash
# 使用FTP命令行
ftp 10.10.10.6
> cd /versions
> binary
> put antenna_v1.0.1.tar.gz
> quit

# 或使用curl
curl -T antenna_v1.0.1.tar.gz --user anonymous: ftp://10.10.10.6/versions/
```

### 4. BBU触发下载

BBU发送CPRI消息 (MsgID: 21) 包含以下参数：
- `file_path`: `/versions/`
- `file_name`: `antenna_v1.0.1.tar.gz`
- `file_ver`: `v1.0.1`
- `file_len`: `524288` (从create脚本输出获取)

### 5. PAAU自动处理

PAAU收到请求后会自动：
1. 下载tar.gz文件
2. 解压到 `/opt/vendor/versions/v1.0.1/`
3. 读取 `checksum.txt`
4. 计算 `antenna_mgmt.bin` 的SHA-256
5. 比对校验和
6. 如果匹配：保存元数据，发送成功结果
7. 如果不匹配：删除文件，发送失败结果

### 6. 激活新版本

BBU发送激活指示 (MsgID: 31)：
- `version_num`: `v1.0.1`

PAAU会：
1. 切换符号链接
2. 发送激活应答
3. 自动重启服务

## 包结构

创建的tar.gz包包含以下文件：

```
antenna_v1.0.1.tar.gz
├── antenna_mgmt.bin        # 可执行程序
└── checksum.txt            # SHA-256校验和
```

**checksum.txt 格式**：
```
a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890
```

## 安全特性

1. **完整性验证**：
   - 创建时自动计算SHA-256
   - 部署前可手动验证
   - PAAU自动验证

2. **防篡改**：
   - 校验和不匹配时拒绝安装
   - 自动删除损坏的文件
   - 记录安全告警日志

3. **可追溯**：
   - 每个包都有唯一的校验和
   - 完整的操作日志
   - 版本元数据记录

## 故障排查

### 问题1: sha256sum命令不存在

**解决方案**：
```bash
# Debian/Ubuntu
sudo apt-get install coreutils

# macOS (使用shasum)
# 脚本会自动检测并使用shasum
```

### 问题2: 包验证失败

**检查步骤**：
```bash
# 1. 检查包内容
tar -tzf antenna_v1.0.1.tar.gz

# 2. 手动解压
mkdir /tmp/test
tar -xzf antenna_v1.0.1.tar.gz -C /tmp/test

# 3. 查看checksum.txt
cat /tmp/test/checksum.txt

# 4. 手动计算校验和
sha256sum /tmp/test/antenna_mgmt.bin
```

### 问题3: FTP上传失败

**检查步骤**：
```bash
# 测试FTP连接
ftp 10.10.10.6

# 测试匿名登录
curl -v ftp://10.10.10.6/

# 检查目录权限
# 确保FTP服务器允许匿名上传
```

## 最佳实践

1. **版本命名**：
   - 使用语义化版本号：`v1.0.1`
   - 包含主版本、次版本、补丁版本

2. **验证流程**：
   - 创建包后立即验证
   - 上传前再次验证
   - 保存校验和记录

3. **备份**：
   - 保留所有历史版本包
   - 记录每个版本的校验和
   - 定期备份FTP服务器

4. **测试**：
   - 在测试环境先验证
   - 确认升级和回滚流程
   - 验证校验和机制

## 示例工作流

```bash
# 1. 编译新版本
make clean
make

# 2. 创建版本包
./scripts/create_version_package.sh -v v1.0.1 -b ./antenna_mgmt -o /tmp/packages

# 3. 验证包
./scripts/verify_package.sh /tmp/packages/antenna_v1.0.1.tar.gz

# 4. 上传到FTP
curl -T /tmp/packages/antenna_v1.0.1.tar.gz \
     --user anonymous: \
     ftp://10.10.10.6/versions/

# 5. BBU触发下载
# (通过BBU管理界面或API发送CPRI消息)

# 6. 监控PAAU日志
ssh paau@10.10.10.8
tail -f /var/log/antenna_mgmt/antenna_mgmt.log

# 7. BBU触发激活
# (通过BBU管理界面或API发送CPRI消息)

# 8. 验证新版本
readlink -f /opt/vendor/current
# 应该指向: /opt/vendor/versions/v1.0.1
```

## 相关文档

- [版本管理系统实现文档](../天线管里面需求/version_manager_implementation.md)
- [校验和验证流程](../天线管里面需求/checksum_verification_implementation.md)
- [部署指南](../天线管里面需求/version_deployment_guide.md)
- [版本切换方案](../天线管里面需求/version_checksum_and_switching.md)
