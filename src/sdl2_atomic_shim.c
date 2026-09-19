/*
 * MSVC ARM64 only exposes the _Interlocked* intrinsics with _acq/_rel/_nf
 * suffixes (plus pointer variants); the plain, unsuffixed 32-bit forms that
 * SDL2's MSVC code uses are NOT defined on ARM64. That makes SDL2's shared DLL
 * fail to link for arm64-windows.
 *
 * This TU provides those missing "_Interlocked*" symbols on ARM64 by forwarding
 * to the Win32 Interlocked* functions exported by kernel32. Declaring them as
 * __declspec(dllimport) (without including <intrin.h>) makes the compiler emit
 * __imp_Interlocked* calls that kernel32.lib resolves.
 *
 * On x86/x64 these symbols are provided by the CRT/compiler intrinsics, so this
 * file compiles to nothing (guarded below) and is harmless if added everywhere.
 */
#if defined(_M_ARM64)

#define TB_DLLIMPORT __declspec(dllimport)

TB_DLLIMPORT long    InterlockedCompareExchange(volatile long *p, long e, long c);
TB_DLLIMPORT long    InterlockedExchange(volatile long *p, long v);
TB_DLLIMPORT long    InterlockedExchangeAdd(volatile long *p, long v);
TB_DLLIMPORT long    InterlockedOr(volatile long *p, long v);
TB_DLLIMPORT long    InterlockedIncrement(volatile long *p);
TB_DLLIMPORT long    InterlockedDecrement(volatile long *p);
TB_DLLIMPORT void*   InterlockedCompareExchangePointer(void *volatile *p, void *e, void *c);
TB_DLLIMPORT void*   InterlockedExchangePointer(void *volatile *p, void *v);

long  _InterlockedCompareExchange(volatile long *p, long e, long c) {
    return InterlockedCompareExchange(p, e, c);
}
void* _InterlockedCompareExchangePointer(void *volatile *p, void *e, void *c) {
    return InterlockedCompareExchangePointer(p, e, c);
}
long  _InterlockedExchange(volatile long *p, long v) {
    return InterlockedExchange(p, v);
}
long  _InterlockedExchangeAdd(volatile long *p, long v) {
    return InterlockedExchangeAdd(p, v);
}
long  _InterlockedOr(volatile long *p, long v) {
    return InterlockedOr(p, v);
}
long  _InterlockedIncrement(volatile long *p) {
    return InterlockedIncrement(p);
}
long  _InterlockedDecrement(volatile long *p) {
    return InterlockedDecrement(p);
}
void* _InterlockedExchangePointer(void *volatile *p, void *v) {
    return InterlockedExchangePointer(p, v);
}
/* _acq/_rel spin-lock forms are not exported by kernel32; use seq_cst (safer). */
long  _InterlockedExchange_acq(volatile long *p, long v) {
    return InterlockedExchange(p, v);
}
long  _InterlockedExchange_rel(volatile long *p, long v) {
    return InterlockedExchange(p, v);
}

#endif /* _M_ARM64 */
