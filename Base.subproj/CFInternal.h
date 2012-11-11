/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
/*	CFInternal.h
	Copyright (c) 1998-2005, Apple, Inc. All rights reserved.
*/

/*
        NOT TO BE USED OUTSIDE CF!
*/

#if !CF_BUILDING_CF
    #error The header file CFInternal.h is for the exclusive use of CoreFoundation. No other project should include it.
#endif

#if !defined(__COREFOUNDATION_CFINTERNAL__)
#define __COREFOUNDATION_CFINTERNAL__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFRunLoop.h>
#include "CFRuntime.h"
#if defined(__MACH__)
#include <xlocale.h>
#include <mach/thread_switch.h>
#include <mach/mach_time.h>
#endif
#if defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
#include <sys/time.h>
#include <pthread.h>
#endif
#include <limits.h>
#include <pthread.h>
#include "auto_stubs.h"
#include <libkern/OSAtomic.h>


#if defined(__MACH__)
#if defined(__ppc__)
// This hack is in here because B&I kernel does not set up comm page with Tiger additions yet.
#define AtomicCompareAndSwap(mem, old, new) ({bool result; if (((void **)OSMemoryBarrier)[1] == 0x0) {result = ((*(mem) == (old)) ? (*(mem) = (new), 1) : 0);} else {result = OSAtomicCompareAndSwap32(old, new, (int32_t *)mem); OSMemoryBarrier();} result;})
#define AtomicAdd32(mem, val) ({if (((void **)OSMemoryBarrier)[1] == 0x0) {*(mem) += (val);} else {OSAtomicAdd32(val, mem); OSMemoryBarrier();} 0;})
#else
#define AtomicCompareAndSwap(mem, old, new) ({bool result; result = OSAtomicCompareAndSwap32(old, new, (int32_t *)mem); OSMemoryBarrier(); result;})
#define AtomicAdd32(mem, val) ({OSAtomicAdd32(val, mem); OSMemoryBarrier(); 0;})
#endif
#endif

#include "ForFoundationOnly.h"

#if !defined(__MACH__)
#define __private_extern__
#endif

CF_EXPORT char **_CFArgv(void);
CF_EXPORT int _CFArgc(void);

CF_EXPORT const char *_CFProcessName(void);
CF_EXPORT CFStringRef _CFProcessNameString(void);

CF_EXPORT Boolean _CFIsCFM(void);

CF_EXPORT Boolean _CFGetCurrentDirectory(char *path, int maxlen);

CF_EXPORT CFStringRef _CFGetUserName(void);
CF_EXPORT CFStringRef _CFStringCreateHostName(void);

CF_EXPORT void __CFSetNastyFile(CFTypeRef cf);

CF_EXPORT void _CFMachPortInstallNotifyPort(CFRunLoopRef rl, CFStringRef mode);

#if defined(__ppc__) || defined(__ppc64__)
    #define HALT asm __volatile__("trap")
#elif defined(__i386__)
    #if defined(__GNUC__)
        #define HALT asm __volatile__("int3")
    #elif defined(_MSC_VER) || defined(__MWERKS__)
        #define HALT __asm int 3;
    #else
        #error Compiler not supported
    #endif
#endif

#if defined(DEBUG)
    #define __CFAssert(cond, prio, desc, a1, a2, a3, a4, a5)	\
	do {			\
	    if (!(cond)) {	\
		CFLog(prio, CFSTR(desc), a1, a2, a3, a4, a5); \
		/* HALT; */		\
	    }			\
	} while (0)
#else
    #define __CFAssert(cond, prio, desc, a1, a2, a3, a4, a5)	\
	do {} while (0)
#endif

#define CFAssert(condition, priority, description)			\
    __CFAssert((condition), (priority), description, 0, 0, 0, 0, 0)
#define CFAssert1(condition, priority, description, a1)			\
    __CFAssert((condition), (priority), description, (a1), 0, 0, 0, 0)
#define CFAssert2(condition, priority, description, a1, a2)		\
    __CFAssert((condition), (priority), description, (a1), (a2), 0, 0, 0)
#define CFAssert3(condition, priority, description, a1, a2, a3)		\
    __CFAssert((condition), (priority), description, (a1), (a2), (a3), 0, 0)
