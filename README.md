# intel-mac-kernel-task-fix

My 2019 16" MacBook Pro (i9) lost its screen, so I took the display off and use it headless. After that it sat at 800 MHz with kernel_task eating 700-800% CPU and both fans maxed out, even at 40 C idle. Turns out the battery connector had come loose during the teardown and the SMC was reporting a max thermal level because of the missing sensors.

This repo is what I ended up with. It lifts the CPU limit macOS puts on and runs the fans off a real CPU temperature. No SIP changes, no kernel patches, no OpenCore. Just two launchd jobs.

Tested on MacBookPro16,1, macOS Sequoia 15.7.7. Other Intel Macs with XCPM should work the same way but I haven't tried them.

If you can reconnect whatever is missing (battery, display cable, a sensor), do that instead. That fixes the cause. This only works around it.

## Symptoms

- kernel_task constantly at several hundred percent
- fans at full speed when the machine is cold
- `pmset -g therm` shows something like `CPU_Speed_Limit = 17` and `CPU_Scheduler_Limit = 40`
- `sudo powermetrics --samplers smc,thermal -i 2000 -n 1` shows CPU thermal level 255 with a cool CPU die and 0 prochots
- `sysctl machdep.xcpm` shows `hard_plimit_max_100mhz_ratio: 8` and `forced_idle_ratio: 40`

If your CPU is actually hot, this isn't your problem. Clean the fans and repaste.

To see which sensor is missing:

```
./bin/smcfan temps
```

Anything reading -127 or 0 is suspect. On mine it was the battery sensors (TB0T, TB1T, TB2T), and `ioreg -rn AppleSmartBattery | grep BatteryInstalled` said `No`.

## Install

```
sudo sh install.sh
```

That copies `xcpm.pl` and `smcfan` to /usr/local/bin, installs the two jobs in /Library/LaunchDaemons and prints the status. Both run at boot, no login needed.

- `local.xcpm.unthrottle` runs `xcpm.pl daemon 1`, which removes the CPU limit and forced idle and checks again every second
- `local.smcfan.curve` runs `smcfan curve TCMX 50 85 3`: fans at minimum up to 50 C on the hottest core, full speed at 85 C, linear in between. If the sensor can't be read the fans go to max.

Check it worked:

```
sudo perl /usr/local/bin/xcpm.pl status
/usr/local/bin/smcfan fans
top -l 2 -s 3 -stats command,cpu | grep kernel_task | tail -1
```

You want the hard limit back to the full range (800-4500 MHz on mine), forced idle at 100% (off), fans in manual near minimum, kernel_task at a few percent.

If your Mac doesn't have a TCMX key, pick another CPU sensor from `smcfan temps` (TCXC or TC0P) and change it in `launchd/local.smcfan.curve.plist` before installing. Do a load test afterwards:

```
for i in $(seq 16); do yes > /dev/null & done
tail -f /var/log/smcfan.log
killall yes
```

If it goes past ~95 C, lower the 85.

Logs are in /var/log/xcpm.log and /var/log/smcfan.log.

## Uninstall

```
sudo sh uninstall.sh
```

## Caveats

- You're turning off macOS's software thermal throttling. The CPU still throttles itself in hardware at TjMax and the fan curve keeps it cool, but keep an eye on temps at first.
- With no battery, some of that throttling is there on purpose. The battery normally covers power spikes the charger can't. Under heavy CPU + GPU load the machine might just switch off. If that happens, cap the CPU lower: in `xcpm.pl`, replace `$s->{smax}` in the `set_hard_plimit` call with a fixed number like `3000` (multiple of 100).
- A macOS update could change the /dev/xcpm interface. If kernel_task goes back up after an update, check the log and `xcpm.pl status`.
- Use at your own risk.

## How it works

Power management on Intel Macs is XCPM, inside the kernel. The `machdep.xcpm` sysctls are read-only, but root can read and write the same values through ioctls on /dev/xcpm. I worked these out from `xcpm_cpu_control` and `bsd_xcpm_ioctl` in the 15.7.9 (24G830) Kernel Debug Kit kernel.

The interesting part: the kernel doesn't throttle based on `cpu_thermal_level` itself. It just stores the value and notifies whoever is listening. The actual throttle is a separate hard P-state limit plus forced idle, set from outside the kernel about 10 seconds after boot (probably by ACPI_SMC_PlatformPlugin or X86PlatformShim). Once you overwrite those, nothing sets them again.

| ioctl | data | what |
|---|---|---|
| 0xc0045815 | 3 x u8 | get cpu/gpu/io thermal level |
| 0xc0045819 | 3 x u8 | get thermal level, second source |
| 0xc0085802 | u32 max MHz, u32 min MHz | get hard P-state limit |
| 0xc0085803 | u32 max MHz, u32 min MHz | set hard P-state limit (multiples of 100) |
| 0xc0085811 | u32 max MHz, u32 min MHz | get soft P-state limit |
| 0xc008580a | u32 ratio, u32 period us | get forced idle (100 = off) |
| 0xc008580b | u32 ratio <= 100, u32 period us <= 1000000 | set forced idle |

Things I tried that didn't help: removing the Mac-XXXX.plist thermal profile from X86PlatformPlugin, and removing X86PlatformPlugin itself (you can't rebuild the kernel collection without a KDK for your exact build, and it's not the thing setting the throttle anyway).

`smcfan` is a small fan tool that talks to the AppleSMC user client (selector 2, the usual 80 byte struct), same as any fan app. It sets `F<n>Md` to 1 for manual mode and writes `F<n>Tg`. The SMC took manual mode fine even with the missing sensor.

## Building smcfan

There's a prebuilt x86_64 binary in bin/. To build it yourself:

```
sh build/build.sh
```

On a Mac it uses the Command Line Tools. On Linux it cross-compiles with clang and lld using the small .tbd stubs in build/, no SDK needed.

## License

MIT
