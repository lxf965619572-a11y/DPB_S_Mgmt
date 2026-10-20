#!/bin/bash
#
# 离线验证 setup-network.sh 中 eth1 等待/重试逻辑。
#
# 用 mock 的 ip 命令模拟以下几种启动时序：
#   1. eth1 设备晚出现
#   2. ip link set eth1 up 先失败几次，之后成功（PHY 晚就绪）
#   3. eth1 始终不出现
#   4. eth1 存在但 open 始终失败
#   5. open 成功但 carrier 一直为 0
#
# 不修改本机任何网络配置，可在 Linux 或 Windows Git Bash 上运行：
#   bash scripts/test/test-eth1-wait.sh

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SETUP="$HERE/../setup-network.sh"

if [ ! -f "$SETUP" ]; then
    echo "找不到 $SETUP"
    exit 1
fi

TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT

MOCK_BIN="$TEST_ROOT/bin"
STATE="$TEST_ROOT/state"
SYSFS="$TEST_ROOT/sys/class/net"
LOG="$TEST_ROOT/out.log"

mkdir -p "$MOCK_BIN" "$STATE" "$SYSFS/eth1"

# ---------------------------------------------------------------- mock ip
cat > "$MOCK_BIN/ip" <<'MOCK'
#!/bin/bash
state="$TEST_ROOT/state"
get() { cat "$state/$1" 2>/dev/null || echo 0; }
put() { printf '%s\n' "$2" > "$state/$1"; }

case "$1 $2" in
    "link show")
        [ "$(get dev_exists)" = "1" ] && exit 0 || exit 1
        ;;
    "link set")
        if [ "$4" = "up" ]; then
            put attempts $(( $(get attempts) + 1 ))
            left=$(get up_fail_left)
            if [ "$left" -gt 0 ]; then
                put up_fail_left $((left - 1))
                exit 1
            fi
            exit 0
        fi
        ;;
    "addr replace")
        printf '%s\n' "$*" >> "$state/addr.log"
        exit 0
        ;;
    "addr show")
        echo "    inet 10.10.10.8/24 scope global $3"
        exit 0
        ;;
    "route replace")
        exit 0
        ;;
esac
exit 0
MOCK
chmod +x "$MOCK_BIN/ip"

# ---------------------------------------------------------------- helpers
reset_state() {
    echo 0 > "$STATE/dev_exists"
    echo 0 > "$STATE/up_fail_left"
    echo 0 > "$STATE/attempts"
    : > "$STATE/addr.log"
    echo 0 > "$SYSFS/eth1/carrier"
}

# run_ensure <设备/open 超时> <carrier 超时>
run_ensure() {
    PATH="$MOCK_BIN:$PATH" \
    TEST_ROOT="$TEST_ROOT" \
    SYSFS_NET="$SYSFS" \
    ETH1_WAIT_TIMEOUT="$1" \
    ETH1_CARRIER_TIMEOUT="$2" \
    bash -c 'source "$1"; NET_MANAGER=manual; ensure_eth1_ready' _ "$SETUP" \
        > "$LOG" 2>&1
}

PASS=0
FAIL=0

ok() {
    PASS=$((PASS + 1))
    echo "  PASS: $1"
}

bad() {
    FAIL=$((FAIL + 1))
    echo "  FAIL: $1"
    if [ -f "$LOG" ]; then
        sed 's/^/    | /' "$LOG" | tail -15
    fi
}

check_rc() {
    local expect="$1" actual="$2" name="$3"
    if [ "$actual" -eq "$expect" ]; then
        ok "$name（返回 $actual）"
    else
        bad "$name（期望 $expect，实际 $actual）"
    fi
}

# ---------------------------------------------------------------- cases
echo "[1] 设备晚出现，carrier 晚到"
reset_state
( sleep 0.5; echo 1 > "$STATE/dev_exists" ) &
( sleep 0.8; echo 1 > "$SYSFS/eth1/carrier" ) &
run_ensure 3 2
rc=$?
wait
check_rc 0 "$rc" "最终应成功"
grep -q 'addr replace 10.10.10.8/24 dev eth1' "$STATE/addr.log" \
    && ok "已执行 ip addr replace 10.10.10.8/24 dev eth1" \
    || bad "没有执行 ip addr replace"
[ "$(cat "$STATE/attempts")" -ge 1 ] && ok "尝试过 up" || bad "没有尝试 up"

echo "[2] 前 5 次 up 失败，之后成功"
reset_state
echo 1 > "$STATE/dev_exists"
echo 5 > "$STATE/up_fail_left"
echo 1 > "$SYSFS/eth1/carrier"
run_ensure 3 2
rc=$?
check_rc 0 "$rc" "重试后应成功"
if [ "$(cat "$STATE/attempts")" -eq 6 ]; then
    ok "up 一共尝试了 6 次"
else
    bad "up 尝试次数期望 6，实际 $(cat "$STATE/attempts")"
fi

echo "[3] 设备始终不出现"
reset_state
run_ensure 1 1
rc=$?
check_rc 1 "$rc" "应超时失败"

echo "[4] 设备存在但 up 始终失败"
reset_state
echo 1 > "$STATE/dev_exists"
echo 999 > "$STATE/up_fail_left"
run_ensure 1 1
rc=$?
check_rc 1 "$rc" "应超时失败"
[ "$(cat "$STATE/attempts")" -ge 2 ] && ok "过程中有多次重试" || bad "重试次数不足"

echo "[5] up 成功但 carrier 一直为 0"
reset_state
echo 1 > "$STATE/dev_exists"
run_ensure 2 1
rc=$?
check_rc 0 "$rc" "地址配置完成即返回成功"
grep -q 'addr replace 10.10.10.8/24 dev eth1' "$STATE/addr.log" \
    && ok "carrier 未就绪时仍配置了地址" \
    || bad "没有配置地址"

echo "[6] 配置文件内容未变化时不重写"
CONF="$TEST_ROOT/test.network"
printf 'same\n' | ( source "$SETUP" >/dev/null 2>&1; install_file_if_changed "$CONF" )
rc1=$?
printf 'same\n' | ( source "$SETUP" >/dev/null 2>&1; install_file_if_changed "$CONF" )
rc2=$?
printf 'different\n' | ( source "$SETUP" >/dev/null 2>&1; install_file_if_changed "$CONF" )
rc3=$?
[ "$rc1" -eq 0 ] && ok "首次写入返回 0" || bad "首次写入返回 $rc1"
[ "$rc2" -eq 1 ] && ok "内容相同时返回 1（不重写）" || bad "内容相同时返回 $rc2"
[ "$rc3" -eq 0 ] && ok "内容变化时返回 0（重写）" || bad "内容变化时返回 $rc3"

echo
echo "结果：通过 $PASS，失败 $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
