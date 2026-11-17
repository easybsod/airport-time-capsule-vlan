#!/bin/sh
# Fixed A1470/7.9.1 configuration only. Run with Apple's native /bin/sh.
# Each SRAB transaction has detached rollback AND a separate hard deadline.
# A failed service disables itself on Flash before any one-shot reboot.
set -u
umask 077
BASE=/mnt/Flash/airport-three-vlan
RUN=/mnt/Memory/airport-three-vlan
SELF=$BASE/service.sh
TOOL=$BASE/switch-config
ARMED=$RUN/armed
LAB_ARMED=/mnt/Flash/airport-srab-lab/armed
export AIRPORT_THREE_VLAN_GUARDED=BOUNDED_SERVICE_TRANSACTION

log()
{
    printf '%s | %s\n' "$(date)" "$*" >> "$BASE/events.log"
}

same_token()
{
    [ -f "$ARMED" ] || return 1
    IFS= read -r current < "$ARMED" || return 1
    [ "$current" = "$1" ]
}

disable()
{
    printf '%s\n' "$*" > "$BASE/disabled.tmp" || return 1
    /bin/mv -f "$BASE/disabled.tmp" "$BASE/disabled" || return 1
    /bin/sync || return 1
    : > "$RUN/stop"
    log "DISABLED $*"
}

stop_manager()
{
    [ -f "$RUN/manager.pid" ] || return 0
    IFS= read -r manager_pid < "$RUN/manager.pid" || return 0
    case $manager_pid in ''|*[!0-9]*) return 0 ;; esac
    if [ "$manager_pid" -gt 1 ] && [ "$manager_pid" != "$$" ]; then
        /bin/kill -TERM "$manager_pid" 2>/dev/null || :
    fi
}

arm()
{
    [ ! -f "$ARMED" ] && [ ! -f "$LAB_ARMED" ] || return 1
    token="$(date +%s)-$$-$1"
    printf '%s\n' "$token" > "$ARMED.tmp" || return 1
    /bin/mv -f "$ARMED.tmp" "$ARMED" || return 1
    /bin/rm -f "$RUN/ready" "$RUN/accepted" "$RUN/commit-request"
    /usr/bin/nohup /bin/sh "$SELF" --rollback "$token" "$2" \
        </dev/null > "$RUN/rollback.log" 2>&1 &
    /usr/bin/nohup /bin/sh "$SELF" --hard "$token" "$(($2 + 60))" \
        </dev/null > "$RUN/hard.log" 2>&1 &
}

finish()
{
    same_token "$1" || return 1
    [ ! -f "$BASE/disabled" ] && [ ! -f "$RUN/stop" ] || return 1
    printf '%s\n' "$1" > "$RUN/last-good" || return 1
    /bin/rm -f "$ARMED" "$RUN/ready"
}

rollback()
{
    rollback_token=$1
    /bin/sleep "$2"
    same_token "$rollback_token" || return 0
    # Persist this first: a subsequent boot must not retry a failed service.
    disable "TRANSACTION_TIMEOUT token=$rollback_token" || return 1
    stop_manager
    "$TOOL" restore > "$RUN/restore.log" 2>&1
    restore_rc=$?
    /bin/cp "$RUN/restore.log" "$BASE/last-restore.log" || :
    if [ "$restore_rc" -eq 0 ] && same_token "$rollback_token"; then
        /bin/rm -f "$ARMED" "$RUN/ready"
        log "ROLLBACK_VERIFIED token=$rollback_token"
        /bin/sync
    fi
    # On failure leave ARMED for the independent hard deadline.
    return "$restore_rc"
}

hard_deadline()
{
    /bin/sleep "$2"
    same_token "$1" || return 0
    # Never risk a reboot loop if the durable disable cannot be recorded.
    disable "HARD_DEADLINE token=$1" || return 1
    log "ONE_SHOT_REBOOT token=$1" || return 1
    /bin/sync || return 1
    /sbin/reboot
}

file_contains()
{
    # Apple's reduced firmware has no grep/awk/sed. Use native sh only.
    needle=$1
    while IFS= read -r line; do
        case $line in *"$needle"*) return 0 ;; esac
    done < "$2"
    return 1
}

management_ipv4()
{
    while read -r first second rest; do
        [ "$first" = inet ] || continue
        case $second in ''|0.0.0.0|169.254.*) continue ;; *) return 0 ;; esac
    done < "$RUN/management.txt"
    return 1
}

management_route()
{
    while read -r first rest; do
        [ "$first" = default ] || continue
        case $rest in *' bridge0') return 0 ;; esac
    done < "$RUN/routes.txt"
    return 1
}

