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
/*	CFSet.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFSet.h>
#include "CFInternal.h"

const CFSetCallBacks kCFTypeSetCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFSetCallBacks kCFCopyStringSetCallBacks = {0, (void *)CFStringCreateCopy, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
static const CFSetCallBacks __kCFNullSetCallBacks = {0, NULL, NULL, NULL, NULL, NULL};

static const uint32_t __CFSetCapacities[42] = {
    4, 8, 17, 29, 47, 76, 123, 199, 322, 521, 843, 1364, 2207, 3571, 5778, 9349,
    15127, 24476, 39603, 64079, 103682, 167761, 271443, 439204, 710647, 1149851, 1860498,
    3010349, 4870847, 7881196, 12752043, 20633239, 33385282, 54018521, 87403803, 141422324,
    228826127, 370248451, 599074578, 969323029, 1568397607, 2537720636U
};

static const uint32_t __CFSetBuckets[42] = {	// primes
    5, 11, 23, 41, 67, 113, 199, 317, 521, 839, 1361, 2207, 3571, 5779, 9349, 15121,
    24473, 39607, 64081, 103681, 167759, 271429, 439199, 710641, 1149857, 1860503, 3010349,
    4870843, 7881193, 12752029, 20633237, 33385273, 54018521, 87403763, 141422317, 228826121,
    370248451, 599074561, 969323023, 1568397599, 2537720629U, 4106118251U
};

CF_INLINE CFIndex __CFSetRoundUpCapacity(CFIndex capacity) {
    CFIndex idx;
    for (idx = 0; idx < 42 && __CFSetCapacities[idx] < (uint32_t)capacity; idx++);
    if (42 <= idx) HALT;
    return __CFSetCapacities[idx];
}

CF_INLINE CFIndex __CFSetNumBucketsForCapacity(CFIndex capacity) {
    CFIndex idx;
    for (idx = 0; idx < 42 && __CFSetCapacities[idx] < (uint32_t)capacity; idx++);
    if (42 <= idx) HALT;
    return __CFSetBuckets[idx];
}

enum {		/* Bits 1-0 */
    __kCFSetImmutable = 0,	/* unchangable and fixed capacity */
    __kCFSetMutable = 1,		/* changeable and variable capacity */
    __kCFSetFixedMutable = 3	/* changeable and fixed capacity */
};

enum {		/* Bits 5-4 (value), 3-2 (key) */
    __kCFSetHasNullCallBacks = 0,
    __kCFSetHasCFTypeCallBacks = 1,
    __kCFSetHasCustomCallBacks = 3	/* callbacks are at end of header */
};

// Under GC, we fudge the key/value memory in two ways
// First, if we had null callbacks or null for both retain/release, we use unscanned memory
// This means that if people were doing addValue:[xxx new] and never removing, well, that doesn't work
//
// Second, if we notice standard retain/release implementations we substitute scanned memory
// and zero out the retain/release callbacks.  This is fine, but when copying we need to restore them

enum {
    __kCFSetRestoreKeys =       (1 << 0),
    __kCFSetRestoreValues =     (1 << 1),
    __kCFSetRestoreStringKeys = (1 << 2),
    __kCFSetWeakKeys =          (1 << 3)
};

struct __CFSet {
    CFRuntimeBase _base;
    CFIndex _count;		/* number of values */
    CFIndex _capacity;		/* maximum number of values */
    CFIndex _bucketsNum;	/* number of slots */
    uintptr_t _marker;
    void *_context;		/* private */
    CFIndex _deletes;
    CFOptionFlags _xflags;      /* bits for GC */
    const void **_keys;		/* can be NULL if not allocated yet */
};

/* Bits 1-0 of the base reserved bits are used for mutability variety */
/* Bits 3-2 of the base reserved bits are used for key callback indicator bits */
/* Bits 5-4 of the base reserved bits are used for value callback indicator bits */
/* Bit 6 is special KVO actions bit */

CF_INLINE CFIndex __CFSetGetType(CFSetRef set) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 1, 0);
}

CF_INLINE CFIndex __CFSetGetSizeOfType(CFIndex t) {
    CFIndex size = sizeof(struct __CFSet);
    if (__CFBitfieldGetValue(t, 3, 2) == __kCFSetHasCustomCallBacks) {
	size += sizeof(CFSetCallBacks);
    }
    return size;
}

CF_INLINE const CFSetCallBacks *__CFSetGetKeyCallBacks(CFSetRef set) {
    CFSetCallBacks *result = NULL;
    switch (__CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
    case __kCFSetHasNullCallBacks:
	return &__kCFNullSetCallBacks;
    case __kCFSetHasCFTypeCallBacks:
	return &kCFTypeSetCallBacks;
    case __kCFSetHasCustomCallBacks:
	break;
    }
    result = (CFSetCallBacks *)((uint8_t *)set + sizeof(struct __CFSet));
    return result;
}

CF_INLINE bool __CFSetCallBacksMatchNull(const CFSetCallBacks *c) {
    return (NULL == c ||
	(c->retain == __kCFNullSetCallBacks.retain &&
	 c->release == __kCFNullSetCallBacks.release &&
	 c->copyDescription == __kCFNullSetCallBacks.copyDescription &&
	 c->equal == __kCFNullSetCallBacks.equal &&
	 c->hash == __kCFNullSetCallBacks.hash));
}

