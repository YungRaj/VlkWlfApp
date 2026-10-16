# the IDE renderer crash (`code: 11`) — root cause & fix

## Result

_(screenshot omitted from repo: the IDE workbench in the Android desktop VM, 6 min after launch, after interaction)_

the IDE now renders the full workbench in the VM and stays alive well past the
old 3.5–4 minute crash point (verified >6 min, with mouse interaction, C++
language mode active, `0` segfaults in `dmesg`, no `renderer process gone`).

## What was actually wrong

The crash was **not** V8 bytecode flushing (the earlier theory). Two separate
QEMU‑TCG emulation problems were stacked on top of each other:

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | Renderer `SIGSEGV` at random V8 sites (parser, ThreadPool worker) | Guest CPU advertises `pku`/`ospke`; V8 uses Memory Protection Keys for JIT W^X. TCG's PKRU emulation is unreliable. | `libnopkey.so` (LD_PRELOAD) makes `pkey_alloc()` fail → V8 falls back to `mprotect()` |
| 2 | Main‑process `SyntaxError: Unexpected string` / `Invalid or unexpected token` on *intact* files; SystemUI ART GC crash; full guest reboot | Under MTTCG a **multi‑threaded process whose threads span several vCPUs** gets memory corruption. Any *single* vCPU is fine (proved: whole tree on CPU 1 works; 5× full parse of the 18 MB bundle on CPU 0 and CPU 3 = 0 errors). | Pin each Electron process to **one** vCPU at `exec` time (constructor in `libnopkey.so`, before threads exist), round‑robin over CPUs 1–7. Processes still run in parallel. |

> [!IMPORTANT]
> A third bug was masking everything: the `--js-flags` string began with
> `--v8-pool-size=1`, which is a **Node** flag, not a V8 flag. V8 rejects it
> and *discards every flag after it*, so none of the earlier `--js-flags`
> tweaks (including `--no-flush-bytecode`) had ever taken effect. Removed.

### Why single‑vCPU pinning used to "work"
`taskset 1` on the whole tree hid bug #2 but put browser + GPU + renderer +
2 extension hosts on one vCPU (renderer got ~23%), hence the crawl. Per‑process
pinning keeps the stability and restores parallelism: workbench now reaches
the editor in ~2.5 min instead of never finishing.

## Files (persisted in the repo under `guest/`)

- [libnopkey.c](../guest/libnopkey.c) — pkey stubs + constructor‑time single‑vCPU pin (`VKWLF_PIN_FIRST`/`VKWLF_PIN_COUNT` env knobs)
- [run_linux_app.sh](../guest/run_linux_app.sh) — `LD_PRELOAD=/opt/example-ide/libnopkey.so`, cleaned `--js-flags`
- [linux_host_daemon.sh](../guest/linux_host_daemon.sh) — no longer wipes the profile, no `taskset`, bind‑mount dedupe fix

Deployed on the VM at `/data/local/linux-root/opt/example-ide/libnopkey.so`,
`/data/local/linux-root/usr/local/bin/run_linux_app.sh`, `/data/local/linux_host_daemon.sh`.

## Other things handled along the way
  sign‑in carousel doesn't block the editor (profile had been wiped earlier).
- The guest rebooted once (`reboot` reason, no panic record) and SystemUI
  crashed once — both during the un‑pinned runs; both are bug #2 hitting
  Android itself, not VkWlfApp.
- Restored user 10 / daemon / mounts after the reboot.

> [!NOTE]
> Environment, not code: nothing in `VkWlfApp/src` needed to change. The
> compositor's zero‑copy `AHardwareBuffer` path (`shm_uploads=0`) is unchanged.

## Not done / open
- The in‑window VS Code custom title bar duplicates Android's (`window.titleBarStyle: custom`). Switch to `native` if you'd rather not see both.
- `Network service crashed, restarting service.` still appears once at startup in every run; it self‑heals and doesn't affect the window.

---

# Part 2 — viewer, colours, and the guest resets

_(screenshot omitted from repo: the IDE in the VM through TigerVNC, correct colours, 15 min uptime, 0 crashes)_

