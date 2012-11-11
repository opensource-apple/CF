/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#define ENABLE_ZOMBIES 1

#include "CFRuntime.h"
#include "CFInternal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <monitor.h>
#include <crt_externs.h>
#include <unistd.h>
#include "auto_stubs.h"

#define __CFRecordAllocationEvent(a, b, c, d, e) ((void)0)

enum {
// retain/release recording constants -- must match values
// used by OA for now; probably will change in the future
__kCFRetainEvent = 28,
__kCFReleaseEvent = 29
};

#include <malloc/malloc.h>

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

static CFRuntimeClass ** __CFRuntimeClassTable = NULL;
int32_t __CFRuntimeClassTableSize = 0;
static int32_t __CFRuntimeClassTableCount = 0;

__private_extern__ void * (*__CFSendObjCMsg)(const void *, SEL, ...) = NULL;

__private_extern__ malloc_zone_t *__CFCollectableZone = NULL;

bool (*__CFObjCIsCollectable)(void *) = NULL;

static const void* objc_AssignIvar_none(const void *value, const void *base, const void **slot) { return (*slot = value); }
const void* (*__CFObjCAssignIvar)(const void *value, const void *base, const void **slot) = objc_AssignIvar_none;

static const void* objc_StrongAssign_none(const void *value, const void **slot) { return (*slot = value); }
const void* (*__CFObjCStrongAssign)(const void *value, const void **slot) = objc_StrongAssign_none;

void* (*__CFObjCMemmoveCollectable)(void *dst, const void *, size_t) = memmove;

// GC: to be moved to objc if necessary.
static void objc_WriteBarrierRange_none(void *ptr, size_t size) {}
static void objc_WriteBarrierRange_auto(void *ptr, size_t size) { auto_zone_write_barrier_range(__CFCollectableZone, ptr, size); }
void (*__CFObjCWriteBarrierRange)(void *, size_t) = objc_WriteBarrierRange_none;

// Compiler uses this symbol name; must match compiler built-in decl
#if __LP64__
int __CFConstantStringClassReference[24] = {0};
#else
int __CFConstantStringClassReference[12] = {0};
#endif

// #warning the whole business of reallocating the ClassTables is not thread-safe, because access to those values is not protected

