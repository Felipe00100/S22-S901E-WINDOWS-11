#!/sbin/sh
# Galaxy S22 (r0q) TWRP Fast Charge Helper
# Clears VOTER_USB_100MA (usb_conf_test=2), unlocks high-current USB/PD & SIOP=100,
# turns off big/mid cores + backlight so the battery charges cleanly even on PC USB
# and at full speed on a USB-PD wall charger.
echo 1 > /sys/class/power_supply/battery/boot_completed 2>/dev/null
echo 100 > /sys/class/power_supply/battery/siop_level 2>/dev/null
echo 0 > /sys/class/power_supply/battery/lcd 2>/dev/null
echo 0 > /sys/class/power_supply/battery/pd_disable 2>/dev/null
echo 1 > /sys/class/power_supply/battery/batt_high_current_usb 2>/dev/null
echo 2 > /sys/class/power_supply/battery/usb_conf_test 2>/dev/null
echo 0 > /sys/class/power_supply/battery/store_mode 2>/dev/null
echo 0 > /sys/class/power_supply/battery/batt_slate_mode 2>/dev/null
echo 0 > /sys/class/backlight/panel0-backlight/brightness 2>/dev/null
echo 0 > /sys/class/backlight/panel/brightness 2>/dev/null
for c in 1 2 3 4 5 6 7; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
echo powersave > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo "S22 TWRP Fast Charge ON: cap=$(cat /sys/class/power_supply/battery/capacity)% type=$(cat /sys/class/power_supply/battery/charge_type) siop=$(cat /sys/class/power_supply/battery/siop_level) curr=$(cat /sys/class/power_supply/battery/current_now)mA volt=$(cat /sys/class/power_supply/battery/voltage_now)uV"
