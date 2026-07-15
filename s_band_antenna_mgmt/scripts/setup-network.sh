#!/bin/bash
#
# S-Band Antenna Management - Network Configuration Script
# 配置网卡IP地址
#   eth0: 192.168.0.10/24 (调试网口)
#   eth1: 10.10.10.8/24   (BBU通信网口)
#

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查是否为root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_error "Please run as root"
        exit 1
    fi
}

# 检测网络配置方式
detect_network_manager() {
    if systemctl is-active --quiet NetworkManager; then
        echo "NetworkManager"
    elif systemctl is-active --quiet systemd-networkd; then
        echo "systemd-networkd"
    elif [ -f /etc/network/interfaces ]; then
        echo "ifupdown"
    else
        echo "unknown"
    fi
}

# 使用systemd-networkd配置
configure_systemd_networkd() {
    log_info "Configuring network using systemd-networkd..."

    # 创建配置目录
    mkdir -p /etc/systemd/network

    # 配置eth0 (调试网口)
    cat > /etc/systemd/network/10-eth0.network <<EOF
[Match]
Name=eth0

[Network]
Address=192.168.0.11/24
EOF

    # 配置eth1 (BBU通信网口)
    cat > /etc/systemd/network/20-eth1.network <<EOF
[Match]
Name=eth1

[Network]
Address=10.10.10.8/24
Gateway=10.10.10.1
DNS=8.8.8.8
EOF

    # 启用并重启systemd-networkd
    systemctl enable systemd-networkd
    systemctl restart systemd-networkd

    log_info "systemd-networkd configuration completed"
}

# 使用ifupdown配置
configure_ifupdown() {
    log_info "Configuring network using /etc/network/interfaces..."

    # 备份原配置
    if [ -f /etc/network/interfaces ]; then
        cp /etc/network/interfaces /etc/network/interfaces.backup.$(date +%Y%m%d_%H%M%S)
    fi

    # 写入配置
    cat > /etc/network/interfaces <<EOF
# Loopback interface
auto lo
iface lo inet loopback

# eth0 - 调试网口
auto eth0
iface eth0 inet static
    address 192.168.0.11
    netmask 255.255.255.0

# eth1 - BBU通信网口
auto eth1
iface eth1 inet static
    address 10.10.10.8
    netmask 255.255.255.0
    gateway 10.10.10.1
    dns-nameservers 8.8.8.8
EOF

    # 重启网络服务
    if command -v ifdown &> /dev/null; then
        ifdown eth0 2>/dev/null || true
        ifdown eth1 2>/dev/null || true
        ifup eth0
        ifup eth1
    else
        systemctl restart networking
    fi

    log_info "ifupdown configuration completed"
}

# 使用NetworkManager配置
configure_networkmanager() {
    log_info "Configuring network using NetworkManager..."

    # 配置eth0
    nmcli connection delete eth0 2>/dev/null || true
    nmcli connection add type ethernet con-name eth0 ifname eth0 \
        ipv4.method manual \
        ipv4.addresses 192.168.0.11/24 \
        autoconnect yes

    # 配置eth1
    nmcli connection delete eth1 2>/dev/null || true
    nmcli connection add type ethernet con-name eth1 ifname eth1 \
        ipv4.method manual \
        ipv4.addresses 10.10.10.8/24 \
        ipv4.gateway 10.10.10.1 \
        ipv4.dns 8.8.8.8 \
        autoconnect yes

    # 激活连接
    nmcli connection up eth0
    nmcli connection up eth1

    log_info "NetworkManager configuration completed"
}

# 手动配置（临时，重启后失效）
configure_manual() {
    log_warn "Using manual configuration (temporary, will be lost after reboot)"

    # 配置eth0
    ip addr flush dev eth0
    ip addr add 192.168.0.11/24 dev eth0
    ip link set eth0 up

    # 配置eth1
    ip addr flush dev eth1
    ip addr add 10.10.10.8/24 dev eth1
    ip link set eth1 up
    ip route add default via 10.10.10.1 dev eth1

    log_info "Manual configuration completed"
}

# 验证配置
verify_configuration() {
    log_info "Verifying network configuration..."

    sleep 2

    # 检查eth0
    if ip addr show eth0 | grep -q "192.168.0.11"; then
        log_info "✓ eth0: 192.168.0.11/24 configured"
    else
        log_error "✗ eth0: Failed to configure"
    fi

    # 检查eth1
    if ip addr show eth1 | grep -q "10.10.10.8"; then
        log_info "✓ eth1: 10.10.10.8/24 configured"
    else
        log_error "✗ eth1: Failed to configure"
    fi

    # 显示当前配置
    log_info "Current network configuration:"
    ip addr show eth0 | grep "inet " || true
    ip addr show eth1 | grep "inet " || true
}

# 主函数
main() {
    log_info "S-Band Antenna Management - Network Configuration"
    log_info "================================================"

    check_root

    # 检测网络管理方式
    local manager=$(detect_network_manager)
    log_info "Detected network manager: $manager"

    case "$1" in
        systemd-networkd)
            configure_systemd_networkd
            ;;
        ifupdown)
            configure_ifupdown
            ;;
        networkmanager|nm)
            configure_networkmanager
            ;;
        manual)
            configure_manual
            ;;
        auto|"")
            # 自动选择
            case "$manager" in
                NetworkManager)
                    configure_networkmanager
                    ;;
                systemd-networkd)
                    configure_systemd_networkd
                    ;;
                ifupdown)
                    configure_ifupdown
                    ;;
                *)
                    log_warn "Unknown network manager, using manual configuration"
                    configure_manual
                    ;;
            esac
            ;;
        *)
            echo "Usage: $0 [auto|systemd-networkd|ifupdown|networkmanager|manual]"
            echo ""
            echo "Options:"
            echo "  auto              - Auto-detect and configure (default)"
            echo "  systemd-networkd  - Use systemd-networkd"
            echo "  ifupdown          - Use /etc/network/interfaces"
            echo "  networkmanager    - Use NetworkManager"
            echo "  manual            - Manual configuration (temporary)"
            exit 1
            ;;
    esac

    verify_configuration

    log_info "Network configuration completed successfully"
    log_info "eth0 (调试): 192.168.0.11/24"
    log_info "eth1 (BBU):  10.10.10.8/24"
}

main "$@"
