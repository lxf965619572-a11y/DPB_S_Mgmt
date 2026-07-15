# 网络配置集成 - 更新总结

## 📋 更新内容

### 🌐 网络配置集成到部署流程

**问题**: 
- 程序启动前需要手动配置网络接口
- eth1 (10.10.10.8) 用于与BBU基带通信，必须配置
- eth0 (192.168.0.11) 用于调试，推荐配置

**解决方案**:
- ✅ 将网络配置集成到 `deploy-autostart.sh` 一键部署脚本
- ✅ 自动配置临时网络（立即生效）
- ✅ 自动配置永久网络（使用systemd-networkd，重启后自动生效）
- ✅ 提供独立的网络配置脚本 `setup-network.sh`

---

## 📦 修改的文件

### 1. scripts/deploy-autostart.sh ⭐ 修改
**新增功能**:
- 步骤1: 配置网络接口（临时配置，立即生效）
  - eth0: 192.168.0.11/24
  - eth1: 10.10.10.8/24
- 步骤2: 配置永久网络（systemd-networkd）
  - 创建 `/etc/systemd/network/10-eth0.network`
  - 创建 `/etc/systemd/network/20-eth1.network`
  - 启用 systemd-networkd 服务

### 2. scripts/setup-network.sh ⭐ 修改
**修改内容**:
- 将 eth0 的IP地址从 `192.168.0.10` 改为 `192.168.0.11`
- 支持多种网络配置方式

### 3. docs/NETWORK_CONFIG.md ⭐ 新增
网络配置完整说明文档

### 4. docs/AUTOSTART_QUICK_GUIDE.md ⭐ 修改
在自动完成操作列表中，第1步增加"配置网络接口"

---

## 🚀 使用方法

### 一键部署（推荐）

```bash
sudo ./scripts/deploy-autostart.sh
```

**会自动完成**:
1. ✅ 配置网络接口（eth0: 192.168.0.11, eth1: 10.10.10.8）
2. ✅ 配置永久网络（重启后自动生效）
3. ✅ 创建目录结构
4. ✅ 部署程序和配置
5. ✅ 安装systemd服务
6. ✅ 启动服务并验证

---

## 📊 网络配置详情

| 接口 | IP地址 | 子网掩码 | 用途 | 对端 |
|------|--------|----------|------|------|
| **eth1** | 10.10.10.8 | /24 | BBU通信（TCP） | 10.10.10.6 (BBU) |
| **eth0** | 192.168.0.11 | /24 | 调试接口 | - |

---

## ✅ 验证步骤

### 1. 检查IP配置

```bash
ip addr show eth0 eth1
```

### 2. 测试连通性

```bash
# 测试与BBU的连接（重要！）
ping -c 4 10.10.10.6
```

### 3. 检查永久配置

```bash
ls -l /etc/systemd/network/
systemctl status systemd-networkd
```

---

## 📞 快速命令参考

```bash
# 一键部署（包含网络配置）
sudo ./scripts/deploy-autostart.sh

# 查看网络配置
ip addr show eth0 eth1

# 测试BBU连接
ping -c 4 10.10.10.6

# 查看服务状态
sudo systemctl status antenna-mgmt.service

# 查看日志
sudo journalctl -u antenna-mgmt.service -f
```

---

## 🎯 优势总结

### 之前（手动配置）
❌ 需要多步操作  
❌ 容易遗漏步骤  
❌ 重启后需要重新配置  

### 现在（自动配置）
✅ 一条命令完成所有配置  
✅ 自动配置临时和永久网络  
✅ 重启后自动生效  
✅ 完善的验证和错误提示  

---

**更新日期**: 2026-07-15  
**更新内容**: 网络配置集成到一键部署脚本
