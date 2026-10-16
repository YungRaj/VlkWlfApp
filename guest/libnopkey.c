#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

/* 1) Pretend the kernel has no Memory Protection Keys so V8/PartitionAlloc
 *    fall back to mprotect(). Works around PKRU mis-emulation under QEMU TCG. */
int pkey_alloc(unsigned int flags, unsigned int access_rights) { (void)flags; (void)access_rights; errno = ENOSPC; return -1; }
int pkey_mprotect(void *addr, unsigned long len, int prot, int pkey) { (void)addr; (void)len; (void)prot; (void)pkey; errno = EINVAL; return -1; }
int pkey_free(int pkey) { (void)pkey; errno = EINVAL; return -1; }

/* 2) Under QEMU MTTCG a multi-threaded process whose threads span several
 *    vCPUs gets corrupted (parser SIGSEGVs, bogus SyntaxErrors). Pin every
 *    exec'd Electron process to ONE vCPU, chosen round-robin by pid, while it
 *    is still single-threaded. Different processes land on different vCPUs,
 *    so the browser/gpu/renderer/extension-host still run in parallel.
 *    Env: VKWLF_PIN_FIRST (default 1), VKWLF_PIN_COUNT (default 7).      */
__attribute__((constructor)) static void pin_to_single_vcpu(void) {
    const char *e;
    int first = (e = getenv("VKWLF_PIN_FIRST")) ? atoi(e) : 1;
    int count = (e = getenv("VKWLF_PIN_COUNT")) ? atoi(e) : 7;
    if (count <= 0) return;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int cpu = first + (int)(getpid() % count);
    if (ncpu > 0 && cpu >= ncpu) cpu = (int)(ncpu - 1);
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}