CF_INLINE bool __CFSetCallBacksMatchCFType(const CFSetCallBacks *c) {
    return (&kCFTypeSetCallBacks == c ||
	(c->retain == kCFTypeSetCallBacks.retain &&
	 c->release == kCFTypeSetCallBacks.release &&
	 c->copyDescription == kCFTypeSetCallBacks.copyDescription &&
	 c->equal == kCFTypeSetCallBacks.equal &&
	 c->hash == kCFTypeSetCallBacks.hash));
}

#define CF_OBJC_KVO_WILLCHANGE(obj, sel)
#define CF_OBJC_KVO_DIDCHANGE(obj, sel)

static CFIndex __CFSetFindBuckets1a(CFSetRef set, const void *key) {
    CFHashCode keyHash = (CFHashCode)key;
    const void **keys = set->_keys;
    uintptr_t marker = set->_marker;
    CFIndex probe = keyHash % set->_bucketsNum;
    CFIndex probeskip = 1;	// See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
	uintptr_t currKey = (uintptr_t)keys[probe];
	if (marker == currKey) {		/* empty */
	    return kCFNotFound;
	} else if (~marker == currKey) {	/* deleted */
	    /* do nothing */
	} else if (currKey == (uintptr_t)key) {
	    return probe;
	}
	probe = probe + probeskip;
	// This alternative to probe % buckets assumes that
	// probeskip is always positive and less than the
	// number of buckets.
	if (set->_bucketsNum <= probe) {
	    probe -= set->_bucketsNum;
	}
	if (start == probe) {
	    return kCFNotFound;
	}
    }
}

static CFIndex __CFSetFindBuckets1b(CFSetRef set, const void *key) {
    const CFSetCallBacks *cb = __CFSetGetKeyCallBacks(set);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, set->_context) : (CFHashCode)key;
    const void **keys = set->_keys;
    uintptr_t marker = set->_marker;
    CFIndex probe = keyHash % set->_bucketsNum;
    CFIndex probeskip = 1;	// See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
	uintptr_t currKey = (uintptr_t)keys[probe];
	if (marker == currKey) {		/* empty */
	    return kCFNotFound;
	} else if (~marker == currKey) {	/* deleted */
	    /* do nothing */
	} else if (currKey == (uintptr_t)key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(const void *, const void *, void*))cb->equal, (void *)currKey, key, set->_context))) {
	    return probe;
	}
	probe = probe + probeskip;
	// This alternative to probe % buckets assumes that
	// probeskip is always positive and less than the
	// number of buckets.
	if (set->_bucketsNum <= probe) {
	    probe -= set->_bucketsNum;
	}
	if (start == probe) {
	    return kCFNotFound;
	}
    }
}

static void __CFSetFindBuckets2(CFSetRef set, const void *key, CFIndex *match, CFIndex *nomatch) {
    const CFSetCallBacks *cb = __CFSetGetKeyCallBacks(set);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, set->_context) : (CFHashCode)key;
    const void **keys = set->_keys;
    uintptr_t marker = set->_marker;
    CFIndex probe = keyHash % set->_bucketsNum;
    CFIndex probeskip = 1;	// See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    *match = kCFNotFound;
    *nomatch = kCFNotFound;
    for (;;) {
	uintptr_t currKey = (uintptr_t)keys[probe];
	if (marker == currKey) {		/* empty */
	    if (nomatch) *nomatch = probe;
	    return;
	} else if (~marker == currKey) {	/* deleted */
	    if (nomatch) {
		*nomatch = probe;
		nomatch = NULL;
	    }
	} else if (currKey == (uintptr_t)key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(const void *, const void *, void*))cb->equal, (void *)currKey, key, set->_context))) {
	    *match = probe;
	    return;
	}
	probe = probe + probeskip;
	// This alternative to probe % buckets assumes that
	// probeskip is always positive and less than the
	// number of buckets.
	if (set->_bucketsNum <= probe) {
	    probe -= set->_bucketsNum;
	}
	if (start == probe) {
	    return;
	}
    }
}

static void __CFSetFindNewMarker(CFSetRef set) {
    const void **keys = set->_keys;
    uintptr_t newMarker;
    CFIndex idx, nbuckets;
    bool hit;

    nbuckets = set->_bucketsNum;
    newMarker = set->_marker;
    do {
	newMarker--;
	hit = false;
	for (idx = 0; idx < nbuckets; idx++) {
	    if (newMarker == (uintptr_t)keys[idx] || ~newMarker == (uintptr_t)keys[idx]) {
		hit = true;
		break;
	    }
	}
    } while (hit);
    for (idx = 0; idx < nbuckets; idx++) {
	if (set->_marker == (uintptr_t)keys[idx]) {
	    keys[idx] = (const void *)newMarker;
	} else if (~set->_marker == (uintptr_t)keys[idx]) {
	    keys[idx] = (const void *)~newMarker;
	}
    }
    ((struct __CFSet *)set)->_marker = newMarker;
}

