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
/*	CFBag.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFBag.h>
#include "CFInternal.h"

const CFBagCallBacks kCFTypeBagCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFBagCallBacks kCFCopyStringBagCallBacks = {0, (void *)CFStringCreateCopy, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
static const CFBagCallBacks __kCFNullBagCallBacks = {0, NULL, NULL, NULL, NULL, NULL};


static const uint32_t __CFBagCapacities[42] = {
    4, 8, 17, 29, 47, 76, 123, 199, 322, 521, 843, 1364, 2207, 3571, 5778, 9349,
    15127, 24476, 39603, 64079, 103682, 167761, 271443, 439204, 710647, 1149851, 1860498,
    3010349, 4870847, 7881196, 12752043, 20633239, 33385282, 54018521, 87403803, 141422324,
    228826127, 370248451, 599074578, 969323029, 1568397607, 2537720636U
}; 

static const uint32_t __CFBagBuckets[42] = {    // primes
    5, 11, 23, 41, 67, 113, 199, 317, 521, 839, 1361, 2207, 3571, 5779, 9349, 15121,
    24473, 39607, 64081, 103681, 167759, 271429, 439199, 710641, 1149857, 1860503, 3010349,
    4870843, 7881193, 12752029, 20633237, 33385273, 54018521, 87403763, 141422317, 228826121,
    370248451, 599074561, 969323023, 1568397599, 2537720629U, 4106118251U
};

CF_INLINE CFIndex __CFBagRoundUpCapacity(CFIndex capacity) {
    CFIndex idx;
    for (idx = 0; idx < 42 && __CFBagCapacities[idx] < (uint32_t)capacity; idx++);
    if (42 <= idx) HALT;
    return __CFBagCapacities[idx];
}

CF_INLINE CFIndex __CFBagNumBucketsForCapacity(CFIndex capacity) {
    CFIndex idx;
    for (idx = 0; idx < 42 && __CFBagCapacities[idx] < (uint32_t)capacity; idx++);
    if (42 <= idx) HALT;
    return __CFBagBuckets[idx];
}

enum {		/* Bits 1-0 */
    __kCFBagImmutable = 0,		/* unchangable and fixed capacity */
    __kCFBagMutable = 1,		/* changeable and variable capacity */
    __kCFBagFixedMutable = 3		/* changeable and fixed capacity */
};

enum {		/* Bits 3-2 */
    __kCFBagHasNullCallBacks = 0,
    __kCFBagHasCFTypeCallBacks = 1,
    __kCFBagHasCustomCallBacks = 3	/* callbacks are at end of header */
};

enum {		/* Bit 4 */
    __kCFCollectionIsWeak = 0,
    __kCFCollectionIsStrong = 1,
};


struct __CFBagBucket {
    const void *_key;
    CFIndex _count;
};

struct __CFBag {
    CFRuntimeBase _base;
    CFIndex _count;		/* number of values */
    CFIndex _capacity;		/* maximum number of values */
    CFIndex _bucketsUsed;	/* number of slots used */
    CFIndex _bucketsNum;	/* number of slots */
    const void *_emptyMarker;
    const void *_deletedMarker;
    void *_context;		/* private */
    struct __CFBagBucket *_buckets;	/* can be NULL if not allocated yet */
};

CF_INLINE bool __CFBagBucketIsEmpty(CFBagRef bag, const struct __CFBagBucket *bucket) {
    return (bag->_emptyMarker == bucket->_key);
}

CF_INLINE bool __CFBagBucketIsDeleted(CFBagRef bag, const struct __CFBagBucket *bucket) {
    return (bag->_deletedMarker == bucket->_key);
}

CF_INLINE bool __CFBagBucketIsOccupied(CFBagRef bag, const struct __CFBagBucket *bucket) {
    return (bag->_emptyMarker != bucket->_key && bag->_deletedMarker != bucket->_key);
}

/* Bits 1-0 of the base reserved bits are used for mutability variety */
/* Bits 3-2 of the base reserved bits are used for callback indicator bits */
/* Bits 4-5 are used by GC */

static bool isStrongMemory(CFTypeRef collection) {
    return  ! __CFBitfieldGetValue(((const CFRuntimeBase *)collection)->_info, 4, 4);
}

static bool needsRestore(CFTypeRef collection) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)collection)->_info, 5, 5);
}


