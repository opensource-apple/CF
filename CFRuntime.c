/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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

/*	CFRuntime.c
	Copyright (c) 1999-2009, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#define ENABLE_ZOMBIES 1

#include <CoreFoundation/CFRuntime.h>
#include "CFInternal.h"
#include "CFBasicHash.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <crt_externs.h>
#include <unistd.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <CoreFoundation/CFStringDefaultEncoding.h>
#define objc_isAuto (0)
#else
#include <objc/runtime.h>
#endif

#if DEPLOYMENT_TARGET_WINDOWS
#define _objc_getFreedObjectClass()       0
#else
extern Class _objc_getFreedObjectClass(void);
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
extern void __CFRecordAllocationEvent(int eventnum, void *ptr, int64_t size, uint64_t data, const char *classname);
#else
#define __CFRecordAllocationEvent(a, b, c, d, e) ((void)0)
#endif


#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
extern void instrumentObjcMessageSends(BOOL flag);
#endif

#if DEPLOYMENT_TARGET_WINDOWS
#include <Shellapi.h>
#endif

enum {
// retain/release recording constants -- must match values
// used by OA for now; probably will change in the future
__kCFRetainEvent = 28,
__kCFReleaseEvent = 29
};

#if DEPLOYMENT_TARGET_WINDOWS
#include <malloc.h>
#else
#include <malloc/malloc.h>
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED

bool __CFOASafe = false;

void (*__CFObjectAllocRecordAllocationFunction)(int, void *, int64_t , uint64_t, const char *) = NULL;
void (*__CFObjectAllocSetLastAllocEventNameFunction)(void *, const char *) = NULL;

void __CFOAInitialize(void) {
    static void (*dyfunc)(void) = (void *)~0;
    if (NULL == __CFgetenv("OAKeepAllocationStatistics")) return;
    if ((void *)~0 == dyfunc) {
	dyfunc = dlsym(RTLD_DEFAULT, "_OAInitialize");
    }
    if (NULL != dyfunc) {
	dyfunc();
	__CFObjectAllocRecordAllocationFunction = dlsym(RTLD_DEFAULT, "_OARecordAllocationEvent");
	__CFObjectAllocSetLastAllocEventNameFunction = dlsym(RTLD_DEFAULT, "_OASetLastAllocationEventName");
	__CFOASafe = true;
    }
}

void __CFRecordAllocationEvent(int eventnum, void *ptr, int64_t size, uint64_t data, const char *classname) {
    if (!__CFOASafe || !__CFObjectAllocRecordAllocationFunction) return;
    __CFObjectAllocRecordAllocationFunction(eventnum, ptr, size, data, classname);
}

void __CFSetLastAllocationEventName(void *ptr, const char *classname) {
    if (!__CFOASafe || !__CFObjectAllocSetLastAllocEventNameFunction) return;
    __CFObjectAllocSetLastAllocEventNameFunction(ptr, classname);
}

#endif

extern void __HALT(void);

static CFTypeID __kCFNotATypeTypeID = _kCFRuntimeNotATypeID;

#if !defined (__cplusplus)
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
#else
void SIG1(CFTypeRef){__HALT();};;
CFTypeRef SIG2(CFAllocatorRef,CFTypeRef){__HALT();return NULL;};
Boolean SIG3(CFTypeRef,CFTypeRef){__HALT();return FALSE;};
CFHashCode SIG4(CFTypeRef){__HALT(); return 0;};
CFStringRef SIG5(CFTypeRef,CFDictionaryRef){__HALT();return NULL;};
CFStringRef SIG6(CFTypeRef){__HALT();return NULL;};

static const CFRuntimeClass __CFNotATypeClass = {
    0,
    "Not A Type",
    SIG1,
    SIG2,
    SIG1,
    SIG3,
    SIG4,
    SIG5,
    SIG6
};

static CFTypeID __kCFTypeTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFTypeClass = {
    0,
    "CFType",
    SIG1,
    SIG2,
    SIG1,
    SIG3,
    SIG4,
    SIG5,
    SIG6
};
#endif //__cplusplus

// the lock does not protect most reading of these; we just leak the old table to allow read-only accesses to continue to work
static CFSpinLock_t __CFBigRuntimeFunnel = CFSpinLockInit;
static CFRuntimeClass ** __CFRuntimeClassTable = NULL;
int32_t __CFRuntimeClassTableSize = 0;
static int32_t __CFRuntimeClassTableCount = 0;

uintptr_t *__CFRuntimeObjCClassTable = NULL;

__private_extern__ void * (*__CFSendObjCMsg)(const void *, SEL, ...) = NULL;

bool (*__CFObjCIsCollectable)(void *) = NULL;

// Compiler uses this symbol name; must match compiler built-in decl, so we use 'int'
#if __LP64__
int __CFConstantStringClassReference[24] = {0};
#else
int __CFConstantStringClassReference[12] = {0};
#endif


Boolean _CFIsObjC(CFTypeID typeID, void *obj) {
    __CFSpinLock(&__CFBigRuntimeFunnel);
    Boolean b = ((typeID >= (CFTypeID)__CFRuntimeClassTableSize) || (((CFRuntimeBase *)obj)->_cfisa != (uintptr_t)__CFRuntimeObjCClassTable[typeID] && ((CFRuntimeBase *)obj)->_cfisa > (uintptr_t)0xFFF));
    __CFSpinUnlock(&__CFBigRuntimeFunnel);
    return b;
}

CFTypeID _CFRuntimeRegisterClass(const CFRuntimeClass * const cls) {
// version field must be 0
// className must be pure ASCII string, non-null
    __CFSpinLock(&__CFBigRuntimeFunnel);
    if (__CFMaxRuntimeTypes <= __CFRuntimeClassTableCount) {
	CFLog(kCFLogLevelWarning, CFSTR("*** CoreFoundation class table full; registration failing for class '%s'.  Program will crash soon."), cls->className);
        __CFSpinUnlock(&__CFBigRuntimeFunnel);
	return _kCFRuntimeNotATypeID;
    }
    if (__CFRuntimeClassTableSize <= __CFRuntimeClassTableCount) {
	int32_t old_size = __CFRuntimeClassTableSize;
	int32_t new_size = __CFRuntimeClassTableSize * 4;

	void *new_table1 = calloc(new_size, sizeof(CFRuntimeClass *));
	memmove(new_table1, __CFRuntimeClassTable, old_size * sizeof(CFRuntimeClass *));
	__CFRuntimeClassTable = (CFRuntimeClass**)new_table1;

	void *new_table2 = calloc(new_size, sizeof(uintptr_t));
	memmove(new_table2, __CFRuntimeObjCClassTable, old_size * sizeof(uintptr_t));
        for (CFIndex idx = old_size; idx < new_size; idx++) {
            ((uintptr_t *)new_table2)[idx] = __CFRuntimeObjCClassTable[0];
        }
	__CFRuntimeObjCClassTable = (uintptr_t *)new_table2;

	__CFRuntimeClassTableSize = new_size;
	// The old value of __CFRuntimeClassTable is intentionally leaked
	// for thread-safety reasons:
	// other threads might have loaded the value of that, in functions here
	// in this file executing in other threads, and may attempt to use it after
	// this thread gets done reallocating here, so freeing is unsafe. We
	// don't want to pay the expense of locking around all uses of these variables.
	// The old value of __CFRuntimeObjCClassTable is intentionally leaked
	// for thread-safety reasons:
	// other threads might have loaded the value of that, since it is
	// accessible via CFBridgingPriv.h, and may attempt to use it after
	// this thread gets done reallocating here, so freeing is unsafe.
    }
    __CFRuntimeClassTable[__CFRuntimeClassTableCount++] = (CFRuntimeClass *)cls;
    CFTypeID typeID = __CFRuntimeClassTableCount - 1;
    __CFSpinUnlock(&__CFBigRuntimeFunnel);
    return typeID;
}

void _CFRuntimeBridgeClasses(CFTypeID cf_typeID, const char *objc_classname) {
    __CFSpinLock(&__CFBigRuntimeFunnel);
    __CFRuntimeObjCClassTable[cf_typeID] = (uintptr_t)objc_getFutureClass(objc_classname);
    __CFSpinUnlock(&__CFBigRuntimeFunnel);
}

const CFRuntimeClass * _CFRuntimeGetClassWithTypeID(CFTypeID typeID) {
    return __CFRuntimeClassTable[typeID]; // hopelessly unthreadsafe
}

void _CFRuntimeUnregisterClassWithTypeID(CFTypeID typeID) {
    __CFSpinLock(&__CFBigRuntimeFunnel);
    __CFRuntimeClassTable[typeID] = NULL;
    __CFSpinUnlock(&__CFBigRuntimeFunnel);
}


#if defined(DEBUG) || defined(ENABLE_ZOMBIES)

/* CFZombieLevel levels:
 *	bit 0: scribble deallocated CF object memory
 *	bit 1: do not scribble on CFRuntimeBase header (when bit 0)
 *	bit 4: do not free CF objects
 *	bit 7: use 3rd-order byte as scribble byte for dealloc (otherwise 0xFC)
 */