static bool __CFSetEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFSetRef set1 = (CFSetRef)cf1;
    CFSetRef set2 = (CFSetRef)cf2;
    const CFSetCallBacks *cb1, *cb2;
    const void **keys;
    CFIndex idx, nbuckets;
    if (set1 == set2) return true;
    if (set1->_count != set2->_count) return false;
    cb1 = __CFSetGetKeyCallBacks(set1);
    cb2 = __CFSetGetKeyCallBacks(set2);
    if (cb1->equal != cb2->equal) return false;
    if (0 == set1->_count) return true; /* after function comparison! */
    keys = set1->_keys;
    nbuckets = set1->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (set1->_marker != (uintptr_t)keys[idx] && ~set1->_marker != (uintptr_t)keys[idx]) {
	    const void *value;
	    if (!CFSetGetValueIfPresent(set2, keys[idx], &value)) return false;
	}
    }
    return true;
}

static CFHashCode __CFSetHash(CFTypeRef cf) {
    CFSetRef set = (CFSetRef)cf;
    return set->_count;
}

static CFStringRef __CFSetCopyDescription(CFTypeRef cf) {
    CFSetRef set = (CFSetRef)cf;
    CFAllocatorRef allocator;
    const CFSetCallBacks *cb;
    const void **keys;
    CFIndex idx, nbuckets;
    CFMutableStringRef result;
    cb = __CFSetGetKeyCallBacks(set);
    keys = set->_keys;
    nbuckets = set->_bucketsNum;
    allocator = CFGetAllocator(set);
    result = CFStringCreateMutable(allocator, 0);
    switch (__CFSetGetType(set)) {
    case __kCFSetImmutable:
	CFStringAppendFormat(result, NULL, CFSTR("<CFSet %p [%p]>{type = immutable, count = %u, capacity = %u, pairs = (\n"), cf, allocator, set->_count, set->_capacity);
	break;
    case __kCFSetFixedMutable:
	CFStringAppendFormat(result, NULL, CFSTR("<CFSet %p [%p]>{type = fixed-mutable, count = %u, capacity = %u, pairs = (\n"), cf, allocator, set->_count, set->_capacity);
	break;
    case __kCFSetMutable:
	CFStringAppendFormat(result, NULL, CFSTR("<CFSet %p [%p]>{type = mutable, count = %u, capacity = %u, pairs = (\n"), cf, allocator, set->_count, set->_capacity);
	break;
    }
    for (idx = 0; idx < nbuckets; idx++) {
	if (set->_marker != (uintptr_t)keys[idx] && ~set->_marker != (uintptr_t)keys[idx]) {
	    CFStringRef kDesc = NULL;
	    if (NULL != cb->copyDescription) {
		kDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(const void *, void *))cb->copyDescription), keys[idx], set->_context);
	    }
	    if (NULL != kDesc) {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@\n"), idx, kDesc);
		CFRelease(kDesc);
	    } else {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p>\n"), idx, keys[idx]);
	    }
	}
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static void __CFSetDeallocate(CFTypeRef cf) {
    CFMutableSetRef set = (CFMutableSetRef)cf;
    CFAllocatorRef allocator = __CFGetAllocator(set);
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        const CFSetCallBacks *kcb = __CFSetGetKeyCallBacks(set);
        if (kcb->retain == NULL && kcb->release == NULL)
            return; // XXX_PCB keep set intact during finalization.
    }
    if (__CFSetGetType(set) == __kCFSetImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)set)->_info, 1, 0, __kCFSetFixedMutable);
    }

    const CFSetCallBacks *cb = __CFSetGetKeyCallBacks(set);
    if (cb->release) {
	const void **keys = set->_keys;
	CFIndex idx, nbuckets = set->_bucketsNum;
	for (idx = 0; idx < nbuckets; idx++) {
	    if (set->_marker != (uintptr_t)keys[idx] && ~set->_marker != (uintptr_t)keys[idx]) {
		const void *oldkey = keys[idx];
		INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, oldkey, set->_context);
	    }
	}
    }

    if (__CFSetGetType(set) == __kCFSetMutable && set->_keys) {
        _CFAllocatorDeallocateGC(allocator, set->_keys);
        set->_keys = NULL;
    }
}

/*
 * When running under GC, we suss up sets with standard string copy to hold
 * onto everything, including the copies of incoming keys, in strong memory without retain counts.
 * This is the routine that makes that copy.
 * Not for inputs of constant strings we'll get a constant string back, and so the result
 * is not guaranteed to be from the auto zone, hence the call to CFRelease since it will figure
 * out where the refcount really is.
 */
static CFStringRef _CFStringCreateCopyCollected(CFAllocatorRef allocator, CFStringRef theString) {
    return CFMakeCollectable(CFStringCreateCopy(NULL, theString));
}

static CFTypeID __kCFSetTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFSetClass = {
    _kCFRuntimeScannedObject,
    "CFSet",
    NULL,	// init
    NULL,	// copy
    __CFSetDeallocate,
    (void *)__CFSetEqual,
    __CFSetHash,
    NULL,	// 
    __CFSetCopyDescription
};

__private_extern__ void __CFSetInitialize(void) {
    __kCFSetTypeID = _CFRuntimeRegisterClass(&__CFSetClass);
}

CFTypeID CFSetGetTypeID(void) {
    return __kCFSetTypeID;
}

