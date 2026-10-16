#!/usr/bin/env python3
"""vnc_rbswap_proxy.py — fix R/B-swapped colours from QEMU's VNC server.

The cuttlefish QEMU build mislabels the guest's virtio-gpu scanout (RGBA in
memory) as BGRA, so every VNC client sees red and blue exchanged.  QEMU's own
`screendump` shows the same swap, so it cannot be fixed on the viewer side.

This proxy sits between a real VNC client (e.g. TigerVNC) and QEMU and swaps the
red/blue *shift* fields in the two pixel-format descriptors that cross the wire
(ServerInit and the client's SetPixelFormat).  QEMU then converts into the
"swapped" layout, which cancels its own error.  All other traffic — framebuffer
updates in any encoding, input, clipboard — is forwarded verbatim.

Usage:  vnc_rbswap_proxy.py [listen_port=6445] [server_host=127.0.0.1] [server_port=6444]
        vncviewer 127.0.0.1:6445
"""
import socket
import struct
import sys
import threading

LISTEN_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 6445
SERVER_HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
SERVER_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 6444

PIXFMT = struct.Struct("!BBBBHHHBBBxxx")  # 16 bytes


def swap_rb(pf: bytes) -> bytes:
    bpp, depth, be, tc, rmax, gmax, bmax, rsh, gsh, bsh = PIXFMT.unpack(pf)
    if not tc:
        return pf
    return PIXFMT.pack(bpp, depth, be, tc, bmax, gmax, rmax, bsh, gsh, rsh)


def recv_exact(s: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf


def pump(src: socket.socket, dst: socket.socket) -> None:
    try:
        while True:
            data = src.recv(1 << 16)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def client_to_server(c: socket.socket, s: socket.socket) -> None:
    """Parse client messages just enough to find SetPixelFormat."""
    try:
        while True:
            t = recv_exact(c, 1)
            mt = t[0]
            if mt == 0:  # SetPixelFormat: 3 pad + 16 pixfmt
                body = recv_exact(c, 19)
                s.sendall(t + body[:3] + swap_rb(body[3:]))
            elif mt == 2:  # SetEncodings
                hdr = recv_exact(c, 3)
                n = struct.unpack("!H", hdr[1:])[0]
                s.sendall(t + hdr + recv_exact(c, 4 * n))
            elif mt == 3:  # FramebufferUpdateRequest
                s.sendall(t + recv_exact(c, 9))
            elif mt == 4:  # KeyEvent
                s.sendall(t + recv_exact(c, 7))
            elif mt == 5:  # PointerEvent
                s.sendall(t + recv_exact(c, 5))
            elif mt == 6:  # ClientCutText
                hdr = recv_exact(c, 7)
                n = struct.unpack("!I", hdr[3:])[0]
                if n & 0x80000000:  # extended clipboard (negative length)
                    n = (-n) & 0xFFFFFFFF
                s.sendall(t + hdr + recv_exact(c, n))
            elif mt == 150:  # EnableContinuousUpdates
                s.sendall(t + recv_exact(c, 9))
            elif mt == 252:  # xvp
                s.sendall(t + recv_exact(c, 3))
            elif mt == 255:  # QEMU client message
                sub = recv_exact(c, 1)
                if sub[0] == 0:  # QEMU extended key event
                    s.sendall(t + sub + recv_exact(c, 10))
                elif sub[0] == 1:  # QEMU audio
                    op = recv_exact(c, 2)
                    extra = recv_exact(c, 4) if struct.unpack("!H", op)[0] == 2 else b""
                    s.sendall(t + sub + op + extra)
                else:
                    raise ConnectionError(f"unknown QEMU submessage {sub[0]}")
            else:
                raise ConnectionError(f"unknown client message type {mt}")
    except (OSError, ConnectionError):
        pass
    finally:
        for x in (c, s):
            try:
                x.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def handle(c: socket.socket) -> None:
    s = socket.create_connection((SERVER_HOST, SERVER_PORT))
    for sk in (c, s):
        sk.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        # --- handshake: version ---
        c.sendall(recv_exact(s, 12))
        s.sendall(recv_exact(c, 12))
        # --- security (RFB 3.7/3.8 style) ---
        ntypes = recv_exact(s, 1)
        c.sendall(ntypes)
        if ntypes[0] == 0:  # failure
            reason_len = recv_exact(s, 4)
            c.sendall(reason_len + recv_exact(s, struct.unpack("!I", reason_len)[0]))
            return
        types = recv_exact(s, ntypes[0])
        c.sendall(types)
        chosen = recv_exact(c, 1)
        s.sendall(chosen)
        if chosen[0] == 2:  # VNC auth: 16-byte challenge / response
            c.sendall(recv_exact(s, 16))
            s.sendall(recv_exact(c, 16))
        result = recv_exact(s, 4)
        c.sendall(result)
        if struct.unpack("!I", result)[0] != 0:
            reason_len = recv_exact(s, 4)
            c.sendall(reason_len + recv_exact(s, struct.unpack("!I", reason_len)[0]))
            return
        # --- ClientInit / ServerInit ---
        s.sendall(recv_exact(c, 1))
        init = recv_exact(s, 24)
        name = recv_exact(s, struct.unpack("!I", init[20:])[0])
        c.sendall(init[:4] + swap_rb(init[4:20]) + init[20:] + name)
        # --- steady state ---
        t = threading.Thread(target=pump, args=(s, c), daemon=True)
        t.start()
        client_to_server(c, s)
        t.join()
    finally:
        c.close()
        s.close()


def main() -> None:
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", LISTEN_PORT))
    ls.listen(4)
    print(f"R/B-swap VNC proxy: 127.0.0.1:{LISTEN_PORT} -> {SERVER_HOST}:{SERVER_PORT}", flush=True)
    while True:
        c, _ = ls.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
