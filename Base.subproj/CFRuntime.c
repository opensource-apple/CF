/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
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
/*	CFRuntime.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFRuntime.h"
#include "CFInternal.h"
#include <string.h>
#include <stdlib.h>
#if defined(__MACH__)
#include <monitor.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#else
#define __is_threaded (1)
#endif

extern size_t malloc_size(const void *ptr);
extern int __setonlyClocaleconv(int val);

#if defined(__MACH__)
extern void __CFRecordAllocationEvent(int eventnum, void *ptr, int size, int data, const char *classname);
#else 
#define __CFRecordAllocationEvent(a, b, c, d, e)
#endif

enum {
// retain/release recording constants -- must match values
// used by OA for now; probably will change in the future
__kCFRetainEvent = 28,
__kCFReleaseEvent = 29
};

/* On Win32 we should use _msize instead of malloc_size
 * (Aleksey Dukhnyakov)
 */
#if defined(__WIN32__)
#include <malloc.h>
CF_INLINE size_t malloc_size(void *memblock) {
    return _msize(memblock);
}
#endif

#if defined(__MACH__)

bool __CFOASafe = false;

void __CFOAInitialize(void) {
    static void (*dyfunc)(void) = (void *)0xFFFFFFFF;
    if (NULL == getenv("OAKeepAllocationStatistics")) return;
    if ((void *)0xFFFFFFFF == dyfunc) {
        dyfunc = NULL;
        if (NSIsSymbolNameDefined("__OAInitialize"))
            dyfunc = (void *)NSAddressOfSymbol(NSLookupAndBindSymbol("__OAInitialize"));
    }
    if (NULL != dyfunc) {
        dyfunc();
        __CFOASafe = true;
    }
}

void __CFRecordAllocationEvent(int eventnum, void *ptr, int size, int data, const char *classname) {
    static void (*dyfunc)(int, void *, int, int, const char *) = (void *)0xFFFFFFFF;
    if (!__CFOASafe) return;
    if ((void *)0xFFFFFFFF == dyfunc) {
        dyfunc = NULL;
        if (NSIsSymbolNameDefined("__OARecordAllocationEvent"))
            dyfunc = (void *)NSAddressOfSymbol(NSLookupAndBindSymbol("__OARecordAllocationEvent"));
    }
    if (NULL != dyfunc) {
        dyfunc(eventnum, ptr, size, data, classname);
    }
}

void __CFSetLastAllocationEventName(void *ptr, const char *classname) {
    static void (*dyfunc)(void *, const char *) = (void *)0xFFFFFFFF;
    if (!__CFOASafe) return;
    if ((void *)0xFFFFFFFF == dyfunc) {
        dyfunc = NULL;
        if (NSIsSymbolNameDefined("__OASetLastAllocationEventName"))
            dyfunc = (void *)NSAddressOfSymbol(NSLookupAndBindSymbol("__OASetLastAllocationEventName"));
    }
    if (NULL != dyfunc) {
        dyfunc(ptr, classname);
    }
}
#endif

extern void __HALT(void);

static CFTypeID __kCFNotATypeTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFNotATypeClass = {
    0,
    "Not A Type",
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT
};

static CFTypeID __kCFTypeTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFTypeClass = {
    0,
    "CFType",
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT
};

/* bits 15-8 in the CFRuntimeBase _info are type */
/* bits 7-0 in the CFRuntimeBase are reserved for CF's use */

static CFRuntimeClass * __CFRuntimeClassTable[__CFMaxRuntimeTypes] = {NULL};
static int32_t __CFRuntimeClassTableCount = 0;

#if defined(__MACH__)

__private_extern__ SEL (*__CFGetObjCSelector)(const char *) = NULL;
__private_extern__ void * (*__CFSendObjCMsg)(const void *, SEL, ...) = NULL;

__private_extern__ struct objc_class *__CFRuntimeObjCClassTable[__CFMaxRuntimeTypes] = {NULL};

#endif

// Compiler uses this symbol name
int __CFConstantStringClassReference[10] = {0};

#if defined(__MACH__)
static struct objc_class __CFNSTypeClass = {{0, 0}, NULL, {0, 0, 0, 0, 0, 0, 0}};
#else
static void *__CFNSTypeClass[10] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
#endif

//static CFSpinLock_t __CFRuntimeLock = 0;

