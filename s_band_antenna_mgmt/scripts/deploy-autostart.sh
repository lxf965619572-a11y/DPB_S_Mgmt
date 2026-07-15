#!/bin/bash
#
# 一键部署自启动服务脚本
# 用途：简化systemd服务部署流程，一条命令完成所有配置
#
# 使用方法：
#   sudo ./scripts/deploy-autostart.sh
#
# 功能：
#   1. 创建必要的目录结构
#   2. 复制服务文件和脚本
#   3. 部署当前版本
#   4. 配置并启用systemd服务
#   5. 提供日志查看方法

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印函数
print_step() {
    echo -e "${BLUE}[步骤]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[成功]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[警告]${NC} $1"
}

print_error() {
    echo -e "${RED}[错误]${NC} $1"
}

# 检查是否以root权限运行
if [ "$EUID" -ne 0 ]; then
    print_error "请使用 sudo 运行此脚本"
    exit 1
fi

# 获取脚本所在目录的上级目录（项目根目录）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "========================================="
echo "  S波段天线管理软件 - 自启动部署工具"
echo "========================================="
echo ""
echo "项目目录: $PROJECT_ROOT"
echo ""

# 检查必要文件是否存在
print_step "检查必要文件..."
REQUIRED_FILES=(
    "$PROJECT_ROOT/antenna_mgmt"
    "$PROJECT_ROOT/config/antenna_mgmt.conf"
    "$SCRIPT_DIR/antenna-mgmt.service"
    "$SCRIPT_DIR/antenna-mgmt-launcher.sh"
    "$SCRIPT_DIR/restart-after-activate.sh"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        print_error "缺少必要文件: $file"
        print_warning "请先编译项目: make"
        exit 1
    fi
done
print_success "所有必要文件检查通过"

# 步骤1: 配置网络接口
print_step "配置网络接口..."
echo "  eth0: 192.168.0.11/24 (调试接口)"
echo "  eth1: 10.10.10.8/24   (基带通信接口)"
echo ""

# 检查eth0接口
if ip link show eth0 &>/dev/null; then
    print_step "配置 eth0..."
    ip addr flush dev eth0 2>/dev/null || true
    ip addr add 192.168.0.11/24 dev eth0 2>/dev/null || true
    ip link set eth0 up
    if ip addr show eth0 | grep -q "192.168.0.11"; then
        print_success "eth0 配置完成: 192.168.0.11/24"
    else
        print_warning "eth0 配置失败，但不影响继续部署"
    fi
else
    print_warning "eth0 接口不存在，跳过配置"
fi

# 检查eth1接口 (必需，用于基带通信)
if ip link show eth1 &>/dev/null; then
    print_step "配置 eth1..."
    ip addr flush dev eth1 2>/dev/null || true
    ip addr add 10.10.10.8/24 dev eth1 2>/dev/null || true
    ip link set eth1 up
    if ip addr show eth1 | grep -q "10.10.10.8"; then
        print_success "eth1 配置完成: 10.10.10.8/24"
    else
        print_error "eth1 配置失败！"
        print_warning "基带通信需要eth1接口，请手动配置后重试"
    fi
else
    print_warning "eth1 接口不存在！"
    print_warning "基带通信需要eth1接口，请检查硬件连接"
fi

echo ""
print_success "网络接口配置完成"

# 步骤2: 配置永久网络（使用systemd-networkd）
print_step "配置永久网络（开机自动生效）..."

# 创建systemd-networkd配置目录
mkdir -p /etc/systemd/network

# 配置eth0 (调试接口)
cat > /etc/systemd/network/10-eth0.network <<'EOF'
[Match]
Name=eth0

[Network]
Address=192.168.0.11/24
EOF

# 配置eth1 (基带通信接口)
cat > /etc/systemd/network/20-eth1.network <<'EOF'
[Match]
Name=eth1

[Network]
Address=10.10.10.8/24
EOF

# 启用systemd-networkd（如果未启用）
if ! systemctl is-enabled systemd-networkd &>/dev/null; then
    systemctl enable systemd-networkd
    print_success "systemd-networkd 已启用"
fi

print_success "永久网络配置完成（重启后自动生效）"

# 步骤3: 创建目录结构
print_step "创建目录结构..."
mkdir -p /opt/vendor/versions
mkdir -p /opt/vendor/downloads
mkdir -p /opt/vendor/config
mkdir -p /opt/vendor/scripts
mkdir -p /var/log/antenna_mgmt

# 设置权限
chown -R root:root /opt/vendor
chmod -R 755 /opt/vendor
print_success "目录结构创建完成"

# 步骤3: 确定版本号
print_step "确定版本号..."
if [ -f "$PROJECT_ROOT/VERSION" ]; then
    VERSION=$(cat "$PROJECT_ROOT/VERSION")
else
    # 从程序中提取版本号，或使用默认值
    VERSION="v1.0.0"
    print_warning "未找到VERSION文件，使用默认版本: $VERSION"
fi
echo "当前版本: $VERSION"

# 步骤4: 部署当前版本
print_step "部署版本 $VERSION..."
VERSION_DIR="/opt/vendor/versions/$VERSION"
mkdir -p "$VERSION_DIR"

# 复制可执行文件
cp "$PROJECT_ROOT/antenna_mgmt" "$VERSION_DIR/"
chmod 755 "$VERSION_DIR/antenna_mgmt"
print_success "可执行文件已复制"

# 复制配置文件
cp "$PROJECT_ROOT/config/antenna_mgmt.conf" /opt/vendor/config/
chmod 644 /opt/vendor/config/antenna_mgmt.conf
print_success "配置文件已复制"

# 生成版本元数据
BUILD_TIME=$(date '+%Y-%m-%d %H:%M:%S')
CHECKSUM=$(sha256sum "$VERSION_DIR/antenna_mgmt" | awk '{print $1}')
FILE_SIZE=$(stat -c%s "$VERSION_DIR/antenna_mgmt")

cat > "$VERSION_DIR/version.json" <<EOF
{
  "version": "$VERSION",
  "build_time": "$BUILD_TIME",
  "checksum": "$CHECKSUM",
  "file_size": $FILE_SIZE,
  "ver_type": 0,
  "install_path": "$VERSION_DIR"
}
EOF
print_success "版本元数据已生成"

# 生成checksum.txt
echo "antenna_mgmt: $CHECKSUM" > "$VERSION_DIR/checksum.txt"
print_success "校验和文件已生成"

# 创建符号链接
if [ -L /opt/vendor/current ]; then
    CURRENT_VERSION=$(readlink /opt/vendor/current)
    print_warning "检测到已存在版本: $CURRENT_VERSION"
    read -p "是否覆盖? (y/n): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_error "部署已取消"
        exit 1
    fi
    rm /opt/vendor/current
fi

ln -sf "$VERSION_DIR" /opt/vendor/current
print_success "符号链接已创建"

# 步骤5: 复制服务文件和脚本
print_step "安装systemd服务..."

# 复制服务文件
cp "$SCRIPT_DIR/antenna-mgmt.service" /etc/systemd/system/
print_success "服务文件已安装"

# 复制启动脚本
cp "$SCRIPT_DIR/antenna-mgmt-launcher.sh" /opt/vendor/scripts/
cp "$SCRIPT_DIR/restart-after-activate.sh" /opt/vendor/scripts/
chmod +x /opt/vendor/scripts/*.sh
print_success "启动脚本已安装"

# 步骤6: 重新加载systemd并启用服务
print_step "配置systemd服务..."
systemctl daemon-reload
print_success "systemd配置已重新加载"

# 检查服务是否已在运行
if systemctl is-active --quiet antenna-mgmt.service; then
    print_warning "服务正在运行，将重启服务..."
    systemctl restart antenna-mgmt.service
else
    systemctl start antenna-mgmt.service
fi

# 启用开机自启
systemctl enable antenna-mgmt.service
print_success "服务已启用开机自启"

# 步骤7: 验证服务状态
print_step "验证服务状态..."
sleep 2  # 等待服务启动

if systemctl is-active --quiet antenna-mgmt.service; then
    print_success "服务运行正常"
else
    print_error "服务启动失败"
    echo ""
    echo "查看错误日志："
    journalctl -u antenna-mgmt.service -n 20 --no-pager
    exit 1
fi

# 完成
echo ""
echo "========================================="
echo -e "${GREEN}✓ 部署完成！${NC}"
echo "========================================="
echo ""
echo "部署信息："
echo "  版本: $VERSION"
echo "  安装路径: $VERSION_DIR"
echo "  配置文件: /opt/vendor/config/antenna_mgmt.conf"
echo "  日志目录: /var/log/antenna_mgmt/"
echo ""
echo "网络配置："
echo "  eth0: 192.168.0.11/24 (调试接口)"
echo "  eth1: 10.10.10.8/24   (基带通信接口，BBU地址: 10.10.10.6)"
echo ""
echo "常用命令："
echo "  查看服务状态:  sudo systemctl status antenna-mgmt.service"
echo "  停止服务:      sudo systemctl stop antenna-mgmt.service"
echo "  启动服务:      sudo systemctl start antenna-mgmt.service"
echo "  重启服务:      sudo systemctl restart antenna-mgmt.service"
echo "  禁用自启:      sudo systemctl disable antenna-mgmt.service"
echo ""
echo "查看日志的方法："
echo -e "${YELLOW}【方法1】查看systemd日志（推荐）${NC}"
echo "  实时查看:      sudo journalctl -u antenna-mgmt.service -f"
echo "  查看最近100行: sudo journalctl -u antenna-mgmt.service -n 100"
echo "  查看错误:      sudo journalctl -u antenna-mgmt.service -p err"
echo ""
echo -e "${YELLOW}【方法2】查看应用日志文件${NC}"
echo "  实时查看:      sudo tail -f /var/log/antenna_mgmt/antenna_mgmt.log"
echo "  查看全部:      sudo cat /var/log/antenna_mgmt/antenna_mgmt.log"
echo ""
echo -e "${YELLOW}【方法3】临时前台运行（用于调试）${NC}"
echo "  1. 停止服务:   sudo systemctl stop antenna-mgmt.service"
echo "  2. 前台运行:   cd /opt/vendor/current && sudo ./antenna_mgmt /opt/vendor/config/antenna_mgmt.conf"
echo "  3. 完成后启动:  sudo systemctl start antenna-mgmt.service"
echo ""
echo "========================================="
