#!/bin/bash
#
# S-Band Antenna Management - Network Configuration Script
# 配置网卡IP地址
#   eth0: 192.168.0.11/24 (调试网口)
#   eth1: 10.10.10.8/24   (BBU通信网口)
#

set -e

# 网络参数（与下面各配置方式保持一致）
ETH1_CIDR="${ETH1_CIDR:-10.10.10.8/24}"
# 等待 FPGA PHY 就绪的最长时间（秒）。
# FPGA 加载时间有抖动，不要用固定 sleep，必须轮询到条件满足。
ETH1_WAIT_TIMEOUT="${ETH1_WAIT_TIMEOUT:-120}"
# 驱动 open 成功后再等链路 carrier 的时间（秒）。
# 对端没上电/网线没接时只等这么久，避免拖住开机。
ETH1_CARRIER_TIMEOUT="${ETH1_CARRIER_TIMEOUT:-20}"

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

# 只有内容变化时才写入目标文件；返回 0 表示已更新，1 表示内容未变。
# 用于避免服务重试时反复重启网络服务、反复备份配置文件。
install_file_if_changed() {
    local dest="$1" tmp
    # 临时文件放在目标目录，保证 mv 是原子的，避免 networkd 读到半截文件
    tmp=$(mktemp "${dest}.tmp.XXXXXX")
    cat > "$tmp"
    if [ -f "$dest" ] && cmp -s "$tmp" "$dest"; then
        rm -f "$tmp"
        return 1
    fi
    chmod 644 "$tmp"
    mv "$tmp" "$dest"
    return 0
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

    local changed=0

    # 配置eth0 (调试网口)
    if install_file_if_changed /etc/systemd/network/10-eth0.network <<EOF
[Match]
Name=eth0

[Network]
Address=192.168.0.11/24
EOF
    then
        changed=1
    fi

    # 配置eth1 (BBU通信网口)
    if install_file_if_changed /etc/systemd/network/20-eth1.network <<EOF
[Match]
Name=eth1

[Network]
Address=10.10.10.8/24
Gateway=10.10.10.1
DNS=8.8.8.8
EOF
    then
        changed=1
    fi

    # 启用systemd-networkd；配置没变化且服务已在运行时不要重启，避免链路闪断
    systemctl enable systemd-networkd >/dev/null 2>&1 || true
    if [ "$changed" -eq 1 ] || ! systemctl is-active --quiet systemd-networkd; then
        systemctl restart systemd-networkd || log_warn "systemd-networkd restart failed"
    fi

    log_info "systemd-networkd configuration completed"
}

# 使用ifupdown配置
configure_ifupdown() {
    log_info "Configuring network using /etc/network/interfaces..."

    local tmp changed=0
    tmp=$(mktemp)
    cat > "$tmp" <<EOF
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

    # 内容没变就不要重写、不要备份，避免服务重试时产生大量备份文件
    if [ -f /etc/network/interfaces ] && cmp -s "$tmp" /etc/network/interfaces; then
        rm -f "$tmp"
    else
        if [ -f /etc/network/interfaces ]; then
            cp /etc/network/interfaces "/etc/network/interfaces.backup.$(date +%Y%m%d_%H%M%S)"
        fi
        mv "$tmp" /etc/network/interfaces
        chmod 644 /etc/network/interfaces
        changed=1
    fi

    # 只有配置变化时才 down，避免每次启动/重试都把已经工作的接口打断
    if [ "$changed" -eq 1 ]; then
        ifdown eth0 2>/dev/null || true
        ifdown eth1 2>/dev/null || true
    fi

    # 这里不再因为 eth1 暂时不存在而让整个脚本退出，后续 ensure_eth1_ready 会等待并兜底
    if command -v ifup &> /dev/null; then
        ifup eth0 2>/dev/null || true
        ifup eth1 2>/dev/null || true
    else
        systemctl restart networking 2>/dev/null || true
    fi

    log_info "ifupdown configuration completed"
}

# 创建或更新一个 NetworkManager 连接（幂等，避免每次重试都删除重建）
nm_apply_connection() {
    local name="$1" ifname="$2" addr="$3" gw="$4" dns="$5"
    local args=(ipv4.method manual ipv4.addresses "$addr" connection.autoconnect yes)

    [ -n "$gw" ] && args+=(ipv4.gateway "$gw")
    [ -n "$dns" ] && args+=(ipv4.dns "$dns")

    if nmcli -t -f NAME connection show | grep -qx "$name"; then
        nmcli connection modify "$name" connection.interface-name "$ifname" "${args[@]}"
    else
        nmcli connection add type ethernet con-name "$name" ifname "$ifname" "${args[@]}"
    fi
}

# 使用NetworkManager配置
configure_networkmanager() {
    log_info "Configuring network using NetworkManager..."

    nm_apply_connection eth0 eth0 192.168.0.11/24 "" ""
    nm_apply_connection eth1 eth1 10.10.10.8/24 10.10.10.1 8.8.8.8

    # --wait 5：设备还没出现时不要让 nmcli 卡住整个启动流程；
    # 真正等 PHY 就绪由 ensure_eth1_ready 负责。
    nmcli --wait 5 connection up eth0 >/dev/null 2>&1 || true
    nmcli --wait 5 connection up eth1 >/dev/null 2>&1 || true

    log_info "NetworkManager configuration completed"
}

