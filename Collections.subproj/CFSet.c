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

static const uint32_t __CFSetBuckets[42] = {    // primes
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
    __kCFSetImmutable = 0,		/* unchangable and fixed capacity */
    __kCFSetMutable = 1,		/* changeable and variable capacity */
    __kCFSetFixedMutable = 3		/* changeable and fixed capacity */
};

enum {		/* Bits 3-2 */
    __kCFSetHasNullCallBacks = 0,
    __kCFSetHasCFTypeCallBacks = 1,
    __kCFSetHasCustomCallBacks = 3	/* callbacks are at end of header */
};

struct __CFSetBucket {
    const void *_key;
};

struct __CFSet {
    CFRuntimeBase _base;
    CFIndex _count;		/* number of values */
    CFIndex _capacity;		/* maximum number of values */
    CFIndex _bucketsUsed;	/* number of slots used */
    CFIndex _bucketsNum;	/* number of slots */
    const void *_emptyMarker;
    const void *_deletedMarker;
    void *_context;		/* private */
    struct __CFSetBucket *_buckets;	/* can be NULL if not allocated yet */
};

CF_INLINE bool __CFSetBucketIsEmpty(CFSetRef set, const struct __CFSetBucket *bucket) {
    return (set->_emptyMarker == bucket->_key);
}

CF_INLINE bool __CFSetBucketIsDeleted(CFSetRef set, const struct __CFSetBucket *bucket) {
    return (set->_deletedMarker == bucket->_key);
}

CF_INLINE bool __CFSetBucketIsOccupied(CFSetRef set, const struct __CFSetBucket *bucket) {
    return (set->_emptyMarker != bucket->_key && set->_deletedMarker != bucket->_key);
}

/* Bits 1-0 of the base reserved bits are used for mutability variety */
/* Bits 3-2 of the base reserved bits are used for callback indicator bits */

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

CF_INLINE const CFSetCallBacks *__CFSetGetCallBacks(CFSetRef set) {
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


static void __CFSetFindBuckets1(CFSetRef set, const void *key, struct __CFSetBucket **match) {
    const CFSetCallBacks *cb = __CFSetGetCallBacks(set);
    struct __CFSetBucket *buckets = set->_buckets;
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, set->_context) : (CFHashCode)key;
    UInt32 start = keyHash % set->_bucketsNum;
    UInt32 probe = start;
    UInt32 probeskip = 1;
    *match = NULL;
    for (;;) {
	struct __CFSetBucket *currentBucket = buckets + probe;
	if (__CFSetBucketIsEmpty(set, currentBucket)) {
	    return;
	} else if (__CFSetBucketIsDeleted(set, currentBucket)) {
	    /* do nothing */
	} else if (currentBucket->_key == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))cb->equal, currentBucket->_key, key, set->_context))) {
	    *match = currentBucket;
	    return;
	}
	probe = (probe + probeskip) % set->_bucketsNum;
	if (start == probe) return;
    }
}

static void __CFSetFindBuckets2(CFSetRef set, const void *key, struct __CFSetBucket **match, struct __CFSetBucket **nomatch) {
    const CFSetCallBacks *cb = __CFSetGetCallBacks(set);
    struct __CFSetBucket *buckets = set->_buckets;
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, set->_context) : (CFHashCode)key;
    UInt32 start = keyHash % set->_bucketsNum;
    UInt32 probe = start;
    UInt32 probeskip = 1;
    *match = NULL;
    *nomatch = NULL;
    for (;;) {
	struct __CFSetBucket *currentBucket = buckets + probe;
	if (__CFSetBucketIsEmpty(set, currentBucket)) {
	    if (!*nomatch) *nomatch = currentBucket;
	    return;
	} else if (__CFSetBucketIsDeleted(set, currentBucket)) {
	    if (!*nomatch) *nomatch = currentBucket;
	} else if (!*match && (currentBucket->_key == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))cb->equal, currentBucket->_key, key, set->_context)))) {
	    *match = currentBucket;
	    if (*nomatch) return;
	}
	probe = (probe + probeskip) % set->_bucketsNum;
	if (start == probe) return;
    }
}