static uint32_t __CFZombieLevel = 0x0;
__private_extern__ uint8_t __CFZombieEnabled = 0;
__private_extern__ uint8_t __CFDeallocateZombies = 0;
#if !__OBJC2__
static void *_original_objc_dealloc = 0;
#endif

void _CFEnableZombies(void) {
    __CFZombieEnabled = 0xFF;
}

#endif /* DEBUG */

// XXX_PCB:  use the class version field as a bitmask, to allow classes to opt-in for GC scanning.

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
CF_INLINE CFOptionFlags CF_GET_COLLECTABLE_MEMORY_TYPE(const CFRuntimeClass *cls)
{
    return ((cls->version & _kCFRuntimeScannedObject) ? __kCFAllocatorGCScannedMemory : 0) | __kCFAllocatorGCObjectMemory;
}
#else
#define CF_GET_COLLECTABLE_MEMORY_TYPE(x) (0)
#endif

CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, CFIndex extraBytes, unsigned char *category) {
    CFAssert1(typeID != _kCFRuntimeNotATypeID, __kCFLogAssertion, "%s(): Uninitialized type id", __PRETTY_FUNCTION__);
    CFRuntimeClass *cls = __CFRuntimeClassTable[typeID];
    if (NULL == cls) {
	return NULL;
    }
    allocator = (NULL == allocator) ? __CFGetDefaultAllocator() : allocator;
    if (kCFAllocatorNull == allocator) {
	return NULL;
    }
    Boolean usesSystemDefaultAllocator = (allocator == kCFAllocatorSystemDefault);
    CFIndex size = sizeof(CFRuntimeBase) + extraBytes + (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef));
    size = (size + 0xF) & ~0xF;	// CF objects are multiples of 16 in size
    // CFType version 0 objects are unscanned by default since they don't have write-barriers and hard retain their innards
    // CFType version 1 objects are scanned and use hand coded write-barriers to store collectable storage within
    CFRuntimeBase *memory = (CFRuntimeBase *)CFAllocatorAllocate(allocator, size, CF_GET_COLLECTABLE_MEMORY_TYPE(cls));
    if (NULL == memory) {
        CFStringRef msg = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("Attempt to allocate %ld bytes for %s failed"), size, category ? (char *)category : (char *)cls->className);
	{
            CFLog(kCFLogLevelCritical, CFSTR("%@"), msg);
            HALT;
        }
        CFRelease(msg);
	return NULL;
    }
    if (!kCFUseCollectableAllocator || !CF_IS_COLLECTABLE_ALLOCATOR(allocator) || !(CF_GET_COLLECTABLE_MEMORY_TYPE(cls) & __kCFAllocatorGCScannedMemory)) {
	memset(memory, 0, size);
    }
    if (__CFOASafe && category) {
	__CFSetLastAllocationEventName(memory, (char *)category);
    } else if (__CFOASafe) {
	__CFSetLastAllocationEventName(memory, (char *)cls->className);
    }
    if (!usesSystemDefaultAllocator) {
        // add space to hold allocator ref for non-standard allocators.
        // (this screws up 8 byte alignment but seems to work)
	*(CFAllocatorRef *)((char *)memory) = (CFAllocatorRef)CFRetain(allocator);
	memory = (CFRuntimeBase *)((char *)memory + sizeof(CFAllocatorRef));
    }
    memory->_cfisa = __CFISAForTypeID(typeID);
#if __LP64__
    *(uint32_t *)(memory->_cfinfo) = (uint32_t)((0 << 24) + ((typeID & 0xFFFF) << 8) + (usesSystemDefaultAllocator ? 0x80 : 0x00));
    memory->_rc = 1;
#else
    *(uint32_t *)(memory->_cfinfo) = (uint32_t)((1 << 24) + ((typeID & 0xFFFF) << 8) + (usesSystemDefaultAllocator ? 0x80 : 0x00));
#endif
    if (NULL != cls->init) {
	(cls->init)(memory);
    }
    return memory;
}

void _CFRuntimeInitStaticInstance(void *ptr, CFTypeID typeID) {
    CFAssert1(typeID != _kCFRuntimeNotATypeID, __kCFLogAssertion, "%s(): Uninitialized type id", __PRETTY_FUNCTION__);
    if (NULL == __CFRuntimeClassTable[typeID]) {
	return;
    }
    CFRuntimeBase *memory = (CFRuntimeBase *)ptr;
    memory->_cfisa = __CFISAForTypeID(typeID);
    *(uint32_t *)(memory->_cfinfo) = (uint32_t)((0 << 24) + ((typeID & 0xFFFF) << 8) + 0x80);
#if __LP64__
    memory->_rc = 0;
#endif
    if (NULL != __CFRuntimeClassTable[typeID]->init) {
	(__CFRuntimeClassTable[typeID]->init)(memory);
    }
}

void _CFRuntimeSetInstanceTypeID(CFTypeRef cf, CFTypeID typeID) {
    *(uint16_t *)(((CFRuntimeBase *)cf)->_cfinfo + 1) = (uint16_t)(typeID & 0xFFFF);
}

__private_extern__ Boolean __CFRuntimeIsFreedObject(id anObject) {
   if (!anObject) return false;
   static Class freedClass = Nil;
   if (!freedClass) freedClass = _objc_getFreedObjectClass();
   Class cls = object_getClass(anObject);
   if (cls == freedClass) return true;
   // in 64-bit, a future class has nil isa, and calling class_getName() on
   // such will crash so we do this test; zombie classes are not future classes
   if (object_getClass((id)cls) == nil) return false;
   const char *cname = class_getName(cls);
   if (cname && 0 == strncmp(cname, "_NSZombie_", 10)) return true;
   return false;
}


enum {
    __kCFObjectRetainedEvent = 12,
    __kCFObjectReleasedEvent = 13
};

#if DEPLOYMENT_TARGET_MACOSX
#define NUM_EXTERN_TABLES 8
#define EXTERN_TABLE_IDX(O) (((uintptr_t)(O) >> 8) & 0x7)
#elif DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS
#define NUM_EXTERN_TABLES 1
#define EXTERN_TABLE_IDX(O) 0
#else
#error
#endif

