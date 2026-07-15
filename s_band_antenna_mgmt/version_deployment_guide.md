# 版本管理系统部署和使用指南

## 1. 系统部署

### 1.1 目录结构创建

```bash
# 创建版本管理目录
sudo mkdir -p /opt/vendor/versions
sudo mkdir -p /opt/vendor/downloads
sudo mkdir -p /opt/vendor/config
sudo mkdir -p /opt/vendor/scripts
sudo mkdir -p /var/log/antenna_mgmt

# 设置权限
sudo chown -R root:root /opt/vendor
sudo chmod -R 755 /opt/vendor
```

### 1.2 安装systemd服务

```bash
# 复制服务文件
sudo cp scripts/antenna-mgmt.service /etc/systemd/system/

# 复制启动脚本
sudo cp scripts/antenna-mgmt-launcher.sh /opt/vendor/scripts/
sudo cp scripts/restart-after-activate.sh /opt/vendor/scripts/

# 设置执行权限
sudo chmod +x /opt/vendor/scripts/*.sh

# 重新加载systemd
sudo systemctl daemon-reload

# 启用服务（开机自启）
sudo systemctl enable antenna-mgmt.service
```

### 1.3 初始版本部署

```bash
# 假设当前版本是 v1.0.0

# 1. 创建版本目录
sudo mkdir -p /opt/vendor/versions/v1.0.0

# 2. 复制可执行文件和配置
sudo cp antenna_mgmt /opt/vendor/versions/v1.0.0/
sudo cp config/antenna_mgmt.conf /opt/vendor/config/

# 3. 创建版本元数据
cat > /opt/vendor/versions/v1.0.0/version.json <<EOF
{
  "version": "v1.0.0",
  "build_time": "$(date '+%Y-%m-%d %H:%M:%S')",
  "checksum": "$(sha256sum /opt/vendor/versions/v1.0.0/antenna_mgmt | awk '{print $1}')",
  "file_size": $(stat -c%s /opt/vendor/versions/v1.0.0/antenna_mgmt),
  "ver_type": 0,
  "install_path": "/opt/vendor/versions/v1.0.0"
}
EOF

# 4. 创建checksum.txt
echo "antenna_mgmt: $(sha256sum /opt/vendor/versions/v1.0.0/antenna_mgmt | awk '{print $1}')" \
    > /opt/vendor/versions/v1.0.0/checksum.txt

# 5. 创建符号链接
sudo ln -sf /opt/vendor/versions/v1.0.0 /opt/vendor/current

# 6. 启动服务
sudo systemctl start antenna-mgmt.service
```

## 2. 版本升级流程

### 2.1 准备新版本tar包

```bash
# 在开发机器上打包新版本
cd /path/to/build/v1.0.1

# 创建checksum.txt
sha256sum antenna_mgmt > checksum.txt

# 创建tar.gz包
tar -czf antenna_v1.0.1.tar.gz antenna_mgmt checksum.txt

# 计算tar包的SHA-256
sha256sum antenna_v1.0.1.tar.gz
# 输出: a1b2c3d4e5f6... antenna_v1.0.1.tar.gz

# 上传到FTP服务器
ftp ftp.example.com
> put antenna_v1.0.1.tar.gz /versions/
```

### 2.2 BBU触发下载

BBU发送版本下载请求 (MsgID: 21)，包含:
- file_path: `/versions/`
- file_name: `antenna_v1.0.1.tar.gz`
- file_ver: `v1.0.1`
- file_len: 文件大小
- expected_checksum: `a1b2c3d4e5f6...` (tar包的SHA-256)

### 2.3 PAAU自动处理

PAAU收到请求后自动执行:
1. 发送应答 (MsgID: 22)
2. FTP下载文件
3. 验证tar包校验和
4. 解压到 `/opt/vendor/versions/v1.0.1/`
5. 验证可执行文件校验和
6. 保存元数据
7. 发送结果指示 (MsgID: 23)

### 2.4 BBU触发激活

BBU发送版本激活指示 (MsgID: 31):
- version_num: `v1.0.1`

PAAU执行:
1. 备份当前版本信息到 `.rollback/`
2. 切换符号链接: `/opt/vendor/current` -> `v1.0.1`
3. 发送激活应答 (MsgID: 32)
4. 自动重启服务