static void __CFSetFindNewEmptyMarker(CFSetRef set) {
    struct __CFSetBucket *buckets;
    const void *newEmpty;
    bool hit;
    CFIndex idx, nbuckets;
    buckets = set->_buckets;
    nbuckets = set->_bucketsNum;
    newEmpty = set->_emptyMarker;
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
	if (set->_emptyMarker == buckets[idx]._key) {
	    buckets[idx]._key = newEmpty;
	}
    }
    ((struct __CFSet *)set)->_emptyMarker = newEmpty;
}

static void __CFSetFindNewDeletedMarker(CFSetRef set) {
    struct __CFSetBucket *buckets;
    const void *newDeleted;
    bool hit;
    CFIndex idx, nbuckets;
    buckets = set->_buckets;
    nbuckets = set->_bucketsNum;
    newDeleted = set->_deletedMarker;
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
	if (set->_deletedMarker == buckets[idx]._key) {
	    buckets[idx]._key = newDeleted;
	}
    }
    ((struct __CFSet *)set)->_deletedMarker = newDeleted;
}

static bool __CFSetEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFSetRef set1 = (CFSetRef)cf1;
    CFSetRef set2 = (CFSetRef)cf2;
    const CFSetCallBacks *cb1, *cb2;
    const struct __CFSetBucket *buckets;
    CFIndex idx, nbuckets;
    if (set1 == set2) return true;
    if (set1->_count != set2->_count) return false;
    cb1 = __CFSetGetCallBacks(set1);
    cb2 = __CFSetGetCallBacks(set2);
    if (cb1->equal != cb2->equal) return false;
    if (0 == set1->_count) return true; /* after function comparison! */
    buckets = set1->_buckets;
    nbuckets = set1->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFSetBucketIsOccupied(set1, &buckets[idx])) {
	    if (1 != CFSetGetCountOfValue(set2, buckets[idx]._key)) {
		return false;
	    }
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
    const CFSetCallBacks *cb;
    const struct __CFSetBucket *buckets;
    CFIndex idx, nbuckets;
    CFMutableStringRef result;
    cb = __CFSetGetCallBacks(set);
    buckets = set->_buckets;
    nbuckets = set->_bucketsNum;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFSet %p [%p]>{count = %u, capacity = %u, values = (\n"), set, CFGetAllocator(set), set->_count, set->_capacity);
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFSetBucketIsOccupied(set, &buckets[idx])) {
	    CFStringRef desc = NULL;
	    if (NULL != cb->copyDescription) {
		desc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(const void *, void *))cb->copyDescription), buckets[idx]._key, set->_context);
	    }
	    if (NULL != desc) {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@\n"), idx, desc, NULL);
		CFRelease(desc);
	    } else {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p>\n"), idx, buckets[idx]._key, NULL);
	    }
	}
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static void __CFSetDeallocate(CFTypeRef cf) {
    CFMutableSetRef set = (CFMutableSetRef)cf;
    CFAllocatorRef allocator = __CFGetAllocator(set);
    if (__CFSetGetType(set) == __kCFSetImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)set)->_info, 1, 0, __kCFSetFixedMutable);
    }
    CFSetRemoveAllValues(set);
    if (__CFSetGetType(set) == __kCFSetMutable && set->_buckets) {
	CFAllocatorDeallocate(allocator, set->_buckets);
    }
}