#define CFAssert4(condition, priority, description, a1, a2, a3, a4)	\
    __CFAssert((condition), (priority), description, (a1), (a2), (a3), (a4), 0)

#define __kCFLogAssertion	15

#if defined(DEBUG)
extern void __CFGenericValidateType_(CFTypeRef cf, CFTypeID type, const char *func);
#define __CFGenericValidateType(cf, type) __CFGenericValidateType_(cf, type, __PRETTY_FUNCTION__)
#else
#define __CFGenericValidateType(cf, type) 
#endif


/* Bit manipulation macros */
/* Bits are numbered from 31 on left to 0 on right */
/* May or may not work if you use them on bitfields in types other than UInt32, bitfields the full width of a UInt32, or anything else for which they were not designed. */
/* In the following, N1 and N2 specify an inclusive range N2..N1 with N1 >= N2 */
#define __CFBitfieldMask(N1, N2)	((((UInt32)~0UL) << (31UL - (N1) + (N2))) >> (31UL - N1))
#define __CFBitfieldGetValue(V, N1, N2)	(((V) & __CFBitfieldMask(N1, N2)) >> (N2))
#define __CFBitfieldSetValue(V, N1, N2, X)	((V) = ((V) & ~__CFBitfieldMask(N1, N2)) | (((X) << (N2)) & __CFBitfieldMask(N1, N2)))
#define __CFBitfieldMaxValue(N1, N2)	__CFBitfieldGetValue(0xFFFFFFFFUL, (N1), (N2))

#define __CFBitIsSet(V, N)  (((V) & (1UL << (N))) != 0)
#define __CFBitSet(V, N)  ((V) |= (1UL << (N)))
#define __CFBitClear(V, N)  ((V) &= ~(1UL << (N)))

typedef struct ___CFThreadSpecificData {
    void *_unused1;
    void *_allocator;
    void *_runLoop;
    int _runLoop_pid;
// If you add things to this struct, add cleanup to __CFFinalizeThreadData()
} __CFThreadSpecificData;

extern __CFThreadSpecificData *__CFGetThreadSpecificData(void);
__private_extern__ void __CFFinalizeThreadData(void *arg);

#if defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
extern pthread_key_t __CFTSDKey;
#endif
#if defined(__WIN32__)
extern DWORD __CFTSDKey;
#endif

//extern void *pthread_getspecific(pthread_key_t key);

CF_INLINE __CFThreadSpecificData *__CFGetThreadSpecificData_inline(void) {
#if defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
    __CFThreadSpecificData *data = pthread_getspecific(__CFTSDKey);
    return data ? data : __CFGetThreadSpecificData();
#elif defined(__WIN32__)
    __CFThreadSpecificData *data = TlsGetValue(__CFTSDKey);
    return data ? data : __CFGetThreadSpecificData();
#endif
}

CF_EXPORT void		CFLog(int p, CFStringRef str, ...);

#define __kCFAllocatorTypeID_CONST	2

CF_INLINE CFAllocatorRef __CFGetDefaultAllocator(void) {
    CFAllocatorRef allocator = __CFGetThreadSpecificData_inline()->_allocator;
    if (NULL == allocator) {
	allocator = kCFAllocatorSystemDefault;
    }
    return allocator;
}

extern CFTypeID __CFGenericTypeID(const void *cf);

// This should only be used in CF types, not toll-free bridged objects!
// It should not be used with CFAllocator arguments!
// Use CFGetAllocator() in the general case, and this inline function in a few limited (but often called) situations.
CF_INLINE CFAllocatorRef __CFGetAllocator(CFTypeRef cf) {	// !!! Use with CF types only, and NOT WITH CFAllocator!
    CFAssert1(__kCFAllocatorTypeID_CONST != __CFGenericTypeID(cf), __kCFLogAssertion, "__CFGetAllocator(): CFAllocator argument", cf);
    if (__builtin_expect(__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 7, 7), 1)) {
	return kCFAllocatorSystemDefault;
    }
    return *(CFAllocatorRef *)((char *)cf - sizeof(CFAllocatorRef));
}

// Don't define a __CFGetCurrentRunLoop(), because even internal clients should go through the real one


#if !defined(LLONG_MAX)
    #if defined(_I64_MAX)
	#define LLONG_MAX	_I64_MAX
    #else
	#warning Arbitrarily defining LLONG_MAX
       #define LLONG_MAX	(int64_t)9223372036854775807
    #endif
