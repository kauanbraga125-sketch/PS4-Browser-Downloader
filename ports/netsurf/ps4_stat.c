/* SPDX-License-Identifier: GPL-2.0-only
 * The OpenOrbis 0.5.4 release asset declares mode_t as 32 bits while the
 * kernel writes a 16-bit mode. Never feed those raw bytes to struct stat.
 * See OpenOrbis PR #278 and orbis-ports/orbis-compat/include/orbis_stat.h.
 * Translate by field; do not change SDK headers or prebuilt library ABIs.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

struct ps4_kernel_timespec { int64_t sec, nsec; };
struct ps4_kernel_stat {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    struct ps4_kernel_timespec atim, mtim, ctim;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    struct ps4_kernel_timespec birthtim;
};
_Static_assert(offsetof(struct ps4_kernel_stat, size) == 0x48, "kernel stat size offset");
_Static_assert(offsetof(struct ps4_kernel_stat, blocks) == 0x50, "kernel stat blocks offset");
_Static_assert(sizeof(struct ps4_kernel_stat) == 120, "kernel stat size");

static void ps4_stat_translate(const struct ps4_kernel_stat *raw, struct stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = raw->dev;
    out->st_ino = raw->ino;
    out->st_mode = raw->mode;
    out->st_nlink = raw->nlink;
    out->st_uid = raw->uid;
    out->st_gid = raw->gid;
    out->st_rdev = raw->rdev;
    out->st_atim.tv_sec = raw->atim.sec;
    out->st_atim.tv_nsec = raw->atim.nsec;
    out->st_mtim.tv_sec = raw->mtim.sec;
    out->st_mtim.tv_nsec = raw->mtim.nsec;
    out->st_ctim.tv_sec = raw->ctim.sec;
    out->st_ctim.tv_nsec = raw->ctim.nsec;
    out->st_size = raw->size;
    out->st_blocks = raw->blocks;
    out->st_blksize = raw->blksize;
#ifdef ORBIS
    out->st_flags = raw->flags;
    out->st_gen = raw->gen;
    out->st_birthtim.tv_sec = raw->birthtim.sec;
    out->st_birthtim.tv_nsec = raw->birthtim.nsec;
#endif
}

#ifdef ORBIS
/* Declare the kernel ABI directly: the release SDK aliases the wrong type. */
extern int sceKernelStat(const char *, void *);
extern int sceKernelFstat(int, void *);

static int ps4_stat_result(int ret, const struct ps4_kernel_stat *raw, struct stat *out)
{
    if (ret < 0) {
        errno = ((uint32_t)ret >> 16) == 0x8002 ? (ret & 0xffff) : EIO;
        return -1;
    }
    ps4_stat_translate(raw, out);
    return 0;
}

int stat(const char *path, struct stat *out)
{
    union { struct ps4_kernel_stat st; unsigned char reserve[256]; } raw = {0};
    if (path == NULL || out == NULL) { errno = EFAULT; return -1; }
    return ps4_stat_result(sceKernelStat(path, &raw), &raw.st, out);
}

int fstat(int fd, struct stat *out)
{
    union { struct ps4_kernel_stat st; unsigned char reserve[256]; } raw = {0};
    if (out == NULL) { errno = EFAULT; return -1; }
    return ps4_stat_result(sceKernelFstat(fd, &raw), &raw.st, out);
}
#endif