// we disguise pointers so that programs like 'leaks' forget about these references
#define DISGUISE(O) (~(uintptr_t)(O))

static struct {
    CFSpinLock_t lock;
    CFBasicHashRef table;
    uint8_t padding[64 - sizeof(CFBasicHashRef) - sizeof(CFSpinLock_t)];
} __NSRetainCounters[NUM_EXTERN_TABLES];

CF_EXPORT uintptr_t __CFDoExternRefOperation(uintptr_t op, id obj) {
    if (nil == obj) HALT;
    uintptr_t idx = EXTERN_TABLE_IDX(obj);
    uintptr_t disguised = DISGUISE(obj);
#if DEPLOYMENT_TARGET_WINDOWS
    // assume threaded on windows for now
    int thr = 1;
#else
    int thr = pthread_is_threaded_np();
#endif
    CFSpinLock_t *lock = &__NSRetainCounters[idx].lock;
    CFBasicHashRef table = __NSRetainCounters[idx].table;
    uintptr_t count;
    switch (op) {
    case 300:   // increment
    case 350:   // increment, no event
        if (thr) __CFSpinLock(lock);
	CFBasicHashAddValue(table, disguised, disguised);
        if (thr) __CFSpinUnlock(lock);
        if (__CFOASafe && op != 350) __CFRecordAllocationEvent(__kCFObjectRetainedEvent, obj, 0, 0, NULL);
        return (uintptr_t)obj;
    case 400:   // decrement
        if (__CFOASafe) __CFRecordAllocationEvent(__kCFObjectReleasedEvent, obj, 0, 0, NULL);
    case 450:   // decrement, no event
        if (thr) __CFSpinLock(lock);
        count = (uintptr_t)CFBasicHashRemoveValue(table, disguised);
        if (thr) __CFSpinUnlock(lock);
        return 0 == count;
    case 500:
        if (thr) __CFSpinLock(lock);
        count = (uintptr_t)CFBasicHashGetCountOfKey(table, disguised);
        if (thr) __CFSpinUnlock(lock);
        return count;
    }
    return 0;
}


CFTypeID __CFGenericTypeID(const void *cf) {
    return (*(uint32_t *)(((CFRuntimeBase *)cf)->_cfinfo) >> 8) & 0xFFFF;
}

CF_INLINE CFTypeID __CFGenericTypeID_inline(const void *cf) {
    return (*(uint32_t *)(((CFRuntimeBase *)cf)->_cfinfo) >> 8) & 0xFFFF;
}

CFTypeID CFTypeGetTypeID(void) {
    return __kCFTypeTypeID;
}

__private_extern__ void __CFGenericValidateType_(CFTypeRef cf, CFTypeID type, const char *func) {
    if (cf && CF_IS_OBJC(type, cf)) return;
    CFAssert2((cf != NULL) && (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]) && (__kCFNotATypeTypeID != __CFGenericTypeID_inline(cf)) && (__kCFTypeTypeID != __CFGenericTypeID_inline(cf)), __kCFLogAssertion, "%s(): pointer %p is not a CF object", func, cf); \
    CFAssert3(__CFGenericTypeID_inline(cf) == type, __kCFLogAssertion, "%s(): pointer %p is not a %s", func, cf, __CFRuntimeClassTable[type]->className);	\
}

#define __CFGenericAssertIsCF(cf) \
    CFAssert2(cf != NULL && (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]) && (__kCFNotATypeTypeID != __CFGenericTypeID_inline(cf)) && (__kCFTypeTypeID != __CFGenericTypeID_inline(cf)), __kCFLogAssertion, "%s(): pointer %p is not a CF object", __PRETTY_FUNCTION__, cf);


#define CFTYPE_IS_OBJC(obj) (false)
#define CFTYPE_OBJC_FUNCDISPATCH0(rettype, obj, sel) do {} while (0)
#define CFTYPE_OBJC_FUNCDISPATCH1(rettype, obj, sel, a1) do {} while (0)


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
    return CFStringCreateWithCString(kCFAllocatorSystemDefault, __CFRuntimeClassTable[type]->className, kCFStringEncodingASCII);
}

// Bit 31 (highest bit) in second word of cf instance indicates external ref count

CF_EXPORT void _CFRelease(CFTypeRef cf);
CF_EXPORT CFTypeRef _CFRetain(CFTypeRef cf);

CFTypeRef CFRetain(CFTypeRef cf) {
    if (NULL == cf) HALT;
    if (CF_IS_COLLECTABLE(cf)) {
       if (CFTYPE_IS_OBJC(cf)) {
           // always honor CFRetain's with a GC-visible retain.
           auto_zone_retain(auto_zone(), (void*)cf);
           return cf;
       } else {
           // special case CF objects for performance.
           return _CFRetain(cf);
       }
    }
    CFTYPE_OBJC_FUNCDISPATCH0(CFTypeRef, cf, "retain");
    if (cf) __CFGenericAssertIsCF(cf);
    return _CFRetain(cf);
}

__private_extern__ void __CFAllocatorDeallocate(CFTypeRef cf);

void CFRelease(CFTypeRef cf) {
    if (NULL == cf) HALT;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if (CF_IS_COLLECTABLE(cf)) {
        if (CFTYPE_IS_OBJC(cf)) {
            // release the GC-visible reference.
            auto_zone_release(auto_zone(), (void*)cf);
        } else {
            // special-case CF objects for better performance.
            _CFRelease(cf);
        }
        return;
    }
#endif
#if 0
    void **addrs[2] = {&&start, &&end};
    start:;
    if (addrs[0] <= __builtin_return_address(0) && __builtin_return_address(0) <= addrs[1]) {
	CFLog(3, CFSTR("*** WARNING: Recursion in CFRelease(%p) : %p '%s' : 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx"), cf, object_getClass(cf), object_getClassName(cf), ((uintptr_t *)cf)[0], ((uintptr_t *)cf)[1], ((uintptr_t *)cf)[2], ((uintptr_t *)cf)[3], ((uintptr_t *)cf)[4], ((uintptr_t *)cf)[5]);
	HALT;
    }
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(void, cf, "release");
    if (cf) __CFGenericAssertIsCF(cf);
    _CFRelease(cf);
#if 0
    end:;
#endif
}


__private_extern__ const void *__CFStringCollectionCopy(CFAllocatorRef allocator, const void *ptr) {
    if (NULL == ptr) HALT;
    CFStringRef theString = (CFStringRef)ptr;
    CFStringRef result = CFStringCreateCopy(allocator, theString);
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        result = (CFStringRef)CFMakeCollectable(result);
    }
    return (const void *)result;
}

extern void CFCollection_non_gc_storage_error(void);

__private_extern__ const void *__CFTypeCollectionRetain(CFAllocatorRef allocator, const void *ptr) {
    if (NULL == ptr) HALT;
    CFTypeRef cf = (CFTypeRef)ptr;
    // only collections allocated in the GC zone can opt-out of reference counting.
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        if (CFTYPE_IS_OBJC(cf)) return cf;  // do nothing for OBJC objects.
        if (auto_zone_is_valid_pointer(auto_zone(), ptr)) {
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)];
            if (cfClass->version & _kCFRuntimeResourcefulObject) {
                // GC: If this a CF object in the GC heap that is marked resourceful, then
                // it must be retained keep it alive in a CF collection.
                // We're basically inlining CFRetain() here, to avoid extra heap membership tests.
                _CFRetain(cf);
            }
            else
                ;   // don't retain normal CF objects
            return cf;
        } else {
            // support constant CFTypeRef objects.
#if __LP64__
            uint32_t lowBits = ((CFRuntimeBase *)cf)->_rc;
#else
            uint32_t lowBits = ((CFRuntimeBase *)cf)->_cfinfo[CF_RC_BITS];
#endif
            if (lowBits == 0) return cf;
            // complain about non-GC objects in GC containers.
            CFLog(kCFLogLevelWarning, CFSTR("storing a non-GC object %p in a GC collection, break on CFCollection_non_gc_storage_error to debug."), cf);
            CFCollection_non_gc_storage_error();
            // XXX should halt, except Patrick is using this somewhere.
            // HALT;
        }
    }
    return CFRetain(cf);
}


