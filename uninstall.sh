#!/bin/sh
# Remove both jobs and give the fans back to the SMC.
# Run:  sudo sh uninstall.sh
# The CPU throttle comes back on the next reboot (or when macOS next sets it).

if [ "$(id -u)" != "0" ]; then
    echo "Run it with sudo: sudo sh uninstall.sh"
    exit 1
fi

for job in local.smcfan.curve local.xcpm.unthrottle; do
    echo "Stopping and removing $job"
    launchctl bootout "system/$job" 2>/dev/null || echo "  (was not running)"
    rm -f "/Library/LaunchDaemons/$job.plist"
done

if [ -x /usr/local/bin/smcfan ]; then
    echo "Returning fans to automatic SMC control"
    /usr/local/bin/smcfan auto
fi

rm -f /usr/local/bin/smcfan /usr/local/bin/xcpm.pl
echo "Removed. Logs left in /var/log/xcpm.log and /var/log/smcfan.log (delete them if you like)."
