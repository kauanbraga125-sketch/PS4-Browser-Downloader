// =============================================================================
// shim.cpp — Resolve hidden symbols that don't match between musl libc.a and
//           libSceLibcInternal.so on the PS4.
//
// This file provides wrappers for internal musl symbols that have hidden
// visibility in libc.a but are implemented in PS4 SDK shared objects.
// =============================================================================

#include <arpa/inet.h>

extern "C" {

// musl libc internal — lookup_ipliteral.c calls __inet_aton as a hidden symbol.
// The actual implementation is in libSceLibcInternal.so, inaccessible via a
// hidden reference. We provide a wrapper that calls the public inet_aton symbol.
int __inet_aton(const char* cp, struct in_addr* inp) {
    return inet_aton(cp, inp);
}

}