CF_INLINE CFIndex __CFBagGetType(CFBagRef bag) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)bag)->_info, 1, 0);
}

CF_INLINE CFIndex __CFBagGetSizeOfType(CFIndex t) {
    CFIndex size = sizeof(struct __CFBag);
    if (__CFBitfieldGetValue(t, 3, 2) == __kCFBagHasCustomCallBacks) {
	size += sizeof(CFBagCallBacks);
    }
    return size;
}

CF_INLINE const CFBagCallBacks *__CFBagGetCallBacks(CFBagRef bag) {
    CFBagCallBacks *result = NULL;
    switch (__CFBitfieldGetValue(((const CFRuntimeBase *)bag)->_info, 3, 2)) {
    case __kCFBagHasNullCallBacks:
	return &__kCFNullBagCallBacks;
    case __kCFBagHasCFTypeCallBacks:
	return &kCFTypeBagCallBacks;
    case __kCFBagHasCustomCallBacks:
	break;
    }
    result = (CFBagCallBacks *)((uint8_t *)bag + sizeof(struct __CFBag));
    return result;
}

CF_INLINE bool __CFBagCallBacksMatchNull(const CFBagCallBacks *c) {
    return (NULL == c ||
	(c->retain == __kCFNullBagCallBacks.retain &&
	 c->release == __kCFNullBagCallBacks.release &&
	 c->copyDescription == __kCFNullBagCallBacks.copyDescription &&
	 c->equal == __kCFNullBagCallBacks.equal &&
	 c->hash == __kCFNullBagCallBacks.hash));
}

CF_INLINE bool __CFBagCallBacksMatchCFType(const CFBagCallBacks *c) {
    return (&kCFTypeBagCallBacks == c ||
	(c->retain == kCFTypeBagCallBacks.retain &&
	 c->release == kCFTypeBagCallBacks.release &&
	 c->copyDescription == kCFTypeBagCallBacks.copyDescription &&
	 c->equal == kCFTypeBagCallBacks.equal &&
	 c->hash == kCFTypeBagCallBacks.hash));
}


static void __CFBagFindBuckets1(CFBagRef bag, const void *key, struct __CFBagBucket **match) {
    const CFBagCallBacks *cb = __CFBagGetCallBacks(bag);
    struct __CFBagBucket *buckets = bag->_buckets;
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, bag->_context) : (CFHashCode)key;
    UInt32 start = keyHash % bag->_bucketsNum;
    UInt32 probe = start;
    UInt32 probeskip = 1;
    *match = NULL;
    for (;;) {
	struct __CFBagBucket *currentBucket = buckets + probe;
	if (__CFBagBucketIsEmpty(bag, currentBucket)) {
	    return;
	} else if (__CFBagBucketIsDeleted(bag, currentBucket)) {
	    /* do nothing */
	} else if (currentBucket->_key == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(const void *, const void *, void *))cb->equal, currentBucket->_key, key, bag->_context))) {
	    *match = currentBucket;
	    return;
	}
	probe = (probe + probeskip) % bag->_bucketsNum;
	if (start == probe) return;
    }
}

static void __CFBagFindBuckets2(CFBagRef bag, const void *key, struct __CFBagBucket **match, struct __CFBagBucket **nomatch) {
    const CFBagCallBacks *cb = __CFBagGetCallBacks(bag);
    struct __CFBagBucket *buckets = bag->_buckets;
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, bag->_context) : (CFHashCode)key;
    UInt32 start = keyHash % bag->_bucketsNum;
    UInt32 probe = start;
    UInt32 probeskip = 1;
    *match = NULL;
    *nomatch = NULL;
    for (;;) {
	struct __CFBagBucket *currentBucket = buckets + probe;
	if (__CFBagBucketIsEmpty(bag, currentBucket)) {
	    if (!*nomatch) *nomatch = currentBucket;
	    return;
	} else if (__CFBagBucketIsDeleted(bag, currentBucket)) {
	    if (!*nomatch) *nomatch = currentBucket;
	} else if (!*match && (currentBucket->_key == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(const void *, const void *, void *))cb->equal, currentBucket->_key, key, bag->_context)))) {
	    *match = currentBucket;
	    return;
	}
	probe = (probe + probeskip) % bag->_bucketsNum;
	if (start == probe) return;
    }
}