CFTypeID _CFRuntimeRegisterClass(const CFRuntimeClass * const cls) {
// version field must be 0
// className must be pure ASCII string, non-null
    if (__CFMaxRuntimeTypes <= __CFRuntimeClassTableCount) {
	CFLog(0, CFSTR("*** CoreFoundation class table full; registration failing for class '%s'.  Program will crash soon."), cls->className);
	return _kCFRuntimeNotATypeID;
    }
    __CFRuntimeClassTable[__CFRuntimeClassTableCount++] = (CFRuntimeClass *)cls;
    return __CFRuntimeClassTableCount - 1;
}

const CFRuntimeClass * _CFRuntimeGetClassWithTypeID(CFTypeID typeID) {
    return __CFRuntimeClassTable[typeID];
}

void _CFRuntimeUnregisterClassWithTypeID(CFTypeID typeID) {
    __CFRuntimeClassTable[typeID] = NULL;
}


#if defined(DEBUG)

/* CFZombieLevel levels:
 *	bit 0: scribble deallocated CF object memory
 *	bit 1: do not scribble on CFRuntimeBase header (when bit 0)
 *	bit 4: do not free CF objects
 *	bit 7: use 3rd-order byte as scribble byte for dealloc (otherwise 0xFC)
 *      bit 16: scribble allocated CF object memory
 *      bit 23: use 1st-order byte as scribble byte for alloc (otherwise 0xCF)
 */

static uint32_t __CFZombieLevel = 0x0;

static void __CFZombifyAllocatedMemory(void *cf) {
    if (__CFZombieLevel & (1 << 16)) {
	void *ptr = cf;
	size_t size = malloc_size(cf);
	uint8_t byte = 0xCF;
	if (__CFZombieLevel & (1 << 23)) {
	    byte = (__CFZombieLevel >> 24) & 0xFF;
	}
	memset(ptr, byte, size);
    }
}

static void __CFZombifyDeallocatedMemory(void *cf) {
    if (__CFZombieLevel & (1 << 0)) {
	void *ptr = cf;
	size_t size = malloc_size(cf);
	uint8_t byte = 0xFC;
	if (__CFZombieLevel & (1 << 1)) {
	    ptr += sizeof(CFRuntimeBase);
	    size -= sizeof(CFRuntimeBase);
	}
	if (__CFZombieLevel & (1 << 7)) {
	    byte = (__CFZombieLevel >> 8) & 0xFF;
	}
	memset(ptr, byte, size);
    }
}

#endif /* DEBUG */

CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, uint32_t extraBytes, unsigned char *category) {
    CFRuntimeBase *memory;
    Boolean usesSystemDefaultAllocator;
    int32_t size;

    if (NULL == __CFRuntimeClassTable[typeID]) {
	return NULL;
    }
    allocator = (NULL == allocator) ? __CFGetDefaultAllocator() : allocator;
    usesSystemDefaultAllocator = (allocator == kCFAllocatorSystemDefault);
    extraBytes = (extraBytes + (sizeof(void *) - 1)) & ~(sizeof(void *) - 1);
    size = sizeof(CFRuntimeBase) + extraBytes + (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef));
    memory = CFAllocatorAllocate(allocator, size, 0);
    if (NULL == memory) {
	return NULL;
    }
#if defined(DEBUG)
    __CFZombifyAllocatedMemory((void *)memory);
#endif
    if (__CFOASafe && category) {
        __CFSetLastAllocationEventName(memory, category);
    } else if (__CFOASafe) {
        __CFSetLastAllocationEventName(memory, __CFRuntimeClassTable[typeID]->className);
    }
    if (!usesSystemDefaultAllocator) {
	*(CFAllocatorRef *)((char *)memory) = CFRetain(allocator);
	memory = (CFRuntimeBase *)((char *)memory + sizeof(CFAllocatorRef));
    }
    memory->_isa = __CFISAForTypeID(typeID);
    memory->_rc = 1;
    memory->_info = 0;
    __CFBitfieldSetValue(memory->_info, 15, 8, typeID);
    if (usesSystemDefaultAllocator) {
	__CFBitfieldSetValue(memory->_info, 7, 7, 1);
    }
    if (NULL != __CFRuntimeClassTable[typeID]->init) {
	(__CFRuntimeClassTable[typeID]->init)(memory);
    }
    return memory;
}

void _CFRuntimeSetInstanceTypeID(CFTypeRef cf, CFTypeID typeID) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_info, 15, 8, typeID);
}

CFTypeID __CFGenericTypeID(const void *cf) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 15, 8);
}

