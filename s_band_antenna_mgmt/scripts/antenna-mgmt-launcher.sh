#!/bin/bash
#
# S-Band Antenna Management Service Launcher
# 通过符号链接启动程序，支持版本切换
#

set -e

# 配置
CURRENT_DIR="/opt/vendor/current"
CONFIG_FILE="/opt/vendor/config/antenna_mgmt.conf"
LOG_DIR="/var/log/antenna_mgmt"
PID_FILE="/var/run/antenna_mgmt.pid"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查符号链接
check_symlink() {
    if [ ! -L "$CURRENT_DIR" ]; then
        log_error "$CURRENT_DIR is not a symbolic link"
        log_error "Please run version activation first"
        exit 1
    fi

    local target=$(readlink -f "$CURRENT_DIR")
    log_info "Current version: $target"
}

# 检查可执行文件
check_executable() {
    local executable="$CURRENT_DIR/antenna_mgmt"

    if [ ! -f "$executable" ]; then
        log_error "Executable not found: $executable"
        exit 1
    fi

    if [ ! -x "$executable" ]; then
        log_error "Executable is not executable: $executable"
        log_info "Fixing permissions..."
        chmod +x "$executable"
    fi

    log_info "Executable: $executable"
}

# 检查配置文件
check_config() {
    if [ ! -f "$CONFIG_FILE" ]; then
        log_warn "Config file not found: $CONFIG_FILE"
        log_warn "Using default configuration"
    else
        log_info "Config file: $CONFIG_FILE"
    fi
}

# 创建日志目录
setup_log_dir() {
    if [ ! -d "$LOG_DIR" ]; then
        mkdir -p "$LOG_DIR"
        log_info "Created log directory: $LOG_DIR"
    fi
}

# 检查是否已运行
check_running() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            log_warn "Service is already running (PID: $pid)"
            return 0
        else
            log_warn "Stale PID file found, removing..."
            rm -f "$PID_FILE"
        fi
    fi
    return 1
}

# 启动服务
start_service() {
    log_info "Starting S-Band Antenna Management Service..."

    # 检查
    check_symlink
    check_executable
    check_config
    setup_log_dir

    # 检查是否已运行
    if check_running; then
        exit 0
    fi

    # 切换到工作目录
    cd "$CURRENT_DIR" || exit 1

    # 启动程序
    local executable="$CURRENT_DIR/antenna_mgmt"

    if [ -f "$CONFIG_FILE" ]; then
        exec "$executable" "$CONFIG_FILE"
    else
        exec "$executable"
    fi
}

# 停止服务
stop_service() {
    log_info "Stopping S-Band Antenna Management Service..."

    if [ ! -f "$PID_FILE" ]; then
        log_warn "PID file not found, service may not be running"
        return 0
    fi

    local pid=$(cat "$PID_FILE")

    if ! kill -0 "$pid" 2>/dev/null; then
        log_warn "Process not found (PID: $pid)"
        rm -f "$PID_FILE"
        return 0
    fi

    # 发送SIGTERM
    log_info "Sending SIGTERM to process $pid..."
    kill -TERM "$pid"

    # 等待进程退出
    local timeout=30
    local count=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        count=$((count + 1))
        if [ $count -ge $timeout ]; then
            log_warn "Process did not exit gracefully, sending SIGKILL..."
            kill -KILL "$pid"
            break
        fi
    done

    rm -f "$PID_FILE"
    log_info "Service stopped"
}

# 重启服务
restart_service() {
    stop_service
    sleep 2
    start_service
}

# 查看状态
status_service() {
    if [ ! -f "$PID_FILE" ]; then
        log_info "Service is not running"
        return 1
    fi

    local pid=$(cat "$PID_FILE")

    if kill -0 "$pid" 2>/dev/null; then
        log_info "Service is running (PID: $pid)"

        # 显示版本信息
        if [ -L "$CURRENT_DIR" ]; then
            local version=$(readlink -f "$CURRENT_DIR")
            log_info "Current version: $version"
        fi

        return 0
    else
        log_warn "PID file exists but process is not running"
        return 1
    fi
}

# 主函数
main() {
    case "${1:-start}" in
        start)
            start_service
            ;;
        stop)
            stop_service
            ;;
        restart)
            restart_service
            ;;
        status)
            status_service
            ;;
        *)
            echo "Usage: $0 {start|stop|restart|status}"
            exit 1
            ;;
    esac
}

main "$@"