#endif /* !defined(LLONG_MAX) */

#if !defined(LLONG_MIN)
    #if defined(_I64_MIN)
	#define LLONG_MIN	_I64_MIN
    #else
	#warning Arbitrarily defining LLONG_MIN
	#define LLONG_MIN	(-LLONG_MAX - (int64_t)1)
    #endif
#endif /* !defined(LLONG_MIN) */

#if defined(__GNUC__) && !defined(__STRICT_ANSI__)
    #define __CFMin(A,B) ({__typeof__(A) __a = (A); __typeof__(B) __b = (B); __a < __b ? __a : __b; })
    #define __CFMax(A,B) ({__typeof__(A) __a = (A); __typeof__(B) __b = (B); __a < __b ? __b : __a; })
#else /* __GNUC__ */
    #define __CFMin(A,B) ((A) < (B) ? (A) : (B))
    #define __CFMax(A,B) ((A) > (B) ? (A) : (B))
#endif /* __GNUC__ */

/* Secret CFAllocator hint bits */
#define __kCFAllocatorTempMemory	0x2
#define __kCFAllocatorNoPointers	0x10
#define __kCFAllocatorDoNotRecordEvent	0x100

CF_EXPORT CFAllocatorRef _CFTemporaryMemoryAllocator(void);

extern SInt64 __CFTimeIntervalToTSR(CFTimeInterval ti);
extern CFTimeInterval __CFTSRToTimeInterval(SInt64 tsr);

extern CFStringRef __CFCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions);

/* result is long long or int, depending on doLonglong
*/
extern Boolean __CFStringScanInteger(CFStringInlineBuffer *buf, CFDictionaryRef locale, SInt32 *indexPtr, Boolean doLonglong, void *result);
extern Boolean __CFStringScanDouble(CFStringInlineBuffer *buf, CFDictionaryRef locale, SInt32 *indexPtr, double *resultPtr); 
extern Boolean __CFStringScanHex(CFStringInlineBuffer *buf, SInt32 *indexPtr, unsigned *result);


#ifdef __CONSTANT_CFSTRINGS__
#define CONST_STRING_DECL(S, V) const CFStringRef S = __builtin___CFStringMakeConstantString(V);
#else

struct CF_CONST_STRING {
    CFRuntimeBase _base;
    uint8_t *_ptr;
    uint32_t _length;
};

extern int __CFConstantStringClassReference[];

/* CFNetwork also has a copy of the CONST_STRING_DECL macro (for use on platforms without constant string support in cc); please warn cfnetwork-core@group.apple.com of any necessary changes to this macro. -- REW, 1/28/2002 */
#if defined(__ppc__) || defined(__ppc64__)
#define CONST_STRING_DECL(S, V)			\
static struct CF_CONST_STRING __ ## S ## __ = {{&__CFConstantStringClassReference, 0x0000, 0x07c8}, V, sizeof(V) - 1}; \
const CFStringRef S = (CFStringRef) & __ ## S ## __;
#elif defined(__i386__)
#define CONST_STRING_DECL(S, V)			\
static struct CF_CONST_STRING __ ## S ## __ = {{&__CFConstantStringClassReference, 0x07c8, 0x0000}, V, sizeof(V) - 1}; \
const CFStringRef S = (CFStringRef) & __ ## S ## __;
#else
#error undefined architecture
#endif
#endif

#if defined(__MACH__)
#define __kCFCharacterSetDir "/System/Library/CoreServices"
#elif defined(__LINUX__) || defined(__FREEBSD__)
#define __kCFCharacterSetDir "/usr/local/share/CoreFoundation"
#elif defined(__WIN32__)
#define __kCFCharacterSetDir "\\Windows\\CoreFoundation"
#endif


/* Buffer size for file pathname */
#if defined(__WIN32__)
    #define CFMaxPathSize ((CFIndex)262)
    #define CFMaxPathLength ((CFIndex)260)
#else
    #define CFMaxPathSize ((CFIndex)1026)
    #define CFMaxPathLength ((CFIndex)1024)
#endif

#if defined(__MACH__)
extern bool __CFOASafe;
extern void __CFSetLastAllocationEventName(void *ptr, const char *classname);
#else
#define __CFOASafe 0
#define __CFSetLastAllocationEventName(a, b)
#endif