## Viewer
- The cuttlefish QEMU (10.2.90) is built with only `none`/`egl-headless` display backends — **no GTK/SDL/SPICE**, so a native QEMU window is not possible with this binary. Its built‑in VNC server (`127.0.0.1:6444`) is the only display path.
- The "flaky VNC" was our hand‑rolled Python client. Replaced by TigerVNC (already installed).

## Colours (red/blue swapped)
- QEMU mislabels the guest's `AB24` (RGBA‑in‑memory) virtio‑gpu scanout; its own `screendump` is already swapped, so no viewer setting can fix it. The old Python client had been silently un‑swapping.
- The guest `display_framebuffer_format=bgra` bootconfig knob is ignored by this composer (scanout stayed `AB24`) — dead end, reverted.
- Fix: [vnc_rbswap_proxy.py](../tools/vnc_rbswap_proxy.py) rewrites only the two pixel‑format descriptors on the wire (ServerInit, SetPixelFormat); QEMU then converts into the "swapped" layout, cancelling its error. Must use a **non‑JPEG** encoding (QEMU's Tight‑JPEG path ignores the negotiated format).
- Always open the VM with [vm_view.sh](../tools/vm_view.sh) — it starts the proxy if needed and connects via `6445`. Connecting to `6444` directly brings the swap back (that is what happened after the VM relaunch).

## Guest resets / "random" crashes — real root cause
With kernel console logging enabled, the next reset left an oops:
`vma_interval_tree_insert_after` faulting on `RAX=0x5d3236303038352e` = ASCII **`".5800062]"`** — a printk timestamp sitting inside kernel VMA structures. Under QEMU MTTCG, heavy virtio‑console (hvc0) traffic corrupts guest memory. That explains the silent reboots, the SystemUI/ART crash, and the burst of V8 crashes right after every reboot (post‑boot load → more console/log traffic).

> [!IMPORTANT]
> Keep `printk` at `0` in the guest (the daemon does this) and **do not launch the IDE until the post‑boot load has settled** (`/proc/loadavg` < ~4, 3–4 min after boot). Under those conditions the IDE ran 15+ min with zero faults.

Tried and ruled out along the way: `-cpu Nehalem-v2` (Android userspace needs x86‑64‑v3 → `init` SIGILL boot loop), `-cpu Skylake-Client` minus PKU/AVX‑512/XSAVES (no effect), disabling AVX/BMI in V8 codegen (no effect).

## Current configuration
| Layer | Setting | Where |
|---|---|---|
| QEMU | `-cpu max`, `-accel tcg,thread=multi`, VNC on 6444 | [qemu_wrapper.py](../tools/qemu_wrapper.py) (deployed at `/tmp/cf_home/bin/x86_64-linux-gnu/qemu/qemu-system-x86_64`) |
| Guest kernel | `printk=0` | [linux_host_daemon.sh](../guest/linux_host_daemon.sh) |
| Electron procs | pkeys disabled, one vCPU per process (constructor pin) | [libnopkey.c](../guest/libnopkey.c) |
| V8 | no bytecode flushing, interpreter‑only JS (`--no-turbofan --no-maglev --no-sparkplug`, regexp interpreter), WASM kept | [run_linux_app.sh](../guest/run_linux_app.sh) |
| Viewer | TigerVNC via R/B‑swap proxy, ZRLE, no JPEG | [vm_view.sh](../tools/vm_view.sh) |

> [!NOTE]
> The interpreter‑only V8 flags were added as a belt‑and‑braces measure during the hunt; the console‑traffic finding came after. They cost JS throughput. Worth re‑enabling Sparkplug/Maglev in a later session and re‑checking stability.

## Relaunch recipe (after a VM restart)
```bash
# host
tools/vm_view.sh
# guest (adb root): switch user, start daemon, start app, wait for load < 4, then trigger
adb shell 'am switch-user 10'
adb shell 'nohup sh /data/local/linux_host_daemon.sh >/dev/null 2>&1 </dev/null &'
adb shell 'am start --user 10 -n com.google.example.vkwlf/.MainActivity'
adb shell 'date +%s > /data/user/10/com.google.example.vkwlf/cache/launch_linux_app.trigger'
```