static CFSetRef __CFSetInit(CFAllocatorRef allocator, uint32_t flags, CFIndex capacity, const CFSetCallBacks *keyCallBacks) {
    struct __CFSet *memory;
    uint32_t size;
    CFIndex idx;
    __CFBitfieldSetValue(flags, 31, 2, 0);
    CFSetCallBacks nonRetainingKeyCallbacks;
    CFOptionFlags xflags = 0;
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        // preserve NULL for key or value CB, otherwise fix up.
        if (!keyCallBacks || (keyCallBacks->retain == NULL && keyCallBacks->release == NULL)) {
	    xflags = __kCFSetWeakKeys;
	}
        else {
	    if (keyCallBacks->retain == __CFTypeCollectionRetain && keyCallBacks->release == __CFTypeCollectionRelease) {
                // copy everything
                nonRetainingKeyCallbacks = *keyCallBacks;
                nonRetainingKeyCallbacks.retain = NULL;
                nonRetainingKeyCallbacks.release = NULL;
                keyCallBacks = &nonRetainingKeyCallbacks;
		xflags = __kCFSetRestoreKeys;
            }
	    else if (keyCallBacks->retain == CFStringCreateCopy && keyCallBacks->release == __CFTypeCollectionRelease) {
                // copy everything
                nonRetainingKeyCallbacks = *keyCallBacks;
                nonRetainingKeyCallbacks.retain = (void *)_CFStringCreateCopyCollected;   // XXX fix with better cast
                nonRetainingKeyCallbacks.release = NULL;
                keyCallBacks = &nonRetainingKeyCallbacks;
		xflags = (__kCFSetRestoreKeys | __kCFSetRestoreStringKeys);
            }
        }
    }
    if (__CFSetCallBacksMatchNull(keyCallBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFSetHasNullCallBacks);
    } else if (__CFSetCallBacksMatchCFType(keyCallBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFSetHasCFTypeCallBacks);
    } else {
	__CFBitfieldSetValue(flags, 3, 2, __kCFSetHasCustomCallBacks);
    }
    size = __CFSetGetSizeOfType(flags) - sizeof(CFRuntimeBase);
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFSetImmutable:
    case __kCFSetFixedMutable:
        size += __CFSetNumBucketsForCapacity(capacity) * sizeof(const void *);
        break;
    case __kCFSetMutable:
        break;
    }
    memory = (struct __CFSet *)_CFRuntimeCreateInstance(allocator, __kCFSetTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFBitfieldSetValue(memory->_base._info, 6, 0, flags);
    memory->_count = 0;
    memory->_marker = (uintptr_t)0xa1b1c1d3;
    memory->_context = NULL;
    memory->_deletes = 0;
    memory->_xflags = xflags;
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFSetImmutable:
        if (CF_IS_COLLECTABLE_ALLOCATOR(allocator) && (xflags & __kCFSetWeakKeys)) { // if weak, don't scan
            auto_zone_set_layout_type(__CFCollectableZone, memory, AUTO_OBJECT_UNSCANNED);
        }
        if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFSet (immutable)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFSetNumBucketsForCapacity(memory->_capacity);
	memory->_keys = (const void **)((uint8_t *)memory + __CFSetGetSizeOfType(flags));
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_keys[idx] = (const void *)memory->_marker;
	}
	break;
    case __kCFSetFixedMutable:
	if (CF_IS_COLLECTABLE_ALLOCATOR(allocator) && (xflags & __kCFSetWeakKeys)) { // if weak, don't scan
	    auto_zone_set_layout_type(__CFCollectableZone, memory, AUTO_OBJECT_UNSCANNED);
	}
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFSet (mutable-fixed)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFSetNumBucketsForCapacity(memory->_capacity);
	memory->_keys = (const void **)((uint8_t *)memory + __CFSetGetSizeOfType(flags));
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_keys[idx] = (const void *)memory->_marker;
	}
	break;
    case __kCFSetMutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFSet (mutable-variable)");
	memory->_capacity = __CFSetRoundUpCapacity(1);
	memory->_bucketsNum = 0;
	memory->_keys = NULL;
	break;
    }
    if (__kCFSetHasCustomCallBacks == __CFBitfieldGetValue(flags, 3, 2)) {
	CFSetCallBacks *cb = (CFSetCallBacks *)__CFSetGetKeyCallBacks((CFSetRef)memory);
	*cb = *keyCallBacks;
	FAULT_CALLBACK((void **)&(cb->retain));
	FAULT_CALLBACK((void **)&(cb->release));
	FAULT_CALLBACK((void **)&(cb->copyDescription));
	FAULT_CALLBACK((void **)&(cb->equal));
	FAULT_CALLBACK((void **)&(cb->hash));
    }
    return (CFSetRef)memory;
}

CFSetRef CFSetCreate(CFAllocatorRef allocator, const void **keys, CFIndex numValues, const CFSetCallBacks *keyCallBacks) {
    CFSetRef result;
    uint32_t flags;
    CFIndex idx;
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%d) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
    result = __CFSetInit(allocator, __kCFSetImmutable, numValues, keyCallBacks);
    flags = __CFBitfieldGetValue(((const CFRuntimeBase *)result)->_info, 1, 0);
    if (flags == __kCFSetImmutable) {
	// tweak flags so that we can add our immutable values
        __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, __kCFSetFixedMutable);
    }
    for (idx = 0; idx < numValues; idx++) {
	CFSetAddValue((CFMutableSetRef)result, keys[idx]);
    }
    __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, flags);
    return result;
}

CFMutableSetRef CFSetCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFSetCallBacks *keyCallBacks) {
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%d) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    return (CFMutableSetRef)__CFSetInit(allocator, (0 == capacity) ? __kCFSetMutable : __kCFSetFixedMutable, capacity, keyCallBacks);
}


