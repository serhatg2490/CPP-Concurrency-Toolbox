#!/usr/bin/env bash
# ============================================================================
#  tune-benchmark-host.sh — OS-level latency tuning validated for the
#  SPSC/SPMC lock-free queue benchmarks (see benchmark/BenchmarkMain.cpp,
#  benchmark/TscBenchmarkMain.cpp, and docs/benchmark-analysis-2026-07-22.md
#  for the full measurement history behind each step below).
#
#  Applies, in order:
#    1) Sets the CPU governor to "performance" and stops irqbalance, if either
#       isn't already in that state (original values saved under /run so
#       --undo can restore them precisely).
#    2) Pins the frequency floor (scaling_min_freq = scaling_max_freq) on the
#       cores the benchmarks pin to, removing intel_pstate/HWP's P-state
#       ramp-up jitter. Standard low-latency-tuning practice; measured on
#       this host to NOT change the residual 1C tail (see doc §21), kept as
#       baseline hygiene the same way governor/C-state/RT-throttle are.
#    3) Disables C-states (C1E/C6/C10) on the cores the benchmarks pin to.
#    4) Disables the kernel's RT runtime throttle.
#    5) Adds isolcpus= / nohz_full= / rcu_nocbs= to the kernel command line
#       (GRUB) so the general scheduler and periodic timer tick stay off
#       those cores entirely.
#
#  Steps 1-4 are runtime-only: instant, safe, and automatically undone by a
#  reboot. Step 5 is persistent and REQUIRES A REBOOT to take effect; it is
#  gated behind a confirmation prompt (skip with --yes) because it edits
#  /etc/default/grub.
#
#  Zero-flag, run-it-twice flow: if isolcpus/nohz_full/rcu_nocbs aren't set
#  yet, steps 1-4 would just be wiped by the reboot step 5 needs anyway, so
#  this script skips them and goes straight to step 5. Rebooting, then
#  running this exact same command again, finds isolcpus already set and
#  applies 1-4 for real while step 5 auto-skips. No flags needed either time.
#
#  Usage:
#    sudo ./scripts/tune-benchmark-host.sh            # run before AND after the reboot
#    sudo ./scripts/tune-benchmark-host.sh --yes       # no confirmation prompts
#    sudo ./scripts/tune-benchmark-host.sh --no-grub   # never touch GRUB, only steps 1-4
#    ./scripts/tune-benchmark-host.sh --status         # report current state, no changes
#    sudo ./scripts/tune-benchmark-host.sh --undo      # revert everything above
#    sudo ./scripts/tune-benchmark-host.sh --undo --yes
# ============================================================================

set -euo pipefail

# Cores actually pinned by the benchmarks: main=0 producer=5 consumers=1,3,6,8
# (see the "Core assignment constants" block in BenchmarkMain.cpp / TscBenchmarkMain.cpp).
CORES="0,1,3,5,6,8"
CORE_LIST="0,1,2,3,4,5,6,7,8"   # isolcpus range: full P-core HT pairs + E-core0 (doc §16)

# State files (tmpfs, cleared on reboot -- matches the reboot-transient nature
# of steps 1-4) used to remember pre-change values so --undo can restore them
# precisely instead of guessing.
GOV_STATE_FILE="/run/tune-benchmark-host.governor"
IRQBALANCE_STATE_FILE="/run/tune-benchmark-host.irqbalance"
FREQ_STATE_FILE="/run/tune-benchmark-host.minfreq"   # lines: "cpu<N> <original_min_freq>"

ASSUME_YES=0
DO_GRUB=1
STATUS_ONLY=0
UNDO=0

for arg in "$@"; do
    case "$arg" in
        --yes|-y)   ASSUME_YES=1 ;;
        --no-grub)  DO_GRUB=0 ;;
        --status)   STATUS_ONLY=1 ;;
        --undo)     UNDO=1 ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