CFTypeID _CFRuntimeRegisterClass(const CFRuntimeClass * const cls) {
// version field must be 0
// className must be pure ASCII string, non-null
    if (__CFMaxRuntimeTypes <= __CFRuntimeClassTableCount) {
	CFLog(kCFLogLevelWarning, CFSTR("*** CoreFoundation class table full; registration failing for class '%s'.  Program will crash soon."), cls->className);
	return _kCFRuntimeNotATypeID;
    }
    if (__CFRuntimeClassTableSize <= __CFRuntimeClassTableCount) {
	int32_t old_size = __CFRuntimeClassTableSize;
	int32_t new_size = __CFRuntimeClassTableSize * 4;

	void *new_table1 = calloc(new_size, sizeof(CFRuntimeClass *));
	memmove(new_table1, __CFRuntimeClassTable, old_size * sizeof(CFRuntimeClass *));
	__CFRuntimeClassTable = (CFRuntimeClass**)new_table1;
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
    return __CFRuntimeClassTableCount - 1;
}

void _CFRuntimeBridgeClasses(CFTypeID cf_typeID, const char *objc_classname) {
    return;
}

const CFRuntimeClass * _CFRuntimeGetClassWithTypeID(CFTypeID typeID) {
    return __CFRuntimeClassTable[typeID];
}

void _CFRuntimeUnregisterClassWithTypeID(CFTypeID typeID) {
    __CFRuntimeClassTable[typeID] = NULL;
}


#if defined(DEBUG) || defined(ENABLE_ZOMBIES)

/* CFZombieLevel levels:
 *	bit 0: scribble deallocated CF object memory
 *	bit 1: do not scribble on CFRuntimeBase header (when bit 0)
 *	bit 4: do not free CF objects
 *	bit 7: use 3rd-order byte as scribble byte for dealloc (otherwise 0xFC)
 */

static uint32_t __CFZombieLevel = 0x0;
static uint8_t __CFZombieEnabled = 0;
static uint8_t __CFDeallocateZombies = 0;
static void *_original_objc_dealloc = 0;

#endif /* DEBUG */

// XXX_PCB:  use the class version field as a bitmask, to allow classes to opt-in for GC scanning.

#define CF_GET_COLLECTABLE_MEMORY_TYPE(x) (0)

CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, CFIndex extraBytes, unsigned char *category) {
    CFRuntimeBase *memory;
    Boolean usesSystemDefaultAllocator;
    CFIndex size;

    CFAssert1(typeID != _kCFRuntimeNotATypeID, __kCFLogAssertion, "%s(): Uninitialized type id", __PRETTY_FUNCTION__);

    if (NULL == __CFRuntimeClassTable[typeID]) {
	return NULL;
    }
    allocator = (NULL == allocator) ? __CFGetDefaultAllocator() : allocator;
    usesSystemDefaultAllocator = (allocator == kCFAllocatorSystemDefault);
    size = sizeof(CFRuntimeBase) + extraBytes + (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef));
    size = (size + 0xF) & ~0xF;	// CF objects are multiples of 16 in size
    // CFType version 0 objects are unscanned by default since they don't have write-barriers and hard retain their innards
    // CFType version 1 objects are scanned and use hand coded write-barriers to store collectable storage within
    memory = (CFRuntimeBase *)CFAllocatorAllocate(allocator, size, CF_GET_COLLECTABLE_MEMORY_TYPE(__CFRuntimeClassTable[typeID]));
    if (NULL == memory) {
	return NULL;
    }
    memset(memory, 0, malloc_size(memory));
    if (__CFOASafe && category) {
	__CFSetLastAllocationEventName(memory, (char *)category);
    } else if (__CFOASafe) {
	__CFSetLastAllocationEventName(memory, (char *)__CFRuntimeClassTable[typeID]->className);
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
    if (NULL != __CFRuntimeClassTable[typeID]->init) {
	(__CFRuntimeClassTable[typeID]->init)(memory);
    }
    return memory;
}

void _CFRuntimeInitStaticInstance(void *ptr, CFTypeID typeID) {
    CFRuntimeBase *memory = (CFRuntimeBase *)ptr;
    CFAssert1(typeID != _kCFRuntimeNotATypeID, __kCFLogAssertion, "%s(): Uninitialized type id", __PRETTY_FUNCTION__);
    if (NULL == __CFRuntimeClassTable[typeID]) {
	return;
    }
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
   if (objc_getClass((id)cls) == nil) return false;
   const char *cname = class_getName(cls);
   if (cname && 0 == strncmp(cname, "_NSZombie_", 10)) return true;
   return false;
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

#define DISGUISE(object)       ((void *)(((uintptr_t)object) + 1))
#define UNDISGUISE(disguised)  ((id)(((uintptr_t)disguised) - 1))

// Bit 31 (highest bit) in second word of cf instance indicates external ref count

CF_EXPORT void _CFRelease(CFTypeRef cf);
CF_EXPORT CFTypeRef _CFRetain(CFTypeRef cf);
CF_EXPORT CFHashCode _CFHash(CFTypeRef cf);

CFTypeRef CFRetain(CFTypeRef cf) {
    if (CF_IS_COLLECTABLE(cf)) {
        // always honor CFRetain's with a GC-visible retain.
        auto_zone_retain(__CFCollectableZone, (void*)cf);
        return cf;
    }
    CFTYPE_OBJC_FUNCDISPATCH0(CFTypeRef, cf, "retain");
    if (cf) __CFGenericAssertIsCF(cf);
    return _CFRetain(cf);
}

__private_extern__ void __CFAllocatorDeallocate(CFTypeRef cf);

void CFRelease(CFTypeRef cf) {
#if !defined(__WIN32__)
    if (CF_IS_COLLECTABLE(cf)) {
        // release the GC-visible reference.
        if (auto_zone_release(__CFCollectableZone, (void*)cf) == 0 && !CFTYPE_IS_OBJC(cf)) {
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)];
            if (cfClass->version & _kCFRuntimeResourcefulObject) {
                if (cfClass->reclaim) cfClass->reclaim(cf);
            }
        }
        return;
    }