__private_extern__ void __CFTypeCollectionRelease(CFAllocatorRef allocator, const void *ptr) {
    if (NULL == ptr) HALT;
    CFTypeRef cf = (CFTypeRef)ptr;
    // only collections allocated in the GC zone can opt-out of reference counting.
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        if (CFTYPE_IS_OBJC(cf)) return; // do nothing for OBJC objects.
        if (auto_zone_is_valid_pointer(auto_zone(), cf)) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
            // GC: If this a CF object in the GC heap that is marked uncollectable, then
            // must balance the retain done in __CFTypeCollectionRetain().
            // We're basically inlining CFRelease() here, to avoid extra heap membership tests.
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)];
            if (cfClass->version & _kCFRuntimeResourcefulObject) {
                // reclaim is called by _CFRelease(), which must be called to keep the
                // CF and GC retain counts in sync.
                _CFRelease(cf);
            } else {
                // avoid releasing normal CF objects.  Like other collections, for example
            }
            return;
#endif
        } else {
            // support constant CFTypeRef objects.
#if __LP64__
            uint32_t lowBits = ((CFRuntimeBase *)cf)->_rc;
#else
            uint32_t lowBits = ((CFRuntimeBase *)cf)->_cfinfo[CF_RC_BITS];
#endif
            if (lowBits == 0) return;
        }
    }
    CFRelease(cf);
}

#if !__LP64__
static CFSpinLock_t __CFRuntimeExternRefCountTableLock = CFSpinLockInit;
static CFMutableBagRef __CFRuntimeExternRefCountTable = NULL;
#endif

static uint64_t __CFGetFullRetainCount(CFTypeRef cf) {
    if (NULL == cf) HALT;
#if __LP64__
    uint32_t lowBits = ((CFRuntimeBase *)cf)->_rc;
    if (0 == lowBits) {
        return (uint64_t)0x0fffffffffffffffULL;
    }
    return lowBits;
#else
    uint32_t lowBits = ((CFRuntimeBase *)cf)->_cfinfo[CF_RC_BITS];
    if (0 == lowBits) {
        return (uint64_t)0x0fffffffffffffffULL;
    }
    uint64_t highBits = 0;
    if ((lowBits & 0x80) != 0) {
        highBits = __CFDoExternRefOperation(500, (id)cf);
    }
    uint64_t compositeRC = (lowBits & 0x7f) + (highBits << 6);
    return compositeRC;
#endif
}

CFTypeRef _CFRetainGC(CFTypeRef cf) {
#if defined(DEBUG)
    if (kCFUseCollectableAllocator && !CF_IS_COLLECTABLE(cf)) {
        fprintf(stderr, "non-auto object %p passed to _CFRetainGC.\n", cf);
        HALT;
    }
#endif
    return kCFUseCollectableAllocator ? cf : CFRetain(cf);
}

void _CFReleaseGC(CFTypeRef cf) {
#if defined(DEBUG)
    if (kCFUseCollectableAllocator && !CF_IS_COLLECTABLE(cf)) {
        fprintf(stderr, "non-auto object %p passed to _CFReleaseGC.\n", cf);
        HALT;
    }
#endif
    if (!kCFUseCollectableAllocator) CFRelease(cf);
}

CFIndex CFGetRetainCount(CFTypeRef cf) {
    if (NULL == cf) HALT;
    if (CF_IS_COLLECTABLE(cf)) {
        if (CFTYPE_IS_OBJC(cf)) return auto_zone_retain_count(auto_zone(), cf);
    } else {
	CFTYPE_OBJC_FUNCDISPATCH0(CFIndex, cf, "retainCount");
	__CFGenericAssertIsCF(cf);
    }
    uint64_t rc = __CFGetFullRetainCount(cf);
    return (rc < (uint64_t)LONG_MAX) ? (CFIndex)rc : (CFIndex)LONG_MAX;
}

CFTypeRef CFMakeCollectable(CFTypeRef cf) {
    if (NULL == cf) return NULL;
    if (CF_IS_COLLECTABLE(cf)) {
        objc_assertRegisteredThreadWithCollector();
#if defined(DEBUG)
        CFAllocatorRef allocator = CFGetAllocator(cf);
        if (!CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
            CFLog(kCFLogLevelWarning, CFSTR("object %p with non-GC allocator %p passed to CFMakeCollectable."), cf, allocator);
            HALT;
        }
#endif
        if (!CFTYPE_IS_OBJC(cf)) {
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)];
            if (cfClass->version & (_kCFRuntimeResourcefulObject)) {
                // don't allow the collector to manage uncollectable objects.
                CFLog(kCFLogLevelWarning, CFSTR("uncollectable object %p passed to CFMakeCollectable."), cf);
                HALT;
            }
        }
        if (CFGetRetainCount(cf) == 0) {
            CFLog(kCFLogLevelWarning, CFSTR("object %p with 0 retain-count passed to CFMakeCollectable."), cf);
            return cf;
        }
        CFRelease(cf);
    }
    return cf;
}

CFTypeRef CFMakeUncollectable(CFTypeRef cf) {
    if (NULL == cf) return NULL;
    if (CF_IS_COLLECTABLE(cf)) {
        CFRetain(cf);
    }
    return cf;
}

Boolean CFEqual(CFTypeRef cf1, CFTypeRef cf2) {
    if (NULL == cf1) HALT;
    if (NULL == cf2) HALT;
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
    if (NULL == cf) HALT;
    CFTYPE_OBJC_FUNCDISPATCH0(CFHashCode, cf, "hash");
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash(cf);
    }
    return (CFHashCode)cf;
}

// definition: produces a normally non-NULL debugging description of the object
CFStringRef CFCopyDescription(CFTypeRef cf) {
    if (NULL == cf) HALT;
    if (CFTYPE_IS_OBJC(cf)) {
        static SEL s = NULL;
        CFStringRef (*func)(void *, SEL) = (CFStringRef (*)(void *, SEL))objc_msgSend;
        if (!s) s = sel_registerName("_copyDescription");
        CFStringRef result = func((void *)cf, s);
        return result;
    }
    // CFTYPE_OBJC_FUNCDISPATCH0(CFStringRef, cf, "_copyDescription");  // XXX returns 0 refcounted item under GC
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyDebugDesc) {
	CFStringRef result;
	result = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyDebugDesc(cf);
	if (NULL != result) return result;
    }
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<%s %p [%p]>"), __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->className, cf, CFGetAllocator(cf));
}

// Definition: if type produces a formatting description, return that string, otherwise NULL
__private_extern__ CFStringRef __CFCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    if (NULL == cf) HALT;
    if (CFTYPE_IS_OBJC(cf)) {
	static SEL s = NULL, r = NULL;
	CFStringRef (*func)(void *, SEL, CFDictionaryRef) = (CFStringRef (*)(void *, SEL, CFDictionaryRef))objc_msgSend;
	BOOL (*rfunc)(void *, SEL, SEL) = (BOOL (*)(void *, SEL, SEL))objc_msgSend;
	if (!s) s = sel_registerName("_copyFormattingDescription:");
	if (!r) r = sel_registerName("respondsToSelector:");
	if (s && rfunc((void *)cf, r, s)) return func((void *)cf, s, formatOptions);
	return NULL;
    }
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc(cf, formatOptions);
    }
    return NULL;
}

