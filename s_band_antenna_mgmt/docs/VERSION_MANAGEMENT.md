# 版本管理说明

## 版本类型

系统支持两种版本类型：

### 1. 软件版本（ver_type=0）

**用途**：更新PAAU主控软件（antenna_mgmt程序本身）

**包内容**：
```
software_v1.0.0.tar.gz
├── antenna_mgmt          # 可执行文件
├── lib/                  # 依赖库（可选）
│   └── libxxx.so
└── metadata.txt          # 元数据文件
```

**metadata.txt格式**：
```
# 软件包元数据文件
file_type=0xFA
file_sub_type=0x01
sha256=<64位十六进制校验和>
version=V1.0.0.20260401
bin_file=antenna_mgmt
```

**激活流程**：
1. BBU发送版本下载请求（IE 14，ver_type=0）
2. PAAU通过FTP下载软件包到`/opt/vendor/downloads/`
3. 解压到`/opt/vendor/versions/{version}/`
4. 验证antenna_mgmt文件的SHA256校验和
5. BBU发送版本激活指示（IE 311，ver_type=0）
6. PAAU更新符号链接`/opt/vendor/current` → `/opt/vendor/versions/{version}/`
7. **重启antenna-mgmt服务**，加载新版本软件
8. 新版本软件启动后继续运行

**重启方式**：
- 推荐：`systemctl restart antenna-mgmt.service`（服务重启）
- 备选：`shutdown -r now`（系统重启，谨慎使用）

---

### 2. 固件版本（ver_type=1）

**用途**：更新FPGA固件

**包内容**：
```
firmware_v1.0.0.tar.gz
├── fpga_firmware.bin     # FPGA固件二进制文件
└── metadata.txt          # 元数据文件
```

**metadata.txt格式**：
```
# FPGA固件包元数据文件
file_type=0xFA
file_sub_type=0x02
sha256=<64位十六进制校验和>
version=V1.0.0.20260401
bin_file=fpga_firmware.bin
```

**激活流程**：
1. BBU发送版本下载请求（IE 14，ver_type=1）
2. PAAU通过FTP下载固件包到`/opt/vendor/downloads/`
3. 解压到`/opt/vendor/versions/{version}/`
4. 验证fpga_firmware.bin文件的SHA256校验和
5. BBU发送版本激活指示（IE 311，ver_type=1）
6. PAAU更新符号链接（记录当前固件版本）
7. **通过RS-422串口上注固件到FPGA**
8. 上注完成后发送结果指示给BBU

**上注方式**：
- 通过`/dev/ttyAMA2`（RS-422串口）
- 使用RS-422协议分包传输
- FPGA接收并烧写固件

---

## 版本包制作

### 软件版本包制作脚本

```bash
#!/bin/bash
# 制作软件版本包

VERSION="V1.0.0.20260401"
BUILD_DIR="build/software_${VERSION}"
PACKAGE_NAME="software_${VERSION}.tar.gz"

# 创建临时目录
mkdir -p "$BUILD_DIR"

# 复制可执行文件
cp build/antenna_mgmt "$BUILD_DIR/"
chmod +x "$BUILD_DIR/antenna_mgmt"

# 复制依赖库（如果有）
# mkdir -p "$BUILD_DIR/lib"
# cp lib/*.so "$BUILD_DIR/lib/"

# 计算SHA256校验和
CHECKSUM=$(sha256sum "$BUILD_DIR/antenna_mgmt" | awk '{print $1}')

# 生成metadata.txt
cat > "$BUILD_DIR/metadata.txt" <<EOF
# 软件包元数据文件
file_type=0xFA
file_sub_type=0x01
sha256=${CHECKSUM}
version=${VERSION}
bin_file=antenna_mgmt
EOF

# 打包
cd build
tar -czf "$PACKAGE_NAME" "software_${VERSION}"
cd ..

echo "软件版本包已创建: build/$PACKAGE_NAME"
echo "SHA256: $CHECKSUM"
```

### 固件版本包制作脚本

```bash
#!/bin/bash
# 制作固件版本包

VERSION="V1.0.0.20260401"
BUILD_DIR="build/firmware_${VERSION}"
PACKAGE_NAME="firmware_${VERSION}.tar.gz"

# 创建临时目录
mkdir -p "$BUILD_DIR"

# 复制固件文件
cp fpga/fpga_firmware.bin "$BUILD_DIR/"

# 计算SHA256校验和
CHECKSUM=$(sha256sum "$BUILD_DIR/fpga_firmware.bin" | awk '{print $1}')

# 生成metadata.txt
cat > "$BUILD_DIR/metadata.txt" <<EOF
# FPGA固件包元数据文件
file_type=0xFA
file_sub_type=0x02
sha256=${CHECKSUM}
version=${VERSION}
bin_file=fpga_firmware.bin
EOF

# 打包
cd build
tar -czf "$PACKAGE_NAME" "firmware_${VERSION}"
cd ..

echo "固件版本包已创建: build/$PACKAGE_NAME"
echo "SHA256: $CHECKSUM"
```

---

## 版本回滚

如果新版本出现问题，可以回滚到之前的版本：

```bash
# 查看可用版本
ls /opt/vendor/versions/

# 查看回滚信息
ls /opt/vendor/versions/.rollback/

# 手动回滚（需要root权限）
sudo ln -sfn /opt/vendor/versions/V1.0.0.20260401 /opt/vendor/current
sudo systemctl restart antenna-mgmt.service
```

---

## 注意事项

1. **软件版本激活会重启服务**，会短暂中断与BBU的连接（约10秒）
2. **固件版本激活不会重启服务**，但FPGA会重新加载固件
3. 版本包的SHA256校验和必须正确，否则会拒绝激活
4. 建议在业务低峰期进行版本升级
5. 升级前确保有足够的磁盘空间（至少100MB）
6. 每次激活都会自动备份当前版本信息到`.rollback/`目录

---

## 故障排查

### 软件版本激活失败

```bash
# 检查服务状态
systemctl status antenna-mgmt.service

# 查看日志
journalctl -u antenna-mgmt.service -n 100

# 检查符号链接
ls -l /opt/vendor/current

# 手动启动测试
/opt/vendor/current/antenna_mgmt /opt/vendor/config/antenna_mgmt.conf
```

### 固件版本上注失败

```bash
# 检查RS-422串口
ls -l /dev/ttyAMA2

# 查看上注日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log | grep -i firmware

# 检查FPGA状态
# （通过UART查询FPGA版本信息）
```