static void __CFSetGrow(CFMutableSetRef set, CFIndex numNewValues);

// This creates a set which is for CFTypes or NSObjects, with an ownership transfer -- 
// the set does not take a retain, and the caller does not need to release the inserted objects.
// The incoming objects must also be collectable if allocated out of a collectable allocator.
CFSetRef _CFSetCreate_ex(CFAllocatorRef allocator, bool mutable, const void **keys, CFIndex numValues) {
    CFSetRef result;
    void *bucketsBase;
    uint32_t flags;
    CFIndex idx;
    result = __CFSetInit(allocator, mutable ? __kCFSetMutable : __kCFSetImmutable, numValues, &kCFTypeSetCallBacks);
    flags = __CFBitfieldGetValue(((const CFRuntimeBase *)result)->_info, 1, 0);
    if (!mutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, __kCFSetFixedMutable);
    }
    if (mutable) {
	if (result->_count == result->_capacity || NULL == result->_keys) {
	    __CFSetGrow((CFMutableSetRef)result, numValues);
	}
    }
    // GC:  since kCFTypeSetCallBacks are used, the keys
    // and values will be allocated contiguously.
    bool collectableContainer = CF_IS_COLLECTABLE_ALLOCATOR(allocator);
    bucketsBase = (collectableContainer ? (void *)auto_zone_base_pointer(__CFCollectableZone, result->_keys) : NULL);
    for (idx = 0; idx < numValues; idx++) {
	CFIndex match, nomatch;
	const void *newKey;
	__CFSetFindBuckets2(result, keys[idx], &match, &nomatch);
	if (kCFNotFound != match) {
	} else {
	    newKey = keys[idx];
	    if (result->_marker == (uintptr_t)newKey || ~result->_marker == (uintptr_t)newKey) {
		__CFSetFindNewMarker(result);
	    }
	    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, bucketsBase, result->_keys[nomatch], newKey);
	    // GC: generation(_keys/_values) <= generation(keys/values), but added for completeness.
	    ((CFMutableSetRef)result)->_count++;
	}
    }
    __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, flags);
    return result;
}

CFSetRef CFSetCreateCopy(CFAllocatorRef allocator, CFSetRef set) {
    CFSetRef result;
    const CFSetCallBacks *cb;
    CFIndex numValues = CFSetGetCount(set);
    const void **list, *buffer[256];
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0); // XXX_PCB GC OK
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFSet (temp)");
    CFSetGetValues(set, list);
    CFSetCallBacks patchedKeyCB;
    if (CF_IS_OBJC(__kCFSetTypeID, set)) {
	cb = &kCFTypeSetCallBacks;
    }
    else {
	cb = __CFSetGetKeyCallBacks(set);
	if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
	    if (set->_xflags & __kCFSetRestoreKeys) {
		patchedKeyCB = *cb;    // copy
		cb = &patchedKeyCB;    // reset to copy
		patchedKeyCB.retain = (set->_xflags & __kCFSetRestoreStringKeys) ? CFStringCreateCopy : __CFTypeCollectionRetain;
		patchedKeyCB.release = __CFTypeCollectionRelease;
	    }
	}
    }
    result = CFSetCreate(allocator, list, numValues, cb);
    if (list != buffer) CFAllocatorDeallocate(allocator, list); // GC OK
    return result;
}

CFMutableSetRef CFSetCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFSetRef set) {
    CFMutableSetRef result;
    const CFSetCallBacks *cb;
    CFIndex idx, numValues = CFSetGetCount(set);
    const void **list, *buffer[256];
    CFAssert3(0 == capacity || numValues <= capacity, __kCFLogAssertion, "%s(): for fixed-mutable sets, capacity (%d) must be greater than or equal to initial number of values (%d)", __PRETTY_FUNCTION__, capacity, numValues);
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0); // XXX_PCB GC OK
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFSet (temp)");
    CFSetGetValues(set, list);
    CFSetCallBacks patchedKeyCB;
    if (CF_IS_OBJC(__kCFSetTypeID, set)) {
	cb = &kCFTypeSetCallBacks;
    }
    else {
	cb = __CFSetGetKeyCallBacks(set);
	if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
	    if (set->_xflags & __kCFSetRestoreKeys) {
		patchedKeyCB = *cb;    // copy
		cb = &patchedKeyCB;    // reset to copy
		patchedKeyCB.retain = (set->_xflags & __kCFSetRestoreStringKeys) ? CFStringCreateCopy : __CFTypeCollectionRetain;
		patchedKeyCB.release = __CFTypeCollectionRelease;
	    }
	}
    }
    result = CFSetCreateMutable(allocator, capacity, cb);
    if (0 == capacity) _CFSetSetCapacity(result, numValues);
    for (idx = 0; idx < numValues; idx++) {
	CFSetAddValue(result, list[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(allocator, list); // XXX_PCB GC OK
    return result;
}

// Used by NSHashTables and KVO
void _CFSetSetContext(CFSetRef set, void *context) {
    CFAllocatorRef allocator = CFGetAllocator(set);
    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, set, set->_context, context);
}

void *_CFSetGetContext(CFSetRef set) {
    return ((struct __CFSet *)set)->_context;
}

CFIndex CFSetGetCount(CFSetRef set) {
    CF_OBJC_FUNCDISPATCH0(__kCFSetTypeID, CFIndex, set, "count");
    __CFGenericValidateType(set, __kCFSetTypeID);
    return set->_count;
}

CFIndex CFSetGetCountOfValue(CFSetRef set, const void *key) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, CFIndex, set, "countForObject:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return 0;
    if (__kCFSetHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
	match = __CFSetFindBuckets1a(set, key);
    } else {
	match = __CFSetFindBuckets1b(set, key);
    }
    return (kCFNotFound != match ? 1 : 0);
}