static void __CFBagFindNewEmptyMarker(CFBagRef bag) {
    struct __CFBagBucket *buckets;
    const void *newEmpty;
    bool hit;
    CFIndex idx, nbuckets;
    buckets = bag->_buckets;
    nbuckets = bag->_bucketsNum;
    newEmpty = bag->_emptyMarker;
    do {
	(intptr_t)newEmpty -= 2;
	hit = false;
	for (idx = 0; idx < nbuckets; idx++) {
	    if (newEmpty == buckets[idx]._key) {
		hit = true;
		break;
	    }
	}
    } while (hit);
    for (idx = 0; idx < nbuckets; idx++) {
	if (bag->_emptyMarker == buckets[idx]._key) {
	    buckets[idx]._key = newEmpty;
	}
    }
    ((struct __CFBag *)bag)->_emptyMarker = newEmpty;
}

static void __CFBagFindNewDeletedMarker(CFBagRef bag) {
    struct __CFBagBucket *buckets;
    const void *newDeleted;
    bool hit;
    CFIndex idx, nbuckets;
    buckets = bag->_buckets;
    nbuckets = bag->_bucketsNum;
    newDeleted = bag->_deletedMarker;
    do {
	(intptr_t)newDeleted += 2;
	hit = false;
	for (idx = 0; idx < nbuckets; idx++) {
	    if (newDeleted == buckets[idx]._key) {
		hit = true;
		break;
	    }
	}
    } while (hit);
    for (idx = 0; idx < nbuckets; idx++) {
	if (bag->_deletedMarker == buckets[idx]._key) {
	    buckets[idx]._key = newDeleted;
	}
    }
    ((struct __CFBag *)bag)->_deletedMarker = newDeleted;
}

static bool __CFBagEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFBagRef bag1 = (CFBagRef)cf1;
    CFBagRef bag2 = (CFBagRef)cf2;
    const CFBagCallBacks *cb1, *cb2;
    const struct __CFBagBucket *buckets;
    CFIndex idx, nbuckets;
    if (bag1 == bag2) return true;
    if (bag1->_count != bag2->_count) return false;
    cb1 = __CFBagGetCallBacks(bag1);
    cb2 = __CFBagGetCallBacks(bag2);
    if (cb1->equal != cb2->equal) return false;
    if (0 == bag1->_count) return true; /* after function comparison! */
    buckets = bag1->_buckets;
    nbuckets = bag1->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFBagBucketIsOccupied(bag1, &buckets[idx])) {
	    if (buckets[idx]._count != CFBagGetCountOfValue(bag2, buckets[idx]._key)) {
		return false;
	    }
	}
    }
    return true;
}

static CFHashCode __CFBagHash(CFTypeRef cf) {
    CFBagRef bag = (CFBagRef)cf;
    return bag->_count;
}

static CFStringRef __CFBagCopyDescription(CFTypeRef cf) {
    CFBagRef bag = (CFBagRef)cf;
    const CFBagCallBacks *cb;
    const struct __CFBagBucket *buckets;
    CFIndex idx, nbuckets;
    CFMutableStringRef result;
    cb = __CFBagGetCallBacks(bag);
    buckets = bag->_buckets;
    nbuckets = bag->_bucketsNum;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFBag %p [%p]>{count = %u, capacity = %u, values = (\n"), bag, CFGetAllocator(bag), bag->_count, bag->_capacity);
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFBagBucketIsOccupied(bag, &buckets[idx])) {
	    CFStringRef desc = NULL;
	    if (NULL != cb->copyDescription) {
		desc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(const void *, void *))cb->copyDescription), buckets[idx]._key, bag->_context);
	    }
	    if (NULL != desc) {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ (%d)\n"), idx, desc, buckets[idx]._count);
		CFRelease(desc);
	    } else {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> (%d)\n"), idx, buckets[idx]._key, buckets[idx]._count);
	    }
	}
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static void __CFBagDeallocate(CFTypeRef cf) {
    CFMutableBagRef bag = (CFMutableBagRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(bag);
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
	const CFBagCallBacks *cb = __CFBagGetCallBacks(bag);
	if (cb->retain == NULL && cb->release == NULL)
	    return; // XXX_PCB keep bag intact during finalization.
    }
    if (__CFBagGetType(bag) == __kCFBagImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)bag)->_info, 1, 0, __kCFBagFixedMutable);
    }
    CFBagRemoveAllValues(bag);
    if (__CFBagGetType(bag) == __kCFBagMutable && bag->_buckets) {
	_CFAllocatorDeallocateGC(allocator, bag->_buckets);
    }
}