### 2.5 验证新版本

```bash
# 检查服务状态
sudo systemctl status antenna-mgmt.service

# 查看当前版本
readlink -f /opt/vendor/current
# 输出: /opt/vendor/versions/v1.0.1

# 查看日志
sudo journalctl -u antenna-mgmt.service -f
```

## 3. 版本回滚

### 3.1 查看可用版本

```bash
# 列出所有已安装版本
ls -l /opt/vendor/versions/
# 输出:
# drwxr-xr-x 2 root root 4096 Mar 13 14:00 v1.0.0
# drwxr-xr-x 2 root root 4096 Mar 13 15:00 v1.0.1

# 查看回滚记录
ls -l /opt/vendor/versions/.rollback/
# 输出:
# drwxr-xr-x 2 root root 4096 Mar 13 15:00 20260313_150000
```

### 3.2 手动回滚

**方法1: 通过API（如果实现）**
```bash
curl -X POST http://localhost:8080/api/v1/rollback \
  -H "Content-Type: application/json" \
  -d '{"version": "v1.0.0", "reason": "performance issue"}'
```

**方法2: 手动切换符号链接**
```bash
# 停止服务
sudo systemctl stop antenna-mgmt.service

# 切换到旧版本
sudo rm /opt/vendor/current
sudo ln -sf /opt/vendor/versions/v1.0.0 /opt/vendor/current

# 启动服务
sudo systemctl start antenna-mgmt.service

# 验证
readlink -f /opt/vendor/current
```

**方法3: 使用回滚脚本（如果实现）**
```bash
# 回滚到上一个版本
/opt/vendor/scripts/rollback-to-previous.sh

# 回滚到指定版本
/opt/vendor/scripts/rollback-to-version.sh v1.0.0 "performance issue"
```

## 4. 故障排查

### 4.1 服务无法启动

```bash
# 查看服务状态
sudo systemctl status antenna-mgmt.service

# 查看详细日志
sudo journalctl -u antenna-mgmt.service -n 100 --no-pager

# 检查符号链接
ls -l /opt/vendor/current

# 检查可执行文件
ls -l /opt/vendor/current/antenna_mgmt

# 手动测试启动
cd /opt/vendor/current
./antenna_mgmt /opt/vendor/config/antenna_mgmt.conf
```

### 4.2 版本下载失败

```bash
# 查看下载日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log | grep "download"

# 检查FTP连接
curl -v ftp://10.10.10.6:21/

# 检查磁盘空间
df -h /opt/vendor

# 手动测试下载
curl -C - -o /tmp/test.tar.gz --user anonymous: \
  ftp://10.10.10.6:21/versions/antenna_v1.0.1.tar.gz
```

### 4.3 校验和失败

```bash
# 查看校验和日志
grep "checksum" /var/log/antenna_mgmt/antenna_mgmt.log

# 手动验证tar包
sha256sum /opt/vendor/downloads/antenna_v1.0.1.tar.gz

# 手动验证可执行文件
sha256sum /opt/vendor/versions/v1.0.1/antenna_mgmt

# 查看checksum.txt
cat /opt/vendor/versions/v1.0.1/checksum.txt
```

### 4.4 激活后无法重启

```bash
# 查看重启日志
cat /var/log/antenna_mgmt/restart.log

# 手动重启服务
sudo systemctl restart antenna-mgmt.service

# 检查systemd服务配置
sudo systemctl cat antenna-mgmt.service

# 重新加载systemd配置
sudo systemctl daemon-reload
```

## 5. 监控和告警

### 5.1 关键指标

```bash
# 服务运行时间
systemctl show antenna-mgmt.service -p ActiveEnterTimestamp

# 重启次数
systemctl show antenna-mgmt.service -p NRestarts

# 内存使用
ps aux | grep antenna_mgmt

# 当前版本
readlink -f /opt/vendor/current
```

### 5.2 日志监控

```bash
# 实时监控日志
sudo journalctl -u antenna-mgmt.service -f

# 查找错误
sudo journalctl -u antenna-mgmt.service -p err -n 50

# 查找版本相关日志
sudo journalctl -u antenna-mgmt.service | grep -i "version"
```

### 5.3 告警规则（示例）