static CFTypeID __kCFSetTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFSetClass = {
    0,
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

static CFSetRef __CFSetInit(CFAllocatorRef allocator, UInt32 flags, CFIndex capacity, const CFSetCallBacks *callBacks) {
    struct __CFSet *memory;
    UInt32 size;
    CFIndex idx;
    __CFBitfieldSetValue(flags, 31, 2, 0);
    if (__CFSetCallBacksMatchNull(callBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFSetHasNullCallBacks);
    } else if (__CFSetCallBacksMatchCFType(callBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFSetHasCFTypeCallBacks);
    } else {
	__CFBitfieldSetValue(flags, 3, 2, __kCFSetHasCustomCallBacks);
    }
    size = __CFSetGetSizeOfType(flags) - sizeof(CFRuntimeBase);
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFSetImmutable:
    case __kCFSetFixedMutable:
	size += __CFSetNumBucketsForCapacity(capacity) * sizeof(struct __CFSetBucket);
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
    memory->_bucketsUsed = 0;
    memory->_emptyMarker = (const void *)0xa1b1c1d3;
    memory->_deletedMarker = (const void *)0xa1b1c1d5;
    memory->_context = NULL;
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFSetImmutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFSet (immutable)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFSetNumBucketsForCapacity(memory->_capacity);
	memory->_buckets = (struct __CFSetBucket *)((uint8_t *)memory + __CFSetGetSizeOfType(flags));
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_buckets[idx]._key = memory->_emptyMarker;
	}
	break;
    case __kCFSetFixedMutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFSet (mutable-fixed)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFSetNumBucketsForCapacity(memory->_capacity);
	memory->_buckets = (struct __CFSetBucket *)((uint8_t *)memory + __CFSetGetSizeOfType(flags));
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_buckets[idx]._key = memory->_emptyMarker;
	}
	break;
    case __kCFSetMutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFSet (mutable-variable)");
	memory->_capacity = __CFSetRoundUpCapacity(1);
	memory->_bucketsNum = 0;
	memory->_buckets = NULL;
	break;
    }
    if (__kCFSetHasCustomCallBacks == __CFBitfieldGetValue(flags, 3, 2)) {
	const CFSetCallBacks *cb = __CFSetGetCallBacks((CFSetRef)memory);
	*(CFSetCallBacks *)cb = *callBacks;
	FAULT_CALLBACK((void **)&(cb->retain));
	FAULT_CALLBACK((void **)&(cb->release));
	FAULT_CALLBACK((void **)&(cb->copyDescription));
	FAULT_CALLBACK((void **)&(cb->equal));
	FAULT_CALLBACK((void **)&(cb->hash));
    }
    return (CFSetRef)memory;
}

CFSetRef CFSetCreate(CFAllocatorRef allocator, const void **values, CFIndex numValues, const CFSetCallBacks *callBacks) {
    CFSetRef result;
    UInt32 flags;
    CFIndex idx;
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%d) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
    result = __CFSetInit(allocator, __kCFSetImmutable, numValues, callBacks);
    flags = __CFBitfieldGetValue(((const CFRuntimeBase *)result)->_info, 1, 0);
    if (flags == __kCFSetImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, __kCFSetFixedMutable);
    }
    for (idx = 0; idx < numValues; idx++) {
	CFSetAddValue((CFMutableSetRef)result, values[idx]);
    }
    __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, flags);
    return result;
}

CFMutableSetRef CFSetCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFSetCallBacks *callBacks) {
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%d) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    return (CFMutableSetRef)__CFSetInit(allocator, (0 == capacity) ? __kCFSetMutable : __kCFSetFixedMutable, capacity, callBacks);
}

CFSetRef CFSetCreateCopy(CFAllocatorRef allocator, CFSetRef set) {
    CFSetRef result;
    const CFSetCallBacks *cb;
    CFIndex numValues = CFSetGetCount(set);
    const void **list, *buffer[256];
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0);
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFSet (temp)");
    CFSetGetValues(set, list);
    cb = CF_IS_OBJC(__kCFSetTypeID, set) ? &kCFTypeSetCallBacks : __CFSetGetCallBacks(set);
    result = CFSetCreate(allocator, list, numValues, cb);
    if (list != buffer) CFAllocatorDeallocate(allocator, list);
    return result;
}

