# 自启动服务部署快速指南

## 🚀 一键部署（推荐）

使用新的一键部署脚本，只需一条命令即可完成所有配置：

```bash
sudo ./scripts/deploy-autostart.sh
```

**脚本会自动完成以下操作**：
1. ✅ 配置网络接口（eth0: 192.168.0.11, eth1: 10.10.10.8）
2. ✅ 创建所需的目录结构
3. ✅ 复制可执行文件和配置
4. ✅ 生成版本元数据和校验和
5. ✅ 安装systemd服务
6. ✅ 启用开机自启
7. ✅ 启动服务并验证状态
8. ✅ 显示日志查看方法

---

## 📋 为什么看不到终端打印？

### 原因说明

当程序以systemd服务方式运行时，它在**后台运行**，标准输出不会显示在终端，而是被systemd接管。

**这是正常的设计**，有以下优点：
- ✅ 服务在后台稳定运行，不受终端关闭影响
- ✅ 开机自动启动
- ✅ 自动重启（崩溃时）
- ✅ 统一的日志管理

---

## 📊 方法1: 使用日志查看脚本（最简单）

```bash
# 实时查看日志（推荐）
./scripts/view-logs.sh -f

# 查看最近200行日志
./scripts/view-logs.sh -n 200

# 只查看错误日志
./scripts/view-logs.sh -e

# 查看应用日志文件（实时）
./scripts/view-logs.sh -a -f
```

---

## 📊 方法2: 使用journalctl查看systemd日志

```bash
# 实时查看日志（类似tail -f）
sudo journalctl -u antenna-mgmt.service -f

# 查看最近100行日志
sudo journalctl -u antenna-mgmt.service -n 100

# 只查看错误级别的日志
sudo journalctl -u antenna-mgmt.service -p err

# 查看从今天开始的日志
sudo journalctl -u antenna-mgmt.service --since today
```

---

## 📊 方法3: 查看应用日志文件

```bash
# 实时查看应用日志
sudo tail -f /var/log/antenna_mgmt/antenna_mgmt.log

# 查看最近100行
sudo tail -n 100 /var/log/antenna_mgmt/antenna_mgmt.log

# 只看错误日志
sudo grep "ERROR" /var/log/antenna_mgmt/antenna_mgmt.log
```

---

## 🛠️ 方法4: 临时前台运行（调试用）

如果需要像开发时一样看到终端输出，可以临时停止systemd服务，手动前台运行：

```bash
# 1. 停止systemd服务
sudo systemctl stop antenna-mgmt.service

# 2. 手动前台运行（会在终端显示所有输出）
cd /opt/vendor/current
sudo ./antenna_mgmt /opt/vendor/config/antenna_mgmt.conf

# 3. 调试完成后，按Ctrl+C停止，然后重新启动服务
sudo systemctl start antenna-mgmt.service
```

---

## 🔧 常用服务管理命令

```bash
# 查看服务状态
sudo systemctl status antenna-mgmt.service

# 启动服务
sudo systemctl start antenna-mgmt.service

# 停止服务
sudo systemctl stop antenna-mgmt.service

# 重启服务
sudo systemctl restart antenna-mgmt.service

# 启用开机自启
sudo systemctl enable antenna-mgmt.service

# 禁用开机自启
sudo systemctl disable antenna-mgmt.service
```

---

## 📁 重要文件路径

```
/opt/vendor/current/antenna_mgmt          # 当前运行的程序
/opt/vendor/config/antenna_mgmt.conf      # 配置文件
/var/log/antenna_mgmt/antenna_mgmt.log    # 应用日志文件
/etc/systemd/system/antenna-mgmt.service  # systemd服务文件
/opt/vendor/versions/                     # 所有版本存储目录
```

---

## 📞 快速参考卡片

```bash
# 一键部署
sudo ./scripts/deploy-autostart.sh

# 实时查看日志（最常用）
sudo journalctl -u antenna-mgmt.service -f
# 或
./scripts/view-logs.sh -f

# 重启服务
sudo systemctl restart antenna-mgmt.service

# 查看服务状态
sudo systemctl status antenna-mgmt.service

# 临时前台调试
sudo systemctl stop antenna-mgmt.service
cd /opt/vendor/current && sudo ./antenna_mgmt /opt/vendor/config/antenna_mgmt.conf
# Ctrl+C 退出后
sudo systemctl start antenna-mgmt.service
```

---

## 总结

✅ **一键部署**：`sudo ./scripts/deploy-autostart.sh`  
✅ **看日志不用愁**：`./scripts/view-logs.sh -f` 或 `sudo journalctl -u antenna-mgmt.service -f`  
✅ **需要调试**：临时前台运行  
✅ **服务管理**：使用 `systemctl` 命令  

systemd服务运行在后台是**设计如此**，通过日志工具查看输出是**标准做法**！