static CFTypeID __kCFBagTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFBagClass = {
    _kCFRuntimeScannedObject,
    "CFBag",
    NULL,	// init
    NULL,	// copy
    __CFBagDeallocate,
    (void *)__CFBagEqual,
    __CFBagHash,
    NULL,	// 
    __CFBagCopyDescription
};

__private_extern__ void __CFBagInitialize(void) {
    __kCFBagTypeID = _CFRuntimeRegisterClass(&__CFBagClass);
}

CFTypeID CFBagGetTypeID(void) {
    return __kCFBagTypeID;
}

static CFBagRef __CFBagInit(CFAllocatorRef allocator, UInt32 flags, CFIndex capacity, const CFBagCallBacks *callBacks) {
    struct __CFBag *memory;
    UInt32 size;
    CFIndex idx;
    CFBagCallBacks nonRetainingCallbacks;
    __CFBitfieldSetValue(flags, 31, 2, 0);
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
	if (!callBacks || (callBacks->retain == NULL && callBacks->release == NULL)) {
	    __CFBitfieldSetValue(flags, 4, 4, 1); // setWeak
	}
	else {
	    if (callBacks->retain == __CFTypeCollectionRetain && callBacks->release == __CFTypeCollectionRelease) {
		nonRetainingCallbacks = *callBacks;
		nonRetainingCallbacks.retain = NULL;
		nonRetainingCallbacks.release = NULL;
		callBacks = &nonRetainingCallbacks;
		__CFBitfieldSetValue(flags, 5, 5, 1); // setNeedsRestore
	    }
	}
    }
    if (__CFBagCallBacksMatchNull(callBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFBagHasNullCallBacks);
    } else if (__CFBagCallBacksMatchCFType(callBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFBagHasCFTypeCallBacks);
    } else {
	__CFBitfieldSetValue(flags, 3, 2, __kCFBagHasCustomCallBacks);
    }
    size = __CFBagGetSizeOfType(flags) - sizeof(CFRuntimeBase);
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFBagImmutable:
    case __kCFBagFixedMutable:
	size += __CFBagNumBucketsForCapacity(capacity) * sizeof(struct __CFBagBucket);
	break;
    case __kCFBagMutable:
	break;
    }
    memory = (struct __CFBag *)_CFRuntimeCreateInstance(allocator, __kCFBagTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFBitfieldSetValue(memory->_base._info, 6, 0, flags);
    memory->_count = 0;
    memory->_bucketsUsed = 0;
    memory->_emptyMarker = (const void *)0xa1b1c1d3;
    memory->_deletedMarker = (const void *)0xa1b1c1d5;
    memory->_context = NULL;
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFBagImmutable:
        if (!isStrongMemory(memory)) {  // if weak, don't scan
            auto_zone_set_layout_type(__CFCollectableZone, memory, AUTO_OBJECT_UNSCANNED);
        }
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFBag (immutable)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFBagNumBucketsForCapacity(memory->_capacity);
	memory->_buckets = (struct __CFBagBucket *)((uint8_t *)memory + __CFBagGetSizeOfType(flags));
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_buckets[idx]._key = memory->_emptyMarker;
	}
	break;
    case __kCFBagFixedMutable:
        if (!isStrongMemory(memory)) {  // if weak, don't scan
            auto_zone_set_layout_type(__CFCollectableZone, memory, AUTO_OBJECT_UNSCANNED);
        }
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFBag (mutable-fixed)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFBagNumBucketsForCapacity(memory->_capacity);
	memory->_buckets = (struct __CFBagBucket *)((uint8_t *)memory + __CFBagGetSizeOfType(flags));
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_buckets[idx]._key = memory->_emptyMarker;
	}
	break;
    case __kCFBagMutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFBag (mutable-variable)");
	memory->_capacity = __CFBagRoundUpCapacity(1);
	memory->_bucketsNum = 0;
	memory->_buckets = NULL;
	break;
    }
    if (__kCFBagHasCustomCallBacks == __CFBitfieldGetValue(flags, 3, 2)) {
	const CFBagCallBacks *cb = __CFBagGetCallBacks((CFBagRef)memory);
	*(CFBagCallBacks *)cb = *callBacks;
	FAULT_CALLBACK((void **)&(cb->retain));
	FAULT_CALLBACK((void **)&(cb->release));
	FAULT_CALLBACK((void **)&(cb->copyDescription));
	FAULT_CALLBACK((void **)&(cb->equal));
	FAULT_CALLBACK((void **)&(cb->hash));
    }
    return (CFBagRef)memory;
}