# 手动配置（临时，重启后失效）
configure_manual() {
    log_warn "Using manual configuration (temporary, will be lost after reboot)"

    # 用 replace 而不是 flush+add：幂等，重试时不会把已有地址先删掉
    ip addr replace 192.168.0.11/24 dev eth0 2>/dev/null || true
    ip link set eth0 up 2>/dev/null || true

    ip addr replace "$ETH1_CIDR" dev eth1 2>/dev/null || true
    ip link set eth1 up 2>/dev/null || true
    ip route replace default via 10.10.10.1 dev eth1 2>/dev/null || true

    log_info "Manual configuration completed"
}

# 链路就绪后让网络管理器补齐路由/DNS 等配置；失败不影响后面的兜底
reapply_eth1_config() {
    local manager="$1" iface="${2:-eth1}"
    case "$manager" in
        ifupdown)
            ifup "$iface" >/dev/null 2>&1 || true
            ;;
        systemd-networkd)
            networkctl reconfigure "$iface" >/dev/null 2>&1 || true
            ;;
        NetworkManager)
            nmcli --wait 5 connection up "$iface" >/dev/null 2>&1 || true
            ;;
    esac
}

# 判断链路是否已 up。优先看 carrier；驱动不提供 carrier 信息时退到 operstate，
# 两者都读不到就认为"无法判断"，按就绪处理，避免白等超时。
link_is_up() {
    local iface="$1" sysfs_net="${2:-/sys/class/net}"
    local carrier operstate

    carrier=$(cat "$sysfs_net/$iface/carrier" 2>/dev/null)
    if [ -n "$carrier" ]; then
        [ "$carrier" = "1" ]
        return
    fi

    operstate=$(cat "$sysfs_net/$iface/operstate" 2>/dev/null)
    case "$operstate" in
        up)   return 0 ;;
        down) return 1 ;;
        *)    return 0 ;;
    esac
}

# 等待 eth1 设备出现，并反复 up 直到驱动成功 open。
#
# 背景：FPGA 侧 PHY 由管理 FPGA 配置，可能比 Linux 启动慢。PHY 未就绪时，
#       GMAC 驱动的 ndo_open 会失败，接口停在 DOWN；地址却可能已经配置上了，
#       所以会出现"ifconfig 看不到、手动 up 一下就好"的现象。
#
# 返回 0：设备存在且至少成功 open 过一次（carrier 可能仍为 0）
# 返回 1：设备在超时内未出现，或 open 始终失败（交给 systemd 重试）
ensure_eth1_ready() {
    local iface="${ETH1_IFACE:-eth1}"
    local sysfs_net="${SYSFS_NET:-/sys/class/net}"
    local timeout="${ETH1_WAIT_TIMEOUT:-120}"
    local deadline=$(( $(date +%s) + timeout ))
    local opened=0

    # 1. 等设备节点出现（驱动 probe / udev 完成）
    while ! ip link show "$iface" >/dev/null 2>&1; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            log_error "$iface 在 ${timeout}s 内未出现"
            return 1
        fi
        sleep 0.2
    done

    # 2. 反复 open，直到驱动成功打开接口。
    #    PHY 未就绪时 up 会失败，每次重试都会重新探测 PHY；不能只 up 一次。
    log_info "$iface 已出现，等待 PHY 就绪..."
    while [ "$opened" -eq 0 ]; do
        if ip link set "$iface" up 2>/dev/null; then
            opened=1
            break
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            log_error "$iface 驱动 open 始终失败（FPGA PHY 未就绪）"
            return 1
        fi
        sleep 0.2
    done
    log_info "$iface 已 up，等待链路..."

    # 3. open 成功后再等 carrier。只等一小段时间：
    #    对端没上电/网线没接时不应该把开机流程拖满 120 秒。
    local carrier_timeout="${ETH1_CARRIER_TIMEOUT:-20}"
    local carrier_deadline=$(( $(date +%s) + carrier_timeout ))
    while ! link_is_up "$iface" "$sysfs_net"; do
        if [ "$(date +%s)" -ge "$carrier_deadline" ]; then
            log_warn "$iface 已 up，但 ${carrier_timeout}s 内 carrier 未就绪（对端或 FPGA 链路未建立）"
            break
        fi
        sleep 0.2
    done
    if link_is_up "$iface" "$sysfs_net"; then
        log_info "$iface link up"
    fi

    # 4. 让管理器补路由/DNS，再用 replace 保证地址存在（幂等）
    reapply_eth1_config "$NET_MANAGER" "$iface"
    ip addr replace "$ETH1_CIDR" dev "$iface" 2>/dev/null || true

    return 0
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
    # 保存为全局变量，ensure_eth1_ready 需要按管理器补配置。
    # 显式指定模式时以参数为准，auto 时才用检测结果。
    case "$1" in
        systemd-networkd)  NET_MANAGER="systemd-networkd" ;;
        ifupdown)          NET_MANAGER="ifupdown" ;;
        networkmanager|nm) NET_MANAGER="NetworkManager" ;;
        manual)            NET_MANAGER="manual" ;;
        *)                 NET_MANAGER="$manager" ;;
    esac
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

    # 等 eth1 真正可用。FPGA PHY 初始化可能晚于系统启动，
    # 不能配置一次就认为完成；失败时退出非 0，由 systemd 重试。
    if ! ensure_eth1_ready; then
        log_error "eth1 未就绪，稍后由 systemd 重试"
        exit 1
    fi

    verify_configuration

    log_info "Network configuration completed successfully"
    log_info "eth0 (调试): 192.168.0.11/24"
    log_info "eth1 (BBU):  10.10.10.8/24"
}

# 直接执行时运行 main；被测试脚本 source 时只加载函数
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
