/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*	CoreFoundation_Prefix.h
	Copyright (c) 2005-2009, Apple Inc. All rights reserved.
*/


#define _DARWIN_UNLIMITED_SELECT 1

#include <CoreFoundation/CFBase.h>


#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#if DEPLOYMENT_TARGET_WINDOWS && defined(__cplusplus)
extern "C" {
#endif
    
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS
#include <objc/objc.h>
#endif

#if DEPLOYMENT_TARGET_WINDOWS
#define DISPATCH_HELPER_FUNCTIONS(PREFIX, QNAME)                        
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED

#import <dispatch/dispatch.h>
#import <libkern/OSAtomic.h>
#import <pthread.h>

/* This macro creates 3 helper functions which are useful in dealing with libdispatch:
 *  __ PREFIX SyncDispatchIsSafe  -- returns bool indicating whether calling dispatch_sync() would be safe from self-deadlock
 *  __ PREFIX Queue -- manages and returns a singleton serial queue
 *
 * Use the macro like this:
 *   DISPATCH_HELPER_FUNCTIONS(fh, NSFileHandle)
 */

#define DISPATCH_HELPER_FUNCTIONS(PREFIX, QNAME)			\
static Boolean __ ## PREFIX ## SyncDispatchIsSafe(dispatch_queue_t Q) {	\
    dispatch_queue_t C = dispatch_get_current_queue();			\
    return (!C || Q != C) ? true : false;				\
}									\
									\
static dispatch_queue_t __ ## PREFIX ## Queue(void) {			\
    static volatile dispatch_queue_t __ ## PREFIX ## dq = NULL;		\
    if (!__ ## PREFIX ## dq) {						\
        dispatch_queue_t dq = dispatch_queue_create(# QNAME, NULL);	\
        void * volatile *loc = (void * volatile *)&__ ## PREFIX ## dq;	\
        if (!OSAtomicCompareAndSwapPtrBarrier(NULL, dq, loc)) {		\
            dispatch_release(dq);					\
        }								\
    }									\
    return __ ## PREFIX ## dq;						\
}									\

#endif

#define LIBAUTO_STUB	1

#ifndef LIBAUTO_STUB

#if DEPLOYMENT_TARGET_MACOSX
#include <auto_zone.h>
#endif
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#include <objc/objc-auto.h>
#endif

#endif // LIBAUTO_STUB

#if DEPLOYMENT_TARGET_WINDOWS
// Compatibility with boolean.h
#if defined(__x86_64__)
typedef unsigned int	boolean_t;
#else
typedef int		boolean_t;
#endif
#endif

#if DEPLOYMENT_TARGET_WINDOWS

#define MAXPATHLEN MAX_PATH
#undef MAX_PATH
#undef INVALID_HANDLE_VALUE

// Define MIN/MAX macros from NSObjCRuntime.h, which are only defined for GNUC
#if !defined(__GNUC__)
    
#if !defined(MIN)
#define MIN(A,B)	((A) < (B) ? (A) : (B))
#endif
    
#if !defined(MAX)
#define MAX(A,B)	((A) > (B) ? (A) : (B))
#endif
    
#if !defined(ABS)
#define ABS(A)	((A) < 0 ? (-(A)) : (A))
#endif
    
#endif

// Defined for source compatibility
#define ino_t _ino_t
#define off_t _off_t
#define mode_t uint16_t
        
// This works because things aren't actually exported from the DLL unless they have a __declspec(dllexport) on them... so extern by itself is closest to __private_extern__ on Mac OS
#define __private_extern__ extern
    
#define __builtin_expect(P1,P2) P1
    
// These are replacements for POSIX calls on Windows, ensuring that the UTF8 parameters are converted to UTF16 before being passed to Windows
CF_EXPORT int _NS_stat(const char *name, struct _stat *st);
CF_EXPORT int _NS_mkdir(const char *name);
CF_EXPORT int _NS_rmdir(const char *name);
CF_EXPORT int _NS_chmod(const char *name, int mode);
CF_EXPORT int _NS_unlink(const char *name);
CF_EXPORT char *_NS_getcwd(char *dstbuf, size_t size);     // Warning: this doesn't support dstbuf as null even though 'getcwd' does
CF_EXPORT char *_NS_getenv(const char *name);
CF_EXPORT int _NS_rename(const char *oldName, const char *newName);
CF_EXPORT int _NS_open(const char *name, int oflag, int pmode = 0);
CF_EXPORT int _NS_chdir(const char *name);
CF_EXPORT int _NS_mkstemp(char *name, int bufSize);
CF_EXPORT int _NS_access(const char *name, int amode);