#endif
    CFTYPE_OBJC_FUNCDISPATCH0(void, cf, "release");
    if (cf) __CFGenericAssertIsCF(cf);
    _CFRelease(cf);
}


__private_extern__ const void *__CFStringCollectionCopy(CFAllocatorRef allocator, const void *ptr) {
    CFStringRef theString = (CFStringRef)ptr;
    CFStringRef result = CFStringCreateCopy(allocator, theString);
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        result = (CFStringRef)CFMakeCollectable(result);
    }
    return (const void *)result;
}

extern void CFCollection_non_gc_storage_error(void);

__private_extern__ const void *__CFTypeCollectionRetain(CFAllocatorRef allocator, const void *ptr) {
    CFTypeRef cf = (CFTypeRef)ptr;
    // only collections allocated in the GC zone can opt-out of reference counting.
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        if (CFTYPE_IS_OBJC(cf)) return cf;  // do nothing for OBJC objects.
        if (auto_zone_is_valid_pointer(__CFCollectableZone, ptr)) {
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)];
            if (cfClass->version & _kCFRuntimeResourcefulObject) {
                // GC: If this a CF object in the GC heap that is marked resourceful, then
                // it must be retained keep it alive in a CF collection.
                // We're basically inlining CFRetain() here, to avoid an extra heap membership test.
                auto_zone_retain(__CFCollectableZone, (void*)cf);
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
    CFTypeRef cf = (CFTypeRef)ptr;
    // only collections allocated in the GC zone can opt-out of reference counting.
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        if (CFTYPE_IS_OBJC(cf)) return; // do nothing for OBJC objects.
        if (auto_zone_is_valid_pointer(__CFCollectableZone, cf)) {
#if !defined(__WIN32__)
            // GC: If this a CF object in the GC heap that is marked uncollectable, then
            // must balance the retain done in __CFTypeCollectionRetain().
            // We're basically inlining CFRelease() here, to avoid an extra heap membership test.
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)];
            if (cfClass->version & _kCFRuntimeResourcefulObject && auto_zone_release(__CFCollectableZone, (void*)cf) == 0) {
                // ResourceFull objects trigger 'reclaim' on transition to zero
                if (cfClass->reclaim) cfClass->reclaim(cf);
            }
            else // avoid releasing normal CF objects.  Like other collections, for example
                ;
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
	__CFSpinLock(&__CFRuntimeExternRefCountTableLock);
	highBits = (uint64_t)CFBagGetCountOfValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
	__CFSpinUnlock(&__CFRuntimeExternRefCountTableLock);
    }
    uint64_t compositeRC = (lowBits & 0x7f) + (highBits << 6);
    return compositeRC;
#endif
}

CFTypeRef _CFRetainGC(CFTypeRef cf) {
#if defined(DEBUG)
    if (CF_USING_COLLECTABLE_MEMORY && !CF_IS_COLLECTABLE(cf)) {
        fprintf(stderr, "non-auto object %p passed to _CFRetainGC.\n", cf);
        HALT;
    }
#endif
    return CF_USING_COLLECTABLE_MEMORY ? cf : CFRetain(cf);
}

void _CFReleaseGC(CFTypeRef cf) {
#if defined(DEBUG)
    if (CF_USING_COLLECTABLE_MEMORY && !CF_IS_COLLECTABLE(cf)) {
        fprintf(stderr, "non-auto object %p passed to _CFReleaseGC.\n", cf);
        HALT;
    }
#endif
    if (!CF_USING_COLLECTABLE_MEMORY) CFRelease(cf);
}

CFIndex CFGetRetainCount(CFTypeRef cf) {
    if (NULL == cf) return 0;
    if (CF_IS_COLLECTABLE(cf)) {
        return auto_zone_retain_count(__CFCollectableZone, cf);
    }
    CFTYPE_OBJC_FUNCDISPATCH0(CFIndex, cf, "retainCount");
    __CFGenericAssertIsCF(cf);
    uint64_t rc = __CFGetFullRetainCount(cf);
    return (rc < (uint64_t)LONG_MAX) ? (CFIndex)rc : (CFIndex)LONG_MAX;
}