CFMutableSetRef CFSetCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFSetRef set) {
    CFMutableSetRef result;
    const CFSetCallBacks *cb;
    CFIndex idx, numValues = CFSetGetCount(set);
    const void **list, *buffer[256];
    CFAssert3(0 == capacity || numValues <= capacity, __kCFLogAssertion, "%s(): for fixed-mutable sets, capacity (%d) must be greater than or equal to initial number of values (%d)", __PRETTY_FUNCTION__, capacity, numValues);
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0);
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFSet (temp)");
    CFSetGetValues(set, list);
    cb = CF_IS_OBJC(__kCFSetTypeID, set) ? &kCFTypeSetCallBacks : __CFSetGetCallBacks(set);
    result = CFSetCreateMutable(allocator, capacity, cb);
    if (0 == capacity) _CFSetSetCapacity(result, numValues);
    for (idx = 0; idx < numValues; idx++) {
	CFSetAddValue(result, list[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(allocator, list);
    return result;
}

void _CFSetSetContext(CFSetRef set, void *context) {
    ((struct __CFSet *)set)->_context = context;
}

CFIndex CFSetGetCount(CFSetRef set) {
    CF_OBJC_FUNCDISPATCH0(__kCFSetTypeID, CFIndex, set, "count");
    __CFGenericValidateType(set, __kCFSetTypeID);
    return set->_count;
}

CFIndex CFSetGetCountOfValue(CFSetRef set, const void *value) {
    struct __CFSetBucket *match;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, CFIndex, set, "countForObject:", value);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return 0;
    __CFSetFindBuckets1(set, value, &match);
    return (match ? 1 : 0);
}

Boolean CFSetContainsValue(CFSetRef set, const void *value) {
    struct __CFSetBucket *match;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, char, set, "containsObject:", value);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return false;
    __CFSetFindBuckets1(set, value, &match);
    return (match ? true : false);
}

const void *CFSetGetValue(CFSetRef set, const void *value) {
    struct __CFSetBucket *match;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, const void *, set, "member:", value);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return NULL;
    __CFSetFindBuckets1(set, value, &match);
    return (match ? match->_key : NULL);
}

Boolean CFSetGetValueIfPresent(CFSetRef set, const void *candidate, const void **value) {
    struct __CFSetBucket *match;
    CF_OBJC_FUNCDISPATCH2(__kCFSetTypeID, char, set, "_getValue:forObj:", (void * *)value, candidate);
    __CFGenericValidateType(set, __kCFSetTypeID);
    if (0 == set->_count) return false;
    __CFSetFindBuckets1(set, candidate, &match);
    return (match ? ((value ? *value = match->_key : NULL), true) : false);
}

void CFSetGetValues(CFSetRef set, const void **values) {
    struct __CFSetBucket *buckets;
    CFIndex idx, cnt, nbuckets;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "getObjects:", (void * *)values);
    __CFGenericValidateType(set, __kCFSetTypeID);
    buckets = set->_buckets;
    nbuckets = set->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFSetBucketIsOccupied(set, &buckets[idx])) {
	    for (cnt = 1; cnt--;) {
		if (values) *values++ = buckets[idx]._key;
	    }
	}
    }
}

void CFSetApplyFunction(CFSetRef set, CFSetApplierFunction applier, void *context) {
    struct __CFSetBucket *buckets;
    CFIndex idx, cnt, nbuckets;
    FAULT_CALLBACK((void **)&(applier));
    CF_OBJC_FUNCDISPATCH2(__kCFSetTypeID, void, set, "_applyValues:context:", applier, context);
    __CFGenericValidateType(set, __kCFSetTypeID);
    buckets = set->_buckets;
    nbuckets = set->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFSetBucketIsOccupied(set, &buckets[idx])) {
	    for (cnt = 1; cnt--;) {
		INVOKE_CALLBACK2(applier, buckets[idx]._key, context);
	    }
	}
    }
}

