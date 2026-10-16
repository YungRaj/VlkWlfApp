#!/system/bin/sh
ROOT=/data/local/linux-root
CACHE=/data/user/10/com.google.example.vkwlf/cache
TRIGGER=$CACHE/launch_linux_app.trigger

# Keep the kernel console (virtio-console/hvc0) quiet: under QEMU MTTCG, heavy
# console traffic corrupts guest memory (printk text found in kernel VMA structs).
echo 0 > /proc/sys/kernel/printk
echo 2 > /proc/sys/kernel/io_uring_disabled 2>/dev/null || true

grep -q " $ROOT/dev " /proc/mounts || mount -o bind /dev $ROOT/dev
mkdir -p $ROOT/dev/pts
grep -q " $ROOT/dev/pts " /proc/mounts || mount -t devpts devpts $ROOT/dev/pts
mkdir -p $ROOT/dev/shm
grep -q " $ROOT/dev/shm " /proc/mounts || mount -t tmpfs -o size=2G,mode=1777 tmpfs $ROOT/dev/shm
chmod 1777 $ROOT/dev/shm
grep -q " $ROOT/proc " /proc/mounts || mount -t proc proc $ROOT/proc
grep -q " $ROOT/sys " /proc/mounts || mount -t sysfs sysfs $ROOT/sys
grep -q " $ROOT/tmp " /proc/mounts || mount -o bind "$CACHE" $ROOT/tmp
chmod 777 $ROOT/tmp



LAST_TS=""
while true; do
  if [ -f "$TRIGGER" ]; then
    CUR_TS=$(cat "$TRIGGER" 2>/dev/null)
    rm -f "$TRIGGER"
    if [ "$CUR_TS" != "$LAST_TS" ]; then
      LAST_TS="$CUR_TS"
      if pgrep -x example-ide >/dev/null 2>&1; then
        sleep 1
        continue
      fi
      grep -q " $ROOT/tmp " /proc/mounts || mount -o bind "$CACHE" $ROOT/tmp
      chmod 777 $ROOT/tmp
      mkdir -p $ROOT/dev/pts
      grep -q " $ROOT/dev/pts " /proc/mounts || mount -t devpts devpts $ROOT/dev/pts
      mkdir -p $ROOT/dev/shm
      grep -q " $ROOT/dev/shm " /proc/mounts || mount -t tmpfs -o size=2G,mode=1777 tmpfs $ROOT/dev/shm
      chmod 1777 $ROOT/dev/shm
      rm -f $ROOT/root/.config/example-ide/Singleton* $ROOT/root/.config/example-ide/code.lock
      rm -rf $ROOT/root/.config/example-ide/Backups $ROOT/root/.config/example-ide/Crashpad
      mkdir -p $ROOT/root/.config/example-ide/User
      cat << "JSON" > $ROOT/root/.config/example-ide/User/settings.json
{
  "window.titleBarStyle": "custom",
  "workbench.startupEditor": "none",
  "security.workspace.trust.enabled": false,
  "telemetry.telemetryLevel": "off",
  "update.mode": "none"
}
JSON
      rm -f $ROOT/tmp/vscode-*.sock
      chroot $ROOT /usr/local/bin/run_linux_app.sh </dev/null >/data/local/linux-root/tmp/linux_app_host.log 2>&1 &
    fi
  fi
  sleep 1
done