CF_EXPORT CFStringRef _CFCreateLimitedUniqueString(void);

extern CFStringRef __CFCopyEthernetAddrString(void);

/* Comparators are passed the address of the values; this is somewhat different than CFComparatorFunction is used in public API usually. */
CF_EXPORT CFIndex	CFBSearch(const void *element, CFIndex elementSize, const void *list, CFIndex count, CFComparatorFunction comparator, void *context);

CF_EXPORT CFHashCode	CFHashBytes(UInt8 *bytes, CFIndex length);

CF_EXPORT CFStringEncoding CFStringFileSystemEncoding(void);

CF_EXPORT CFStringRef __CFStringCreateImmutableFunnel3(CFAllocatorRef alloc, const void *bytes, CFIndex numBytes, CFStringEncoding encoding, Boolean possiblyExternalFormat, Boolean tryToReduceUnicode, Boolean hasLengthByte, Boolean hasNullByte, Boolean noCopy, CFAllocatorRef contentsDeallocator, UInt32 converterFlags);

extern const void *__CFTypeCollectionRetain(CFAllocatorRef allocator, const void *ptr);
extern void __CFTypeCollectionRelease(CFAllocatorRef allocator, const void *ptr);


#if defined(__MACH__)

typedef OSSpinLock CFSpinLock_t;

#define CFSpinLockInit OS_SPINLOCK_INIT

#if defined(__i386__)
extern void _spin_lock(CFSpinLock_t *lockp);
extern void _spin_unlock(CFSpinLock_t *lockp);
#define OSSpinLockLock(p) _spin_lock(p)
#define OSSpinLockUnlock(p) _spin_unlock(p)
#endif

CF_INLINE void __CFSpinLock(CFSpinLock_t *lockp) {
    OSSpinLockLock(lockp);
}

CF_INLINE void __CFSpinUnlock(CFSpinLock_t *lockp) {
    OSSpinLockUnlock(lockp);
}

#elif defined(__WIN32__)

typedef LONG CFSpinLock_t;

CF_INLINE void __CFSpinLock(CFSpinLock_t *slock) {
    while (InterlockedExchange(slock, 1) != 0) {
        Sleep(1);   // 1ms
    }
}

CF_INLINE void __CFSpinUnlock(CFSpinLock_t *lock) {
    *lock = 0;
}

#else

#warning CF spin locks not defined for this platform -- CF is not thread-safe
#define __CFSpinLock(A)		do {} while (0)
#define __CFSpinUnlock(A)	do {} while (0)

#endif

#if defined(__svr4__) || defined(__hpux__) || defined(__WIN32__)
#include <errno.h>
#elif defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
#include <sys/errno.h>
#endif

#define thread_errno() errno
#define thread_set_errno(V) do {errno = (V);} while (0)

extern void *__CFStartSimpleThread(void *func, void *arg);

/* ==================== Simple file access ==================== */
/* For dealing with abstract types.  MF:!!! These ought to be somewhere else and public. */
    
CF_EXPORT CFStringRef _CFCopyExtensionForAbstractType(CFStringRef abstractType);

/* ==================== Simple file access ==================== */
/* These functions all act on a c-strings which must be in the file system encoding. */
    
CF_EXPORT Boolean _CFCreateDirectory(const char *path);
CF_EXPORT Boolean _CFRemoveDirectory(const char *path);
CF_EXPORT Boolean _CFDeleteFile(const char *path);

CF_EXPORT Boolean _CFReadBytesFromFile(CFAllocatorRef alloc, CFURLRef url, void **bytes, CFIndex *length, CFIndex maxLength);
    /* resulting bytes are allocated from alloc which MUST be non-NULL. */
    /* maxLength of zero means the whole file.  Otherwise it sets a limit on the number of bytes read. */

CF_EXPORT Boolean _CFWriteBytesToFile(CFURLRef url, const void *bytes, CFIndex length);
#if defined(__MACH__)
CF_EXPORT Boolean _CFWriteBytesToFileWithAtomicity(CFURLRef url, const void *bytes, unsigned int length, SInt32 mode, Boolean atomic);
#endif