CFBagRef CFBagCreate(CFAllocatorRef allocator, const void **values, CFIndex numValues, const CFBagCallBacks *callBacks) {
    CFBagRef result;
    UInt32 flags;
    CFIndex idx;
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%d) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
    result = __CFBagInit(allocator, __kCFBagImmutable, numValues, callBacks);
    flags = __CFBitfieldGetValue(((const CFRuntimeBase *)result)->_info, 1, 0);
    if (flags == __kCFBagImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, __kCFBagFixedMutable);
    }
    for (idx = 0; idx < numValues; idx++) {
	CFBagAddValue((CFMutableBagRef)result, values[idx]);
    }
    __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, flags);
    return result;
}

CFMutableBagRef CFBagCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFBagCallBacks *callBacks) {
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%d) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    return (CFMutableBagRef)__CFBagInit(allocator, (0 == capacity) ? __kCFBagMutable : __kCFBagFixedMutable, capacity, callBacks);
}

CFBagRef CFBagCreateCopy(CFAllocatorRef allocator, CFBagRef bag) {
    CFBagRef result;
    const CFBagCallBacks *cb;
    CFIndex numValues = CFBagGetCount(bag);
    const void **list, *buffer[256];
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0); // XXX_PCB GC OK
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFBag (temp)");
    CFBagGetValues(bag, list);
    CFBagCallBacks patchedCB;
    if (CF_IS_OBJC(__kCFBagTypeID, bag)) {
	cb = &kCFTypeBagCallBacks; 
    }
    else {
	cb = __CFBagGetCallBacks(bag);
	if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
	    if (needsRestore(bag)) {
		patchedCB = *cb;    // copy
		cb = &patchedCB;    // reset to copy
		patchedCB.retain = __CFTypeCollectionRetain;
		patchedCB.release = __CFTypeCollectionRelease;
	    }
	}
    }
    result = CFBagCreate(allocator, list, numValues, cb);
    if (list != buffer) CFAllocatorDeallocate(allocator, list); // XXX_PCB GC OK
    return result;
}