CF_INLINE CFTypeID __CFGenericTypeID_inline(const void *cf) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 15, 8);
}

CFTypeID CFTypeGetTypeID(void) {
    return __kCFTypeTypeID;
}

__private_extern__ void __CFGenericValidateType_(CFTypeRef cf, CFTypeID type, const char *func) {
    if (cf && CF_IS_OBJC(type, cf)) return;
    CFAssert2((cf != NULL) && (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]) && (__kCFNotATypeTypeID != __CFGenericTypeID_inline(cf)) && (__kCFTypeTypeID != __CFGenericTypeID_inline(cf)), __kCFLogAssertion, "%s(): pointer 0x%x is not a CF object", func, cf); \
    CFAssert3(__CFGenericTypeID_inline(cf) == type, __kCFLogAssertion, "%s(): pointer 0x%x is not a %s", func, cf, __CFRuntimeClassTable[type]->className);	\
}

#define __CFGenericAssertIsCF(cf) \
    CFAssert2(cf != NULL && (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]) && (__kCFNotATypeTypeID != __CFGenericTypeID_inline(cf)) && (__kCFTypeTypeID != __CFGenericTypeID_inline(cf)), __kCFLogAssertion, "%s(): pointer 0x%x is not a CF object", __PRETTY_FUNCTION__, cf);

#if !defined(__MACH__)

#define CFTYPE_IS_OBJC(obj) (false)
#define CFTYPE_OBJC_FUNCDISPATCH0(rettype, obj, sel) do {} while (0)
#define CFTYPE_OBJC_FUNCDISPATCH1(rettype, obj, sel, a1) do {} while (0)

#endif

#if defined(__MACH__)

CF_INLINE int CFTYPE_IS_OBJC(const void *obj) {
    CFTypeID typeID = __CFGenericTypeID_inline(obj);
    return CF_IS_OBJC(typeID, obj) && __CFSendObjCMsg;
}

#define CFTYPE_OBJC_FUNCDISPATCH0(rettype, obj, sel) \
	if (CFTYPE_IS_OBJC(obj)) \
        {rettype (*func)(void *, SEL) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = __CFGetObjCSelector(sel); \
        return func((void *)obj, s);}
#define CFTYPE_OBJC_FUNCDISPATCH1(rettype, obj, sel, a1) \
	if (CFTYPE_IS_OBJC(obj)) \
        {rettype (*func)(void *, SEL, ...) = (void *)__CFSendObjCMsg; \
        static SEL s = NULL; if (!s) s = __CFGetObjCSelector(sel); \
        return func((void *)obj, s, (a1));}

#endif

CFTypeID CFGetTypeID(CFTypeRef cf) {
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(CFTypeID, cf, "_cfTypeID");
    __CFGenericAssertIsCF(cf);
    return __CFGenericTypeID_inline(cf);
}

CFStringRef CFCopyTypeIDDescription(CFTypeID type) {
    CFAssert2((NULL != __CFRuntimeClassTable[type]) && __kCFNotATypeTypeID != type && __kCFTypeTypeID != type, __kCFLogAssertion, "%s(): type %d is not a CF type ID", __PRETTY_FUNCTION__, type);
    return CFStringCreateWithCString(kCFAllocatorDefault, __CFRuntimeClassTable[type]->className, kCFStringEncodingASCII);
}

static CFSpinLock_t __CFGlobalRetainLock = 0;
static CFMutableDictionaryRef __CFRuntimeExternRefCountTable = NULL;

#define DISGUISE(object)       ((void *)(((unsigned)object) + 1))
#define UNDISGUISE(disguised)  ((id)(((unsigned)disguised) - 1))
 
extern void _CFDictionaryIncrementValue(CFMutableDictionaryRef dict, const void *key);
extern int _CFDictionaryDecrementValue(CFMutableDictionaryRef dict, const void *key);

// Bit 31 (highest bit) in second word of cf instance indicates external ref count