```bash
# 检查服务状态
if ! systemctl is-active --quiet antenna-mgmt.service; then
    echo "ALERT: antenna-mgmt service is not running"
fi

# 检查版本切换失败
if grep -q "DOWNLOAD_RESULT_CHECKSUM" /var/log/antenna_mgmt/antenna_mgmt.log; then
    echo "ALERT: Version download checksum verification failed"
fi

# 检查磁盘空间
USAGE=$(df -h /opt/vendor | awk 'NR==2 {print $5}' | sed 's/%//')
if [ $USAGE -gt 80 ]; then
    echo "ALERT: Disk usage is above 80%"
fi
```

## 6. 维护操作

### 6.1 清理旧版本

```bash
# 列出所有版本及大小
du -sh /opt/vendor/versions/*

# 删除旧版本（保留最近3个）
cd /opt/vendor/versions
ls -t | tail -n +4 | xargs rm -rf

# 清理下载目录
rm -f /opt/vendor/downloads/*.tar.gz
```

### 6.2 备份配置

```bash
# 备份配置文件
tar -czf antenna_mgmt_config_$(date +%Y%m%d).tar.gz \
  /opt/vendor/config/ \
  /etc/systemd/system/antenna-mgmt.service

# 备份版本元数据
tar -czf antenna_mgmt_versions_$(date +%Y%m%d).tar.gz \
  /opt/vendor/versions/*/version.json \
  /opt/vendor/versions/*/checksum.txt
```

### 6.3 日志轮转

```bash
# 创建logrotate配置
cat > /etc/logrotate.d/antenna-mgmt <<EOF
/var/log/antenna_mgmt/*.log {
    daily
    rotate 90
    compress
    delaycompress
    missingok
    notifempty
    create 0644 root root
    postrotate
        systemctl reload antenna-mgmt.service > /dev/null 2>&1 || true
    endscript
}
EOF
```

## 7. 安全建议

### 7.1 文件权限

```bash
# 确保版本目录只有root可写
sudo chown -R root:root /opt/vendor/versions
sudo chmod -R 755 /opt/vendor/versions

# 可执行文件权限
sudo chmod 755 /opt/vendor/versions/*/antenna_mgmt

# 配置文件权限（可能包含敏感信息）
sudo chmod 600 /opt/vendor/config/antenna_mgmt.conf
```

### 7.2 校验和验证

```bash
# 定期验证当前版本的完整性
CURRENT_VERSION=$(readlink -f /opt/vendor/current)
EXPECTED=$(cat $CURRENT_VERSION/checksum.txt | awk '{print $2}')
ACTUAL=$(sha256sum $CURRENT_VERSION/antenna_mgmt | awk '{print $1}')

if [ "$EXPECTED" != "$ACTUAL" ]; then
    echo "WARNING: Current version checksum mismatch!"
fi
```

### 7.3 访问控制

```bash
# 限制FTP访问（在FTP服务器端配置）
# 只允许特定IP访问
# 使用TLS加密传输

# 限制systemd服务权限
# 在antenna-mgmt.service中添加:
# PrivateTmp=true
# ProtectSystem=strict
# ProtectHome=true
```

## 8. 性能优化

### 8.1 下载优化

```bash
# 使用本地缓存
# 在配置文件中设置:
DOWNLOAD_CACHE_DIR=/opt/vendor/cache

# 并行下载（如果支持多个文件）
# 使用aria2c替代curl
```

### 8.2 启动优化

```bash
# 预加载共享库
# 在systemd服务中添加:
Environment="LD_PRELOAD=/opt/vendor/current/lib/libcommon.so"

# 使用内存文件系统加速临时文件
# mount -t tmpfs -o size=100M tmpfs /opt/vendor/tmp
```

## 9. 总结

本指南涵盖了版本管理系统的完整部署、使用和维护流程。关键要点:

1. **使用systemd管理服务** - 自动重启、日志管理
2. **符号链接实现版本切换** - 原子操作、快速切换
3. **三层校验和验证** - tar包、checksum.txt、可执行文件
4. **完整的回滚机制** - 自动备份、快速恢复
5. **详细的日志和监控** - 便于故障排查

遵循本指南可以确保版本管理系统的稳定运行和安全升级。