CF_EXPORT CFMutableArrayRef _CFContentsOfDirectory(CFAllocatorRef alloc, char *dirPath, void *dirSpec, CFURLRef dirURL, CFStringRef matchingAbstractType);
    /* On Mac OS 8/9, one of dirSpec, dirPath and dirURL must be non-NULL */
    /* On all other platforms, one of path and dirURL must be non-NULL */
    /* If both are present, they are assumed to be in-synch; that is, they both refer to the same directory.  */
    /* alloc may be NULL */
    /* return value is CFArray of CFURLs */

CF_EXPORT SInt32 _CFGetFileProperties(CFAllocatorRef alloc, CFURLRef pathURL, Boolean *exists, SInt32 *posixMode, SInt64 *size, CFDateRef *modTime, SInt32 *ownerID, CFArrayRef *dirContents);
    /* alloc may be NULL */
    /* any of exists, posixMode, size, modTime, and dirContents can be NULL.  Usually it is not a good idea to pass NULL for exists, since interpretting the other values sometimes requires that you know whether the file existed or not.  Except for dirContents, it is pretty cheap to compute any of these things as loing as one of them must be computed. */


/* ==================== Simple path manipulation ==================== */
/* These functions all act on a UniChar buffers. */

CF_EXPORT Boolean _CFIsAbsolutePath(UniChar *unichars, CFIndex length);
CF_EXPORT Boolean _CFStripTrailingPathSlashes(UniChar *unichars, CFIndex *length);
CF_EXPORT Boolean _CFAppendPathComponent(UniChar *unichars, CFIndex *length, CFIndex maxLength, UniChar *component, CFIndex componentLength);
CF_EXPORT Boolean _CFAppendPathExtension(UniChar *unichars, CFIndex *length, CFIndex maxLength, UniChar *extension, CFIndex extensionLength);
CF_EXPORT Boolean _CFTransmutePathSlashes(UniChar *unichars, CFIndex *length, UniChar replSlash);
CF_EXPORT CFIndex _CFStartOfLastPathComponent(UniChar *unichars, CFIndex length);
CF_EXPORT CFIndex _CFLengthAfterDeletingLastPathComponent(UniChar *unichars, CFIndex length);
CF_EXPORT CFIndex _CFStartOfPathExtension(UniChar *unichars, CFIndex length);
CF_EXPORT CFIndex _CFLengthAfterDeletingPathExtension(UniChar *unichars, CFIndex length);

#if !defined(__MACH__)

#define CF_IS_OBJC(typeID, obj)	(false)

#define CF_OBJC_VOIDCALL0(obj, sel)
#define CF_OBJC_VOIDCALL1(obj, sel, a1)
#define CF_OBJC_VOIDCALL2(obj, sel, a1, a2)

#define CF_OBJC_CALL0(rettype, retvar, obj, sel)
#define CF_OBJC_CALL1(rettype, retvar, obj, sel, a1)
#define CF_OBJC_CALL2(rettype, retvar, obj, sel, a1, a2)

#define CF_OBJC_FUNCDISPATCH0(typeID, rettype, obj, sel)
#define CF_OBJC_FUNCDISPATCH1(typeID, rettype, obj, sel, a1)
#define CF_OBJC_FUNCDISPATCH2(typeID, rettype, obj, sel, a1, a2)
#define CF_OBJC_FUNCDISPATCH3(typeID, rettype, obj, sel, a1, a2, a3)
#define CF_OBJC_FUNCDISPATCH4(typeID, rettype, obj, sel, a1, a2, a3, a4)
#define CF_OBJC_FUNCDISPATCH5(typeID, rettype, obj, sel, a1, a2, a3, a4, a5)

#endif

#if defined(__LINUX__) || defined(__FREEBSD__) || defined(__WIN32__)
#define __CFISAForTypeID(x) (NULL)
#endif

#define __CFMaxRuntimeTypes	256

#if defined(__MACH__)

#include <objc/objc-class.h>
	
extern struct objc_class *__CFRuntimeObjCClassTable[];
CF_INLINE void *__CFISAForTypeID(CFTypeID typeID) {
    return (void *)(__CFRuntimeObjCClassTable[typeID]);
}

#if defined(__ppc__)
#define __CFSendObjCMsg 0xfffeff00
#else
extern void * (*__CFSendObjCMsg)(const void *, SEL, ...);
#endif