CFTypeRef CFRetain(CFTypeRef cf) {
    CFIndex lowBits = 0;
    bool is_threaded = __is_threaded; 
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(CFTypeRef, cf, "retain");
    __CFGenericAssertIsCF(cf);
    if (is_threaded) __CFSpinLock(&__CFGlobalRetainLock);
    lowBits = ((CFRuntimeBase *)cf)->_rc;
    if (0 == lowBits) {	// Constant CFTypeRef
	if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	return cf;
    }
    lowBits++;
    if ((lowBits & 0x07fff) == 0) {
	// Roll over another bit to the external ref count
	_CFDictionaryIncrementValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
	lowBits = 0x8000; // Bit 16 indicates external ref count
    }
    ((CFRuntimeBase *)cf)->_rc = lowBits;
    if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
    if (__CFOASafe) {
	uint64_t compositeRC;
	compositeRC = (lowBits & 0x7fff) + ((uint64_t)(uintptr_t)CFDictionaryGetValue(__CFRuntimeExternRefCountTable, DISGUISE(cf)) << 15);
	if (compositeRC > (uint64_t)0x7fffffff) compositeRC = (uint64_t)0x7fffffff;
	__CFRecordAllocationEvent(__kCFRetainEvent, (void *)cf, 0, compositeRC, NULL);
    }
    return cf;
}

__private_extern__ void __CFAllocatorDeallocate(CFTypeRef cf);

void CFRelease(CFTypeRef cf) {
    CFIndex lowBits = 0;
    bool is_threaded = __is_threaded;
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(void, cf, "release");
    __CFGenericAssertIsCF(cf);
    if (is_threaded) __CFSpinLock(&__CFGlobalRetainLock);
    lowBits = ((CFRuntimeBase *)cf)->_rc;
    if (0 == lowBits) {	// Constant CFTypeRef
	if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	return;
    }
    if (1 == lowBits) {
        if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	if (__CFOASafe) __CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
	if (__kCFAllocatorTypeID_CONST == __CFGenericTypeID_inline(cf)) {
#if defined(DEBUG)
	    __CFZombifyDeallocatedMemory((void *)cf);
	    if (!(__CFZombieLevel & (1 << 4))) {
		__CFAllocatorDeallocate((void *)cf);
	    }
#else
	    __CFAllocatorDeallocate((void *)cf);
#endif
	} else {
	    CFAllocatorRef allocator;
	    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->finalize) {
		__CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->finalize(cf);
	    }
	    if (__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 7, 7)) {
		allocator = kCFAllocatorSystemDefault;
	    } else {
		allocator = CFGetAllocator(cf);
		(intptr_t)cf -= sizeof(CFAllocatorRef);
	    }
#if defined(DEBUG)
	    __CFZombifyDeallocatedMemory((void *)cf);
	    if (!(__CFZombieLevel & (1 << 4))) {
		CFAllocatorDeallocate(allocator, (void *)cf);
	    }
#else
	    CFAllocatorDeallocate(allocator, (void *)cf);
#endif
	    if (kCFAllocatorSystemDefault != allocator) {
		CFRelease(allocator);
	    }
	}
    } else {
	if (0x8000 == lowBits) {
	    // Time to remove a bit from the external ref count
	    if (0 == _CFDictionaryDecrementValue(__CFRuntimeExternRefCountTable, DISGUISE(cf))) {
		lowBits = 0x07fff;
	    } else {
		lowBits = 0x0ffff;
	    }
 	} else {
	    lowBits--;
	}
	((CFRuntimeBase *)cf)->_rc = lowBits;
        if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	if (__CFOASafe) {
	    uint64_t compositeRC;
	    compositeRC = (lowBits & 0x7fff) + ((uint64_t)(uintptr_t)CFDictionaryGetValue(__CFRuntimeExternRefCountTable, DISGUISE(cf)) << 15);
	    if (compositeRC > (uint64_t)0x7fffffff) compositeRC = (uint64_t)0x7fffffff;
	    __CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, compositeRC, NULL);
	}
    }
}

static uint64_t __CFGetFullRetainCount(CFTypeRef cf) {
    uint32_t lowBits = 0;
    uint64_t highBits = 0, compositeRC;
    lowBits = ((CFRuntimeBase *)cf)->_rc;
    if (0 == lowBits) {
	return (uint64_t)0x00FFFFFFFFFFFFFFULL;
    }
    if ((lowBits & 0x08000) != 0) {
        highBits = (uint64_t)(uintptr_t)CFDictionaryGetValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
    }
    compositeRC = (lowBits & 0x7fff) + (highBits << 15);
    return compositeRC;
}

CFIndex CFGetRetainCount(CFTypeRef cf) {
    uint64_t rc;
    CFIndex result;
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(CFIndex, cf, "retainCount");
    __CFGenericAssertIsCF(cf);
    rc = __CFGetFullRetainCount(cf);
    result = (rc < (uint64_t)0x7FFFFFFF) ? (CFIndex)rc : (CFIndex)0x7FFFFFFF;
    return result;
}

