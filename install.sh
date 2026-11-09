#!/bin/sh
# Install both background jobs:
#   local.xcpm.unthrottle  removes the bogus CPU throttle (xcpm.pl daemon)
#   local.smcfan.curve     drives the fans from a real CPU temperature (smcfan curve)
# Run from the repository folder:  sudo sh install.sh
set -e
cd "$(dirname "$0")"

if [ "$(id -u)" != "0" ]; then
    echo "Run it with sudo: sudo sh install.sh"
    exit 1
fi

echo "[1/4] Copying tools to /usr/local/bin"
mkdir -p /usr/local/bin
cp xcpm.pl /usr/local/bin/xcpm.pl
cp bin/smcfan /usr/local/bin/smcfan
chmod 755 /usr/local/bin/xcpm.pl /usr/local/bin/smcfan
# Files downloaded with a browser get a quarantine flag that can block them
xattr -d com.apple.quarantine /usr/local/bin/smcfan 2>/dev/null || true
xattr -d com.apple.quarantine /usr/local/bin/xcpm.pl 2>/dev/null || true

echo "[2/4] Checking the tools run"
/usr/local/bin/smcfan fans
perl /usr/local/bin/xcpm.pl status

for job in local.xcpm.unthrottle local.smcfan.curve; do
    echo "[3/4] Installing $job"
    launchctl bootout "system/$job" 2>/dev/null || true
    cp "launchd/$job.plist" /Library/LaunchDaemons/
    chown root:wheel "/Library/LaunchDaemons/$job.plist"
    chmod 644 "/Library/LaunchDaemons/$job.plist"
    launchctl bootstrap system "/Library/LaunchDaemons/$job.plist"
done

echo "[4/4] Waiting 3 seconds, then showing the result"
sleep 3
launchctl print system/local.xcpm.unthrottle | grep -m1 "state"
launchctl print system/local.smcfan.curve | grep -m1 "state"
tail -n 4 /var/log/xcpm.log
tail -n 2 /var/log/smcfan.log
/usr/local/bin/smcfan fans
echo "Done. Logs: /var/log/xcpm.log and /var/log/smcfan.log"
