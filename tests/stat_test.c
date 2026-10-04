#include <assert.h>
#include <stdio.h>
#include "../ports/netsurf/ps4_stat.c"
int main(void)
{
    struct ps4_kernel_stat raw = {
        .mode = S_IFREG | 0644, .nlink = 1, .size = 64857, .blocks = 128,
        .uid = 23, .gid = 47, .mtim = {1700000000, 123456789}
    };
    struct stat out;
    int64_t old_size;
    /* Reproduce the old SDK's offset: it saw 128 bytes instead of 64857. */
    memcpy(&old_size, (unsigned char *)&raw + 0x50, sizeof(old_size));
    assert(old_size == 128 && old_size != raw.size);
    ps4_stat_translate(&raw, &out);
    assert(out.st_size == 64857 && out.st_blocks == 128);
    assert(S_ISREG(out.st_mode) && out.st_nlink == 1);
    assert(out.st_uid == 23 && out.st_gid == 47);
    assert(out.st_mtim.tv_sec == 1700000000 && out.st_mtim.tv_nsec == 123456789);
    raw.mode = S_IFDIR | 0755; raw.size = 0;
    ps4_stat_translate(&raw, &out);
    assert(S_ISDIR(out.st_mode) && out.st_size == 0);
    puts("PASS: old stat truncation reproduced; sizes, types and timestamps corrected");
}