CFMutableBagRef CFBagCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFBagRef bag) {
    CFMutableBagRef result;
    const CFBagCallBacks *cb;
    CFIndex idx, numValues = CFBagGetCount(bag);
    const void **list, *buffer[256];
    CFAssert3(0 == capacity || numValues <= capacity, __kCFLogAssertion, "%s(): for fixed-mutable bags, capacity (%d) must be greater than or equal to initial number of values (%d)", __PRETTY_FUNCTION__, capacity, numValues);
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0); // XXX_PCB GC OK
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFBag (temp)");
    CFBagGetValues(bag, list);
    CFBagCallBacks patchedCB;
    if (CF_IS_OBJC(__kCFBagTypeID, bag)) {
	cb = &kCFTypeBagCallBacks; 
    }
    else {
	cb = __CFBagGetCallBacks(bag);
	if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
	    if (needsRestore(bag)) {
		patchedCB = *cb;    // copy
		cb = &patchedCB;    // reset to copy
		patchedCB.retain = __CFTypeCollectionRetain;
		patchedCB.release = __CFTypeCollectionRelease;
	    }
	}
    }
    result = CFBagCreateMutable(allocator, capacity, cb);
    if (0 == capacity) _CFBagSetCapacity(result, numValues);
    for (idx = 0; idx < numValues; idx++) {
	CFBagAddValue(result, list[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(allocator, list); // GC OK
    return result;
}


void _CFBagSetContext(CFBagRef bag, void *context) {
    CFAllocatorRef allocator = CFGetAllocator(bag);
    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, bag, bag->_context, context);
}

CFIndex CFBagGetCount(CFBagRef bag) {
    __CFGenericValidateType(bag, __kCFBagTypeID);
    return bag->_count;
}

CFIndex CFBagGetCountOfValue(CFBagRef bag, const void *value) {
    struct __CFBagBucket *match;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    if (0 == bag->_count) return 0;
    __CFBagFindBuckets1(bag, value, &match);
    return (match ? match->_count : 0);
}

Boolean CFBagContainsValue(CFBagRef bag, const void *value) {
    struct __CFBagBucket *match;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    if (0 == bag->_count) return false;
    __CFBagFindBuckets1(bag, value, &match);
    return (match ? true : false);
}

const void *CFBagGetValue(CFBagRef bag, const void *value) {
    struct __CFBagBucket *match;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    if (0 == bag->_count) return NULL;
    __CFBagFindBuckets1(bag, value, &match);
    return (match ? match->_key : NULL);
}

Boolean CFBagGetValueIfPresent(CFBagRef bag, const void *candidate, const void **value) {
    struct __CFBagBucket *match;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    if (0 == bag->_count) return false;
    __CFBagFindBuckets1(bag, candidate, &match);
    return (match ? ((value ? __CFObjCStrongAssign(match->_key, value) : NULL), true) : false);
}

void CFBagGetValues(CFBagRef bag, const void **values) {
    struct __CFBagBucket *buckets;
    CFIndex idx, cnt, nbuckets;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    if (CF_USING_COLLECTABLE_MEMORY) {
	// GC: speculatively issue a write-barrier on the copied to buffers (3743553).
	__CFObjCWriteBarrierRange(values, bag->_count * sizeof(void *));
    }
    buckets = bag->_buckets;
    nbuckets = bag->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFBagBucketIsOccupied(bag, &buckets[idx])) {
	    for (cnt = buckets[idx]._count; cnt--;) {
		if (values) *values++ = buckets[idx]._key;
	    }
	}
    }
}

void CFBagApplyFunction(CFBagRef bag, CFBagApplierFunction applier, void *context) {
    struct __CFBagBucket *buckets;
    CFIndex idx, cnt, nbuckets;
    FAULT_CALLBACK((void **)&(applier));
    __CFGenericValidateType(bag, __kCFBagTypeID);
    buckets = bag->_buckets;
    nbuckets = bag->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFBagBucketIsOccupied(bag, &buckets[idx])) {
	    for (cnt = buckets[idx]._count; cnt--;) {
		INVOKE_CALLBACK2(applier, buckets[idx]._key, context);
	    }
	}
    }
}

static void __CFBagGrow(CFMutableBagRef bag, CFIndex numNewValues) {
    struct __CFBagBucket *oldbuckets = bag->_buckets;
    CFIndex idx, oldnbuckets = bag->_bucketsNum;
    CFIndex oldCount = bag->_count;
    CFAllocatorRef allocator = CFGetAllocator(bag);
    bag->_capacity = __CFBagRoundUpCapacity(oldCount + numNewValues);
    bag->_bucketsNum = __CFBagNumBucketsForCapacity(bag->_capacity);
    void *bucket = _CFAllocatorAllocateGC(allocator, bag->_bucketsNum * sizeof(struct __CFBagBucket), isStrongMemory(bag) ? AUTO_MEMORY_SCANNED : AUTO_MEMORY_UNSCANNED);
    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, bag, bag->_buckets, bucket);
    if (__CFOASafe) __CFSetLastAllocationEventName(bag->_buckets, "CFBag (store)");
    if (NULL == bag->_buckets) HALT;
    for (idx = bag->_bucketsNum; idx--;) {
	bag->_buckets[idx]._key = bag->_emptyMarker;
    }
    if (NULL == oldbuckets) return;
    for (idx = 0; idx < oldnbuckets; idx++) {
	if (__CFBagBucketIsOccupied(bag, &oldbuckets[idx])) {
	    struct __CFBagBucket *match, *nomatch;
	    __CFBagFindBuckets2(bag, oldbuckets[idx]._key, &match, &nomatch);
	    CFAssert3(!match, __kCFLogAssertion, "%s(): two values (%p, %p) now hash to the same slot; mutable value changed while in table or hash value is not immutable", __PRETTY_FUNCTION__, oldbuckets[idx]._key, match->_key);
	    if (nomatch) {
		CF_WRITE_BARRIER_ASSIGN(allocator, nomatch->_key, oldbuckets[idx]._key);
		nomatch->_count = oldbuckets[idx]._count;
	    }
	}
    }
    CFAssert1(bag->_count == oldCount, __kCFLogAssertion, "%s(): bag count differs after rehashing; error", __PRETTY_FUNCTION__);
    _CFAllocatorDeallocateGC(CFGetAllocator(bag), oldbuckets);
}