static void __CFSetGrow(CFMutableSetRef set, CFIndex numNewValues) {
    struct __CFSetBucket *oldbuckets = set->_buckets;
    CFIndex idx, oldnbuckets = set->_bucketsNum;
    CFIndex oldCount = set->_count;
    set->_capacity = __CFSetRoundUpCapacity(oldCount + numNewValues);
    set->_bucketsNum = __CFSetNumBucketsForCapacity(set->_capacity);
    set->_buckets = CFAllocatorAllocate(__CFGetAllocator(set), set->_bucketsNum * sizeof(struct __CFSetBucket), 0);
    if (NULL == set->_buckets) HALT;
    if (__CFOASafe) __CFSetLastAllocationEventName(set->_buckets, "CFSet (store)");
    for (idx = set->_bucketsNum; idx--;) {
	set->_buckets[idx]._key = set->_emptyMarker;
    }
    if (NULL == oldbuckets) return;
    for (idx = 0; idx < oldnbuckets; idx++) {
	if (__CFSetBucketIsOccupied(set, &oldbuckets[idx])) {
	    struct __CFSetBucket *match, *nomatch;
	    __CFSetFindBuckets2(set, oldbuckets[idx]._key, &match, &nomatch);
	    CFAssert3(!match, __kCFLogAssertion, "%s(): two values (%p, %p) now hash to the same slot; mutable value changed while in table or hash value is not immutable", __PRETTY_FUNCTION__, oldbuckets[idx]._key, match->_key);
	    nomatch->_key = oldbuckets[idx]._key;
	}
    }
    CFAssert1(set->_count == oldCount, __kCFLogAssertion, "%s(): set count differs after rehashing; error", __PRETTY_FUNCTION__);
    CFAllocatorDeallocate(__CFGetAllocator(set), oldbuckets);
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

// This function is for Foundation's benefit; no one else should use it.
bool _CFSetIsMutable(CFSetRef set) { 
    return (__CFSetGetType(set) != __kCFSetImmutable);
}       

void CFSetAddValue(CFMutableSetRef set, const void *value) {
    struct __CFSetBucket *match, *nomatch;
    const CFSetCallBacks *cb;
    const void *newValue;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "addObject:", value);
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
	if (set->_bucketsUsed == set->_capacity || NULL == set->_buckets) {
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
    __CFSetFindBuckets2(set, value, &match, &nomatch);
    if (match) {
    } else {
	cb = __CFSetGetCallBacks(set);
	if (cb->retain) {
	    newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), __CFGetAllocator(set), value, set->_context);
	} else {
	    newValue = value;
	}
	if (set->_emptyMarker == newValue) {
	    __CFSetFindNewEmptyMarker(set);
	}
	if (set->_deletedMarker == newValue) {
	    __CFSetFindNewDeletedMarker(set);
	}
	nomatch->_key = newValue;
	set->_bucketsUsed++;
	set->_count++;
    }
}

__private_extern__ const void *__CFSetAddValueAndReturn(CFMutableSetRef set, const void *value) {
    struct __CFSetBucket *match, *nomatch;
    const CFSetCallBacks *cb;
    const void *newValue;
// #warning not toll-free bridged, but internal
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
	if (set->_bucketsUsed == set->_capacity || NULL == set->_buckets) {
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
    __CFSetFindBuckets2(set, value, &match, &nomatch);
    if (match) {
	return match->_key;
    } else {
	cb = __CFSetGetCallBacks(set);
	if (cb->retain) {
	    newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), __CFGetAllocator(set), value, set->_context);
	} else {
	    newValue = value;
	}
	if (set->_emptyMarker == newValue) {
	    __CFSetFindNewEmptyMarker(set);
	}
	if (set->_deletedMarker == newValue) {
	    __CFSetFindNewDeletedMarker(set);
	}
	nomatch->_key = newValue;
	set->_bucketsUsed++;
	set->_count++;
	return newValue;
    }
}