#define BOOL WINDOWS_BOOL

#define WIN32_LEAN_AND_MEAN

#ifndef WINVER
#define WINVER  0x0501
#endif
    
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

// The order of these includes is important
#include <winsock2.h>
#include <windows.h>

#undef BOOL

#ifndef HAVE_STRUCT_TIMESPEC
#define HAVE_STRUCT_TIMESPEC 1
struct timespec {
        long tv_sec;
        long tv_nsec;
};
#endif /* HAVE_STRUCT_TIMESPEC */

#define __PRETTY_FUNCTION__ __FUNCTION__

#define malloc_default_zone() (void *)0
#define malloc_zone_from_ptr(a) (void *)0
#define malloc_zone_malloc(zone,size) malloc(size)
#define malloc_zone_calloc(zone,count,size) calloc(count,size)
#define bcopy(b1,b2,len) memmove(b2, b1, (size_t)(len))
typedef int malloc_zone_t;
typedef int uid_t;
typedef int gid_t;
#define geteuid() 0
#define getuid() 0
#define getegid() 0

#define fsync(a) _commit(a)
#define malloc_create_zone(a,b) 123
#define malloc_set_zone_name(zone,name)
#define malloc_zone_realloc(zone,ptr,size) realloc(ptr,size)
#define malloc_zone_free(zone,ptr) free(ptr)

// implemented in CFInternal.h
#define OSSpinLockLock(A) __CFSpinLock(A)
#define OSSpinLockUnlock(A) __CFSpinUnlock(A)
    
typedef int32_t OSSpinLock;

#define OS_SPINLOCK_INIT       0

#include <stdint.h>
#include <stdbool.h>
//#include <search.h>
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
CF_INLINE size_t malloc_size(void *memblock) {
    return _msize(memblock);
}

#define mach_absolute_time() ((uint64_t)(CFAbsoluteTimeGetCurrent() * 1000000000.0))

extern int pthread_main_np();

#define strtod_l(a,b,locale) strtod(a,b)
#define strtoul_l(a,b,c,locale) strtoul(a,b,c)
#define strtol_l(a,b,c,locale) strtol(a,b,c)

#define _fprintf_l(a,b,locale,...) fprintf(a, b, __VA_ARGS__)

#define strlcat(a,b,c) strncat(a,b,c)

#define issetugid() 0

// CF exports these useful atomic operation functions on Windows
CF_EXPORT bool OSAtomicCompareAndSwapPtr(void *oldp, void *newp, void *volatile *dst);
CF_EXPORT bool OSAtomicCompareAndSwapLong(long oldl, long newl, long volatile *dst);
CF_EXPORT bool OSAtomicCompareAndSwapPtrBarrier(void *oldp, void *newp, void *volatile *dst);

CF_EXPORT int32_t OSAtomicDecrement32Barrier(volatile int32_t *dst);
CF_EXPORT int32_t OSAtomicIncrement32Barrier(volatile int32_t *dst);
CF_EXPORT int32_t OSAtomicIncrement32(volatile int32_t *theValue);
CF_EXPORT int32_t OSAtomicDecrement32(volatile int32_t *theValue);
    
CF_EXPORT int32_t OSAtomicAdd32Barrier( int32_t theAmount, volatile int32_t *theValue );
CF_EXPORT bool OSAtomicCompareAndSwap32Barrier( int32_t oldValue, int32_t newValue, volatile int32_t *theValue );

/*
CF_EXPORT bool OSAtomicCompareAndSwap64( int64_t __oldValue, int64_t __newValue, volatile int64_t *__theValue );
CF_EXPORT bool OSAtomicCompareAndSwap64Barrier( int64_t __oldValue, int64_t __newValue, volatile int64_t *__theValue );
    
CF_EXPORT int64_t OSAtomicAdd64( int64_t __theAmount, volatile int64_t *__theValue );
CF_EXPORT int64_t OSAtomicAdd64Barrier( int64_t __theAmount, volatile int64_t *__theValue );
*/
    