CFTypeRef CFMakeCollectable(CFTypeRef cf) {
    if (NULL == cf) return NULL;
    if (CF_IS_COLLECTABLE(cf)) {
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
        if (auto_zone_retain_count(__CFCollectableZone, cf) == 0) {
            CFLog(kCFLogLevelWarning, CFSTR("object %p with 0 retain-count passed to CFMakeCollectable."), cf);
            return cf;
        }
        auto_zone_release(__CFCollectableZone, (void *)cf);
    }
    return cf;
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
    CFTYPE_OBJC_FUNCDISPATCH0(CFHashCode, cf, "hash");
    __CFGenericAssertIsCF(cf);
    return _CFHash(cf);
}

// definition: produces a normally non-NULL debugging description of the object
CFStringRef CFCopyDescription(CFTypeRef cf) {
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
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
#if defined(DEBUG)
    if (NULL == cf) HALT;
#endif
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc(cf, formatOptions);
    }
    return NULL;
}

extern CFAllocatorRef __CFAllocatorGetAllocator(CFTypeRef);

CFAllocatorRef CFGetAllocator(CFTypeRef cf) {
    if (NULL == cf) return kCFAllocatorSystemDefault;
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
extern void __CFErrorInitialize(void);
extern void __CFTimeZoneInitialize(void);
extern void __CFTreeInitialize(void);
extern void __CFURLInitialize(void);
#if DEPLOYMENT_TARGET_MACOSX
extern void __CFMachPortInitialize(void);
#endif
#if DEPLOYMENT_TARGET_MACOSX
extern void __CFMessagePortInitialize(void);
#endif
#if DEPLOYMENT_TARGET_MACOSX || defined(__WIN32__)
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
extern void __CFStreamInitialize(void);

static void __exceptionInit(void) {}
static void __collatorInit(void) {}
static void __forwarding_prep_0___(void) {}
static void __forwarding_prep_1___(void) {}
static void __NSFastEnumerationMutationHandler(id obj) {}
const void *__CFArgStuff = NULL;
__private_extern__ void *__CFAppleLanguages = NULL;

bool kCFUseCollectableAllocator = false;


#if DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
static void __CFInitialize(void) __attribute__ ((constructor));
static
#endif
void __CFInitialize(void) {
    static int __done = 0;

    if (!__done) {
        __done = 1;

#if defined(DEBUG) || defined(ENABLE_ZOMBIES)
	const char *value = getenv("NSZombieEnabled");
	if (value && (*value == 'Y' || *value == 'y')) __CFZombieEnabled = 0xff;
	value = getenv("NSDeallocateZombies");
	if (value && (*value == 'Y' || *value == 'y')) __CFDeallocateZombies = 0xff;

	value = getenv("CFZombieLevel");
	if (NULL != value) {
	    __CFZombieLevel = (uint32_t)strtoul_l(value, NULL, 0, NULL);
	}
	if (0x0 == __CFZombieLevel) __CFZombieLevel = 0x0000FC00; // default
#endif

        __CFRuntimeClassTableSize = 1024;
        __CFRuntimeClassTable = (CFRuntimeClass **)calloc(__CFRuntimeClassTableSize, sizeof(CFRuntimeClass *));
        __CFBaseInitialize();

        /* Here so that two runtime classes get indices 0, 1. */
        __kCFNotATypeTypeID = _CFRuntimeRegisterClass(&__CFNotATypeClass);
        __kCFTypeTypeID = _CFRuntimeRegisterClass(&__CFTypeClass);

        /* Here so that __kCFAllocatorTypeID gets index 2. */
        __CFAllocatorInitialize();

#if DEPLOYMENT_TARGET_MACOSX
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


        /* CFBag needs to be up before CFString. */
        __CFBagInitialize();

#if !__LP64__
        // Creating this lazily in CFRetain causes recursive call to CFRetain
        __CFRuntimeExternRefCountTable = CFBagCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
#endif

        /*** _CFRuntimeCreateInstance() can finally be called generally after this line. ***/

	__CFRuntimeClassTableCount = 7;
	__CFStringInitialize();		// CFString's TypeID must be 0x7, now and forever
	__CFRuntimeClassTableCount = 16;
        __CFDictionaryInitialize();
        __CFArrayInitialize();
        __CFDataInitialize();
        __CFSetInitialize();
        __CFNullInitialize();		// See above for hard-coding of this position
        __CFBooleanInitialize();	// See above for hard-coding of this position
        __CFNumberInitialize();		// See above for hard-coding of this position


        __CFDateInitialize();	// just initializes the time goo
//	_CFRuntimeBridgeClasses(CFDateGetTypeID(), objc_lookUpClass("NSCFDate") ? "NSCFDate" : "__NSCFDate");
        __CFTimeZoneInitialize();
//	_CFRuntimeBridgeClasses(CFTimeZoneGetTypeID(), "NSCFTimeZone");
        __CFBinaryHeapInitialize();
        __CFBitVectorInitialize();
        __CFCharacterSetInitialize();
        __CFStorageInitialize();
        __CFErrorInitialize();
        __CFTreeInitialize();
        __CFURLInitialize();
        __CFBundleInitialize();
#if DEPLOYMENT_TARGET_MACOSX
        __CFPlugInInitialize();
        __CFPlugInInstanceInitialize();
#endif //__MACH__
        __CFUUIDInitialize();
#if DEPLOYMENT_TARGET_MACOSX
    __CFMessagePortInitialize();
#endif
#if DEPLOYMENT_TARGET_MACOSX
        __CFMachPortInitialize();
#endif
        __CFStreamInitialize();
        __CFPreferencesDomainInitialize();
#if DEPLOYMENT_TARGET_MACOSX || defined(__WIN32__)
        __CFRunLoopInitialize();
        __CFRunLoopObserverInitialize();
        __CFRunLoopSourceInitialize();
        __CFRunLoopTimerInitialize();
        __CFSocketInitialize();
#endif


#if DEPLOYMENT_TARGET_MACOSX
        {
            CFIndex idx, cnt;
            char **args;
            args = *_NSGetArgv();
            cnt = *_NSGetArgc();
            CFIndex count;
            CFStringRef *list, buffer[256];
            list = (cnt <= 256) ? buffer : malloc(cnt * sizeof(CFStringRef));
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
#endif
        _CFProcessPath();	// cache this early

        if (__CFRuntimeClassTableCount < 256) __CFRuntimeClassTableCount = 256;

#if defined(DEBUG) && !defined(__WIN32__)
        CFLog(kCFLogLevelWarning, CFSTR("Assertions enabled"));
#endif
    }
}

//#if defined(__WIN32__)

#ifdef _BUILD_NET_FOUNDATION_
#ifdef __cplusplus
extern "C"{
#endif //C++
extern void _CFFTPCleanup(void);
extern void _CFHTTPMessageCleanup(void);
extern void _CFHTTPStreamCleanup(void);
#ifdef __cplusplus
}
#endif //C++

#endif //_BUILD_NET_FOUNDATION_

#if 0

/* We have to call __CFInitialize when library is attached to the process.
 * (Sergey Zubarev)
 */
#if defined(_BUILD_NET_FOUNDATION_)
extern "C" {
BOOL WINAPI CoreFoundationDllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID pReserved );
}

BOOL WINAPI CoreFoundationDllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID pReserved ) {
#else
BOOL WINAPI DllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID pReserved ) {
#endif
    static CFBundleRef cfBundle = NULL;
    if (dwReason == DLL_PROCESS_ATTACH) {
	__CFInitialize();
        cfBundle = RegisterCoreFoundationBundle();
    } else if (dwReason == DLL_PROCESS_DETACH) {
        if (cfBundle) CFRelease(cfBundle);
#if !0
        __CFStringCleanup();
        __CFSocketCleanup();
#endif
        __CFUniCharCleanup();
        __CFStreamCleanup();
        __CFBaseCleanup();
    } else if (dwReason == DLL_THREAD_DETACH) {
        __CFFinalizeThreadData(NULL);
    }
    return TRUE;
}