void CFSetReplaceValue(CFMutableSetRef set, const void *value) {
    struct __CFSetBucket *match;
    const CFSetCallBacks *cb;
    const void *newValue;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "_replaceObject:", value);
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
    __CFSetFindBuckets1(set, value, &match);
    if (!match) return;
    cb = __CFSetGetCallBacks(set);
    if (cb->retain) {
	newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), __CFGetAllocator(set), value, set->_context);
    } else {
	newValue = value;
    }
    if (cb->release) {
	INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), __CFGetAllocator(set), match->_key, set->_context);
	match->_key = set->_deletedMarker;
    }
    if (set->_emptyMarker == newValue) {
	__CFSetFindNewEmptyMarker(set);
    }
    if (set->_deletedMarker == newValue) {
	__CFSetFindNewDeletedMarker(set);
    }
    match->_key = newValue;
}

void CFSetSetValue(CFMutableSetRef set, const void *value) {
    struct __CFSetBucket *match, *nomatch;
    const CFSetCallBacks *cb;
    const void *newValue;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "_setObject:", value);
    __CFGenericValidateType(set, __kCFSetTypeID);
    switch (__CFSetGetType(set)) {
    case __kCFSetMutable:
	if (set->_bucketsUsed == set->_capacity || NULL == set->_buckets) {
	    __CFSetGrow(set, 1);
	}
	break;
    case __kCFSetFixedMutable:
	break;
    default:
	CFAssert2(__CFSetGetType(set) != __kCFSetImmutable, __kCFLogAssertion, "%s(): immutable set %p passed to mutating operation", __PRETTY_FUNCTION__, set);
	break;
    }
    __CFSetFindBuckets2(set, value, &match, &nomatch);
    cb = __CFSetGetCallBacks(set);
    if (cb->retain) {
	newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), __CFGetAllocator(set), value, set->_context);
    } else {
	newValue = value;
    }
    if (match) {
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), __CFGetAllocator(set), match->_key, set->_context);
	    match->_key = set->_deletedMarker;
	}
	if (set->_emptyMarker == newValue) {
	    __CFSetFindNewEmptyMarker(set);
	}
	if (set->_deletedMarker == newValue) {
	    __CFSetFindNewDeletedMarker(set);
	}
	match->_key = newValue;
    } else {
	CFAssert3(__kCFSetFixedMutable != __CFSetGetType(set) || set->_count < set->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity set %p (capacity = %d)", __PRETTY_FUNCTION__, set, set->_capacity);
	if (set->_emptyMarker == newValue) {
	    __CFSetFindNewEmptyMarker(set);
	}
	if (set->_deletedMarker == newValue) {
	    __CFSetFindNewDeletedMarker(set);
	}
	nomatch->_key = newValue;
	set->_bucketsUsed++;
	set->_count++;
    }
}

void CFSetRemoveValue(CFMutableSetRef set, const void *value) {
    struct __CFSetBucket *match;
    const CFSetCallBacks *cb;
    CF_OBJC_FUNCDISPATCH1(__kCFSetTypeID, void, set, "removeObject:", value);
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
    __CFSetFindBuckets1(set, value, &match);
    if (!match) return;
    set->_count--;
    if (1) {
	cb = __CFSetGetCallBacks(set);
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), __CFGetAllocator(set), match->_key, set->_context);
	}
        match->_key = set->_deletedMarker;
	set->_bucketsUsed--;
    }
}

void CFSetRemoveAllValues(CFMutableSetRef set) {
    struct __CFSetBucket *buckets;
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
    buckets = set->_buckets;
    nbuckets = set->_bucketsNum;
    cb = __CFSetGetCallBacks(set);
    allocator = __CFGetAllocator(set);
    for (idx = 0; idx < nbuckets; idx++) {
	if (__CFSetBucketIsOccupied(set, &buckets[idx])) {
	    if (cb->release) {
		INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, buckets[idx]._key, set->_context);
	    }
	    buckets[idx]._key = set->_emptyMarker;
	}
    }
    set->_bucketsUsed = 0;
    set->_count = 0;
}