// This function is for Foundation's benefit; no one else should use it.
void _CFBagSetCapacity(CFMutableBagRef bag, CFIndex cap) {
    if (CF_IS_OBJC(__kCFBagTypeID, bag)) return;
#if defined(DEBUG)
    __CFGenericValidateType(bag, __kCFBagTypeID);
    CFAssert1(__CFBagGetType(bag) != __kCFBagImmutable && __CFBagGetType(bag) != __kCFBagFixedMutable, __kCFLogAssertion, "%s(): bag is immutable or fixed-mutable", __PRETTY_FUNCTION__);
    CFAssert3(bag->_count <= cap, __kCFLogAssertion, "%s(): desired capacity (%d) is less than count (%d)", __PRETTY_FUNCTION__, cap, bag->_count);
#endif
    __CFBagGrow(bag, cap - bag->_count);
}

void CFBagAddValue(CFMutableBagRef bag, const void *value) {
    struct __CFBagBucket *match, *nomatch;
    CFAllocatorRef allocator;
    const CFBagCallBacks *cb;
    const void *newValue;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    switch (__CFBagGetType(bag)) {
    case __kCFBagMutable:
	if (bag->_bucketsUsed == bag->_capacity || NULL == bag->_buckets) {
	    __CFBagGrow(bag, 1);
	}
	break;
    case __kCFBagFixedMutable:
	CFAssert3(bag->_count < bag->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity bag %p (capacity = %d)", __PRETTY_FUNCTION__, bag, bag->_capacity);
	break;
    default:
	CFAssert2(__CFBagGetType(bag) != __kCFBagImmutable, __kCFLogAssertion, "%s(): immutable bag %p passed to mutating operation", __PRETTY_FUNCTION__, bag);
	break;
    }
    __CFBagFindBuckets2(bag, value, &match, &nomatch);
    if (match) {
	match->_count++; bag->_count++;
    } else {
        allocator = CFGetAllocator(bag);
	cb = __CFBagGetCallBacks(bag);
	if (cb->retain) {
	    newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, value, bag->_context);
	} else {
	    newValue = value;
	}
	if (bag->_emptyMarker == newValue) {
	    __CFBagFindNewEmptyMarker(bag);
	}
	if (bag->_deletedMarker == newValue) {
	    __CFBagFindNewDeletedMarker(bag);
	}
	CF_WRITE_BARRIER_ASSIGN(allocator, nomatch->_key, newValue);
	nomatch->_count = 1;
	bag->_bucketsUsed++;
	bag->_count++;
    }
}

void CFBagReplaceValue(CFMutableBagRef bag, const void *value) {
    struct __CFBagBucket *match;
    CFAllocatorRef allocator;
    const CFBagCallBacks *cb;
    const void *newValue;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    switch (__CFBagGetType(bag)) {
    case __kCFBagMutable:
    case __kCFBagFixedMutable:
	break;
    default:
	CFAssert2(__CFBagGetType(bag) != __kCFBagImmutable, __kCFLogAssertion, "%s(): immutable bag %p passed to mutating operation", __PRETTY_FUNCTION__, bag);
	break;
    }
    if (0 == bag->_count) return;
    __CFBagFindBuckets1(bag, value, &match);
    if (!match) return;
    allocator = CFGetAllocator(bag);
    cb = __CFBagGetCallBacks(bag);
    if (cb->retain) {
	newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, value, bag->_context);
    } else {
	newValue = value;
    }
    if (cb->release) {
	INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, match->_key, bag->_context);
	match->_key = bag->_deletedMarker;
    }
    if (bag->_emptyMarker == newValue) {
	__CFBagFindNewEmptyMarker(bag);
    }
    if (bag->_deletedMarker == newValue) {
	__CFBagFindNewDeletedMarker(bag);
    }
    CF_WRITE_BARRIER_ASSIGN(allocator, match->_key, newValue);
}