#define sleep(x) Sleep(1000*x)
#define strlcpy strncpy
    
//#ifndef NTDDI_VERSION
//#define NTDDI_VERSION NTDDI_WINXP
//#endif

#include <io.h>
#include <fcntl.h>
#include <errno.h>

static __inline int flsl( long mask ) {
    int idx = 0;
	while (mask != 0) mask = (unsigned long)mask >> 1, idx++;
	return idx;
}

static __inline int asprintf(char **ret, const char *format, ...) {
    va_list args;
    size_t sz = 1024;
    *ret = (char *) malloc(sz * sizeof(char));
    if (!*ret) return -1;
    va_start(args, format);
    int cnt = vsnprintf(*ret, sz, format, args);
    va_end(args);
    if (cnt < sz - 1) return cnt;
    sz = cnt + 8;
    char *oldret = *ret;
    *ret = (char *) realloc(*ret, sz * sizeof(char));
    if (!*ret && oldret) free(oldret);
    if (!*ret) return -1;
    va_start(args, format);
    cnt = vsnprintf(*ret, sz, format, args);
    va_end(args);
    if (cnt < sz - 1) return cnt;
    free(*ret);
    *ret = NULL;
    return -1;
}

#define __unused

#define __weak
#define __strong
    
#endif

#ifdef LIBAUTO_STUB

/* Stubs for functions in libauto. */

enum {OBJC_GENERATIONAL = (1 << 0)};

enum {
    OBJC_RATIO_COLLECTION        = (0 << 0), 
    OBJC_GENERATIONAL_COLLECTION = (1 << 0),
    OBJC_FULL_COLLECTION         = (2 << 0),
    OBJC_EXHAUSTIVE_COLLECTION   = (3 << 0),
    OBJC_COLLECT_IF_NEEDED       = (1 << 3),
    OBJC_WAIT_UNTIL_DONE         = (1 << 4),
};

    
enum { AUTO_TYPE_UNKNOWN = -1, AUTO_UNSCANNED = 1, AUTO_OBJECT = 2, AUTO_MEMORY_SCANNED = 0, AUTO_MEMORY_UNSCANNED = AUTO_UNSCANNED, AUTO_OBJECT_SCANNED = AUTO_OBJECT, AUTO_OBJECT_UNSCANNED = AUTO_OBJECT | AUTO_UNSCANNED };
typedef unsigned long auto_memory_type_t;
typedef struct _auto_zone_t auto_zone_t;
typedef struct auto_weak_callback_block {
    struct auto_weak_callback_block *next;
    void (*callback_function)(void *arg1, void *arg2);
    void *arg1;
    void *arg2;
} auto_weak_callback_block_t;

CF_INLINE void *objc_memmove_collectable(void *a, const void *b, size_t c) { return memmove(a, b, c); }

CF_INLINE auto_zone_t *auto_zone(void) { return 0; }
CF_INLINE void *auto_zone_allocate_object(void *zone, size_t size, auto_memory_type_t type, int rc, int clear) { return 0; }
CF_INLINE const void *auto_zone_base_pointer(void *zone, const void *ptr) { return 0; }
CF_INLINE void auto_zone_retain(void *zone, void *ptr) {}
CF_INLINE unsigned int auto_zone_release(void *zone, void *ptr) { return 0; }
CF_INLINE unsigned int auto_zone_retain_count(void *zone, const void *ptr) { return 0; }
CF_INLINE void auto_zone_set_unscanned(auto_zone_t *zone, void *ptr) {}
CF_INLINE void auto_zone_set_nofinalize(auto_zone_t *zone, void *ptr) {}
CF_INLINE int auto_zone_is_finalized(void *zone, const void *ptr) { return 0; }
CF_INLINE size_t auto_zone_size(void *zone, const void *ptr) { return 0; }
CF_INLINE void auto_register_weak_reference(void *zone, const void *referent, void **referrer, uintptr_t *counter, void **listHead, void **listElement) {}
CF_INLINE void auto_unregister_weak_reference(void *zone, const void *referent, void **referrer) {}
CF_INLINE void auto_zone_register_thread(void *zone) {}
CF_INLINE void auto_zone_unregister_thread(void *zone) {}
CF_INLINE int auto_zone_is_valid_pointer(void *zone, const void *ptr) { return 0; }
CF_INLINE BOOL objc_isAuto(id object) { return 0; }
CF_INLINE void* auto_read_weak_reference(void *zone, void **referrer) { void *result = *referrer; return result; }
CF_INLINE void auto_assign_weak_reference(void *zone, const void *referent, void **referrer, auto_weak_callback_block_t *block) { *referrer = (void *)referent; }
CF_INLINE auto_memory_type_t auto_zone_get_layout_type(void *zone, void *ptr) { return AUTO_UNSCANNED; }
CF_INLINE boolean_t auto_zone_set_write_barrier(auto_zone_t *zone, const void *dest, const void *new_value) { return false; }