Boolean CFEqual(CFTypeRef cf1, CFTypeRef cf2) {
#if defined(DEBUG)
    if (NULL == cf1) HALT;
    if (NULL == cf2) HALT;
#endif
    if (cf1 == cf2) return true;
    CFTYPE_OBJC_FUNCDISPATCH1(Boolean, cf1, "isEqual:", cf2);
    CFTYPE_OBJC_FUNCDISPATCH1(Boolean, cf2, "isEqual:", cf1);
    __CFGenericAssertIsCF(cf1);
    __CFGenericAssertIsCF(cf2);
    if (__CFGenericTypeID_inline(cf1) != __CFGenericTypeID_inline(cf2)) return false;
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal(cf1, cf2);
    }
    return false;
}

CFHashCode CFHash(CFTypeRef cf) {
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(CFHashCode, cf, "hash");
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash(cf);
    }
    return (CFHashCode)cf;
}

// definition: produces a normally non-NULL debugging description of the object
CFStringRef CFCopyDescription(CFTypeRef cf) {
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(CFStringRef, cf, "_copyDescription");
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyDebugDesc) {
	CFStringRef result;
	result = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyDebugDesc(cf);
	if (NULL != result) return result;
    }
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<%s %p [%p]>"), __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->className, cf, CFGetAllocator(cf));
}

// Definition: if type produces a formatting description, return that string, otherwise NULL
__private_extern__ CFStringRef __CFCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
#if defined(__MACH__)
    if (CFTYPE_IS_OBJC(cf)) {
	static SEL s = NULL, r = NULL;
	CFStringRef (*func)(void *, SEL, ...) = (void *)__CFSendObjCMsg;
	if (!s) s = __CFGetObjCSelector("_copyFormattingDescription:");
	if (!r) r = __CFGetObjCSelector("respondsToSelector:");
	if (s && func((void *)cf, r, s)) return func((void *)cf, s, formatOptions);
	return NULL;
    }
#endif
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc(cf, formatOptions);
    }
    return NULL;
}

extern CFAllocatorRef __CFAllocatorGetAllocator(CFTypeRef);

CFAllocatorRef CFGetAllocator(CFTypeRef cf) {
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
// CF: need to get allocator from objc objects in better way...how?
// -> bridging of CFAllocators and malloc_zone_t will help this
    if (CFTYPE_IS_OBJC(cf)) return __CFGetDefaultAllocator();
    if (__kCFAllocatorTypeID_CONST == __CFGenericTypeID_inline(cf)) {
	return __CFAllocatorGetAllocator(cf);
    }
    return __CFGetAllocator(cf);
}

extern void __CFBaseInitialize(void);
extern void __CFNullInitialize(void);
extern void __CFAllocatorInitialize(void);
extern void __CFStringInitialize(void);
extern void __CFArrayInitialize(void);
extern void __CFBagInitialize(void);
extern void __CFBooleanInitialize(void);
extern void __CFCharacterSetInitialize(void);
extern void __CFDataInitialize(void);
extern void __CFDateInitialize(void);
extern void __CFDictionaryInitialize(void);
extern void __CFNumberInitialize(void);
extern void __CFSetInitialize(void);
extern void __CFStorageInitialize(void);
extern void __CFTimeZoneInitialize(void);
extern void __CFTreeInitialize(void);
extern void __CFURLInitialize(void);
extern void __CFXMLNodeInitialize(void);
extern void __CFXMLParserInitialize(void);
#if defined(__MACH__)
extern void __CFMessagePortInitialize(void);
extern void __CFMachPortInitialize(void);
#endif
#if defined(__MACH__) || defined(__WIN32__)
extern void __CFRunLoopInitialize(void);
extern void __CFRunLoopObserverInitialize(void);
extern void __CFRunLoopSourceInitialize(void);
extern void __CFRunLoopTimerInitialize(void);
extern void __CFSocketInitialize(void);
#endif
extern void __CFBundleInitialize(void);
extern void __CFPlugInInitialize(void);
extern void __CFPlugInInstanceInitialize(void);
extern void __CFUUIDInitialize(void);
extern void __CFBinaryHeapInitialize(void);
extern void __CFBitVectorInitialize(void);

#if defined(DEBUG)
#define DO_SYSCALL_TRACE_HELPERS 1
#endif
#if defined(DO_SYSCALL_TRACE_HELPERS) && defined(__MACH__)
extern void ptrace(int, int, int, int);
#define SYSCALL_TRACE(N)        do ptrace(N, 0, 0, 0); while (0)
#else
#define SYSCALL_TRACE(N)        do {} while (0)
#endif