Boolean CFSetContainsValue(CFSetRef set, const void *key) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, char, set, "containsObject:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return false;
    if (__kCFSetHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
	match = __CFSetFindBuckets1a(set, key);
    } else {
	match = __CFSetFindBuckets1b(set, key);
    }
    return (kCFNotFound != match ? true : false);
}

const void *CFSetGetValue(CFSetRef set, const void *key) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, const void *, set, "member:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return NULL;
    if (__kCFSetHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
	match = __CFSetFindBuckets1a(set, key);
    } else {
	match = __CFSetFindBuckets1b(set, key);
    }
    return (kCFNotFound != match ? set->_keys[match] : NULL);
}

Boolean CFSetGetValueIfPresent(CFSetRef set, const void *key, const void **actualkey) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH2(__kCFSetTypeID, char, set, "_getValue:forObj:", (void * *) actualkey, key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return false;
    if (__kCFSetHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
	match = __CFSetFindBuckets1a(set, key);
    } else {
	match = __CFSetFindBuckets1b(set, key);
    }
    return (kCFNotFound != match ? ((actualkey ? __CFObjCStrongAssign(set->_keys[match], actualkey) : NULL), true) : false);
}

void CFSetGetValues(CFSetRef set, const void **keys) {
    CFIndex idx, cnt, nbuckets;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "getObjects:", (void * *) keys);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (CF_USING_COLLECTABLE_MEMORY) {
	// GC: speculatively issue a write-barrier on the copied to buffers (3743553).
	__CFObjCWriteBarrierRange(keys, set->_count * sizeof(void *));
    }
    nbuckets = set->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (set->_marker != (uintptr_t)set->_keys[idx] && ~set->_marker != (uintptr_t)set->_keys[idx]) {
	    for (cnt = 1; cnt--;) {
		if (keys) CF_WRITE_BARRIER_ASSIGN(NULL, *keys++, set->_keys[idx]);
	    }
	}
    }
}

void CFSetApplyFunction(CFSetRef set, CFSetApplierFunction applier, void *context) {
    const void **keys;
    CFIndex idx, cnt, nbuckets;
    FAULT_CALLBACK((void **)&(applier));
    CF_OBJC_FUNCDISPATCH2(__kCFSetTypeID, void, set, "_applyValues:context:", applier, context);
    __CFGenericValidateType(set, __kCFSetTypeID);
    keys = set->_keys;
    nbuckets = set->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (set->_marker != (uintptr_t)keys[idx] && ~set->_marker != (uintptr_t)keys[idx]) {
	    for (cnt = 1; cnt--;) {
		INVOKE_CALLBACK2(applier, keys[idx], context);
	    }
	}
    }
}


static void __CFSetGrow(CFMutableSetRef set, CFIndex numNewValues) {
    const void **oldkeys = set->_keys;
    CFIndex idx, oldnbuckets = set->_bucketsNum;
    CFIndex oldCount = set->_count;
    CFAllocatorRef allocator = __CFGetAllocator(set), keysAllocator;
    void *keysBase;
    set->_capacity = __CFSetRoundUpCapacity(oldCount + numNewValues);
    set->_bucketsNum = __CFSetNumBucketsForCapacity(set->_capacity);
    set->_deletes = 0;
    void *buckets = _CFAllocatorAllocateGC(allocator, set->_bucketsNum * sizeof(const void *), (set->_xflags & __kCFSetWeakKeys) ? AUTO_MEMORY_UNSCANNED : AUTO_MEMORY_SCANNED);
    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, set, set->_keys, buckets);
    keysAllocator = allocator;
    keysBase = set->_keys;
    if (NULL == set->_keys) HALT;
    if (__CFOASafe) __CFSetLastAllocationEventName(set->_keys, "CFSet (store)");
    for (idx = set->_bucketsNum; idx--;) {
        set->_keys[idx] = (const void *)set->_marker;
    }
    if (NULL == oldkeys) return;
    for (idx = 0; idx < oldnbuckets; idx++) {
        if (set->_marker != (uintptr_t)oldkeys[idx] && ~set->_marker != (uintptr_t)oldkeys[idx]) {
            CFIndex match, nomatch;
            __CFSetFindBuckets2(set, oldkeys[idx], &match, &nomatch);
            CFAssert3(kCFNotFound == match, __kCFLogAssertion, "%s(): two values (%p, %p) now hash to the same slot; mutable value changed while in table or hash value is not immutable", __PRETTY_FUNCTION__, oldkeys[idx], set->_keys[match]);
            if (kCFNotFound != nomatch) {
                CF_WRITE_BARRIER_BASE_ASSIGN(keysAllocator, keysBase, set->_keys[nomatch], oldkeys[idx]);
            }
        }
    }
    CFAssert1(set->_count == oldCount, __kCFLogAssertion, "%s(): set count differs after rehashing; error", __PRETTY_FUNCTION__);
    _CFAllocatorDeallocateGC(allocator, oldkeys);
}