CF_INLINE void objc_assertRegisteredThreadWithCollector(void) {}

// from objc-auto.h

static OBJC_INLINE BOOL objc_atomicCompareAndSwapPtr(id predicate, id replacement, volatile id *objectLocation) 
{ return OSAtomicCompareAndSwapPtr((void *)predicate, (void *)replacement, (void * volatile *)objectLocation); }

static OBJC_INLINE BOOL objc_atomicCompareAndSwapPtrBarrier(id predicate, id replacement, volatile id *objectLocation) 
{ return OSAtomicCompareAndSwapPtrBarrier((void *)predicate, (void *)replacement, (void * volatile *)objectLocation); }

static OBJC_INLINE BOOL objc_atomicCompareAndSwapGlobal(id predicate, id replacement, volatile id *objectLocation) 
{ return OSAtomicCompareAndSwapPtr((void *)predicate, (void *)replacement, (void * volatile *)objectLocation); }

static OBJC_INLINE BOOL objc_atomicCompareAndSwapGlobalBarrier(id predicate, id replacement, volatile id *objectLocation) 
{ return OSAtomicCompareAndSwapPtrBarrier((void *)predicate, (void *)replacement, (void * volatile *)objectLocation); }

static OBJC_INLINE BOOL objc_atomicCompareAndSwapInstanceVariable(id predicate, id replacement, volatile id *objectLocation) 
{ return OSAtomicCompareAndSwapPtr((void *)predicate, (void *)replacement, (void * volatile *)objectLocation); }

static OBJC_INLINE BOOL objc_atomicCompareAndSwapInstanceVariableBarrier(id predicate, id replacement, volatile id *objectLocation) 
{ return OSAtomicCompareAndSwapPtrBarrier((void *)predicate, (void *)replacement, (void * volatile *)objectLocation); }

static OBJC_INLINE id objc_assign_strongCast(id val, id *dest) 
{ return (*dest = val); }

static OBJC_INLINE id objc_assign_global(id val, id *dest) 
{ return (*dest = val); }

static OBJC_INLINE id objc_assign_ivar(id val, id dest, ptrdiff_t offset) 
{ return (*(id*)((char *)dest+offset) = val); }

//static OBJC_INLINE void *objc_memmove_collectable(void *dst, const void *src, size_t size) { return memmove(dst, src, size); }

static OBJC_INLINE id objc_read_weak(id *location) 
{ return *location; }

static OBJC_INLINE id objc_assign_weak(id value, id *location) 
{ return (*location = value); }


static OBJC_INLINE void objc_finalizeOnMainThread(Class cls) { }
static OBJC_INLINE BOOL objc_is_finalized(void *ptr) { return NO; }
static OBJC_INLINE void objc_clear_stack(unsigned long options) { }

static OBJC_INLINE BOOL objc_collectingEnabled(void) { return NO; }
static OBJC_INLINE void objc_start_collector_thread(void) { }

static OBJC_INLINE void objc_collect(unsigned long options) { }
    
#endif

// Need to use the _O_BINARY flag on Windows to get the correct behavior
#if DEPLOYMENT_TARGET_WINDOWS
    #define CF_OPENFLGS	(_O_BINARY|_O_NOINHERIT)
#else
    #define CF_OPENFLGS	(0)
#endif


#if DEPLOYMENT_TARGET_WINDOWS && defined(__cplusplus)
} // extern "C"
#endif