#if 0
// Although it might seem to make better performance to check for NULL
// first, doing the other check first is better.
CF_INLINE int CF_IS_OBJC(CFTypeID typeID, const void *obj) {
    return (((CFRuntimeBase *)obj)->_isa != __CFISAForTypeID(typeID) && ((CFRuntimeBase *)obj)->_isa > (void *)0xFFF);
}
#else
#define CF_IS_OBJC(typeID, obj) (false)
#endif

// Invoke an ObjC method that returns void.
// Assumes CF_IS_OBJC has already been checked.
#define CF_OBJC_VOIDCALL0(obj, sel) \
        {void (*func)(const void *, SEL) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = sel_registerName(sel); \
        func((const void *)obj, s);}
#define CF_OBJC_VOIDCALL1(obj, sel, a1) \
        {void (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = sel_registerName(sel); \
        func((const void *)obj, s, (a1));}
#define CF_OBJC_VOIDCALL2(obj, sel, a1, a2) \
        {void (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = sel_registerName(sel); \
        func((const void *)obj, s, (a1), (a2));}


// Invoke an ObjC method, leaving the result in "retvar".
// Assumes CF_IS_OBJC has already been checked.
#define CF_OBJC_CALL0(rettype, retvar, obj, sel) \
        {rettype (*func)(const void *, SEL) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = sel_registerName(sel); \
        retvar = func((const void *)obj, s);}
#define CF_OBJC_CALL1(rettype, retvar, obj, sel, a1) \
        {rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = sel_registerName(sel); \
        retvar = func((const void *)obj, s, (a1));}
#define CF_OBJC_CALL2(rettype, retvar, obj, sel, a1, a2) \
        {rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = sel_registerName(sel); \
        retvar = func((const void *)obj, s, (a1), (a2));}

// Invoke an ObjC method, return the result
#define CF_OBJC_FUNCDISPATCH0(typeID, rettype, obj, sel) \
	if (__builtin_expect(CF_IS_OBJC(typeID, obj), 0)) \
	{rettype (*func)(const void *, SEL) = (void *)__CFSendObjCMsg; \
	static SEL s = NULL; if (!s) s = sel_registerName(sel); \
	return func((const void *)obj, s);}
#define CF_OBJC_FUNCDISPATCH1(typeID, rettype, obj, sel, a1) \
	if (__builtin_expect(CF_IS_OBJC(typeID, obj), 0)) \
	{rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	static SEL s = NULL; if (!s) s = sel_registerName(sel); \
	return func((const void *)obj, s, (a1));}
#define CF_OBJC_FUNCDISPATCH2(typeID, rettype, obj, sel, a1, a2) \
	if (__builtin_expect(CF_IS_OBJC(typeID, obj), 0)) \
	{rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	static SEL s = NULL; if (!s) s = sel_registerName(sel); \
	return func((const void *)obj, s, (a1), (a2));}
#define CF_OBJC_FUNCDISPATCH3(typeID, rettype, obj, sel, a1, a2, a3) \
	if (__builtin_expect(CF_IS_OBJC(typeID, obj), 0)) \
	{rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	static SEL s = NULL; if (!s) s = sel_registerName(sel); \
	return func((const void *)obj, s, (a1), (a2), (a3));}
#define CF_OBJC_FUNCDISPATCH4(typeID, rettype, obj, sel, a1, a2, a3, a4) \
	if (__builtin_expect(CF_IS_OBJC(typeID, obj), 0)) \
	{rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	static SEL s = NULL; if (!s) s = sel_registerName(sel); \
	return func((const void *)obj, s, (a1), (a2), (a3), (a4));}
#define CF_OBJC_FUNCDISPATCH5(typeID, rettype, obj, sel, a1, a2, a3, a4, a5) \
	if (__builtin_expect(CF_IS_OBJC(typeID, obj), 0)) \
	{rettype (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	static SEL s = NULL; if (!s) s = sel_registerName(sel); \
	return func((const void *)obj, s, (a1), (a2), (a3), (a4), (a5));}

#endif

/* See comments in CFBase.c
*/
#if defined(__ppc__) && defined(__MACH__)
extern void __CF_FAULT_CALLBACK(void **ptr);
extern void *__CF_INVOKE_CALLBACK(void *, ...);

#define FAULT_CALLBACK(V) __CF_FAULT_CALLBACK(V)
#define INVOKE_CALLBACK1(P, A) (((uintptr_t)(P) & 0x2) ? __CF_INVOKE_CALLBACK(P, A) : (P)(A))
#define INVOKE_CALLBACK2(P, A, B) (((uintptr_t)(P) & 0x2) ? __CF_INVOKE_CALLBACK(P, A, B) : (P)(A, B))
#define INVOKE_CALLBACK3(P, A, B, C) (((uintptr_t)(P) & 0x2) ? __CF_INVOKE_CALLBACK(P, A, B, C) : (P)(A, B, C))
#define INVOKE_CALLBACK4(P, A, B, C, D) (((uintptr_t)(P) & 0x2) ? __CF_INVOKE_CALLBACK(P, A, B, C, D) : (P)(A, B, C, D))
#define INVOKE_CALLBACK5(P, A, B, C, D, E) (((uintptr_t)(P) & 0x2) ? __CF_INVOKE_CALLBACK(P, A, B, C, D, E) : (P)(A, B, C, D, E))
#else
#define FAULT_CALLBACK(V)
#define INVOKE_CALLBACK1(P, A) (P)(A)
#define INVOKE_CALLBACK2(P, A, B) (P)(A, B)
#define INVOKE_CALLBACK3(P, A, B, C) (P)(A, B, C)
#define INVOKE_CALLBACK4(P, A, B, C, D) (P)(A, B, C, D)
#define INVOKE_CALLBACK5(P, A, B, C, D, E) (P)(A, B, C, D, E)
#endif

#if defined(__MACH__)

/* For the support of functionality which needs CarbonCore or other frameworks */
extern void *__CFLookupCarbonCoreFunction(const char *name);
extern void *__CFLookupCFNetworkFunction(const char *name);

// These macros define an upcall or weak "symbol-lookup" wrapper function.
// The parameters are:
//   R : the return type of the function
//   N : the name of the function (in the other library)
//   P : the parenthesized parameter list of the function
//   A : the parenthesized actual argument list to be passed
//  opt: a fifth optional argument can be passed in which is the
//       return value of the wrapper when the function cannot be
//       found; should be of type R, & can be a function call
// The name of the resulting wrapper function is:
//    __CFCarbonCore_N (where N is the second parameter)
//    __CFNetwork_N (where N is the second parameter)
//
// Example:
//   DEFINE_WEAK_CARBONCORE_FUNC(void, DisposeHandle, (Handle h), (h))
//

#define DEFINE_WEAK_CARBONCORE_FUNC(R, N, P, A, ...)		\
static R __CFCarbonCore_ ## N P {				\
    static R (*dyfunc) P = (void *)(~(uintptr_t)0);		\
    if ((void *)(~(uintptr_t)0) == dyfunc) {			\
        dyfunc = __CFLookupCarbonCoreFunction(#N); }		\
    return dyfunc ? dyfunc A : (R)(0 , ## __VA_ARGS__);		\
}

#define DEFINE_WEAK_CFNETWORK_FUNC(R, N, P, A, ...)		\
static R __CFNetwork_ ## N P {					\
    static R (*dyfunc) P = (void *)(~(uintptr_t)0);		\
    if ((void *)(~(uintptr_t)0) == dyfunc) {			\
        dyfunc = __CFLookupCFNetworkFunction(#N); }		\
    return dyfunc ? dyfunc A : (R)(0 , ## __VA_ARGS__);		\
}

#else

#define DEFINE_WEAK_CARBONCORE_FUNC(R, N, P, A, ...)
#define DEFINE_WEAK_CFNETWORK_FUNC(R, N, P, A, ...)

#endif


__private_extern__ CFArrayRef _CFBundleCopyUserLanguages(Boolean useBackstops);

/* GC related internal SPIs. */
extern malloc_zone_t *__CFCollectableZone;
__private_extern__ void _CFStorageSetWeak(struct __CFStorage *storage);

#if defined(__WIN32__)
__private_extern__ const char *_CFDLLPath(void);
__private_extern__ void __CFStringCleanup(void);
__private_extern__ void __CFSocketCleanup(void);
__private_extern__ void __CFUniCharCleanup(void);
__private_extern__ void __CFStreamCleanup(void);
// We should export this as SPI or API to clients - 3514284
CF_EXPORT CFAbsoluteTime _CFAbsoluteTimeFromFileTime(const FILETIME *ft);
#endif

#endif /* ! __COREFOUNDATION_CFINTERNAL__ */