// This function is for Foundation's benefit; no one else should use it.
void _CFSetSetCapacity(CFMutableSetRef set, CFIndex cap) {
    if (CF_IS_OBJC(__kCFSetTypeID, set)) return;
#if defined(DEBUG)
    __CFGenericValidateType(set, __kCFSetTypeID);
    CFAssert1(__CFSetGetType(set) != __kCFSetImmutable && __CFSetGetType(set) != __kCFSetFixedMutable, __kCFLogAssertion, "%s(): set is immutable or fixed-mutable", __PRETTY_FUNCTION__);
    CFAssert3(set->_count <= cap, __kCFLogAssertion, "%s(): desired capacity (%d) is less than count (%d)", __PRETTY_FUNCTION__, cap, set->_count);
#endif
    __CFSetGrow(set, cap - set->_count);
}


void CFSetAddValue(CFMutableSetRef set, const void *key) {
    CFIndex match, nomatch;
    const CFSetCallBacks *cb;
    const void *newKey;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "addObject:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
	if (set->_count == set->_capacity || NULL == set->_keys) {
	    __CFSetGrow(set, 1);
	}
	break;
    case __kCFSetFixedMutable:
	CFAssert3(set->_count < set->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity set %p (capacity = %d)", __PRETTY_FUNCTION__, set, set->_capacity);
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    __CFSetFindBuckets2(set, key, &match, &nomatch);
    if (kCFNotFound != match) {
    } else {
        CFAllocatorRef allocator = __CFGetAllocator(set);
        CFAllocatorRef keysAllocator = (set->_xflags & __kCFSetWeakKeys) ? kCFAllocatorNull : allocator;
	cb = __CFSetGetKeyCallBacks(set);
	if (cb->retain) {
	    newKey = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, key, set->_context);
	} else {
	    newKey = key;
	}
	if (set->_marker == (uintptr_t)newKey || ~set->_marker == (uintptr_t)newKey) {
	    __CFSetFindNewMarker(set);
	}
	CF_OBJC_KVO_WILLCHANGE(set, key);
	CF_WRITE_BARRIER_ASSIGN(keysAllocator, set->_keys[nomatch], newKey);
	set->_count++;
	CF_OBJC_KVO_DIDCHANGE(set, key);
    }
}

__private_extern__ const void *__CFSetAddValueAndReturn(CFMutableSetRef set, const void *key) {
    CFIndex match, nomatch;
    const CFSetCallBacks *cb;
    const void *newKey;
// #warning not toll-free bridged, but internal
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
	if (set->_count == set->_capacity || NULL == set->_keys) {
	    __CFSetGrow(set, 1);
	}
	break;
    case __kCFSetFixedMutable:
	CFAssert3(set->_count < set->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity set %p (capacity = %d)", __PRETTY_FUNCTION__, set, set->_capacity);
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    __CFSetFindBuckets2(set, key, &match, &nomatch);
    if (kCFNotFound != match) {
	return set->_keys[match];
    } else {
        CFAllocatorRef allocator = __CFGetAllocator(set);
        CFAllocatorRef keysAllocator = (set->_xflags & __kCFSetWeakKeys) ? kCFAllocatorNull : allocator;
	cb = __CFSetGetKeyCallBacks(set);
	if (cb->retain) {
	    newKey = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, key, set->_context);
	} else {
	    newKey = key;
	}
	if (set->_marker == (uintptr_t)newKey || ~set->_marker == (uintptr_t)newKey) {
	    __CFSetFindNewMarker(set);
	}
	CF_OBJC_KVO_WILLCHANGE(set, key);
	CF_WRITE_BARRIER_ASSIGN(keysAllocator, set->_keys[nomatch], newKey);
	set->_count++;
	CF_OBJC_KVO_DIDCHANGE(set, key);
	return newKey;
    }
}

void CFSetReplaceValue(CFMutableSetRef set, const void *key) {
    CFIndex match;
    const CFSetCallBacks *cb;
    const void *newKey;
    CFAllocatorRef allocator;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "_replaceObject:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
    case __kCFSetFixedMutable:
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    if (0 == set->_count) return;
    if (__kCFSetHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
	match = __CFSetFindBuckets1a(set, key);
    } else {
	match = __CFSetFindBuckets1b(set, key);
    }
    if (kCFNotFound == match) return;
    cb = __CFSetGetKeyCallBacks(set);
    allocator = (set->_xflags & __kCFSetWeakKeys) ? kCFAllocatorNull : __CFGetAllocator(set);
    if (cb->retain) {
	newKey = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, key, set->_context);
    } else {
	newKey = key;
    }
    CF_OBJC_KVO_WILLCHANGE(set, key);
    if (cb->release) {
	INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, set->_keys[match], set->_context);
    }
    CF_WRITE_BARRIER_ASSIGN(allocator, set->_keys[match], newKey);
    CF_OBJC_KVO_DIDCHANGE(set, key);
}

