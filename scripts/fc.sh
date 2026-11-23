#!/sbin/sh
# Galaxy S22 (r0q) TWRP Fast Charge Helper
echo 100 > /sys/class/power_supply/battery/siop_level 2>/dev/null
for c in 5 6 7; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
for c in 0 1 2 3 4; do echo powersave > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor 2>/dev/null; done
echo "S22 TWRP Fast Charge ON: cap=$(cat /sys/class/power_supply/battery/capacity)% siop=$(cat /sys/class/power_supply/battery/siop_level) curr=$(cat /sys/class/power_supply/battery/current_now)uA"
