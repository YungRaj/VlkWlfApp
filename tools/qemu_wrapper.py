#!/usr/bin/env python3
import os, sys

real_bin = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qemu-system-x86_64.real")
args = sys.argv[1:]
new_args = []
i = 0
while i < len(args):
    a = args[i]
    nxt = args[i+1] if i + 1 < len(args) else ""
    if a == "-accel" and nxt == "kvm":
        new_args.extend(["-accel", "tcg,thread=multi"])
        i += 2
        continue
    if a == "-cpu" and nxt.startswith("host"):
        new_args.extend(["-cpu", "max"])
        i += 2
        continue
    if a == "-netdev" and nxt.startswith("tap,id="):
        net_id = nxt.split("id=")[1].split(",")[0]
        new_args.extend(["-netdev", f"user,id={net_id}"])
        i += 2
        continue
    if a == "-chardev" and ("id=switches0" in nxt or "id=rotary0" in nxt):
        i += 2
        continue
    if a == "-device" and nxt.startswith("vhost-user-input-pci"):
        i += 2
        continue
    if a == "-device" and nxt.startswith("vhost-vsock-pci"):
        cid = "3"
        if "guest-cid=" in nxt:
            cid = nxt.split("guest-cid=")[1].split(",")[0]
        uid = os.getuid()
        vhost_sock = f"/tmp/vsock_{cid}_{uid}/vhost.socket"
        new_args.extend([
            "-chardev", f"socket,id=char-vsock0,path={vhost_sock}",
            "-device", "vhost-user-vsock-pci-non-transitional,chardev=char-vsock0"
        ])
        i += 2
        continue
    new_args.append(a)
    i += 1

os.execv(real_bin, [real_bin] + new_args)