void CFSetSetValue(CFMutableSetRef set, const void *key) {
    CFIndex match, nomatch;
    const CFSetCallBacks *cb;
    const void *newKey;
    CFAllocatorRef allocator;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "_setObject:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
	if (set->_count == set->_capacity || NULL == set->_keys) {
	    __CFSetGrow(set, 1);
	}
	break;
    case __kCFSetFixedMutable:
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    __CFSetFindBuckets2(set, key, &match, &nomatch);
    cb = __CFSetGetKeyCallBacks(set);
    allocator = (set->_xflags & __kCFSetWeakKeys) ? kCFAllocatorNull : __CFGetAllocator(set);
    if (cb->retain) {
	newKey = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, key, set->_context);
    } else {
	newKey = key;
    }
    if (kCFNotFound != match) {
	CF_OBJC_KVO_WILLCHANGE(set, key);
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, set->_keys[match], set->_context);
	}
	CF_WRITE_BARRIER_ASSIGN(allocator, set->_keys[match], newKey);
	CF_OBJC_KVO_DIDCHANGE(set, key);
    } else {
	CFAssert3(__kCFSetFixedMutable != __CFSetGetType(set) || set->_count < set->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity set %p (capacity = %d)", __PRETTY_FUNCTION__, set, set->_capacity);
	if (set->_marker == (uintptr_t)newKey || ~set->_marker == (uintptr_t)newKey) {
	    __CFSetFindNewMarker(set);
	}
	CF_OBJC_KVO_WILLCHANGE(set, key);
	CF_WRITE_BARRIER_ASSIGN(allocator, set->_keys[nomatch], newKey);
	set->_count++;
	CF_OBJC_KVO_DIDCHANGE(set, key);
    }
}

void CFSetRemoveValue(CFMutableSetRef set, const void *key) {
    CFIndex match;
    const CFSetCallBacks *cb;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "removeObject:", key);
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
    case __kCFSetFixedMutable:
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    if (0 == set->_count) return;
    if (__kCFSetHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)set)->_info, 3, 2)) {
	match = __CFSetFindBuckets1a(set, key);
    } else {
	match = __CFSetFindBuckets1b(set, key);
    }
    if (kCFNotFound == match) return;
    cb = __CFSetGetKeyCallBacks(set);
    if (1) {
	const void *oldkey = set->_keys[match];
	CF_OBJC_KVO_WILLCHANGE(set, oldkey);
        set->_keys[match] = (const void *)~set->_marker;
	set->_count--;
	CF_OBJC_KVO_DIDCHANGE(set, oldkey);
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), __CFGetAllocator(set), oldkey, set->_context);
	}
	set->_deletes++;
	if ((__kCFSetMutable == __CFSetGetType(set)) && (set->_bucketsNum < 4 * set->_deletes || (512 < set->_capacity && 3.236067 * set->_count < set->_capacity))) {
	    // 3.236067 == 2 * golden_mean; this comes about because we're trying to resize down
	    // when the count is less than 2 capacities smaller, but not right away when count
	    // is just less than 2 capacities smaller, because an add would then force growth;
	    // well, the ratio between one capacity and the previous is close to the golden
	    // mean currently, so (cap / m / m) would be two smaller; but so we're not close,
	    // we take the average of that and the prior cap (cap / m / m / m). Well, after one
	    // does the algebra, and uses the convenient fact that m^(x+2) = m^(x+1) + m^x if m
	    // is the golden mean, this reduces to cap / 2m for the threshold. In general, the
	    // possible threshold constant is 1 / (2 * m^k), k = 0, 1, 2, ... under this scheme.
	    // Rehash; currently only for mutable-variable sets
	    __CFSetGrow(set, 0);
	} else {
	    // When the probeskip == 1 always and only, a DELETED slot followed by an EMPTY slot
	    // can be converted to an EMPTY slot.  By extension, a chain of DELETED slots followed
	    // by an EMPTY slot can be converted to EMPTY slots, which is what we do here.
	    // _CFSetDecrementValue() below has this same code.
	    if (match < set->_bucketsNum - 1 && set->_keys[match + 1] == (const void *)set->_marker) {
		while (0 <= match && set->_keys[match] == (const void *)~set->_marker) {
		    set->_keys[match] = (const void *)set->_marker;
		    set->_deletes--;
		    match--;
		}
	    }
	}
    }
}

void CFSetRemoveAllValues(CFMutableSetRef set) {
    const void **keys;
    const CFSetCallBacks *cb;
    CFAllocatorRef allocator;
    CFIndex idx, nbuckets;
    CF_OBJC_FUNCDISPATCH0(__kCFSetTypeID, void, set, "removeAllObjects");
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
    case __kCFSetFixedMutable:
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    if (0 == set->_count) return;
    keys = set->_keys;
    nbuckets = set->_bucketsNum;
    cb = __CFSetGetKeyCallBacks(set);
    allocator = __CFGetAllocator(set);
    for (idx = 0; idx < nbuckets; idx++) {
	if (set->_marker != (uintptr_t)keys[idx] && ~set->_marker != (uintptr_t)keys[idx]) {
	    const void *oldkey = keys[idx];
	    CF_OBJC_KVO_WILLCHANGE(set, oldkey);
	    keys[idx] = (const void *)~set->_marker;
	    set->_count--;
	    CF_OBJC_KVO_DIDCHANGE(set, oldkey);
	    if (cb->release) {
		INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, oldkey, set->_context);
	    }
	}
    }
    // XXX need memset here
    for (idx = 0; idx < nbuckets; idx++) {
	keys[idx] = (const void *)set->_marker;
    }
    set->_count = 0;
    set->_deletes = 0;
    if ((__kCFSetMutable == __CFSetGetType(set)) && (512 < set->_capacity)) {
	__CFSetGrow(set, 256);
    }
}

