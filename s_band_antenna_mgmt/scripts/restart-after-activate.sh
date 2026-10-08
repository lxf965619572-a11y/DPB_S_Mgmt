#!/bin/bash
#
# 版本激活后自动重启脚本
# 在版本切换完成后调用，延迟重启服务
#

set -e

LOG_FILE="/var/log/antenna_mgmt/restart.log"

# 日志函数
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

log "Version activation restart script started"

# 等待激活应答发送完成
log "Waiting for activation response to be sent..."
sleep 3

# 检查systemd服务是否存在
if systemctl list-unit-files | grep -q "antenna-mgmt.service"; then
    log "Restarting antenna-mgmt service via systemd..."
    systemctl restart antenna-mgmt.service

    # 等待服务启动
    sleep 5

    # 检查服务状态
    if systemctl is-active --quiet antenna-mgmt.service; then
        log "Service restarted successfully"
        exit 0
    else
        log "ERROR: Service failed to start"
        systemctl status antenna-mgmt.service >> "$LOG_FILE" 2>&1
        exit 1
    fi
else
    log "WARNING: systemd service not found, attempting manual restart..."

    # 查找进程
    PID=$(pgrep -f "antenna_mgmt" | head -1)

    if [ -n "$PID" ]; then
        log "Stopping process $PID..."
        kill -TERM "$PID"

        # 等待进程退出
        timeout=30
        count=0
        while kill -0 "$PID" 2>/dev/null; do
            sleep 1
            count=$((count + 1))
            if [ $count -ge $timeout ]; then
                log "Process did not exit gracefully, sending SIGKILL..."
                kill -KILL "$PID"
                break
            fi
        done
    fi

    # 重新启动
    log "Starting new version..."
    /opt/vendor/current/antenna_mgmt /opt/vendor/config/antenna_mgmt.conf &

    log "Service restarted manually"
fi

log "Restart script completed"
