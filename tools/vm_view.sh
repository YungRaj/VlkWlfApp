#!/usr/bin/env bash
# Open a TigerVNC window on the cuttlefish VM with correct colours.
#
# QEMU's built-in VNC server (127.0.0.1:6444) mislabels the virtio-gpu scanout
# so red/blue come out swapped in every standards-compliant viewer.  We always
# connect through tools/vnc_rbswap_proxy.py (127.0.0.1:6445), which cancels the
# swap, and use a non-JPEG encoding so QEMU honours the negotiated pixel format.
#
# Usage: tools/vm_view.sh            # connect
#        tools/vm_view.sh --restart  # also restart the proxy
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU_VNC_PORT=6444
PROXY_PORT=6445

if [[ "${1:-}" == "--restart" ]]; then
  pkill -f "vnc_rbswap_proxy.py ${PROXY_PORT}" || true
  sleep 0.5
fi
if ! ss -ltn | grep -q ":${PROXY_PORT} "; then
  echo "starting R/B-swap proxy on ${PROXY_PORT}"
  setsid nohup python3 "${HERE}/vnc_rbswap_proxy.py" "${PROXY_PORT}" 127.0.0.1 "${QEMU_VNC_PORT}" \
    > /tmp/vnc_rbswap_proxy.log 2>&1 &
  sleep 0.5
fi

# Do NOT connect to ${QEMU_VNC_PORT} directly: colours will be wrong.
exec vncviewer -Shared -FullColor -PreferredEncoding ZRLE -NoJPEG=1 -AutoSelect=0 \
  -RemoteResize=0 "127.0.0.1:${PROXY_PORT}" "$@"