#endif

// Functions that avoid ObC dispatch and CF type validation, for use by NSNotifyingCFArray, etc.
// Hopefully all of this will just go away.  3321464.  M.P. To Do - 7/9/03

Boolean _CFEqual(CFTypeRef cf1, CFTypeRef cf2) {
    if (cf1 == cf2) return true;
    if (NULL == cf1) return false;
    if (NULL == cf2) return false;
    __CFGenericAssertIsCF(cf1);
    __CFGenericAssertIsCF(cf2);
    if (__CFGenericTypeID_inline(cf1) != __CFGenericTypeID_inline(cf2)) return false;
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal(cf1, cf2);
    }
    return false;
}

CFIndex _CFGetRetainCount(CFTypeRef cf) {
    if (NULL == cf) return 0;
    if (CF_IS_COLLECTABLE(cf)) {
        return auto_zone_retain_count(__CFCollectableZone, cf);
    }
    uint64_t rc = __CFGetFullRetainCount(cf);
    return (rc < (uint64_t)LONG_MAX) ? (CFIndex)rc : (CFIndex)LONG_MAX;
}

CFHashCode _CFHash(CFTypeRef cf) {
    if (NULL == cf) return 0;
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash(cf);
    }
    return (CFHashCode)cf;
}

#if 0 || 0
static inline bool myOSAtomicCompareAndSwap32Barrier(int32_t oldValue, int32_t newValue, volatile int32_t *theValue) {
    int32_t actualOldValue = InterlockedCompareExchange((volatile LONG *)theValue, newValue, oldValue);
    return actualOldValue == oldValue ? true : false;
}
#else
static bool (*myOSAtomicCompareAndSwap32Barrier)(int32_t __oldValue, int32_t __newValue, volatile int32_t *__theValue) = OSAtomicCompareAndSwap32Barrier;
#endif