extern CFAllocatorRef __CFAllocatorGetAllocator(CFTypeRef);

CFAllocatorRef CFGetAllocator(CFTypeRef cf) {
    if (NULL == cf) return kCFAllocatorSystemDefault;
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
extern void __CFBooleanInitialize(void);
extern void __CFCharacterSetInitialize(void);
extern void __CFDataInitialize(void);
extern void __CFNumberInitialize(void);
extern void __CFStorageInitialize(void);
extern void __CFErrorInitialize(void);
extern void __CFTreeInitialize(void);
extern void __CFURLInitialize(void);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
extern void __CFMachPortInitialize(void);
#endif
extern void __CFMessagePortInitialize(void);
extern void __CFRunLoopInitialize(void);
extern void __CFRunLoopObserverInitialize(void);
extern void __CFRunLoopSourceInitialize(void);
extern void __CFRunLoopTimerInitialize(void);
extern void __CFBundleInitialize(void);
extern void __CFPlugInInitialize(void);
extern void __CFPlugInInstanceInitialize(void);
extern void __CFUUIDInitialize(void);
extern void __CFBinaryHeapInitialize(void);
extern void __CFBitVectorInitialize(void);
#if DEPLOYMENT_TARGET_WINDOWS
extern void __CFWindowsMessageQueueInitialize(void);
extern void __CFWindowsNamedPipeInitialize(void);
extern void __CFBaseCleanup(void);
#endif
extern void __CFStreamInitialize(void);

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
__private_extern__ uint8_t __CF120290 = false;
__private_extern__ uint8_t __CF120291 = false;
__private_extern__ uint8_t __CF120293 = false;
__private_extern__ char * __crashreporter_info__ = NULL;
asm(".desc ___crashreporter_info__, 0x10");

static void __01121__(void) {
    __CF120291 = pthread_is_threaded_np() ? true : false;
}

static void __01123__(void) {
    // Ideally, child-side atfork handlers should be async-cancel-safe, as fork()
    // is async-cancel-safe and can be called from signal handlers.  See also
    // http://standards.ieee.org/reading/ieee/interp/1003-1c-95_int/pasc-1003.1c-37.html
    // This is not a problem for CF.
    if (__CF120290) {
	__CF120293 = true;
	if (__CF120291) {
	    __crashreporter_info__ = "*** multi-threaded process forked ***";
	} else {
	    __crashreporter_info__ = "*** single-threaded process forked ***";
	}
    }
}

#define EXEC_WARNING_STRING_1 "The process has forked and you cannot use this CoreFoundation functionality safely. You MUST exec().\n"
#define EXEC_WARNING_STRING_2 "Break on __THE_PROCESS_HAS_FORKED_AND_YOU_CANNOT_USE_THIS_COREFOUNDATION_FUNCTIONALITY___YOU_MUST_EXEC__() to debug.\n"

__private_extern__ void __THE_PROCESS_HAS_FORKED_AND_YOU_CANNOT_USE_THIS_COREFOUNDATION_FUNCTIONALITY___YOU_MUST_EXEC__(void) {
    write(2, EXEC_WARNING_STRING_1, sizeof(EXEC_WARNING_STRING_1) - 1);
    write(2, EXEC_WARNING_STRING_2, sizeof(EXEC_WARNING_STRING_2) - 1);
//    HALT;
}
#endif


CF_EXPORT const void *__CFArgStuff;
const void *__CFArgStuff = NULL;
__private_extern__ void *__CFAppleLanguages = NULL;

static struct {
    const char *name;
    const char *value;
} __CFEnv[] = {
    {"PATH", NULL},
    {"HOME", NULL},
    {"USER", NULL},
    {"HOMEPATH", NULL},
    {"HOMEDRIVE", NULL},
    {"USERNAME", NULL},
    {"TZFILE", NULL},
    {"TZ", NULL},
    {"NEXT_ROOT", NULL},
    {"DYLD_IMAGE_SUFFIX", NULL},
    {"CFProcessPath", NULL},
    {"CFFIXED_USER_HOME", NULL},
    {"CFNETWORK_LIBRARY_PATH", NULL},
    {"CFUUIDVersionNumber", NULL},
    {"CFDebugNamedDataSharing", NULL},
    {"CFPropertyListAllowImmutableCollections", NULL},
    {"CFBundleUseDYLD", NULL},
    {"CFBundleDisableStringsSharing", NULL},
    {"CFCharacterSetCheckForExpandedSet", NULL},
    {"__CF_DEBUG_EXPANDED_SET", NULL},
    {"CFStringDisableROM", NULL},
    {"CF_CHARSET_PATH", NULL},
    {"__CF_USER_TEXT_ENCODING", NULL},
    {NULL, NULL}, // the last one is for optional "COMMAND_MODE" "legacy", do not use this slot, insert before
};

__private_extern__ const char *__CFgetenv(const char *n) {
    for (CFIndex idx = 0; idx < sizeof(__CFEnv) / sizeof(__CFEnv[0]); idx++) {
	if (__CFEnv[idx].name && 0 == strcmp(n, __CFEnv[idx].name)) return __CFEnv[idx].value;
    }
    return getenv(n);
}

#if DEPLOYMENT_TARGET_WINDOWS
#define kNilPthreadT  { nil, nil }
#else
#define kNilPthreadT  (pthread_t)0
#endif


CF_EXPORT pthread_t _CFMainPThread;
pthread_t _CFMainPThread = kNilPthreadT;

CF_EXPORT bool kCFUseCollectableAllocator = false;

__private_extern__ Boolean __CFProphylacticAutofsAccess = false;

#if DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
static void __CFInitialize(void) __attribute__ ((constructor));
static
#endif
#if DEPLOYMENT_TARGET_WINDOWS
CF_EXPORT
#endif
void __CFInitialize(void) {
    static int __done = 0;

    if (!__done) {
        __done = 1;

	if (!pthread_main_np()) HALT;	// CoreFoundation must be initialized on the main thread

	_CFMainPThread = pthread_self();

        __CFProphylacticAutofsAccess = true;

	for (CFIndex idx = 0; idx < sizeof(__CFEnv) / sizeof(__CFEnv[0]); idx++) {
	    __CFEnv[idx].value = __CFEnv[idx].name ? getenv(__CFEnv[idx].name) : NULL;
	}

        kCFUseCollectableAllocator = objc_collectingEnabled();
        if (kCFUseCollectableAllocator) {
            __CFObjCIsCollectable = (bool (*)(void *))objc_isAuto;
        }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	UInt32 s, r;
	__CFStringGetUserDefaultEncoding(&s, &r); // force the potential setenv to occur early
	pthread_atfork(__01121__, NULL, __01123__);
        const char *value2 = __CFgetenv("NSObjCMessageLoggingEnabled");
        if (value2 && (*value2 == 'Y' || *value2 == 'y')) instrumentObjcMessageSends(1);
#elif DEPLOYMENT_TARGET_WINDOWS
#else
#endif

#if defined(DEBUG) || defined(ENABLE_ZOMBIES)
	const char *value = __CFgetenv("NSZombieEnabled");
	if (value && (*value == 'Y' || *value == 'y')) __CFZombieEnabled = 0xff;
	value = __CFgetenv("NSDeallocateZombies");
	if (value && (*value == 'Y' || *value == 'y')) __CFDeallocateZombies = 0xff;
#if !__OBJC2__
	_original_objc_dealloc = (void *)_dealloc;
#endif

	value = __CFgetenv("CFZombieLevel");
	if (NULL != value) {
	    __CFZombieLevel = (uint32_t)strtoul_l(value, NULL, 0, NULL);
	}
	if (0x0 == __CFZombieLevel) __CFZombieLevel = 0x0000FC00; // default

#endif

        __CFRuntimeClassTableSize = 1024;
        __CFRuntimeClassTable = (CFRuntimeClass **)calloc(__CFRuntimeClassTableSize, sizeof(CFRuntimeClass *));
	__CFRuntimeObjCClassTable = (uintptr_t *)calloc(__CFRuntimeClassTableSize, sizeof(uintptr_t));
        __CFBaseInitialize();

        _CFRuntimeBridgeClasses(0, "__NSCFType");
        for (CFIndex idx = 1; idx < __CFRuntimeClassTableSize; idx++) {
            __CFRuntimeObjCClassTable[idx] = __CFRuntimeObjCClassTable[0];
        }

        /* Here so that two runtime classes get indices 0, 1. */
        __kCFNotATypeTypeID = _CFRuntimeRegisterClass(&__CFNotATypeClass);
        __kCFTypeTypeID = _CFRuntimeRegisterClass(&__CFTypeClass);

        /* Here so that __kCFAllocatorTypeID gets index 2. */
        __CFAllocatorInitialize();

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        {
            CFIndex idx, cnt;
            char **args = *_NSGetArgv();
            cnt = *_NSGetArgc();
            for (idx = 1; idx < cnt - 1; idx++) {
                if (NULL == args[idx]) continue;
                if (0 == strcmp(args[idx], "-AppleLanguages") && args[idx + 1]) {
                    CFIndex length = strlen(args[idx + 1]);
                    __CFAppleLanguages = malloc(length + 1);
                    memmove(__CFAppleLanguages, args[idx + 1], length + 1);
                    break;
                }
            }
        }
#endif


        CFBasicHashGetTypeID();
        CFBagGetTypeID();
#if !__LP64__
        // Creating this lazily in CFRetain causes recursive call to CFRetain
        __CFRuntimeExternRefCountTable = CFBagCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
#endif

	for (CFIndex idx = 0; idx < NUM_EXTERN_TABLES; idx++) {
	    __NSRetainCounters[idx].table = CFBasicHashCreate(kCFAllocatorSystemDefault, kCFBasicHashHasCounts | kCFBasicHashLinearHashing | kCFBasicHashAggressiveGrowth, &CFBasicHashNullCallbacks);
	    CFBasicHashSetCapacity(__NSRetainCounters[idx].table, 40);
	    __NSRetainCounters[idx].lock = CFSpinLockInit;
	}

        /*** _CFRuntimeCreateInstance() can finally be called generally after this line. ***/

	__CFRuntimeClassTableCount = 7;
	__CFStringInitialize();		// CFString's TypeID must be 0x7, now and forever
	__CFRuntimeClassTableCount = 16;
        CFSetGetTypeID();		// See above for hard-coding of this position
        CFDictionaryGetTypeID();	// See above for hard-coding of this position
        __CFArrayInitialize();		// See above for hard-coding of this position
        __CFDataInitialize();		// See above for hard-coding of this position
        __CFNullInitialize();		// See above for hard-coding of this position
        __CFBooleanInitialize();	// See above for hard-coding of this position
        __CFNumberInitialize();		// See above for hard-coding of this position

        __CFBinaryHeapInitialize();
        __CFBitVectorInitialize();
        __CFCharacterSetInitialize();
        __CFStorageInitialize();
        __CFErrorInitialize();
        __CFTreeInitialize();
        __CFURLInitialize();
        __CFBundleInitialize();
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        __CFPlugInInitialize();
        __CFPlugInInstanceInitialize();
#endif
        __CFUUIDInitialize();
	__CFMessagePortInitialize();
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        __CFMachPortInitialize();
#endif
        __CFStreamInitialize();
        __CFRunLoopInitialize();
        __CFRunLoopObserverInitialize();
        __CFRunLoopSourceInitialize();
        __CFRunLoopTimerInitialize();


        {
            CFIndex idx, cnt;
        char **args;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
            args = *_NSGetArgv();
            cnt = *_NSGetArgc();
#elif DEPLOYMENT_TARGET_WINDOWS
            wchar_t *commandLine = GetCommandLineW();
            wchar_t **wideArgs = CommandLineToArgvW(commandLine, (int *)&cnt);
            args = (char **)malloc(sizeof(char *) * cnt);
            for (int y=0; y < cnt; y++) {
                int bufSize = lstrlenW(wideArgs[y]) + 20;
                char *arg = (char *)malloc(sizeof(char) * bufSize);
                int res = WideCharToMultiByte(CP_ACP, 1024 /*WC_NO_BEST_FIT_CHARS*/, wideArgs[y], -1, arg, bufSize, NULL, NULL);
                if (!res)
                    printf("CF - Error converting command line arg string to ascii: %x\n", (unsigned int)wideArgs[y]);
                args[y] = arg;
            }
#endif
            CFIndex count;
            CFStringRef *list, buffer[256];
            list = (cnt <= 256) ? buffer : (CFStringRef *)malloc(cnt * sizeof(CFStringRef));
            for (idx = 0, count = 0; idx < cnt; idx++) {
                if (NULL == args[idx]) continue;
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

        _CFProcessPath();	// cache this early


#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        __CFOAInitialize();
#endif

        if (__CFRuntimeClassTableCount < 256) __CFRuntimeClassTableCount = 256;
	__CFSendObjCMsg = (void *(*)(const void *, SEL, ...))objc_msgSend;

#if DEPLOYMENT_TARGET_MACOSX
#elif DEPLOYMENT_TARGET_WINDOWS || DEPLOYMENT_TARGET_EMBEDDED
#else
#error
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	if (!_CFExecutableLinkedOnOrAfter(CFSystemVersionLeopard)) {
	    setenv("COMMAND_MODE", "legacy", 1);
	    __CFEnv[sizeof(__CFEnv) / sizeof(__CFEnv[0]) - 1].name = "COMMAND_MODE";
	    __CFEnv[sizeof(__CFEnv) / sizeof(__CFEnv[0]) - 1].value = "legacy";
	}
#elif DEPLOYMENT_TARGET_WINDOWS
#else
#error
#endif

#if defined(DEBUG) && (DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED)
        CFLog(kCFLogLevelWarning, CFSTR("Assertions enabled"));
#endif

        __CFProphylacticAutofsAccess = false;
    }
}


#if DEPLOYMENT_TARGET_WINDOWS

static CFBundleRef RegisterCoreFoundationBundle(void) {
#ifdef _DEBUG
    // might be nice to get this from the project file at some point
    wchar_t *DLLFileName = (wchar_t *)L"CoreFoundation_debug.dll";
#else
    wchar_t *DLLFileName = (wchar_t *)L"CoreFoundation.dll";
#endif
    wchar_t path[MAX_PATH+1];
    path[0] = path[1] = 0;
    DWORD wResult;
    CFIndex idx;
    HMODULE ourModule = GetModuleHandleW(DLLFileName);

    CFAssert(ourModule, __kCFLogAssertion, "GetModuleHandle failed");

    wResult = GetModuleFileNameW(ourModule, path, MAX_PATH+1);
    CFAssert1(wResult > 0, __kCFLogAssertion, "GetModuleFileName failed: %d", GetLastError());
    CFAssert1(wResult < MAX_PATH+1, __kCFLogAssertion, "GetModuleFileName result truncated: %s", path);

    // strip off last component, the DLL name
    for (idx = wResult - 1; idx; idx--) {
        if ('\\' == path[idx]) {
            path[idx] = '\0';
            break;
        }
    }

    CFStringRef fsPath = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar*)path, idx);
    CFURLRef dllURL = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, fsPath, kCFURLWindowsPathStyle, TRUE);
    CFURLRef bundleURL = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, dllURL, CFSTR("CoreFoundation.resources"), TRUE);
    CFRelease(fsPath);
    CFRelease(dllURL);

    // this registers us so subsequent calls to CFBundleGetBundleWithIdentifier will succeed
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorSystemDefault, bundleURL);
    CFRelease(bundleURL);

    return bundle;
}


