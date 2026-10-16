#!/bin/bash
export UV_USE_IO_URING=0
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root
export XDG_RUNTIME_DIR=/tmp
export WAYLAND_DISPLAY=wayland-0
export DISPLAY=:0
export XKB_CONFIG_ROOT=/usr/share/X11/xkb
export FONTCONFIG_PATH=/etc/fonts
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/opt/example-ide
export LD_PRELOAD=/opt/example-ide/libnopkey.so
exec /opt/example-ide/example-ide \
  --no-sandbox \
  --disable-gpu \
  --ozone-platform=wayland \
  --enable-features=UseOzonePlatform \
  --disable-features=ScriptStreaming,PrecompileInlineScripts,BackgroundResourceFetch \
  --js-flags="--no-concurrent-marking --no-concurrent-recompilation --no-concurrent-sweeping --no-parallel-marking --no-parallel-scavenge --no-flush-bytecode --no-turbofan --no-maglev --no-sparkplug --regexp-interpret-all --no-regexp-tier-up" \
  --disable-extension=vscode.git \
  --disable-extension=vscode.github \
  --user-data-dir=/root/.config/example-ide \
  /workspace/wayland_server_engine.cpp
