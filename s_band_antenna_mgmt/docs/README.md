# 文档索引

本目录包含S波段天线管理软件的所有文档。

## 📚 快速导航

### 🚀 快速开始
- **[自启动快速指南](AUTOSTART_QUICK_GUIDE.md)** - 一键部署和日志查看方法
- **[网络配置说明](NETWORK_CONFIG.md)** - 网络接口配置详细指南

### 🔧 更新说明
- **[完整更新总结 (2026-07-15)](COMPLETE_UPDATE_20260715.md)** ⭐ 最新更新
- **[网络配置集成说明](NETWORK_INTEGRATION.md)** - 网络自动配置
- **[更新总结 (2026-07-14)](UPDATE_20260714.md)** - 问题修复和优化

### 🐛 问题修复
- **[问题修复详细说明 (2026-07-14)](fixes/ISSUE_FIX_20260714.md)** - 详细的修复文档

---

## 🎯 按场景查找

### 场景1: 首次部署
1. 阅读 [自启动快速指南](AUTOSTART_QUICK_GUIDE.md)
2. 阅读 [网络配置说明](NETWORK_CONFIG.md)
3. 运行 `sudo ./scripts/deploy-autostart.sh`

### 场景2: 查看日志
1. 阅读 [自启动快速指南](AUTOSTART_QUICK_GUIDE.md)
2. 使用 `./scripts/view-logs.sh -f`

### 场景3: 网络配置问题
1. 阅读 [网络配置说明](NETWORK_CONFIG.md)
2. 使用 `sudo ./scripts/setup-network.sh`

### 场景4: 了解最新更新
1. 阅读 [完整更新总结](COMPLETE_UPDATE_20260715.md)

---

## 📞 快速命令

```bash
# 一键部署
sudo ./scripts/deploy-autostart.sh

# 查看日志
./scripts/view-logs.sh -f

# 服务管理
sudo systemctl status antenna-mgmt.service
sudo systemctl restart antenna-mgmt.service

# 网络配置
ip addr show eth0 eth1
ping -c 4 10.10.10.6
```

---

**最后更新**: 2026-07-15