#define DLL_PROCESS_ATTACH   1
#define DLL_THREAD_ATTACH    2
#define DLL_THREAD_DETACH    3
#define DLL_PROCESS_DETACH   0

int DllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID pReserved ) {
    static CFBundleRef cfBundle = NULL;
    if (dwReason == DLL_PROCESS_ATTACH) {
	__CFInitialize();
        cfBundle = RegisterCoreFoundationBundle();
    } else if (dwReason == DLL_PROCESS_DETACH) {
        __CFStreamCleanup();
        __CFSocketCleanup();
        __CFUniCharCleanup();
        __CFBaseCleanup();
	// do these last
	if (cfBundle) CFRelease(cfBundle);
        __CFStringCleanup();
    } else if (dwReason == DLL_THREAD_DETACH) {
        __CFFinalizeThreadData(NULL);
    }
    return TRUE;
}

#endif

CF_EXPORT CFTypeRef _CFRetain(CFTypeRef cf) {
    if (NULL == cf) return NULL;
    Boolean didAuto = false;
#if __LP64__
    if (0 == ((CFRuntimeBase *)cf)->_rc && !CF_IS_COLLECTABLE(cf)) return cf;	// Constant CFTypeRef 
    uint32_t lowBits;
    do {
	lowBits = ((CFRuntimeBase *)cf)->_rc;
    } while (!OSAtomicCompareAndSwap32Barrier(lowBits, lowBits + 1, (int32_t *)&((CFRuntimeBase *)cf)->_rc));
    // GC:  0 --> 1 transition? then add a GC retain count, to root the object. we'll remove it on the 1 --> 0 transition.
    if (lowBits == 0 && CF_IS_COLLECTABLE(cf)) {
	auto_zone_retain(auto_zone(), (void*)cf);
	didAuto = true;
    }
#else
#define RC_START 24
#define RC_END 31
    volatile UInt32 *infoLocation = (UInt32 *)&(((CFRuntimeBase *)cf)->_cfinfo);
    CFIndex rcLowBits = __CFBitfieldGetValue(*infoLocation, RC_END, RC_START);
    if (__builtin_expect(0 == rcLowBits, 0) && !CF_IS_COLLECTABLE(cf)) return cf;	// Constant CFTypeRef
    bool success = 0;
    do {
        UInt32 initialCheckInfo = *infoLocation;
        UInt32 prospectiveNewInfo = initialCheckInfo; // don't want compiler to generate prospectiveNewInfo = *infoLocation.  This is why infoLocation is declared as a pointer to volatile memory.
        prospectiveNewInfo += (1 << RC_START);
        rcLowBits = __CFBitfieldGetValue(prospectiveNewInfo, RC_END, RC_START);
        if (__builtin_expect((rcLowBits & 0x7f) == 0, 0)) {
            /* Roll over another bit to the external ref count
             Real ref count = low 7 bits of info[CF_RC_BITS]  + external ref count << 6
             Bit 8 of low bits indicates that external ref count is in use.
             External ref count is shifted by 6 rather than 7 so that we can set the low
		bits to to 1100 0000 rather than 1000 0000.
		This prevents needing to access the external ref count for successive retains and releases
		when the composite retain count is right around a multiple of 1 << 7.
             */
            prospectiveNewInfo = initialCheckInfo;
            __CFBitfieldSetValue(prospectiveNewInfo, RC_END, RC_START, ((1 << 7) | (1 << 6)));
            __CFSpinLock(&__CFRuntimeExternRefCountTableLock);
            success = OSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
            if (__builtin_expect(success, 1)) {
                __CFDoExternRefOperation(350, (id)cf);
            }
            __CFSpinUnlock(&__CFRuntimeExternRefCountTableLock);
        } else {
            success = OSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
            // XXX_PCB:  0 --> 1 transition? then add a GC retain count, to root the object. we'll remove it on the 1 --> 0 transition.
            if (success && __CFBitfieldGetValue(initialCheckInfo, RC_END, RC_START) == 0 && CF_IS_COLLECTABLE(cf)) {
		auto_zone_retain(auto_zone(), (void*)cf);
		didAuto = true;
	    }
        }
    } while (__builtin_expect(!success, 0));
#endif
    if (!didAuto && __builtin_expect(__CFOASafe, 0)) {
	__CFRecordAllocationEvent(__kCFRetainEvent, (void *)cf, 0, 0, NULL);
    }
    return cf;
}

