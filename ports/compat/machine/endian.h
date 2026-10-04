#ifndef PS4_BROWSER_COMPAT_MACHINE_ENDIAN_H
#define PS4_BROWSER_COMPAT_MACHINE_ENDIAN_H

/*
 * OpenOrbis targets a FreeBSD-flavoured ABI, so third-party code sees
 * __FreeBSD__. Its public sysroot does not provide machine/endian.h.
 * libnsfb only needs the classic byte-order constants, which Clang
 * already exposes reliably for the x86-64 PS4 target.
 */
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN    4321
#define _PDP_ENDIAN    3412

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define _BYTE_ORDER _BIG_ENDIAN
#else
#define _BYTE_ORDER _LITTLE_ENDIAN
#endif

#endif