#if defined(__MACH__) && defined(PROFILE)
static void _CF_mcleanup(void) {
    monitor(0,0,0,0,0);
}
#endif

extern CFTypeID CFTimeZoneGetTypeID(void);
extern CFTypeID CFNumberGetTypeID(void);
extern CFTypeID CFBooleanGetTypeID(void);

const void *__CFArgStuff = NULL;
__private_extern__ void *__CFAppleLanguages = NULL;
__private_extern__ void *__CFSessionID = NULL;

#if defined(__LINUX__) || defined(__FREEBSD__)
static void __CFInitialize(void) __attribute__ ((constructor));
static
#endif
#if defined(__WIN32__)
CF_EXPORT
#endif
void __CFInitialize(void) {
    static int __done = 0;
    if (sizeof(int) != sizeof(long) || 4 != sizeof(long)) __HALT();
    if (!__done) {
	CFIndex idx, cnt;

	__done = 1;
	SYSCALL_TRACE(0xC000);

	__setonlyClocaleconv(1);
#if defined(DEBUG)
	{
	    const char *value = getenv("CFZombieLevel");
	    if (NULL != value) {
		__CFZombieLevel = strtoul(value, NULL, 0);
	    }
	    if (0x0 == __CFZombieLevel) __CFZombieLevel = 0xCF00FC00; // default
	}
#endif
#if defined(__MACH__) && defined(PROFILE)
	{
	    const char *v = getenv("DYLD_IMAGE_SUFFIX");
	    const char *p = getenv("CFPROF_ENABLE");
	    // ckane: People were upset that I added this feature to allow for the profiling of
	    // libraries using unprofiled apps/executables, so ensure they cannot get this accidentally.
	    if (v && p && 0 == strcmp("_profile", v) && 0 == strcmp(crypt(p + 2, p) + 2, "eQJhkVvMm.w")) {
		// Unfortunately, no way to know if this has already been done,
		// or I would not do it.  Not much information will be lost.
		atexit(_CF_mcleanup);
		moninit();
	    }
	}
#endif

	__CFBaseInitialize();

#if defined(__MACH__)
	for (idx = 0; idx < __CFMaxRuntimeTypes; idx++) {
	    __CFRuntimeObjCClassTable[idx] = &__CFNSTypeClass;
	}
#endif

	/* Here so that two runtime classes get indices 0, 1. */
	__kCFNotATypeTypeID = _CFRuntimeRegisterClass(&__CFNotATypeClass);
	__kCFTypeTypeID = _CFRuntimeRegisterClass(&__CFTypeClass);

	/* Here so that __kCFAllocatorTypeID gets index 2. */
	__CFAllocatorInitialize();

	/* Basic collections need to be up before CFString. */
	__CFDictionaryInitialize();
	__CFArrayInitialize();
	__CFDataInitialize();
	__CFSetInitialize();

#if defined(__MACH__)
	{
	    char **args = *_NSGetArgv();
	    cnt = *_NSGetArgc();
	    for (idx = 1; idx < cnt - 1; idx++) {
		if (0 == strcmp(args[idx], "-AppleLanguages")) {
		    CFIndex length = strlen(args[idx + 1]);
		    __CFAppleLanguages = malloc(length + 1);
		    memmove(__CFAppleLanguages, args[idx + 1], length + 1);
		    break;
		}
	    }
	}
#endif

#if defined(__MACH__)
	// Pre-initialize for possible future toll-free bridging
	// These have to be initialized before the *Initialize functions below are called
	__CFRuntimeObjCClassTable[CFDictionaryGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[CFArrayGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[CFDataGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[CFSetGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[0x7] = (struct objc_class *)&__CFConstantStringClassReference;
	__CFRuntimeObjCClassTable[0x8] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[0x9] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[0xa] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[0xb] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[0xc] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
#endif

        // Creating this lazily in CFRetain causes recursive call to CFRetain
        __CFRuntimeExternRefCountTable = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);

        /*** _CFRuntimeCreateInstance() can finally be called generally after this line. ***/

        __CFStringInitialize();		// CFString's TypeID must be 0x7, now and forever
	__CFNullInitialize();		// See above for hard-coding of this position
	__CFBooleanInitialize();	// See above for hard-coding of this position
	__CFNumberInitialize();		// See above for hard-coding of this position
	__CFDateInitialize();		// See above for hard-coding of this position
	__CFTimeZoneInitialize();	// See above for hard-coding of this position

	__CFBinaryHeapInitialize();
	__CFBitVectorInitialize();
	__CFBagInitialize();
	__CFCharacterSetInitialize();
	__CFStorageInitialize();
	__CFTreeInitialize();
	__CFURLInitialize();
        __CFXMLNodeInitialize();
	__CFXMLParserInitialize();
	__CFBundleInitialize();
	__CFPlugInInitialize();
	__CFPlugInInstanceInitialize();
	__CFUUIDInitialize();
#if defined(__MACH__)
	__CFMessagePortInitialize();
	__CFMachPortInitialize();
#endif
#if defined(__MACH__) || defined(__WIN32__)
	__CFRunLoopInitialize();
	__CFRunLoopObserverInitialize();
	__CFRunLoopSourceInitialize();
	__CFRunLoopTimerInitialize();
	__CFSocketInitialize();
#endif

#if defined(__MACH__)
        // Pre-initialize for possible future toll-free bridging, continued
	__CFRuntimeObjCClassTable[CFRunLoopTimerGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[CFMachPortGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[CFURLGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
	__CFRuntimeObjCClassTable[CFCharacterSetGetTypeID()] = (struct objc_class *)calloc(sizeof(struct objc_class), 1);
#endif

	SYSCALL_TRACE(0xC001);

#if defined(__MACH__)
	{
	    char **args = *_NSGetArgv();
	    CFIndex count;
	    cnt = *_NSGetArgc();
	    CFStringRef *list, buffer[256];
	    list = (cnt <= 256) ? buffer : malloc(cnt * sizeof(CFStringRef));
	    for (idx = 0, count = 0; idx < cnt && args[idx]; idx++) {
		list[count] = CFStringCreateWithCString(kCFAllocatorSystemDefault, args[idx], kCFStringEncodingUTF8);
		if (NULL == list[count]) {
		    list[count] = CFStringCreateWithCString(kCFAllocatorSystemDefault, args[idx], kCFStringEncodingISOLatin1);
		    // We CANNOT use the string SystemEncoding here;
		    // Do not argue: it is not initialized yet, but these
		    // arguments MUST be initialized before it is.
		    // We should just ignore the argument if the UTF-8
		    // conversion fails, but out of charity we try once
		    // more with ISO Latin1, a standard unix encoding.
		}
		if (NULL != list[count]) count++;
	    }
	    __CFArgStuff = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)list, count, &kCFTypeArrayCallBacks);
	}

	__CFSessionID = getenv("SECURITYSESSIONID");
#endif
	_CFProcessPath();	// cache this early

#if defined(__MACH__)
        __CFOAInitialize();
	SYSCALL_TRACE(0xC003);
#endif

#if defined(DEBUG) && !defined(__WIN32__)
	// Don't log on MacOS 8 as this will create a log file unnecessarily
	CFLog (0, CFSTR("Assertions enabled"));
#endif
	SYSCALL_TRACE(0xC0FF);
    }
}

#if defined(__WIN32__)

/* We have to call __CFInitialize when library is attached to the process.
 * (Sergey Zubarev)
 */
WINBOOL WINAPI DllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID pReserved ) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        __CFInitialize();
    } else if (dwReason == DLL_PROCESS_DETACH) {
    }
    return TRUE;
}

#endif


// Functions that avoid ObC dispatch and CF type validation, for use by NSNotifyingCFArray, etc.
// Hopefully all of this will just go away.  3321464.  M.P. To Do - 7/9/03

Boolean _CFEqual(CFTypeRef cf1, CFTypeRef cf2) {
#if defined(DEBUG)
    if (NULL == cf1) HALT;
    if (NULL == cf2) HALT;
#endif
    if (cf1 == cf2) return true;
    __CFGenericAssertIsCF(cf1);
    __CFGenericAssertIsCF(cf2);
    if (__CFGenericTypeID(cf1) != __CFGenericTypeID(cf2)) return false;
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID(cf1)]->equal) {
	return __CFRuntimeClassTable[__CFGenericTypeID(cf1)]->equal(cf1, cf2);
    }
    return false;
}

