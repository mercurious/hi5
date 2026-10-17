#!/bin/sh
# HI5 launcher for OnionOS. Mirrors Onion's own GMU launcher: take the audio
# device from audioserver and use Onion's MI_AO/MI_GFX-backed SDL 1.2.
cd "$(dirname "$0")"

echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
[ -f /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh ] && . /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh

touch /tmp/stay_awake
unset LD_PRELOAD
export LD_LIBRARY_PATH="/mnt/SDCARD/.tmp_update/lib/parasyte:/lib:/config/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"

# Optional overrides if audio crackles: HI5_BUFFER=2048, HI5_RATE=44100
./hi5 > log.txt 2>&1

rm -f /tmp/stay_awake
