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
2. PAAU通过FTP下载软件包到`/opt/vendor/downloads/software/`
3. 解压到`/opt/vendor/versions/{version}/`
4. 验证antenna_mgmt文件的SHA256校验和
5. BBU发送版本激活指示（IE 311，ver_type=0）
6. PAAU更新符号链接`/opt/vendor/current` → `/opt/vendor/versions/{version}/`
7. **重启antenna-mgmt服务**，加载新版本软件
8. 新版本软件启动后继续运行

**重启方式**：
- 推荐：`systemctl restart antenna-mgmt.service`（服务重启）
- 备选：`shutdown -r now`（系统重启，谨慎使用）

**试运行与自动回退**：

BBU 激活新软件版本后，新版本先进入"试运行"，失败时自动切回上一个版本：

1. 激活时，程序先写试运行记录`/opt/vendor/versions/.rollback/trial`（新版本、上一个版本、启动次数），再原子地切换`/opt/vendor/current`。
2. systemd 每次启动服务前执行`/opt/vendor/scripts/upgrade-guard.sh`（`ExecStartPre`），启动次数 +1。
3. 新版本与 BBU **完成通道建立** → 确认健康，删除试运行记录。
4. 新版本连续运行满`UPGRADE_TRIAL_STABLE_SEC`秒（默认 120）→ 启动次数清零，此后断电/正常重启不计为失败，记录保留到 BBU 确认。
5. 连续启动`UPGRADE_TRIAL_MAX_STARTS`次（默认 3）仍未确认 → `upgrade-guard.sh`把`current`切回上一个版本，写回退通知`auto_rollback.last`。新版本一启动就崩时，约 30~40 秒回到旧版本。
6. 旧版本启动后读到回退通知，产生"版本激活失败"告警（1158，附加信息`auto rollback V2.0 -> V1.0`），TCP 建链时上报 BBU；通道建立后删除通知文件。

查看试运行/回退日志：`journalctl -u antenna-mgmt.service | grep upgrade-guard`。

限制：
- 只覆盖"起不来/启动后很快崩溃"。运行超过`UPGRADE_TRIAL_STABLE_SEC`后才崩溃、或进程不崩但卡死的情况不会触发回退。
- 首次安装（没有上一个版本）不进入试运行。
- 自动回退依赖`upgrade-guard.sh`与 service 中的`ExecStartPre`，它们不在软件版本包内，需通过`deploy-autostart.sh`安装一次。
- 回退到的旧版本若早于本功能，不会产生 1158 告警（但回退本身由脚本完成，不受影响）。

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

**FPGA 种类（由 metadata.txt 的 `file_sub_type` 区分）**：

| file_sub_type | FPGA 种类 | 存储目录 |
|---|---|---|
| 0x01 / 0x02 | V7A | `/opt/vendor/firmware/v7a/{version}/` |
| 0x13 / 0x14 | V7B | `/opt/vendor/firmware/v7b/{version}/` |
| 0x11 / 0x12 | 子阵 | `/opt/vendor/firmware/subarray/{version}/` |

其他子类型的固件包在下载阶段即被拒收（结果指示返回"其他失败"）。

**存储布局（软件与固件分开、不同 FPGA 分开）**：
```
/opt/vendor/
├── versions/{version}/              # 软件版本（/opt/vendor/current 指向这里）
├── firmware/
│   ├── v7a/{version}/               # V7A 固件
│   ├── v7b/{version}/               # V7B 固件
│   ├── subarray/{version}/          # 子阵固件
│   └── .staging/                    # 解压暂存区（判定种类前）
└── downloads/
    ├── software/                    # 软件包下载暂存
    └── firmware/                    # 固件包下载暂存
```
不同 FPGA 的固件即使版本号相同也互不覆盖；只有同一种 FPGA 的同版本包会被新包替换。

**激活流程**：
1. BBU发送版本下载请求（IE 14，ver_type=1）
2. PAAU通过FTP下载固件包到`/opt/vendor/downloads/firmware/`
3. 解压到暂存区`/opt/vendor/firmware/.staging/`，验证fpga_firmware.bin的SHA256校验和
4. 读取metadata.txt，按`file_sub_type`判定FPGA种类，以`version`为版本号移入`/opt/vendor/firmware/{种类}/{version}/`
5. BBU发送版本激活指示（IE 311，ver_type=1）
6. PAAU在v7a/v7b/subarray三个目录中查找该版本（大小写不敏感），并核对包内`file_sub_type`与所在目录一致
7. **通过RS-422串口上注固件到FPGA**
8. 上注完成后发送结果指示给BBU

**注意**：
- IE 311 只带版本号、不带FPGA种类。若多种FPGA存在同一版本号，取最近入库的那个并在日志中告警，因此**建议不同FPGA的固件使用不同的版本号**（如带上 V7A/V7B/SUB 前缀）。
- 激活固件时不会到`/opt/vendor/versions/`下查找（软件包也带`file_sub_type`，避免误把软件当固件上注）。旧版本程序存放在`versions/`下的固件，升级后需重新下载一次。

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