void CFBagSetValue(CFMutableBagRef bag, const void *value) {
    struct __CFBagBucket *match, *nomatch;
    CFAllocatorRef allocator;
    const CFBagCallBacks *cb;
    const void *newValue;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    switch (__CFBagGetType(bag)) {
    case __kCFBagMutable:
	if (bag->_bucketsUsed == bag->_capacity || NULL == bag->_buckets) {
	    __CFBagGrow(bag, 1);
	}
	break;
    case __kCFBagFixedMutable:
	break;
    default:
	CFAssert2(__CFBagGetType(bag) != __kCFBagImmutable, __kCFLogAssertion, "%s(): immutable bag %p passed to mutating operation", __PRETTY_FUNCTION__, bag);
	break;
    }
    __CFBagFindBuckets2(bag, value, &match, &nomatch);
    allocator = CFGetAllocator(bag);
    cb = __CFBagGetCallBacks(bag);
    if (cb->retain) {
	newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), allocator, value, bag->_context);
    } else {
	newValue = value;
    }
    if (match) {
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, match->_key, bag->_context);
	    match->_key = bag->_deletedMarker;
	}
	if (bag->_emptyMarker == newValue) {
	    __CFBagFindNewEmptyMarker(bag);
	}
	if (bag->_deletedMarker == newValue) {
	    __CFBagFindNewDeletedMarker(bag);
	}
	CF_WRITE_BARRIER_ASSIGN(allocator, match->_key, newValue);
    } else {
	CFAssert3(__kCFBagFixedMutable != __CFBagGetType(bag) || bag->_count < bag->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity bag %p (capacity = %d)", __PRETTY_FUNCTION__, bag, bag->_capacity);
	if (bag->_emptyMarker == newValue) {
	    __CFBagFindNewEmptyMarker(bag);
	}
	if (bag->_deletedMarker == newValue) {
	    __CFBagFindNewDeletedMarker(bag);
	}
	CF_WRITE_BARRIER_ASSIGN(allocator, nomatch->_key, newValue);
	nomatch->_count = 1;
	bag->_bucketsUsed++;
	bag->_count++;
    }
}

void CFBagRemoveValue(CFMutableBagRef bag, const void *value) {
    struct __CFBagBucket *match;
    const CFBagCallBacks *cb;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    switch (__CFBagGetType(bag)) {
    case __kCFBagMutable:
    case __kCFBagFixedMutable:
	break;
    default:
	CFAssert2(__CFBagGetType(bag) != __kCFBagImmutable, __kCFLogAssertion, "%s(): immutable bag %p passed to mutating operation", __PRETTY_FUNCTION__, bag);
	break;
    }
    if (0 == bag->_count) return;
    __CFBagFindBuckets1(bag, value, &match);
    if (!match) return;
    match->_count--; bag->_count--;
    if (0 == match->_count) {
	cb = __CFBagGetCallBacks(bag);
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), CFGetAllocator(bag), match->_key, bag->_context);
	}
        match->_key = bag->_deletedMarker;
	bag->_bucketsUsed--;
    }
}

void CFBagRemoveAllValues(CFMutableBagRef bag) {
    struct __CFBagBucket *buckets;
    const CFBagCallBacks *cb;
    CFAllocatorRef allocator;
    CFIndex idx, nbuckets;
    __CFGenericValidateType(bag, __kCFBagTypeID);
    switch (__CFBagGetType(bag)) {
    case __kCFBagMutable:
    case __kCFBagFixedMutable:
	break;
    default:
	CFAssert2(__CFBagGetType(bag) != __kCFBagImmutable, __kCFLogAssertion, "%s(): immutable bag %p passed to mutating operation", __PRETTY_FUNCTION__, bag);
	break;
    }
    if (0 == bag->_count) return;
    buckets = bag->_buckets;
    nbuckets = bag->_bucketsNum;
    cb = __CFBagGetCallBacks(bag);
    allocator = CFGetAllocator(bag);
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFBagBucketIsOccupied(bag, &buckets[idx])) {
	    if (cb->release) {
		INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, buckets[idx]._key, bag->_context);
	    }
	    buckets[idx]._key = bag->_emptyMarker;
	    buckets[idx]._count = 0;
	}
    }
    bag->_bucketsUsed = 0;
    bag->_count = 0;
}