CF_EXPORT void _CFRelease(CFTypeRef cf) {
    CFTypeID typeID = __CFGenericTypeID_inline(cf);
    Boolean isAllocator = (__kCFAllocatorTypeID_CONST == typeID);
    Boolean didAuto = false;
#if __LP64__
    uint32_t lowBits;
    do {
	lowBits = ((CFRuntimeBase *)cf)->_rc;
	if (0 == lowBits) {
	    if (CF_IS_COLLECTABLE(cf)) auto_zone_release(auto_zone(), (void*)cf);
	    return;        // Constant CFTypeRef
	}
	if (1 == lowBits) {
	    // CANNOT WRITE ANY NEW VALUE INTO [CF_RC_BITS] UNTIL AFTER FINALIZATION
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[typeID];
            if (cfClass->version & _kCFRuntimeResourcefulObject && cfClass->reclaim != NULL) {
                cfClass->reclaim(cf);
            }
	    if (!CF_IS_COLLECTABLE(cf)) {
                void (*func)(CFTypeRef) = __CFRuntimeClassTable[typeID]->finalize;
	        if (NULL != func) {
		    func(cf);
	        }
	        // We recheck lowBits to see if the object has been retained again during
	        // the finalization process.  This allows for the finalizer to resurrect,
	        // but the main point is to allow finalizers to be able to manage the
	        // removal of objects from uniquing caches, which may race with other threads
	        // which are allocating (looking up and finding) objects from those caches,
	        // which (that thread) would be the thing doing the extra retain in that case.
	        if (isAllocator || OSAtomicCompareAndSwap32Barrier(1, 0, (int32_t *)&((CFRuntimeBase *)cf)->_rc)) {
		    goto really_free;
	        }
	    }
	}
    } while (!OSAtomicCompareAndSwap32Barrier(lowBits, lowBits - 1, (int32_t *)&((CFRuntimeBase *)cf)->_rc));
    if (lowBits == 1 && CF_IS_COLLECTABLE(cf)) {
        // GC:  release the collector's hold over the object, which will call the finalize function later on.
	auto_zone_release(auto_zone(), (void*)cf);
        didAuto = true;
    }
#else
    volatile UInt32 *infoLocation = (UInt32 *)&(((CFRuntimeBase *)cf)->_cfinfo);
    CFIndex rcLowBits = __CFBitfieldGetValue(*infoLocation, RC_END, RC_START);
    if (__builtin_expect(0 == rcLowBits, 0)) {
        if (CF_IS_COLLECTABLE(cf)) auto_zone_release(auto_zone(), (void*)cf);
        return;        // Constant CFTypeRef
    }
    bool success = 0;
    do {
        UInt32 initialCheckInfo = *infoLocation;
        rcLowBits = __CFBitfieldGetValue(initialCheckInfo, RC_END, RC_START);
        if (__builtin_expect(1 == rcLowBits, 0)) {
            // we think cf should be deallocated
	    // CANNOT WRITE ANY NEW VALUE INTO [CF_RC_BITS] UNTIL AFTER FINALIZATION
	    CFRuntimeClass *cfClass = __CFRuntimeClassTable[typeID];
	    if (cfClass->version & _kCFRuntimeResourcefulObject && cfClass->reclaim != NULL) {
		cfClass->reclaim(cf);
	    }
	    if (CF_IS_COLLECTABLE(cf)) {
                UInt32 prospectiveNewInfo = initialCheckInfo - (1 << RC_START);
                success = OSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
                // GC:  release the collector's hold over the object, which will call the finalize function later on.
                if (success) {
		    auto_zone_release(auto_zone(), (void*)cf);
		    didAuto = true;
		}
             } else {
		if (isAllocator) {
		    goto really_free;
		} else {
                    void (*func)(CFTypeRef) = __CFRuntimeClassTable[typeID]->finalize;
                    if (NULL != func) {
		        func(cf);
		    }
		    // We recheck rcLowBits to see if the object has been retained again during
		    // the finalization process.  This allows for the finalizer to resurrect,
		    // but the main point is to allow finalizers to be able to manage the
		    // removal of objects from uniquing caches, which may race with other threads
		    // which are allocating (looking up and finding) objects from those caches,
		    // which (that thread) would be the thing doing the extra retain in that case.
		    rcLowBits = __CFBitfieldGetValue(*infoLocation, RC_END, RC_START);
		    success = (1 == rcLowBits);
		    if (__builtin_expect(success, 1)) {
			goto really_free;
		    }
		}
            }
        } else {
            // not yet junk
            UInt32 prospectiveNewInfo = initialCheckInfo; // don't want compiler to generate prospectiveNewInfo = *infoLocation.  This is why infoLocation is declared as a pointer to volatile memory.
            if (__builtin_expect((1 << 7) == rcLowBits, 0)) {
                // Time to remove a bit from the external ref count
                __CFSpinLock(&__CFRuntimeExternRefCountTableLock);
                CFIndex rcHighBitsCnt = __CFDoExternRefOperation(500, (id)cf);
                if (1 == rcHighBitsCnt) {
                    __CFBitfieldSetValue(prospectiveNewInfo, RC_END, RC_START, (1 << 6) - 1);
                } else {
                    __CFBitfieldSetValue(prospectiveNewInfo, RC_END, RC_START, ((1 << 6) | (1 << 7)) - 1);
                }
                success = OSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
                if (__builtin_expect(success, 1)) {
		    __CFDoExternRefOperation(450, (id)cf);
                }
                __CFSpinUnlock(&__CFRuntimeExternRefCountTableLock);
            } else {
                prospectiveNewInfo -= (1 << RC_START);
                success = OSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
            }
        }
    } while (__builtin_expect(!success, 0));

#endif
    if (!didAuto && __builtin_expect(__CFOASafe, 0)) {
	__CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
    }
    return;

    really_free:;
    if (!didAuto && __builtin_expect(__CFOASafe, 0)) {
	// do not use CFGetRetainCount() because cf has been freed if it was an allocator
	__CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
    }
    // cannot zombify allocators, which get deallocated by __CFAllocatorDeallocate (finalize)
    if (isAllocator) {
        __CFAllocatorDeallocate((void *)cf);
	} else {
	CFAllocatorRef allocator = kCFAllocatorSystemDefault;
	Boolean usesSystemDefaultAllocator = true;

	if (!__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 7, 7)) {
	    allocator = CFGetAllocator(cf);
	usesSystemDefaultAllocator = (allocator == kCFAllocatorSystemDefault);
	}

	if (__CFZombieEnabled && !kCFUseCollectableAllocator) {
	    Class cls = object_getClass((id)cf);
	    const char *name = NULL;
            __CFSpinLock(&__CFBigRuntimeFunnel);
	    for (CFIndex idx = 0; !name && idx < __CFRuntimeClassTableCount; idx++) {
		if ((uintptr_t)cls == __CFRuntimeObjCClassTable[idx]) {
		    CFRuntimeClass *c = __CFRuntimeClassTable[idx];
		    if (c) name = c->className;
		}
	    }
            __CFSpinUnlock(&__CFBigRuntimeFunnel);
	    // in 64-bit, a future class has nil isa, and calling class_getName()
	    // on such will crash so we do this test
	    if (!name && object_getClass((id)cls)) {
		name = class_getName(cls);
	    }
	    if (!name) name = "$class-unknown$";
	    char *cname = NULL;
	    asprintf(&cname, "_NSZombie_%s", name);
	    Class zclass = (Class)objc_lookUpClass(cname);
	    if (!zclass) {
	       zclass = objc_duplicateClass((Class)objc_lookUpClass("_NSZombie_"), cname, 0);
	    }
	    free(cname);

#if DEPLOYMENT_TARGET_MACOSX
	    if (object_getClass((id)cls)) {
	        objc_destructInstance((id)cf);
	    }
#endif
	    if (__CFDeallocateZombies) {
#if __OBJC2__
	        object_setClass((id)cf, zclass);
#else
	        //  Set 'isa' pointer only if using standard deallocator
	        // However, _internal_object_dispose is not exported from libobjc
	        if (_dealloc == _original_objc_dealloc) {
		    object_setClass((id)cf, zclass);
	        }
#endif
		CFAllocatorDeallocate(allocator, (uint8_t *)cf - (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef)));
	    } else {
		object_setClass((id)cf, zclass);
	    }

#if 0
	    extern uintptr_t __CFFindPointer(uintptr_t ptr, uintptr_t start);
	    uintptr_t res = __CFFindPointer((uintptr_t)cf, 0);
	    while (0 != res) {
		if (res < (uintptr_t)&cf - 4 * 4096 || (uintptr_t)&cf + 4096 < res) {
		    printf("*** NSZombie warning: object %p deallocated, but reference still found at %p (%p %p)\n", cf, res);
		}
		res = __CFFindPointer((uintptr_t)cf, res + 1);
	    }
#endif

	} else {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	    if (kCFUseCollectableAllocator || !(__CFZombieLevel & (1 << 4))) {
	        Class cls = object_getClass((id)cf);
	        if (object_getClass((id)cls)) {
		    objc_removeAssociatedObjects((id)cf);
		}
	    }
#endif
	    if ((__CFZombieLevel & (1 << 0)) && !kCFUseCollectableAllocator) {
		uint8_t *ptr = (uint8_t *)cf - (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef));
		size_t size = malloc_size(ptr);
		uint8_t byte = 0xFC;
		if (__CFZombieLevel & (1 << 1)) {
		    ptr = (uint8_t *)cf + sizeof(CFRuntimeBase);
		    size = size - sizeof(CFRuntimeBase) - (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef));
		}
		if (__CFZombieLevel & (1 << 7)) {
		    byte = (__CFZombieLevel >> 8) & 0xFF;
		}
		memset(ptr, byte, size);
	    }
	    if (kCFUseCollectableAllocator || !(__CFZombieLevel & (1 << 4))) {
		CFAllocatorDeallocate(allocator, (uint8_t *)cf - (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef)));
	    }
	}

	if (kCFAllocatorSystemDefault != allocator) {
	    CFRelease(allocator);
	}
    }
}

#undef __kCFAllocatorTypeID_CONST
#undef __CFGenericAssertIsCF

