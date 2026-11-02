#!/bin/bash
#
# 软件升级试运行保护（systemd ExecStartPre）
#
# antenna-mgmt.service 每次启动前执行本脚本：
#   - 没有试运行记录：什么都不做；
#   - 有试运行记录且 current 仍指向新版本：启动次数 +1；
#   - 启动次数已达上限（UPGRADE_TRIAL_MAX_STARTS，默认 3）仍未被程序确认：
#     把 /opt/vendor/current 切回上一个版本，写回退通知，删除试运行记录。
#
# 试运行记录由 antenna_mgmt 在 BBU 激活新版本时写入（version_manager.c 的
# switch_to_version），与 BBU 通道建立后由程序删除（确认健康），稳定运行
# UPGRADE_TRIAL_STABLE_SEC 秒后由程序把启动次数清零。
# 记录格式（key=value，每行一项）：
#   new=<新版本目录>  previous=<上一个版本目录>  attempts=<次数>  time=<时间>
#
# 本脚本任何情况下都以 0 退出，绝不阻止服务启动（service 中也以 "-" 前缀调用）。
# 输出进 journal：journalctl -u antenna-mgmt.service | grep upgrade-guard

VENDOR_DIR="/opt/vendor"
CURRENT_LINK="$VENDOR_DIR/current"
ROLLBACK_DIR="$VENDOR_DIR/versions/.rollback"
TRIAL_FILE="$ROLLBACK_DIR/trial"
NOTICE_FILE="$ROLLBACK_DIR/auto_rollback.last"
CONFIG_FILE="$VENDOR_DIR/config/antenna_mgmt.conf"
DEFAULT_MAX_STARTS=3

log() {
    echo "[upgrade-guard] $*"
}

[ -f "$TRIAL_FILE" ] || exit 0

trial_get() {
    sed -n "s/^$1=//p" "$TRIAL_FILE" | head -n 1 | tr -d '\r'
}

new_dir=$(trial_get new)
prev_dir=$(trial_get previous)
attempts=$(trial_get attempts)
trial_time=$(trial_get time)
case "$attempts" in
    ''|*[!0-9]*) attempts=0 ;;
esac

max_starts=$(sed -n 's/^[[:space:]]*UPGRADE_TRIAL_MAX_STARTS[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
             "$CONFIG_FILE" 2>/dev/null | tail -n 1)
if [ -z "$max_starts" ] || [ "$max_starts" -lt 1 ]; then
    max_starts=$DEFAULT_MAX_STARTS
fi

current=$(readlink "$CURRENT_LINK" 2>/dev/null)

# 记录与当前版本不符（切换前断电、人工改过软链接等）：记录作废
if [ -z "$new_dir" ] || [ "$current" != "$new_dir" ]; then
    log "stale trial record (trial new=$new_dir, current=$current), removing"
    rm -f "$TRIAL_FILE"
    exit 0
fi

# 未达上限：计一次启动
if [ "$attempts" -lt "$max_starts" ]; then
    attempts=$((attempts + 1))
    tmp="$TRIAL_FILE.tmp"
    if printf 'new=%s\nprevious=%s\nattempts=%s\ntime=%s\n' \
              "$new_dir" "$prev_dir" "$attempts" "$trial_time" > "$tmp" \
       && sync && mv -f "$tmp" "$TRIAL_FILE"; then
        log "trial run of $new_dir: start $attempts/$max_starts (fallback: $prev_dir)"
    else
        log "WARNING: failed to update trial record"
        rm -f "$tmp"
    fi
    exit 0
fi

# 已连续启动 max_starts 次仍未被确认：回退
log "new version $new_dir not confirmed after $attempts starts, rolling back"

if [ -z "$prev_dir" ] || [ ! -f "$prev_dir/antenna_mgmt" ]; then
    log "ERROR: cannot roll back, previous version missing: '$prev_dir' (keeping $new_dir)"
    rm -f "$TRIAL_FILE"
    exit 0
fi

# 原子切换软链接：先建临时链接，再 mv -T 覆盖
if ln -sfn "$prev_dir" "$CURRENT_LINK.tmp" && mv -Tf "$CURRENT_LINK.tmp" "$CURRENT_LINK"; then
    ts=$(date '+%Y%m%d_%H%M%S')
    # 回退通知：程序下次启动读到后上报“版本激活失败”告警，与 BBU 通道建立后删除
    printf 'time=%s\nfrom=%s\nto=%s\nattempts=%s\n' "$ts" "$new_dir" "$prev_dir" "$attempts" \
        > "$NOTICE_FILE.tmp" && mv -f "$NOTICE_FILE.tmp" "$NOTICE_FILE"
    # 审计记录，与激活时的 rollback.json 放在同一处
    mkdir -p "$ROLLBACK_DIR/$ts"
    printf '{\n  "timestamp": "%s",\n  "previous_version": "%s",\n  "new_version": "%s",\n  "reason": "auto rollback: not confirmed after %s starts"\n}\n' \
        "$ts" "$new_dir" "$prev_dir" "$attempts" > "$ROLLBACK_DIR/$ts/rollback.json"
    rm -f "$TRIAL_FILE"
    sync
    log "ROLLED BACK: $new_dir -> $prev_dir"
else
    rm -f "$CURRENT_LINK.tmp"
    rm -f "$TRIAL_FILE"
    log "ERROR: failed to switch $CURRENT_LINK back to $prev_dir"
fi

exit 0
