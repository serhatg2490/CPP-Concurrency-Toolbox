#!/usr/bin/env bash
# ============================================================================
#  tune-benchmark-host.sh — OS-level latency tuning validated for the
#  SPSC/SPMC lock-free queue benchmarks (see benchmark/BenchmarkMain.cpp,
#  benchmark/TscBenchmarkMain.cpp, and docs/benchmark-analysis-2026-07-22.md
#  for the full measurement history behind each step below).
#
#  Applies, in order:
#    1) Disables deep C-states (C6/C10) on the cores the benchmarks pin to.
#    2) Disables the kernel's RT runtime throttle.
#    3) Adds isolcpus= / nohz_full= / rcu_nocbs= to the kernel command line
#       (GRUB) so the general scheduler and periodic timer tick stay off
#       those cores entirely.
#
#  Steps 1-2 are runtime-only: instant, safe, and automatically undone by a
#  reboot. Step 3 is persistent and REQUIRES A REBOOT to take effect; it is
#  gated behind a confirmation prompt (skip with --yes) because it edits
#  /etc/default/grub.
#
#  Usage:
#    sudo ./scripts/tune-benchmark-host.sh            # interactive
#    sudo ./scripts/tune-benchmark-host.sh --yes       # no prompts
#    sudo ./scripts/tune-benchmark-host.sh --no-grub   # skip step 3 entirely
#    ./scripts/tune-benchmark-host.sh --status         # report current state, no changes
#
#  Undo:
#    sudo cpupower -c "$CORES" idle-set -e 2 && sudo cpupower -c "$CORES" idle-set -e 3
#    sudo sysctl -w kernel.sched_rt_runtime_us=950000
#    sudo cp /etc/default/grub.bak /etc/default/grub && sudo update-grub && sudo reboot
# ============================================================================

set -euo pipefail

# Cores actually pinned by the benchmarks: main=0 producer=5 consumers=1,3,6,8
# (see the "Core assignment constants" block in BenchmarkMain.cpp / TscBenchmarkMain.cpp).
CORES="0,1,3,5,6,8"
CORE_LIST="0,1,2,3,4,5,6,7,8"   # isolcpus range: full P-core HT pairs + E-core0 (doc §16)

ASSUME_YES=0
DO_GRUB=1
STATUS_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --yes|-y)   ASSUME_YES=1 ;;
        --no-grub)  DO_GRUB=0 ;;
        --status)   STATUS_ONLY=1 ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

report_status() {
    echo "== C-state (C6/C10 disable status, cores $CORES) =="
    IFS=',' read -ra core_arr <<< "$CORES"
    for c in "${core_arr[@]}"; do
        for state_dir in /sys/devices/system/cpu/cpu"$c"/cpuidle/state*/; do
            name=$(cat "$state_dir/name")
            [[ "$name" == "C6" || "$name" == "C10" ]] || continue
            dis=$(cat "$state_dir/disable")
            echo "cpu$c $name: disable=$dis"
        done
    done
    echo
    echo "== RT throttling =="
    echo "sched_rt_runtime_us = $(cat /proc/sys/kernel/sched_rt_runtime_us)"
    echo
    echo "== isolcpus/nohz_full/rcu_nocbs (kernel cmdline) =="
    grep -o 'isolcpus=[^ ]*\|nohz_full=[^ ]*\|rcu_nocbs=[^ ]*' /proc/cmdline || echo "(not set)"
    echo
    echo "== Isolated cores (as recognized by the kernel) =="
    cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "(none)"
}

if [[ "$STATUS_ONLY" -eq 1 ]]; then
    report_status
    exit 0
fi

if [[ "$EUID" -ne 0 ]]; then
    echo "This script requires root (for cpupower/sysctl/GRUB). Run it with sudo." >&2
    exit 1
fi

echo "############################################################"
echo "# 1) Disable deep C-states (C6, C10) — cores: $CORES"
echo "############################################################"
cpupower -c "$CORES" idle-set -d 2   # C6
cpupower -c "$CORES" idle-set -d 3   # C10
echo "Done. (Reset automatically on reboot.)"
echo

echo "############################################################"
echo "# 2) Disable RT runtime throttling"
echo "############################################################"
sysctl -w kernel.sched_rt_runtime_us=-1
echo "Done. (Reset automatically on reboot.)"
echo

if [[ "$DO_GRUB" -eq 0 ]]; then
    echo "--no-grub given, skipping the isolcpus/nohz_full/rcu_nocbs step."
    exit 0
fi

TARGET_LINE="isolcpus=${CORE_LIST} nohz_full=${CORE_LIST} rcu_nocbs=${CORE_LIST}"

if grep -q "isolcpus=${CORE_LIST}" /etc/default/grub 2>/dev/null; then
    echo "############################################################"
    echo "# 3) isolcpus/nohz_full/rcu_nocbs already set, skipping."
    echo "############################################################"
    exit 0
fi

echo "############################################################"
echo "# 3) isolcpus/nohz_full/rcu_nocbs will be added to GRUB (PERSISTENT, REQUIRES REBOOT)"
echo "############################################################"
echo "About to add: $TARGET_LINE"
echo "Affected cores ($CORE_LIST) will be fully withdrawn from the general scheduler —"
echo "the rest of the desktop/system load will keep running on the remaining CPUs."

if [[ "$ASSUME_YES" -ne 1 ]]; then
    read -r -p "Proceed? [y/N] " reply
    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Cancelled. Steps (1) and (2) were applied, (3) was skipped."; exit 0 ;;
    esac
fi

cp /etc/default/grub /etc/default/grub.bak

if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
    sed -i -E "s|^GRUB_CMDLINE_LINUX_DEFAULT=\"([^\"]*)\"|GRUB_CMDLINE_LINUX_DEFAULT=\"\1 ${TARGET_LINE}\"|" \
        /etc/default/grub
else
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"${TARGET_LINE}\"" >> /etc/default/grub
fi

grep GRUB_CMDLINE_LINUX_DEFAULT /etc/default/grub
update-grub

echo
echo "Done. A REBOOT is required for the change to take effect:"
echo "  sudo reboot"
echo "Post-reboot verification:"
echo "  cat /proc/cmdline"
echo "  cat /sys/devices/system/cpu/isolated   # should return ${CORE_LIST}"
echo "To revert:"
echo "  sudo cp /etc/default/grub.bak /etc/default/grub && sudo update-grub && sudo reboot"