report_status() {
    echo "== CPU governor =="
    cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u || echo "(cpufreq not available)"
    echo
    echo "== irqbalance =="
    systemctl is-active irqbalance 2>/dev/null || true
    echo
    echo "== CPU frequency floor (cores $CORES) =="
    IFS=',' read -ra freq_core_arr <<< "$CORES"
    for c in "${freq_core_arr[@]}"; do
        fd="/sys/devices/system/cpu/cpu$c/cpufreq"
        [[ -d "$fd" ]] || continue
        echo "cpu$c: min=$(cat "$fd/scaling_min_freq") max=$(cat "$fd/scaling_max_freq") cur=$(cat "$fd/scaling_cur_freq")"
    done
    echo
    echo "== C-state (C1E/C6/C10 disable status, cores $CORES) =="
    IFS=',' read -ra core_arr <<< "$CORES"
    for c in "${core_arr[@]}"; do
        for state_dir in /sys/devices/system/cpu/cpu"$c"/cpuidle/state*/; do
            name=$(cat "$state_dir/name")
            [[ "$name" == "C1E" || "$name" == "C6" || "$name" == "C10" ]] || continue
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

if [[ "$UNDO" -eq 1 ]]; then
    echo "############################################################"
    echo "# Undo 1) Restore CPU governor and irqbalance"
    echo "############################################################"
    if [[ -f "$IRQBALANCE_STATE_FILE" ]]; then
        systemctl start irqbalance
        rm -f "$IRQBALANCE_STATE_FILE"
        echo "irqbalance restarted."
    else
        echo "irqbalance: nothing to restore (was already inactive, or not touched)."
    fi
    if [[ -f "$GOV_STATE_FILE" ]]; then
        orig_gov=$(cat "$GOV_STATE_FILE")
        cpupower frequency-set -g "$orig_gov" >/dev/null
        rm -f "$GOV_STATE_FILE"
        echo "Governor restored to '$orig_gov'."
    else
        echo "Governor: nothing to restore (was already 'performance', or not touched)."
    fi
    echo

    echo "############################################################"
    echo "# Undo 2) Restore CPU frequency floor — cores: $CORES"
    echo "############################################################"
    if [[ -f "$FREQ_STATE_FILE" ]]; then
        while read -r cpu_name orig_min; do
            c="${cpu_name#cpu}"
            fd="/sys/devices/system/cpu/cpu$c/cpufreq"
            [[ -d "$fd" ]] || continue
            echo "$orig_min" > "$fd/scaling_min_freq"
            echo "cpu$c: min restored to $orig_min"
        done < "$FREQ_STATE_FILE"
        rm -f "$FREQ_STATE_FILE"
    else
        echo "Nothing to restore (was already at default, or not touched)."
    fi
    echo

    echo "############################################################"
    echo "# Undo 3) Re-enable C-states (C1E, C6, C10) — cores: $CORES"
    echo "############################################################"
    cpupower -c "$CORES" idle-set -e 1   # C1E
    cpupower -c "$CORES" idle-set -e 2   # C6
    cpupower -c "$CORES" idle-set -e 3   # C10
    echo "Done."
    echo

    echo "############################################################"
    echo "# Undo 4) Restore RT runtime throttling to the kernel default"
    echo "############################################################"
    sysctl -w kernel.sched_rt_runtime_us=950000
    echo "Done."
    echo

    if [[ ! -f /etc/default/grub.bak ]]; then
        echo "############################################################"
        echo "# Undo 5) No /etc/default/grub.bak found — skipping GRUB revert."
        echo "############################################################"
        echo "(Steps 1-4 were reverted; isolcpus/nohz_full/rcu_nocbs, if set, must be"
        echo " removed from /etc/default/grub by hand, followed by update-grub + reboot.)"
        exit 0
    fi

    echo "############################################################"
    echo "# Undo 5) Restore /etc/default/grub from backup (PERSISTENT, REQUIRES REBOOT)"
    echo "############################################################"
    diff -u /etc/default/grub /etc/default/grub.bak || true

    if [[ "$ASSUME_YES" -ne 1 ]]; then
        read -r -p "Restore the backup and run update-grub? [y/N] " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "Cancelled. Steps 1-4 were reverted, GRUB was left untouched."; exit 0 ;;
        esac
    fi

    cp /etc/default/grub.bak /etc/default/grub
    update-grub
    echo
    echo "Done. A REBOOT is required for the GRUB change to take effect:"
    echo "  sudo reboot"
    exit 0
fi

# Check GRUB state up front: if isolcpus/nohz_full/rcu_nocbs aren't set yet,
# steps 1-4 (runtime-only) would just be wiped seconds later by the reboot
# that step 5 is about to require -- so skip them and go straight to step 5.
# After rebooting, running this exact same command again finds isolcpus
# already set, so it applies 1-4 for real and step 5 auto-skips. No flags
# needed on either run.
TARGET_LINE="isolcpus=${CORE_LIST} nohz_full=${CORE_LIST} rcu_nocbs=${CORE_LIST}"
GRUB_ALREADY_SET=0
if grep -q "isolcpus=${CORE_LIST}" /etc/default/grub 2>/dev/null; then
    GRUB_ALREADY_SET=1
fi

if [[ "$DO_GRUB" -eq 1 && "$GRUB_ALREADY_SET" -eq 0 ]]; then
    echo "isolcpus/nohz_full/rcu_nocbs are not set yet, and applying them (step 5)"
    echo "requires a reboot that would immediately wipe steps 1-4 anyway -- skipping"
    echo "governor/irqbalance, frequency floor, C-state and RT-throttle tuning this run."
    echo "Re-run this exact same command after rebooting to apply them for real."
    echo
else
    echo "############################################################"
    echo "# 1) Set CPU governor to performance, stop irqbalance"
    echo "############################################################"
    cur_gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "")
    if [[ -n "$cur_gov" && "$cur_gov" != "performance" ]]; then
        echo "$cur_gov" > "$GOV_STATE_FILE"
        cpupower frequency-set -g performance >/dev/null
        echo "Governor: $cur_gov -> performance (original saved for --undo)."
    else
        echo "Governor already 'performance' (or cpufreq unavailable) — nothing to do."
    fi
    if systemctl is-active --quiet irqbalance 2>/dev/null; then
        touch "$IRQBALANCE_STATE_FILE"
        systemctl stop irqbalance
        echo "irqbalance: active -> stopped (will be restarted by --undo)."
    else
        echo "irqbalance already inactive (or not installed) — nothing to do."
    fi
    echo "(Both reset automatically on reboot.)"
    echo

    echo "############################################################"
    echo "# 2) Pin CPU frequency floor to max — cores: $CORES"
    echo "############################################################"
    : > "$FREQ_STATE_FILE"
    IFS=',' read -ra freq_core_arr <<< "$CORES"
    for c in "${freq_core_arr[@]}"; do
        fd="/sys/devices/system/cpu/cpu$c/cpufreq"
        if [[ ! -d "$fd" ]]; then
            echo "cpu$c: no cpufreq directory, skipping"
            continue
        fi
        cur_min=$(cat "$fd/scaling_min_freq")
        max=$(cat "$fd/scaling_max_freq")
        if [[ "$cur_min" != "$max" ]]; then
            echo "cpu$c $cur_min" >> "$FREQ_STATE_FILE"
            echo "$max" > "$fd/scaling_min_freq"
            echo "cpu$c: min $cur_min -> $max (original saved for --undo)"
        else
            echo "cpu$c: already pinned to max ($max) — nothing to do"
        fi
    done
    echo "(Reset automatically on reboot. Measured on this host to NOT change the"
    echo " residual 1C tail -- kept as standard low-latency hygiene regardless.)"
    echo

    echo "############################################################"
    echo "# 3) Disable C-states (C1E, C6, C10) — cores: $CORES"
    echo "############################################################"
    cpupower -c "$CORES" idle-set -d 1   # C1E
    cpupower -c "$CORES" idle-set -d 2   # C6
    cpupower -c "$CORES" idle-set -d 3   # C10
    echo "Done. (Reset automatically on reboot. Measured on this host to NOT change"
    echo " the residual 1C tail -- kept as standard low-latency hygiene regardless.)"
    echo

    echo "############################################################"
    echo "# 4) Disable RT runtime throttling"
    echo "############################################################"
    sysctl -w kernel.sched_rt_runtime_us=-1
    echo "Done. (Reset automatically on reboot.)"
    echo