CF_EXPORT CFTypeRef _CFRetain(CFTypeRef cf) {
    if (NULL == cf) return NULL;
#if __LP64__
    uint32_t lowBits;
    do {
	lowBits = ((CFRuntimeBase *)cf)->_rc;
	if (0 == lowBits) return cf;	// Constant CFTypeRef
    } while (!myOSAtomicCompareAndSwap32Barrier(lowBits, lowBits + 1, (int32_t *)&((CFRuntimeBase *)cf)->_rc));
#else
#define RC_START 24
#define RC_END 31
    volatile UInt32 *infoLocation = (UInt32 *)&(((CFRuntimeBase *)cf)->_cfinfo);
    CFIndex rcLowBits = __CFBitfieldGetValue(*infoLocation, RC_END, RC_START);
    if (__builtin_expect(0 == rcLowBits, 0)) return cf;	// Constant CFTypeRef
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
            success = myOSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
            if (__builtin_expect(success, 1)) {
                CFBagAddValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
            }
            __CFSpinUnlock(&__CFRuntimeExternRefCountTableLock);
        } else {
            success = myOSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
        }
    } while (__builtin_expect(!success, 0));
#endif
    if (__builtin_expect(__CFOASafe, 0)) {
	__CFRecordAllocationEvent(__kCFRetainEvent, (void *)cf, 0, _CFGetRetainCount(cf), NULL);
    }
    return cf;
}

