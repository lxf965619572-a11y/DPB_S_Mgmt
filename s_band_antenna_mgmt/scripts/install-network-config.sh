#!/bin/bash
#
# 安装网络配置脚本和服务
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查root权限
if [ "$EUID" -ne 0 ]; then
    log_error "Please run as root"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log_info "Installing network configuration..."

# 创建目标目录
mkdir -p /opt/vendor/scripts

# 复制网络配置脚本
cp "$SCRIPT_DIR/setup-network.sh" /opt/vendor/scripts/
chmod +x /opt/vendor/scripts/setup-network.sh
log_info "Installed: /opt/vendor/scripts/setup-network.sh"

# 安装systemd服务
cp "$SCRIPT_DIR/antenna-mgmt-network.service" /etc/systemd/system/
log_info "Installed: /etc/systemd/system/antenna-mgmt-network.service"

# 重新加载systemd
systemctl daemon-reload

# 启用服务（开机自启动）
systemctl enable antenna-mgmt-network.service
log_info "Enabled antenna-mgmt-network.service for auto-start"

# 询问是否立即配置网络
read -p "Do you want to configure network now? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    log_info "Configuring network..."
    /opt/vendor/scripts/setup-network.sh auto
else
    log_info "Network configuration skipped. Run manually with:"
    log_info "  sudo /opt/vendor/scripts/setup-network.sh auto"
fi

log_info "Installation completed!"
log_info ""
log_info "Network will be configured automatically on next boot."
log_info "To manually configure network: sudo /opt/vendor/scripts/setup-network.sh auto"
log_info "To check service status: systemctl status antenna-mgmt-network"
