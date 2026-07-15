#!/bin/bash
#
# 日志查看辅助脚本
# 用途：方便查看天线管理软件的日志
#
# 使用方法：
#   ./scripts/view-logs.sh [选项]
#
# 选项：
#   -f, --follow      实时跟踪日志（类似tail -f）
#   -n, --lines NUM   显示最近NUM行日志（默认100）
#   -e, --error       只显示错误日志
#   -s, --systemd     查看systemd日志（默认）
#   -a, --app         查看应用日志文件
#   -h, --help        显示帮助信息

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 默认参数
FOLLOW=false
LINES=100
ERROR_ONLY=false
LOG_TYPE="systemd"

# 显示帮助
show_help() {
    cat << EOF
S波段天线管理软件 - 日志查看工具

使用方法:
    $0 [选项]

选项:
    -f, --follow          实时跟踪日志（类似tail -f）
    -n, --lines NUM       显示最近NUM行日志（默认100）
    -e, --error           只显示错误日志
    -s, --systemd         查看systemd日志（默认）
    -a, --app             查看应用日志文件
    -h, --help            显示此帮助信息

示例:
    # 实时查看systemd日志
    $0 -f

    # 查看最近200行日志
    $0 -n 200

    # 只查看错误日志
    $0 -e

    # 实时查看应用日志文件
    $0 -a -f

    # 查看应用日志文件的最近50行
    $0 -a -n 50

常用快捷命令:
    journalctl -u antenna-mgmt.service -f              # systemd实时日志
    tail -f /var/log/antenna_mgmt/antenna_mgmt.log    # 应用实时日志

EOF
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--follow)
            FOLLOW=true
            shift
            ;;
        -n|--lines)
            LINES="$2"
            shift 2
            ;;
        -e|--error)
            ERROR_ONLY=true
            shift
            ;;
        -s|--systemd)
            LOG_TYPE="systemd"
            shift
            ;;
        -a|--app)
            LOG_TYPE="app"
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo -e "${RED}错误: 未知选项 $1${NC}"
            show_help
            exit 1
            ;;
    esac
done

# 检查是否需要sudo权限
check_sudo() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${YELLOW}提示: 查看日志可能需要sudo权限${NC}"
        echo ""
    fi
}

# 查看systemd日志
view_systemd_log() {
    local cmd="journalctl -u antenna-mgmt.service"

    if [ "$ERROR_ONLY" = true ]; then
        cmd="$cmd -p err"
    fi

    if [ "$FOLLOW" = true ]; then
        cmd="$cmd -f"
        echo -e "${GREEN}实时查看systemd日志...${NC}"
        echo -e "${YELLOW}按 Ctrl+C 退出${NC}"
        echo ""
    else
        cmd="$cmd -n $LINES --no-pager"
        echo -e "${GREEN}显示最近 $LINES 行systemd日志...${NC}"
        echo ""
    fi

    eval $cmd
}

# 查看应用日志文件
view_app_log() {
    local log_file="/var/log/antenna_mgmt/antenna_mgmt.log"

    if [ ! -f "$log_file" ]; then
        echo -e "${RED}错误: 日志文件不存在: $log_file${NC}"
        exit 1
    fi

    if [ "$FOLLOW" = true ]; then
        echo -e "${GREEN}实时查看应用日志文件...${NC}"
        echo -e "${YELLOW}按 Ctrl+C 退出${NC}"
        echo ""

        if [ "$ERROR_ONLY" = true ]; then
            tail -f "$log_file" | grep --line-buffered "ERROR"
        else
            tail -f "$log_file"
        fi
    else
        echo -e "${GREEN}显示最近 $LINES 行应用日志...${NC}"
        echo ""

        if [ "$ERROR_ONLY" = true ]; then
            tail -n "$LINES" "$log_file" | grep "ERROR"
        else
            tail -n "$LINES" "$log_file"
        fi
    fi
}

# 主逻辑
echo "========================================="
echo "  S波段天线管理软件 - 日志查看工具"
echo "========================================="
echo ""

check_sudo

case $LOG_TYPE in
    systemd)
        view_systemd_log
        ;;
    app)
        view_app_log
        ;;
    *)
        echo -e "${RED}错误: 未知的日志类型${NC}"
        exit 1
        ;;
esac