CF_EXPORT void _CFRelease(CFTypeRef cf) {
    Boolean isAllocator = false;
#if __LP64__
    uint32_t lowBits;
    do {
	lowBits = ((CFRuntimeBase *)cf)->_rc;
	if (0 == lowBits) return;	// Constant CFTypeRef
	if (1 == lowBits) {
	    // CANNOT WRITE ANY NEW VALUE INTO [CF_RC_BITS] UNTIL AFTER FINALIZATION
	    CFTypeID typeID = __CFGenericTypeID_inline(cf);
	    isAllocator = (__kCFAllocatorTypeID_CONST == typeID);
            CFRuntimeClass *cfClass = __CFRuntimeClassTable[typeID];
            if (cfClass->version & _kCFRuntimeResourcefulObject && cfClass->reclaim != NULL) {
                cfClass->reclaim(cf);
            }
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
	    if (isAllocator || myOSAtomicCompareAndSwap32Barrier(1, 0, (int32_t *)&((CFRuntimeBase *)cf)->_rc)) {
		goto really_free;
	    }
	}
    } while (!myOSAtomicCompareAndSwap32Barrier(lowBits, lowBits - 1, (int32_t *)&((CFRuntimeBase *)cf)->_rc));
#else
    volatile UInt32 *infoLocation = (UInt32 *)&(((CFRuntimeBase *)cf)->_cfinfo);
    CFIndex rcLowBits = __CFBitfieldGetValue(*infoLocation, RC_END, RC_START);
    if (__builtin_expect(0 == rcLowBits, 0)) return;        // Constant CFTypeRef
    bool success = 0;
    do {
        UInt32 initialCheckInfo = *infoLocation;
        rcLowBits = __CFBitfieldGetValue(initialCheckInfo, RC_END, RC_START);
        if (__builtin_expect(1 == rcLowBits, 0)) {
            // we think cf should be deallocated
            if (__builtin_expect(__kCFAllocatorTypeID_CONST == __CFGenericTypeID_inline(cf), 0)) {
                if (__builtin_expect(__CFOASafe, 0)) __CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
               __CFAllocatorDeallocate((void *)cf);
                success = 1;
            } else {
                // CANNOT WRITE ANY NEW VALUE INTO [CF_RC_BITS] UNTIL AFTER FINALIZATION
                CFTypeID typeID = __CFGenericTypeID_inline(cf);
                CFRuntimeClass *cfClass = __CFRuntimeClassTable[typeID];
                if (cfClass->version & _kCFRuntimeResourcefulObject && cfClass->reclaim != NULL) {
                    cfClass->reclaim(cf);
                }
                if (NULL != __CFRuntimeClassTable[typeID]->finalize) {
                    __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->finalize(cf);
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
        } else {
            // not yet junk
            UInt32 prospectiveNewInfo = initialCheckInfo; // don't want compiler to generate prospectiveNewInfo = *infoLocation.  This is why infoLocation is declared as a pointer to volatile memory.
            if (__builtin_expect((1 << 7) == rcLowBits, 0)) {
                // Time to remove a bit from the external ref count
                __CFSpinLock(&__CFRuntimeExternRefCountTableLock);
                CFIndex rcHighBitsCnt = CFBagGetCountOfValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
                if (1 == rcHighBitsCnt) {
                    __CFBitfieldSetValue(prospectiveNewInfo, RC_END, RC_START, (1 << 6) - 1);
                } else {
                    __CFBitfieldSetValue(prospectiveNewInfo, RC_END, RC_START, ((1 << 6) | (1 << 7)) - 1);
                }
                success = myOSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
                if (__builtin_expect(success, 1)) {
                    CFBagRemoveValue(__CFRuntimeExternRefCountTable, DISGUISE(cf));
                }
                __CFSpinUnlock(&__CFRuntimeExternRefCountTableLock);
            } else {
                prospectiveNewInfo -= (1 << RC_START);
                success = myOSAtomicCompareAndSwap32Barrier(*(int32_t *)&initialCheckInfo, *(int32_t *)&prospectiveNewInfo, (int32_t *)infoLocation);
            }
        }
    } while (__builtin_expect(!success, 0));

#endif
    if (__builtin_expect(__CFOASafe, 0)) {
	__CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, _CFGetRetainCount(cf), NULL);
    }
    return;

    really_free:;
    if (__builtin_expect(__CFOASafe, 0)) {
	// do not use _CFGetRetainCount() because cf has been freed if it was an allocator
	__CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
    }
    // cannot zombify allocators, which get deallocated by __CFAllocatorDeallocate (finalize)
    if (!isAllocator) {
	CFAllocatorRef allocator;
	Boolean usesSystemDefaultAllocator;

	if (__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 7, 7)) {
	    allocator = kCFAllocatorSystemDefault;
	} else {
	    allocator = CFGetAllocator(cf);
	}
	usesSystemDefaultAllocator = (allocator == kCFAllocatorSystemDefault);

	if (__CFZombieLevel & (1 << 0)) {
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
	if (!(__CFZombieLevel & (1 << 4))) {
	    CFAllocatorDeallocate(allocator, (uint8_t *)cf - (usesSystemDefaultAllocator ? 0 : sizeof(CFAllocatorRef)));
	}
	
	if (kCFAllocatorSystemDefault != allocator) {
	    CFRelease(allocator);
	}
    }
}

#undef __kCFAllocatorTypeID_CONST
#undef __CFGenericAssertIsCF