prerequisites()
{
    /sbin/brconfig bridge0 > "$RUN/bridge0.txt" 2>&1 || return 1
    /sbin/brconfig bridge1 > "$RUN/bridge1.txt" 2>&1 || return 1
    for member in bcmeth0 wlan0 wlan2; do
        file_contains "$member flags=" "$RUN/bridge0.txt" || return 1
    done
    for member in vlan0 wlan1 wlan3; do
        file_contains "$member flags=" "$RUN/bridge1.txt" || return 1
    done
    /sbin/ifconfig vlan0 > "$RUN/vlan0.txt" 2>&1 || return 1
    file_contains 'vlan: 1003 parent interface: bcmeth0' "$RUN/vlan0.txt" || return 1
    /sbin/ifconfig bridge0 > "$RUN/management.txt" 2>&1 || return 1
    management_ipv4 || return 1
    /usr/bin/netstat -rn -f inet > "$RUN/routes.txt" 2>&1 || return 1
    management_route
}

reconcile()
{
    reapplied=no
    if "$TOOL" verify > "$RUN/readback.log" 2>&1; then
        return 0
    fi
    "$TOOL" baseline > "$RUN/baseline.log" 2>&1 || return 1
    /bin/cp "$RUN/baseline.log" "$BASE/last-baseline.log" || return 1
    /bin/sync || return 1
    "$TOOL" apply > "$RUN/apply.log" 2>&1 || return 1
    /bin/cp "$RUN/apply.log" "$BASE/last-apply.log" || return 1
    log "APPLIED_FIXED_CONFIG token=$token" || return 1
    reapplied=yes
}

fail_transaction()
{
    log "FAIL $* token=$token" || :
    # Use the existing token so both detached deadline processes still apply.
    rollback "$token" 0
    exit 1
}

manager_cleanup()
{
    if [ -f "$RUN/manager.pid" ]; then
        IFS= read -r owner < "$RUN/manager.pid" || owner=
        if [ "$owner" = "$$" ]; then
            /bin/rm -f "$RUN/manager.pid"
            /bin/rmdir "$RUN/lock" 2>/dev/null || :
        fi
    fi
}

manager()
{
    printf '%s\n' "$$" > "$RUN/manager.pid" || exit 1
    trap manager_cleanup 0
    arm boot 240 || exit 1
    log "BOOT_TRANSACTION token=$token" || fail_transaction log
    /sbin/sysctl kern.boottime > "$BASE/last-boot.txt" || fail_transaction boot_snapshot
    # ACPd must finish constructing both bridges before any configuration write.
    count=0
    while [ "$count" -lt 30 ]; do
        if prerequisites; then
            if "$TOOL" verify > "$RUN/readback.log" 2>&1 ||
               "$TOOL" baseline > "$RUN/baseline.log" 2>&1; then
                break
            fi
        fi
        /bin/sleep 3
        count=$((count + 1))
    done
    [ "$count" -lt 30 ] || fail_transaction initialization_timeout
    reconcile || fail_transaction initial_configuration
    /bin/sleep 20
    prerequisites && "$TOOL" verify > "$RUN/readback.log" 2>&1 || fail_transaction initial_health
    /bin/cp "$RUN/readback.log" "$BASE/last-boot-readback.log" || fail_transaction snapshot
    printf '%s\n' "$token" > "$RUN/ready" || fail_transaction ready
    log "LOCAL_HEALTH_PASSED token=$token" || fail_transaction log
    if [ -f "$BASE/trial" ]; then
        log "AWAIT_FRESH_EXTERNAL_SSH_ACK token=$token" || fail_transaction log
        while same_token "$token"; do
            if [ -f "$BASE/disabled" ] || [ -f "$RUN/stop" ]; then exit 1; fi
            accepted=
            [ ! -f "$RUN/accepted" ] || IFS= read -r accepted < "$RUN/accepted"
            [ "$accepted" != "$token" ] || break
            /bin/sleep 1
        done
        same_token "$token" || exit 1
        if [ -f "$RUN/commit-request" ]; then
            /bin/rm -f "$BASE/trial" || fail_transaction commit
            /bin/sync || fail_transaction commit_sync
            log "COMMITTED_FOR_FUTURE_BOOTS token=$token" || fail_transaction log
        fi
        log "EXTERNAL_ACK_ACCEPTED token=$token" || fail_transaction log
    fi
    finish "$token" || exit 1
    sequence=0
    while [ ! -f "$BASE/disabled" ] && [ ! -f "$RUN/stop" ]; do
        /bin/sleep 60
        [ ! -f "$BASE/disabled" ] && [ ! -f "$RUN/stop" ] || exit 0
        sequence=$((sequence + 1))
        # A live research watchdog takes precedence; never overlap its SRAB work.
        [ ! -f "$LAB_ARMED" ] || continue
        arm "monitor-$sequence" 90 || exit 1
        prerequisites && reconcile || fail_transaction monitor_configuration
        if [ "$reapplied" = yes ]; then /bin/sleep 20; fi
        prerequisites && "$TOOL" verify > "$RUN/readback.log" 2>&1 || fail_transaction monitor_health
        finish "$token" || exit 1
    done
}