CFIndex _CFGetRetainCount(CFTypeRef cf) {
    uint64_t rc;
    CFIndex result;
    rc = __CFGetFullRetainCount(cf);
    result = (rc < (uint64_t)0x7FFFFFFF) ? (CFIndex)rc : (CFIndex)0x7FFFFFFF;
    return result;
}

CFHashCode _CFHash(CFTypeRef cf) {
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash(cf);
    }
    return (CFHashCode)cf;
}

CF_EXPORT CFTypeRef _CFRetain(CFTypeRef cf) {
    CFIndex lowBits = 0;
    bool is_threaded = __is_threaded;
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    if (is_threaded) __CFSpinLock(&__CFGlobalRetainLock);
    lowBits = ((CFRuntimeBase *)cf)->_rc;
    if (0 == lowBits) {	// Constant CFTypeRef
	if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	return cf;
    }
    lowBits++;
    if ((lowBits & 0x07fff) == 0) {
	// Roll over another bit to the external ref count
	_CFDictionaryIncrementValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
	lowBits = 0x8000; // Bit 16 indicates external ref count
    }
    ((CFRuntimeBase *)cf)->_rc = lowBits;
    if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
    if (__CFOASafe) {
	uint64_t compositeRC;
	compositeRC = (lowBits & 0x7fff) + ((uint64_t)(uintptr_t)CFDictionaryGetValue(__CFRuntimeExternRefCountTable, DISGUISE(cf)) << 15);
	if (compositeRC > (uint64_t)0x7fffffff) compositeRC = (uint64_t)0x7fffffff;
	__CFRecordAllocationEvent(__kCFRetainEvent, (void *)cf, 0, compositeRC, NULL);
    }
    return cf;
}

