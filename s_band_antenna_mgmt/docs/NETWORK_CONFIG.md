# 网络配置说明

## 📡 网络接口配置

天线管理软件需要配置两个网络接口：

| 接口 | IP地址 | 用途 | 必需性 |
|------|--------|------|--------|
| **eth1** | 10.10.10.8/24 | 与BBU基带通信（TCP连接） | ✅ 必需 |
| **eth0** | 192.168.0.11/24 | 调试接口 | ⭐ 推荐 |

### BBU基带侧配置
- BBU IP地址: `10.10.10.6`
- PAAU天线侧: `10.10.10.8`
- 通信协议: TCP（CPRI over TCP）

---

## 🚀 自动配置（推荐）

使用一键部署脚本会**自动配置网络**：

```bash
sudo ./scripts/deploy-autostart.sh
```

脚本会完成：
1. ✅ 临时配置：立即生效（无需重启）
2. ✅ 永久配置：使用systemd-networkd，重启后自动生效
3. ✅ 验证配置：检查IP是否正确设置

---

## 🔧 手动配置方法

### 方法1: 使用网络配置脚本

```bash
# 自动检测并配置
sudo ./scripts/setup-network.sh

# 或指定配置方式
sudo ./scripts/setup-network.sh manual              # 临时配置
sudo ./scripts/setup-network.sh systemd-networkd    # 永久配置
```

### 方法2: 使用ip命令（临时配置，重启失效）

```bash
# 配置eth0 (调试接口)
sudo ip addr flush dev eth0
sudo ip addr add 192.168.0.11/24 dev eth0
sudo ip link set eth0 up

# 配置eth1 (基带通信接口) - 必需！
sudo ip addr flush dev eth1
sudo ip addr add 10.10.10.8/24 dev eth1
sudo ip link set eth1 up

# 验证配置
ip addr show eth0
ip addr show eth1
```

### 方法3: 使用systemd-networkd（永久配置）

```bash
# 创建配置目录
sudo mkdir -p /etc/systemd/network

# 配置eth0
sudo cat > /etc/systemd/network/10-eth0.network <<EOF
[Match]
Name=eth0

[Network]
Address=192.168.0.11/24
EOF

# 配置eth1
sudo cat > /etc/systemd/network/20-eth1.network <<EOF
[Match]
Name=eth1

[Network]
Address=10.10.10.8/24
EOF

# 启用并重启服务
sudo systemctl enable systemd-networkd
sudo systemctl restart systemd-networkd
```

---

## ✅ 验证网络配置

### 检查IP配置

```bash
# 查看所有网络接口
ip addr

# 查看特定接口
ip addr show eth0
ip addr show eth1
```

### 测试连通性

```bash
# 测试eth0（如果有调试主机）
ping -c 4 -I eth0 192.168.0.1

# 测试eth1与BBU的连接（重要！）
ping -c 4 -I eth1 10.10.10.6

# 检查路由表
ip route show
```

---

## 🔍 故障排查

### 问题1: eth1接口不存在

```bash
# 检查所有网络接口
ip link show

# 如果eth1不存在，检查硬件连接
# 或者网卡名称可能不是eth1
```

### 问题2: 无法连接BBU（10.10.10.6）

```bash
# 1. 检查eth1的IP配置
ip addr show eth1 | grep "inet "

# 2. 测试ping
ping -c 4 10.10.10.6

# 3. 查看服务日志
sudo journalctl -u antenna-mgmt.service | grep "10.10.10.6"
```

### 问题3: 重启后配置丢失

```bash
# 检查永久配置
ls -l /etc/systemd/network/

# 重新运行部署脚本
sudo ./scripts/deploy-autostart.sh
```

---

## 📝 配置检查清单

- [ ] eth1接口存在: `ip link show eth1`
- [ ] eth1 IP配置正确: `ip addr show eth1 | grep 10.10.10.8`
- [ ] eth1接口已启用: `ip link show eth1 | grep "state UP"`
- [ ] 可以ping通BBU: `ping -c 4 10.10.10.6`
- [ ] 永久配置已设置: `ls /etc/systemd/network/20-eth1.network`

---

## 📞 快速命令参考

```bash
# 一键部署（包含网络配置）
sudo ./scripts/deploy-autostart.sh

# 手动配置网络（临时）
sudo ip addr add 10.10.10.8/24 dev eth1
sudo ip link set eth1 up

# 测试BBU连接
ping -c 4 10.10.10.6

# 查看网络状态
ip addr show eth0 eth1

# 查看服务日志
sudo journalctl -u antenna-mgmt.service -f
```

---

## 总结

✅ **推荐方式**: 使用 `sudo ./scripts/deploy-autostart.sh` 一键完成所有配置  
✅ **eth1必需**: 用于与BBU基带通信（10.10.10.8 ↔ 10.10.10.6）  
✅ **eth0可选**: 用于调试（192.168.0.11）  
✅ **永久配置**: 使用systemd-networkd，重启后自动生效  
✅ **验证连接**: 部署后务必 `ping 10.10.10.6` 验证与BBU的连通性