start()
{
    [ -f "$BASE/enabled" ] && [ ! -f "$BASE/disabled" ] || {
        echo SERVICE_NOT_ENABLED_OR_DISABLED; return 0;
    }
    [ ! -f "$LAB_ARMED" ] && [ ! -f "$ARMED" ] || {
        echo REFUSE_CONCURRENT_TRANSACTION >&2; return 1;
    }
    if ! /bin/mkdir "$RUN/lock" 2>/dev/null; then
        echo SERVICE_LOCK_EXISTS_INSPECT_STATUS >&2; return 1
    fi
    /bin/rm -f "$RUN/stop"
    "$TOOL" abi > "$RUN/abi.txt" 2>&1 || { /bin/rmdir "$RUN/lock"; return 1; }
    IFS= read -r abi < "$RUN/abi.txt" || abi=
    [ "$abi" = 'AIRPORT_THREE_VLAN_ABI=1 config=strict-8021q-v1 read=0xc0106d02 write=0x80106d03 size=16' ] || {
        /bin/rmdir "$RUN/lock"; return 1;
    }
    [ ! -f "$BASE/events.log" ] || /bin/mv -f "$BASE/events.log" "$BASE/events.previous.log"
    /usr/bin/nohup /bin/sh "$SELF" --manager </dev/null > "$RUN/manager.log" 2>&1 &
    echo "SERVICE_MANAGER_LAUNCHED pid=$!"
}

ack()
{
    [ -f "$BASE/trial" ] && [ -f "$RUN/ready" ] || {
        echo NO_PENDING_TRIAL_STARTUP_ACK >&2; return 1;
    }
    IFS= read -r ack_token < "$RUN/ready" || return 1
    same_token "$ack_token" || return 1
    [ ! -f "$BASE/disabled" ] && prerequisites || return 1
    "$TOOL" verify > "$RUN/external-ack-readback.log" 2>&1 || return 1
    [ "$1" != commit ] || : > "$RUN/commit-request"
    printf '%s\n' "$ack_token" > "$RUN/accepted" || return 1
    log "FRESH_EXTERNAL_SSH_ACK action=$1 token=$ack_token" || return 1
    echo "EXTERNAL_ACK_RECORDED action=$1 token=$ack_token"
}

/bin/mkdir -p "$RUN" || exit 1
case ${1:-} in
    start) start ;;
    --manager) manager ;;
    --rollback) [ "$#" -eq 3 ] || exit 2; rollback "$2" "$3" ;;
    --hard) [ "$#" -eq 3 ] || exit 2; hard_deadline "$2" "$3" ;;
    ack|commit) ack "$1" ;;
    stop)
        [ ! -f "$LAB_ARMED" ] || exit 1
        if [ -f "$ARMED" ]; then IFS= read -r token < "$ARMED" || exit 1
        else arm manual-stop 90 || exit 1; fi
        rollback "$token" 0 ;;
    status)
        for state in enabled disabled trial; do
            if [ -f "$BASE/$state" ]; then echo "FLASH_$state=PRESENT"; fi
        done
        if [ -f "$RUN/manager.pid" ]; then
            IFS= read -r pid < "$RUN/manager.pid" || exit 1
            case $pid in ''|*[!0-9]*) exit 1 ;; esac
            if /bin/kill -0 "$pid" 2>/dev/null; then echo "MANAGER_LIVE_PID=$pid"
            else echo "MANAGER_PID_NOT_LIVE=$pid"; fi
        fi
        if [ -f "$ARMED" ]; then echo TRANSACTION_ARMED; /bin/cat "$ARMED"
        else echo TRANSACTION_NOT_ARMED; fi
        # This is explicitly a cached observation; external audit uses SRAB.
        [ ! -f "$RUN/last-good" ] || { echo LAST_SUCCESSFUL_READBACK_TOKEN; /bin/cat "$RUN/last-good"; }
        ;;
    *) echo "usage: $0 start|status|ack|commit|stop" >&2; exit 2 ;;
esac