fi

if [[ "$DO_GRUB" -eq 0 ]]; then
    echo "--no-grub given, skipping the isolcpus/nohz_full/rcu_nocbs step."
    exit 0
fi

if [[ "$GRUB_ALREADY_SET" -eq 1 ]]; then
    echo "############################################################"
    echo "# 5) isolcpus/nohz_full/rcu_nocbs already set, skipping."
    echo "############################################################"
    exit 0
fi

echo "############################################################"
echo "# 5) isolcpus/nohz_full/rcu_nocbs will be added to GRUB (PERSISTENT, REQUIRES REBOOT)"
echo "############################################################"
echo "About to add: $TARGET_LINE"
echo "Affected cores ($CORE_LIST) will be fully withdrawn from the general scheduler —"
echo "the rest of the desktop/system load will keep running on the remaining CPUs."

if [[ "$ASSUME_YES" -ne 1 ]]; then
    read -r -p "Proceed? [y/N] " reply
    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Cancelled. Nothing was changed."; exit 0 ;;
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
echo "After rebooting, run this exact same command again (no flags needed) to"
echo "apply governor/irqbalance/frequency-floor/C-state/RT-throttle tuning for"
echo "real -- step 5 will auto-detect isolcpus is already set and skip itself."
echo "Post-reboot verification:"
echo "  cat /proc/cmdline"
echo "  cat /sys/devices/system/cpu/isolated   # should return ${CORE_LIST}"
echo "To revert:"
echo "  sudo cp /etc/default/grub.bak /etc/default/grub && sudo update-grub && sudo reboot"