CF_EXPORT void _CFRelease(CFTypeRef cf) {
    CFIndex lowBits = 0;
    bool is_threaded = __is_threaded;
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    if (is_threaded) __CFSpinLock(&__CFGlobalRetainLock);
    lowBits = ((CFRuntimeBase *)cf)->_rc;
    if (0 == lowBits) {	// Constant CFTypeRef
	if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	return;
    }
    if (1 == lowBits) {
        if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	if (__CFOASafe) __CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
	if (__kCFAllocatorTypeID_CONST == __CFGenericTypeID(cf)) {
#if defined(DEBUG)
	    __CFZombifyDeallocatedMemory((void *)cf);
	    if (!(__CFZombieLevel & (1 << 4))) {
		__CFAllocatorDeallocate((void *)cf);
	    }
#else
	    __CFAllocatorDeallocate((void *)cf);
#endif
	} else {
	    CFAllocatorRef allocator;
	    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID(cf)]->finalize) {
		__CFRuntimeClassTable[__CFGenericTypeID(cf)]->finalize(cf);
	    }
	    if (__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 7, 7)) {
		allocator = kCFAllocatorSystemDefault;
	    } else {
		allocator = CFGetAllocator(cf);
		(intptr_t)cf -= sizeof(CFAllocatorRef);
	    }
#if defined(DEBUG)
	    __CFZombifyDeallocatedMemory((void *)cf);
	    if (!(__CFZombieLevel & (1 << 4))) {
		CFAllocatorDeallocate(allocator, (void *)cf);
	    }
#else
	    CFAllocatorDeallocate(allocator, (void *)cf);
#endif
	    if (kCFAllocatorSystemDefault != allocator) {
		CFRelease(allocator);
	    }
	}
    } else {
	if (0x8000 == lowBits) {
	    // Time to remove a bit from the external ref count
	    if (0 == _CFDictionaryDecrementValue(__CFRuntimeExternRefCountTable, DISGUISE(cf))) {
		lowBits = 0x07fff;
	    } else {
		lowBits = 0x0ffff;
	    }
	} else {
	    lowBits--;
	}
	((CFRuntimeBase *)cf)->_rc = lowBits;
        if (is_threaded) __CFSpinUnlock(&__CFGlobalRetainLock);
	if (__CFOASafe) {
	    uint64_t compositeRC;
	    compositeRC = (lowBits & 0x7fff) + ((uint64_t)(uintptr_t)CFDictionaryGetValue(__CFRuntimeExternRefCountTable, DISGUISE(cf)) << 15);
	    if (compositeRC > (uint64_t)0x7fffffff) compositeRC = (uint64_t)0x7fffffff;
	    __CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, compositeRC, NULL);
	}
    }
}

#undef DO_SYSCALL_TRACE_HELPERS
#undef SYSCALL_TRACE
#undef __kCFAllocatorTypeID_CONST
#undef __CFGenericAssertIsCF
