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
/*	CFDictionary.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFDictionary.h>
#include "CFInternal.h"

const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFDictionaryKeyCallBacks kCFCopyStringDictionaryKeyCallBacks = {0, (void *)CFStringCreateCopy, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual};
static const CFDictionaryKeyCallBacks __kCFNullDictionaryKeyCallBacks = {0, NULL, NULL, NULL, NULL, NULL};
static const CFDictionaryValueCallBacks __kCFNullDictionaryValueCallBacks = {0, NULL, NULL, NULL, NULL};

static const uint32_t __CFDictionaryCapacities[42] = {
    4, 8, 17, 29, 47, 76, 123, 199, 322, 521, 843, 1364, 2207, 3571, 5778, 9349,
    15127, 24476, 39603, 64079, 103682, 167761, 271443, 439204, 710647, 1149851, 1860498,
    3010349, 4870847, 7881196, 12752043, 20633239, 33385282, 54018521, 87403803, 141422324,
    228826127, 370248451, 599074578, 969323029, 1568397607, 2537720636U
};

static const uint32_t __CFDictionaryBuckets[42] = {	// primes
    5, 11, 23, 41, 67, 113, 199, 317, 521, 839, 1361, 2207, 3571, 5779, 9349, 15121,
    24473, 39607, 64081, 103681, 167759, 271429, 439199, 710641, 1149857, 1860503, 3010349,
    4870843, 7881193, 12752029, 20633237, 33385273, 54018521, 87403763, 141422317, 228826121,
    370248451, 599074561, 969323023, 1568397599, 2537720629U, 4106118251U
};

CF_INLINE CFIndex __CFDictionaryRoundUpCapacity(CFIndex capacity) {
    CFIndex idx;
    for (idx = 0; idx < 42 && __CFDictionaryCapacities[idx] < (uint32_t)capacity; idx++);
    if (42 <= idx) HALT;
    return __CFDictionaryCapacities[idx];
}

CF_INLINE CFIndex __CFDictionaryNumBucketsForCapacity(CFIndex capacity) {
    CFIndex idx;
    for (idx = 0; idx < 42 && __CFDictionaryCapacities[idx] < (uint32_t)capacity; idx++);
    if (42 <= idx) HALT;
    return __CFDictionaryBuckets[idx];
}

enum {		/* Bits 1-0 */
    __kCFDictionaryImmutable = 0,	/* unchangable and fixed capacity */
    __kCFDictionaryMutable = 1,		/* changeable and variable capacity */
    __kCFDictionaryFixedMutable = 3	/* changeable and fixed capacity */
};

enum {		/* Bits 5-4 (value), 3-2 (key) */
    __kCFDictionaryHasNullCallBacks = 0,
    __kCFDictionaryHasCFTypeCallBacks = 1,
    __kCFDictionaryHasCustomCallBacks = 3	/* callbacks are at end of header */
};

struct __CFDictionary {
    CFRuntimeBase _base;
    CFIndex _count;		/* number of values */
    CFIndex _capacity;		/* maximum number of values */
    CFIndex _bucketsNum;	/* number of slots */
    uintptr_t _marker;
    void *_context;		/* private */
    CFIndex _deletes;
    const void **_keys;		/* can be NULL if not allocated yet */
    const void **_values;	/* can be NULL if not allocated yet */
};

/* Bits 1-0 of the base reserved bits are used for mutability variety */
/* Bits 3-2 of the base reserved bits are used for key callback indicator bits */
/* Bits 5-4 of the base reserved bits are used for value callback indicator bits */
/* Bit 6 is special KVO actions bit */

CF_INLINE CFIndex __CFDictionaryGetType(CFDictionaryRef dict) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 1, 0);
}

CF_INLINE CFIndex __CFDictionaryGetSizeOfType(CFIndex t) {
    CFIndex size = sizeof(struct __CFDictionary);
    if (__CFBitfieldGetValue(t, 3, 2) == __kCFDictionaryHasCustomCallBacks) {
	size += sizeof(CFDictionaryKeyCallBacks);
    }
    if (__CFBitfieldGetValue(t, 5, 4) == __kCFDictionaryHasCustomCallBacks) {
	size += sizeof(CFDictionaryValueCallBacks);
    }
    return size;
}

CF_INLINE const CFDictionaryKeyCallBacks *__CFDictionaryGetKeyCallBacks(CFDictionaryRef dict) {
    CFDictionaryKeyCallBacks *result = NULL;
    switch (__CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
    case __kCFDictionaryHasNullCallBacks:
	return &__kCFNullDictionaryKeyCallBacks;
    case __kCFDictionaryHasCFTypeCallBacks:
	return &kCFTypeDictionaryKeyCallBacks;
    case __kCFDictionaryHasCustomCallBacks:
	break;
    }
    result = (CFDictionaryKeyCallBacks *)((uint8_t *)dict + sizeof(struct __CFDictionary));
    return result;
}

CF_INLINE bool __CFDictionaryKeyCallBacksMatchNull(const CFDictionaryKeyCallBacks *c) {
    return (NULL == c ||
	(c->retain == __kCFNullDictionaryKeyCallBacks.retain &&
	 c->release == __kCFNullDictionaryKeyCallBacks.release &&
	 c->copyDescription == __kCFNullDictionaryKeyCallBacks.copyDescription &&
	 c->equal == __kCFNullDictionaryKeyCallBacks.equal &&
	 c->hash == __kCFNullDictionaryKeyCallBacks.hash));
}

CF_INLINE bool __CFDictionaryKeyCallBacksMatchCFType(const CFDictionaryKeyCallBacks *c) {
    return (&kCFTypeDictionaryKeyCallBacks == c ||
	(c->retain == kCFTypeDictionaryKeyCallBacks.retain &&
	 c->release == kCFTypeDictionaryKeyCallBacks.release &&
	 c->copyDescription == kCFTypeDictionaryKeyCallBacks.copyDescription &&
	 c->equal == kCFTypeDictionaryKeyCallBacks.equal &&
	 c->hash == kCFTypeDictionaryKeyCallBacks.hash));
}

CF_INLINE const CFDictionaryValueCallBacks *__CFDictionaryGetValueCallBacks(CFDictionaryRef dict) {
    CFDictionaryValueCallBacks *result = NULL;
    switch (__CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 5, 4)) {
    case __kCFDictionaryHasNullCallBacks:
	return &__kCFNullDictionaryValueCallBacks;
    case __kCFDictionaryHasCFTypeCallBacks:
	return &kCFTypeDictionaryValueCallBacks;
    case __kCFDictionaryHasCustomCallBacks:
	break;
    }
    if (__CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2) == __kCFDictionaryHasCustomCallBacks) {
	result = (CFDictionaryValueCallBacks *)((uint8_t *)dict + sizeof(struct __CFDictionary) + sizeof(CFDictionaryKeyCallBacks));
    } else {
	result = (CFDictionaryValueCallBacks *)((uint8_t *)dict + sizeof(struct __CFDictionary));
    }
    return result;
}

CF_INLINE bool __CFDictionaryValueCallBacksMatchNull(const CFDictionaryValueCallBacks *c) {
    return (NULL == c ||
	(c->retain == __kCFNullDictionaryValueCallBacks.retain &&
	 c->release == __kCFNullDictionaryValueCallBacks.release &&
	 c->copyDescription == __kCFNullDictionaryValueCallBacks.copyDescription &&
	 c->equal == __kCFNullDictionaryValueCallBacks.equal));
}

CF_INLINE bool __CFDictionaryValueCallBacksMatchCFType(const CFDictionaryValueCallBacks *c) {
    return (&kCFTypeDictionaryValueCallBacks == c ||
	(c->retain == kCFTypeDictionaryValueCallBacks.retain &&
	 c->release == kCFTypeDictionaryValueCallBacks.release &&
	 c->copyDescription == kCFTypeDictionaryValueCallBacks.copyDescription &&
	 c->equal == kCFTypeDictionaryValueCallBacks.equal));
}

CFIndex _CFDictionaryGetKVOBit(CFDictionaryRef dict) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 6, 6);
}

void _CFDictionarySetKVOBit(CFDictionaryRef dict, CFIndex bit) {
    __CFBitfieldSetValue(((CFRuntimeBase *)dict)->_info, 6, 6, (bit & 0x1));
}

#if !defined(__MACH__)

#define CF_OBJC_KVO_WILLCHANGE(obj, sel, a1)
#define CF_OBJC_KVO_DIDCHANGE(obj, sel, a1)

#else

static SEL __CF_KVO_WillChangeSelector = 0;
static SEL __CF_KVO_DidChangeSelector = 0;

#define CF_OBJC_KVO_WILLCHANGE(obj, key) \
	if (__CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 6, 6)) \
	{void (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	if (!__CF_KVO_WillChangeSelector) __CF_KVO_WillChangeSelector = __CFGetObjCSelector("willChangeValueForKey:"); \
	func((const void *)(obj), __CF_KVO_WillChangeSelector, (key));}

#define CF_OBJC_KVO_DIDCHANGE(obj, key) \
	if (__CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 6, 6)) \
	{void (*func)(const void *, SEL, ...) = (void *)__CFSendObjCMsg; \
	if (!__CF_KVO_DidChangeSelector) __CF_KVO_DidChangeSelector = __CFGetObjCSelector("didChangeValueForKey:"); \
	func((const void *)(obj), __CF_KVO_DidChangeSelector, (key));}

#endif


static CFIndex __CFDictionaryFindBuckets1a(CFDictionaryRef dict, const void *key) {
    CFHashCode keyHash = (CFHashCode)key;
    const void **keys = dict->_keys;
    uintptr_t marker = dict->_marker;
    CFIndex probe = keyHash % dict->_bucketsNum;
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
	if (dict->_bucketsNum <= probe) {
	    probe -= dict->_bucketsNum;
	}
	if (start == probe) {
	    return kCFNotFound;
	}
    }
}

static CFIndex __CFDictionaryFindBuckets1b(CFDictionaryRef dict, const void *key) {
    const CFDictionaryKeyCallBacks *cb = __CFDictionaryGetKeyCallBacks(dict);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, dict->_context) : (CFHashCode)key;
    const void **keys = dict->_keys;
    uintptr_t marker = dict->_marker;
    CFIndex probe = keyHash % dict->_bucketsNum;
    CFIndex probeskip = 1;	// See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
	uintptr_t currKey = (uintptr_t)keys[probe];
	if (marker == currKey) {		/* empty */
	    return kCFNotFound;
	} else if (~marker == currKey) {	/* deleted */
	    /* do nothing */
	} else if (currKey == (uintptr_t)key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))cb->equal, currKey, key, dict->_context))) {
	    return probe;
	}
	probe = probe + probeskip;
	// This alternative to probe % buckets assumes that
	// probeskip is always positive and less than the
	// number of buckets.
	if (dict->_bucketsNum <= probe) {
	    probe -= dict->_bucketsNum;
	}
	if (start == probe) {
	    return kCFNotFound;
	}
    }
}

static void __CFDictionaryFindBuckets2(CFDictionaryRef dict, const void *key, CFIndex *match, CFIndex *nomatch) {
    const CFDictionaryKeyCallBacks *cb = __CFDictionaryGetKeyCallBacks(dict);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(const void *, void *))cb->hash), key, dict->_context) : (CFHashCode)key;
    const void **keys = dict->_keys;
    uintptr_t marker = dict->_marker;
    CFIndex probe = keyHash % dict->_bucketsNum;
    CFIndex probeskip = 1;	// See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    *match = kCFNotFound;
    *nomatch = kCFNotFound;
    for (;;) {
	uintptr_t currKey = (uintptr_t)keys[probe];
	if (marker == currKey) {		/* empty */
	    if (kCFNotFound == *nomatch) *nomatch = probe;
	    return;
	} else if (~marker == currKey) {	/* deleted */
	    if (kCFNotFound == *nomatch) *nomatch = probe;
	    if (kCFNotFound != *match) {
		return;
	    }
	} else if (kCFNotFound == *match && (currKey == (uintptr_t)key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))cb->equal, currKey, key, dict->_context)))) {
	    *match = probe;
	    if (kCFNotFound != *nomatch) {
		return;
	    }
	}
	probe = probe + probeskip;
	// This alternative to probe % buckets assumes that
	// probeskip is always positive and less than the
	// number of buckets.
	if (dict->_bucketsNum <= probe) {
	    probe -= dict->_bucketsNum;
	}
	if (start == probe) {
	    return;
	}
    }
}

static void __CFDictionaryFindNewMarker(CFDictionaryRef dict) {
    const void **keys = dict->_keys;
    uintptr_t newMarker;
    CFIndex idx, nbuckets;
    bool hit;

    nbuckets = dict->_bucketsNum;
    newMarker = dict->_marker;
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
	if (dict->_marker == (uintptr_t)keys[idx]) {
	    keys[idx] = (const void *)newMarker;
	} else if (~dict->_marker == (uintptr_t)keys[idx]) {
	    keys[idx] = (const void *)~newMarker;
	}
    }
    ((struct __CFDictionary *)dict)->_marker = newMarker;
}

static bool __CFDictionaryEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFDictionaryRef dict1 = (CFDictionaryRef)cf1;
    CFDictionaryRef dict2 = (CFDictionaryRef)cf2;
    const CFDictionaryKeyCallBacks *cb1, *cb2;
    const CFDictionaryValueCallBacks *vcb1, *vcb2;
    const void **keys;
    CFIndex idx, nbuckets;
    if (dict1 == dict2) return true;
    if (dict1->_count != dict2->_count) return false;
    cb1 = __CFDictionaryGetKeyCallBacks(dict1);
    cb2 = __CFDictionaryGetKeyCallBacks(dict2);
    if (cb1->equal != cb2->equal) return false;
    vcb1 = __CFDictionaryGetValueCallBacks(dict1);
    vcb2 = __CFDictionaryGetValueCallBacks(dict2);
    if (vcb1->equal != vcb2->equal) return false;
    if (0 == dict1->_count) return true; /* after function comparison! */
    keys = dict1->_keys;
    nbuckets = dict1->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict1->_marker != (uintptr_t)keys[idx] && ~dict1->_marker != (uintptr_t)keys[idx]) {
	    const void *value;
	    if (!CFDictionaryGetValueIfPresent(dict2, keys[idx], &value)) return false;
	    if (dict1->_values[idx] != value && vcb1->equal && !INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))vcb1->equal, dict1->_values[idx], value, dict1->_context)) {
		return false;
	    }
	}
    }
    return true;
}

static CFHashCode __CFDictionaryHash(CFTypeRef cf) {
    CFDictionaryRef dict = (CFDictionaryRef)cf;
    return dict->_count;
}

static CFStringRef __CFDictionaryCopyDescription(CFTypeRef cf) {
    CFDictionaryRef dict = (CFDictionaryRef)cf;
    CFAllocatorRef allocator;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    const void **keys;
    CFIndex idx, nbuckets;
    CFMutableStringRef result;
    cb = __CFDictionaryGetKeyCallBacks(dict);
    vcb = __CFDictionaryGetValueCallBacks(dict);
    keys = dict->_keys;
    nbuckets = dict->_bucketsNum;
    allocator = CFGetAllocator(dict);
    result = CFStringCreateMutable(allocator, 0);
    switch (__CFDictionaryGetType(dict)) {
    case __kCFDictionaryImmutable:
	CFStringAppendFormat(result, NULL, CFSTR("<CFDictionary %p [%p]>{type = immutable, count = %u, capacity = %u, pairs = (\n"), cf, allocator, dict->_count, dict->_capacity);
	break;
    case __kCFDictionaryFixedMutable:
	CFStringAppendFormat(result, NULL, CFSTR("<CFDictionary %p [%p]>{type = fixed-mutable, count = %u, capacity = %u, pairs = (\n"), cf, allocator, dict->_count, dict->_capacity);
	break;
    case __kCFDictionaryMutable:
	CFStringAppendFormat(result, NULL, CFSTR("<CFDictionary %p [%p]>{type = mutable, count = %u, capacity = %u, pairs = (\n"), cf, allocator, dict->_count, dict->_capacity);
	break;
    }
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict->_marker != (uintptr_t)keys[idx] && ~dict->_marker != (uintptr_t)keys[idx]) {
	    CFStringRef kDesc = NULL, vDesc = NULL;
	    if (NULL != cb->copyDescription) {
		kDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(const void *, void *))cb->copyDescription), keys[idx], dict->_context);
	    }
	    if (NULL != vcb->copyDescription) {
		vDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(const void *, void *))vcb->copyDescription), dict->_values[idx], dict->_context);
	    }
	    if (NULL != kDesc && NULL != vDesc) {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ = %@\n"), idx, kDesc, vDesc);
		CFRelease(kDesc);
		CFRelease(vDesc);
	    } else if (NULL != kDesc) {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ = <%p>\n"), idx, kDesc, dict->_values[idx]);
		CFRelease(kDesc);
	    } else if (NULL != vDesc) {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> = %@\n"), idx, keys[idx], vDesc);
		CFRelease(vDesc);
	    } else {
		CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> = <%p>\n"), idx, keys[idx], dict->_values[idx]);
	    }
	}
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static void __CFDictionaryDeallocate(CFTypeRef cf) {
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)cf;
    CFAllocatorRef allocator = __CFGetAllocator(dict);
    if (__CFDictionaryGetType(dict) == __kCFDictionaryImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)dict)->_info, 1, 0, __kCFDictionaryFixedMutable);
    }
    CFDictionaryRemoveAllValues(dict);
    if (__CFDictionaryGetType(dict) == __kCFDictionaryMutable && dict->_keys) {
	CFAllocatorDeallocate(allocator, dict->_keys);
    }
}

static CFTypeID __kCFDictionaryTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFDictionaryClass = {
    0,
    "CFDictionary",
    NULL,	// init
    NULL,	// copy
    __CFDictionaryDeallocate,
    (void *)__CFDictionaryEqual,
    __CFDictionaryHash,
    NULL,	// 
    __CFDictionaryCopyDescription
};

__private_extern__ void __CFDictionaryInitialize(void) {
    __kCFDictionaryTypeID = _CFRuntimeRegisterClass(&__CFDictionaryClass);
}

CFTypeID CFDictionaryGetTypeID(void) {
    return __kCFDictionaryTypeID;
}

static CFDictionaryRef __CFDictionaryInit(CFAllocatorRef allocator, uint32_t flags, CFIndex capacity, const CFDictionaryKeyCallBacks *callBacks, const CFDictionaryValueCallBacks *valueCallBacks) {
    struct __CFDictionary *memory;
    uint32_t size;
    CFIndex idx;
    __CFBitfieldSetValue(flags, 31, 2, 0);
    if (__CFDictionaryKeyCallBacksMatchNull(callBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFDictionaryHasNullCallBacks);
    } else if (__CFDictionaryKeyCallBacksMatchCFType(callBacks)) {
	__CFBitfieldSetValue(flags, 3, 2, __kCFDictionaryHasCFTypeCallBacks);
    } else {
	__CFBitfieldSetValue(flags, 3, 2, __kCFDictionaryHasCustomCallBacks);
    }
    if (__CFDictionaryValueCallBacksMatchNull(valueCallBacks)) {
	__CFBitfieldSetValue(flags, 5, 4, __kCFDictionaryHasNullCallBacks);
    } else if (__CFDictionaryValueCallBacksMatchCFType(valueCallBacks)) {
	__CFBitfieldSetValue(flags, 5, 4, __kCFDictionaryHasCFTypeCallBacks);
    } else {
	__CFBitfieldSetValue(flags, 5, 4, __kCFDictionaryHasCustomCallBacks);
    }
    size = __CFDictionaryGetSizeOfType(flags) - sizeof(CFRuntimeBase);
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFDictionaryImmutable:
    case __kCFDictionaryFixedMutable:
	size += 2 * __CFDictionaryNumBucketsForCapacity(capacity) * sizeof(const void *);
	break;
    case __kCFDictionaryMutable:
	break;
    }
    memory = (struct __CFDictionary *)_CFRuntimeCreateInstance(allocator, __kCFDictionaryTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFBitfieldSetValue(memory->_base._info, 6, 0, flags);
    memory->_count = 0;
    memory->_marker = (uintptr_t)0xa1b1c1d3;
    memory->_context = NULL;
    memory->_deletes = 0;
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFDictionaryImmutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFDictionary (immutable)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFDictionaryNumBucketsForCapacity(memory->_capacity);
	memory->_keys = (const void **)((uint8_t *)memory + __CFDictionaryGetSizeOfType(flags));
	memory->_values = (const void **)(memory->_keys + memory->_bucketsNum);
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_keys[idx] = (const void *)memory->_marker;
	}
	break;
    case __kCFDictionaryFixedMutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFDictionary (mutable-fixed)");
	memory->_capacity = capacity;	/* Don't round up capacity */
	memory->_bucketsNum = __CFDictionaryNumBucketsForCapacity(memory->_capacity);
	memory->_keys = (const void **)((uint8_t *)memory + __CFDictionaryGetSizeOfType(flags));
	memory->_values = (const void **)(memory->_keys + memory->_bucketsNum);
	for (idx = memory->_bucketsNum; idx--;) {
	    memory->_keys[idx] = (const void *)memory->_marker;
	}
	break;
    case __kCFDictionaryMutable:
	if (__CFOASafe) __CFSetLastAllocationEventName(memory, "CFDictionary (mutable-variable)");
	memory->_capacity = __CFDictionaryRoundUpCapacity(1);
	memory->_bucketsNum = 0;
	memory->_keys = NULL;
	memory->_values = NULL;
	break;
    }
    if (__kCFDictionaryHasCustomCallBacks == __CFBitfieldGetValue(flags, 3, 2)) {
	const CFDictionaryKeyCallBacks *cb = __CFDictionaryGetKeyCallBacks((CFDictionaryRef)memory);
	*(CFDictionaryKeyCallBacks *)cb = *callBacks;
	FAULT_CALLBACK((void **)&(cb->retain));
	FAULT_CALLBACK((void **)&(cb->release));
	FAULT_CALLBACK((void **)&(cb->copyDescription));
	FAULT_CALLBACK((void **)&(cb->equal));
	FAULT_CALLBACK((void **)&(cb->hash));
    }
    if (__kCFDictionaryHasCustomCallBacks == __CFBitfieldGetValue(flags, 5, 4)) {
	const CFDictionaryValueCallBacks *vcb = __CFDictionaryGetValueCallBacks((CFDictionaryRef)memory);
	*(CFDictionaryValueCallBacks *)vcb = *valueCallBacks;
	FAULT_CALLBACK((void **)&(vcb->retain));
	FAULT_CALLBACK((void **)&(vcb->release));
	FAULT_CALLBACK((void **)&(vcb->copyDescription));
	FAULT_CALLBACK((void **)&(vcb->equal));
    }
    return (CFDictionaryRef)memory;
}

CFDictionaryRef CFDictionaryCreate(CFAllocatorRef allocator, const void **keys, const void **values, CFIndex numValues, const CFDictionaryKeyCallBacks *keyCallBacks, const CFDictionaryValueCallBacks *valueCallBacks) {
    CFDictionaryRef result;
    uint32_t flags;
    CFIndex idx;
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%d) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
    result = __CFDictionaryInit(allocator, __kCFDictionaryImmutable, numValues, keyCallBacks, valueCallBacks);
    flags = __CFBitfieldGetValue(((const CFRuntimeBase *)result)->_info, 1, 0);
    if (flags == __kCFDictionaryImmutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, __kCFDictionaryFixedMutable);
    }
    for (idx = 0; idx < numValues; idx++) {
	CFDictionaryAddValue((CFMutableDictionaryRef)result, keys[idx], values[idx]);
    }
    __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, flags);
    return result;
}

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFDictionaryKeyCallBacks *keyCallBacks, const CFDictionaryValueCallBacks *valueCallBacks) {
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%d) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    return (CFMutableDictionaryRef)__CFDictionaryInit(allocator, (0 == capacity) ? __kCFDictionaryMutable : __kCFDictionaryFixedMutable, capacity, keyCallBacks, valueCallBacks);
}

static void __CFDictionaryGrow(CFMutableDictionaryRef dict, CFIndex numNewValues);

CFDictionaryRef _CFDictionaryCreate_ex(CFAllocatorRef allocator, bool mutable, const void **keys, const void **values, CFIndex numValues) {
    CFDictionaryRef result;
    uint32_t flags;
    CFIndex idx;
    result = __CFDictionaryInit(allocator, mutable ? __kCFDictionaryMutable : __kCFDictionaryImmutable, numValues, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    flags = __CFBitfieldGetValue(((const CFRuntimeBase *)result)->_info, 1, 0);
    if (!mutable) {
        __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, __kCFDictionaryFixedMutable);
    }
    if (mutable) {
	if (result->_count == result->_capacity || NULL == result->_keys) {
	    __CFDictionaryGrow((CFMutableDictionaryRef)result, numValues);
	}
    }
    for (idx = 0; idx < numValues; idx++) {
	CFIndex match, nomatch;
	const void *newKey;
	__CFDictionaryFindBuckets2(result, keys[idx], &match, &nomatch);
	if (kCFNotFound != match) {
	    CFRelease(result->_values[match]);
	    result->_values[match] = values[idx];
	} else {
	    newKey = keys[idx];
	    if (result->_marker == (uintptr_t)newKey || ~result->_marker == (uintptr_t)newKey) {
		__CFDictionaryFindNewMarker(result);
	    }
	    result->_keys[nomatch] = newKey;
	    result->_values[nomatch] = values[idx];
	    ((CFMutableDictionaryRef)result)->_count++;
	}
    }
    __CFBitfieldSetValue(((CFRuntimeBase *)result)->_info, 1, 0, flags);
    return result;
}

CFDictionaryRef CFDictionaryCreateCopy(CFAllocatorRef allocator, CFDictionaryRef dict) {
    CFDictionaryRef result;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    CFIndex numValues = CFDictionaryGetCount(dict);
    const void **list, *buffer[256];
    const void **vlist, *vbuffer[256];
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0);
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFDictionary (temp)");
    vlist = (numValues <= 256) ? vbuffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0);
    if (vlist != vbuffer && __CFOASafe) __CFSetLastAllocationEventName(vlist, "CFDictionary (temp)");
    CFDictionaryGetKeysAndValues(dict, list, vlist);
    cb = CF_IS_OBJC(__kCFDictionaryTypeID, dict) ? &kCFTypeDictionaryKeyCallBacks : __CFDictionaryGetKeyCallBacks(dict);
    vcb = CF_IS_OBJC(__kCFDictionaryTypeID, dict) ? &kCFTypeDictionaryValueCallBacks : __CFDictionaryGetValueCallBacks(dict);
    result = CFDictionaryCreate(allocator, list, vlist, numValues, cb, vcb);
    if (list != buffer) CFAllocatorDeallocate(allocator, list);
    if (vlist != vbuffer) CFAllocatorDeallocate(allocator, vlist);
    return result;
}

CFMutableDictionaryRef CFDictionaryCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFDictionaryRef dict) {
    CFMutableDictionaryRef result;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    CFIndex idx, numValues = CFDictionaryGetCount(dict);
    const void **list, *buffer[256];
    const void **vlist, *vbuffer[256];
    CFAssert3(0 == capacity || numValues <= capacity, __kCFLogAssertion, "%s(): for fixed-mutable dicts, capacity (%d) must be greater than or equal to initial number of values (%d)", __PRETTY_FUNCTION__, capacity, numValues);
    list = (numValues <= 256) ? buffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0);
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFDictionary (temp)");
    vlist = (numValues <= 256) ? vbuffer : CFAllocatorAllocate(allocator, numValues * sizeof(void *), 0);
    if (vlist != vbuffer && __CFOASafe) __CFSetLastAllocationEventName(vlist, "CFDictionary (temp)");
    CFDictionaryGetKeysAndValues(dict, list, vlist);
    cb = CF_IS_OBJC(__kCFDictionaryTypeID, dict) ? &kCFTypeDictionaryKeyCallBacks : __CFDictionaryGetKeyCallBacks(dict);
    vcb = CF_IS_OBJC(__kCFDictionaryTypeID, dict) ? &kCFTypeDictionaryValueCallBacks : __CFDictionaryGetValueCallBacks(dict);
    result = CFDictionaryCreateMutable(allocator, capacity, cb, vcb);
    if (0 == capacity) _CFDictionarySetCapacity(result, numValues);
    for (idx = 0; idx < numValues; idx++) {
	CFDictionaryAddValue(result, list[idx], vlist[idx]);
    }
    if (list != buffer) CFAllocatorDeallocate(allocator, list);
    if (vlist != vbuffer) CFAllocatorDeallocate(allocator, vlist);
    return result;
}

// Used by NSMapTables and KVO
void _CFDictionarySetContext(CFDictionaryRef dict, void *context) {
    ((struct __CFDictionary *)dict)->_context = context;
}

void *_CFDictionaryGetContext(CFDictionaryRef dict) {
    return ((struct __CFDictionary *)dict)->_context;
}

CFIndex CFDictionaryGetCount(CFDictionaryRef dict) {
    CF_OBJC_FUNCDISPATCH0(__kCFDictionaryTypeID, CFIndex, dict, "count");
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    return dict->_count;
}

CFIndex CFDictionaryGetCountOfKey(CFDictionaryRef dict, const void *key) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH1(__kCFDictionaryTypeID, CFIndex, dict, "countForKey:", key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return 0;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    return (kCFNotFound != match ? 1 : 0);
}

CFIndex CFDictionaryGetCountOfValue(CFDictionaryRef dict, const void *value) {
    const void **keys;
    const CFDictionaryValueCallBacks *vcb;
    CFIndex idx, cnt = 0, nbuckets;
    CF_OBJC_FUNCDISPATCH1(__kCFDictionaryTypeID, CFIndex, dict, "countForObject:", value);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return 0;
    keys = dict->_keys;
    nbuckets = dict->_bucketsNum;
    vcb = __CFDictionaryGetValueCallBacks(dict);
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict->_marker != (uintptr_t)keys[idx] && ~dict->_marker != (uintptr_t)keys[idx]) {
	    if ((dict->_values[idx] == value) || (vcb->equal && INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))vcb->equal, dict->_values[idx], value, dict->_context))) {
		cnt++;
	    }
	}
    }
    return cnt;
}

Boolean CFDictionaryContainsKey(CFDictionaryRef dict, const void *key) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH1(__kCFDictionaryTypeID, char, dict, "containsKey:", key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return false;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    return (kCFNotFound != match ? true : false);
}

Boolean CFDictionaryContainsValue(CFDictionaryRef dict, const void *value) {
    const void **keys;
    const CFDictionaryValueCallBacks *vcb;
    CFIndex idx, nbuckets;
    CF_OBJC_FUNCDISPATCH1(__kCFDictionaryTypeID, char, dict, "containsObject:", value);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return false;
    keys = dict->_keys;
    nbuckets = dict->_bucketsNum;
    vcb = __CFDictionaryGetValueCallBacks(dict);
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict->_marker != (uintptr_t)keys[idx] && ~dict->_marker != (uintptr_t)keys[idx]) {
	    if ((dict->_values[idx] == value) || (vcb->equal && INVOKE_CALLBACK3((Boolean (*)(void *, void *, void*))vcb->equal, dict->_values[idx], value, dict->_context))) {
		return true;
	    }
	}
    }
    return false;
}

const void *CFDictionaryGetValue(CFDictionaryRef dict, const void *key) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH1(__kCFDictionaryTypeID, const void *, dict, "objectForKey:", key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return NULL;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    return (kCFNotFound != match ? dict->_values[match] : NULL);
}

Boolean CFDictionaryGetValueIfPresent(CFDictionaryRef dict, const void *key, const void **value) {
    CFIndex match;
    CF_OBJC_FUNCDISPATCH2(__kCFDictionaryTypeID, char, dict, "_getValue:forKey:", (void * *)value, key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return false;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    return (kCFNotFound != match ? ((value ? *value = dict->_values[match] : NULL), true) : false);
}

bool CFDictionaryGetKeyIfPresent(CFDictionaryRef dict, const void *key, const void **actualkey) {
    CFIndex match;
//#warning CF: not toll-free bridged
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    if (0 == dict->_count) return false;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    return (kCFNotFound != match ? ((actualkey ? *actualkey = dict->_keys[match] : NULL), true) : false);
}

void CFDictionaryGetKeysAndValues(CFDictionaryRef dict, const void **keys, const void **values) {
    CFIndex idx, cnt, nbuckets;
    CF_OBJC_FUNCDISPATCH2(__kCFDictionaryTypeID, void, dict, "getObjects:andKeys:", (void * *)values, (void * *)keys);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    nbuckets = dict->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict->_marker != (uintptr_t)dict->_keys[idx] && ~dict->_marker != (uintptr_t)dict->_keys[idx]) {
	    for (cnt = 1; cnt--;) {
		if (keys) *keys++ = dict->_keys[idx];
		if (values) *values++ = dict->_values[idx];
	    }
	}
    }
}

void CFDictionaryApplyFunction(CFDictionaryRef dict, CFDictionaryApplierFunction applier, void *context) {
    const void **keys;
    CFIndex idx, cnt, nbuckets;
    FAULT_CALLBACK((void **)&(applier));
    CF_OBJC_FUNCDISPATCH2(__kCFDictionaryTypeID, void, dict, "_apply:context:", applier, context);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    keys = dict->_keys;
    nbuckets = dict->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict->_marker != (uintptr_t)keys[idx] && ~dict->_marker != (uintptr_t)keys[idx]) {
	    for (cnt = 1; cnt--;) {
		INVOKE_CALLBACK3(applier, keys[idx], dict->_values[idx], context);
	    }
	}
    }
}

static void __CFDictionaryGrow(CFMutableDictionaryRef dict, CFIndex numNewValues) {
    const void **oldkeys = dict->_keys;
    const void **oldvalues = dict->_values;
    CFIndex idx, oldnbuckets = dict->_bucketsNum;
    CFIndex oldCount = dict->_count;
    dict->_capacity = __CFDictionaryRoundUpCapacity(oldCount + numNewValues);
    dict->_bucketsNum = __CFDictionaryNumBucketsForCapacity(dict->_capacity);
    dict->_deletes = 0;
    dict->_keys = CFAllocatorAllocate(__CFGetAllocator(dict), 2 * dict->_bucketsNum * sizeof(const void *), 0);
    dict->_values = (const void **)(dict->_keys + dict->_bucketsNum);
    if (NULL == dict->_keys) HALT;
    if (__CFOASafe) __CFSetLastAllocationEventName(dict->_keys, "CFDictionary (store)");
    for (idx = dict->_bucketsNum; idx--;) {
	dict->_keys[idx] = (const void *)dict->_marker;
    }
    if (NULL == oldkeys) return;
    for (idx = 0; idx < oldnbuckets; idx++) {
	if (dict->_marker != (uintptr_t)oldkeys[idx] && ~dict->_marker != (uintptr_t)oldkeys[idx]) {
	    CFIndex match, nomatch;
	    __CFDictionaryFindBuckets2(dict, oldkeys[idx], &match, &nomatch);
	    CFAssert3(kCFNotFound == match, __kCFLogAssertion, "%s(): two values (%p, %p) now hash to the same slot; mutable value changed while in table or hash value is not immutable", __PRETTY_FUNCTION__, oldkeys[idx], dict->_keys[match]);
	    dict->_keys[nomatch] = oldkeys[idx];
	    dict->_values[nomatch] = oldvalues[idx];
	}
    }
    CFAssert1(dict->_count == oldCount, __kCFLogAssertion, "%s(): dict count differs after rehashing; error", __PRETTY_FUNCTION__);
    CFAllocatorDeallocate(__CFGetAllocator(dict), oldkeys);
}

// This function is for Foundation's benefit; no one else should use it.
void _CFDictionarySetCapacity(CFMutableDictionaryRef dict, CFIndex cap) {
    if (CF_IS_OBJC(__kCFDictionaryTypeID, dict)) return;
#if defined(DEBUG)
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    CFAssert1(__CFDictionaryGetType(dict) != __kCFDictionaryImmutable && __CFDictionaryGetType(dict) != __kCFDictionaryFixedMutable, __kCFLogAssertion, "%s(): dict is immutable or fixed-mutable", __PRETTY_FUNCTION__);
    CFAssert3(dict->_count <= cap, __kCFLogAssertion, "%s(): desired capacity (%d) is less than count (%d)", __PRETTY_FUNCTION__, cap, dict->_count);
#endif
    __CFDictionaryGrow(dict, cap - dict->_count);
}

// This function is for Foundation's benefit; no one else should use it.
bool _CFDictionaryIsMutable(CFDictionaryRef dict) {
    return (__CFDictionaryGetType(dict) != __kCFDictionaryImmutable);
}

void CFDictionaryAddValue(CFMutableDictionaryRef dict, const void *key, const void *value) {
    CFIndex match, nomatch;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    const void *newKey, *newValue;
    CF_OBJC_FUNCDISPATCH2(__kCFDictionaryTypeID, void, dict, "_addObject:forKey:", value, key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    switch (__CFDictionaryGetType(dict)) {
    case __kCFDictionaryMutable:
	if (dict->_count == dict->_capacity || NULL == dict->_keys) {
	    __CFDictionaryGrow(dict, 1);
	}
	break;
    case __kCFDictionaryFixedMutable:
	CFAssert3(dict->_count < dict->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity dict %p (capacity = %d)", __PRETTY_FUNCTION__, dict, dict->_capacity);
	break;
    default:
	CFAssert2(__CFDictionaryGetType(dict) != __kCFDictionaryImmutable, __kCFLogAssertion, "%s(): immutable dict %p passed to mutating operation", __PRETTY_FUNCTION__, dict);
	break;
    }
    __CFDictionaryFindBuckets2(dict, key, &match, &nomatch);
    if (kCFNotFound != match) {
    } else {
	cb = __CFDictionaryGetKeyCallBacks(dict);
	vcb = __CFDictionaryGetValueCallBacks(dict);
	if (cb->retain) {
	    newKey = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), __CFGetAllocator(dict), key, dict->_context);
	} else {
	    newKey = key;
	}
	if (vcb->retain) {
	    newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))vcb->retain), __CFGetAllocator(dict), value, dict->_context);
	} else {
	    newValue = value;
	}
	if (dict->_marker == (uintptr_t)newKey || ~dict->_marker == (uintptr_t)newKey) {
	    __CFDictionaryFindNewMarker(dict);
	}
	CF_OBJC_KVO_WILLCHANGE(dict, key);
	dict->_keys[nomatch] = newKey;
	dict->_values[nomatch] = newValue;
	dict->_count++;
	CF_OBJC_KVO_DIDCHANGE(dict, key);
    }
}

void CFDictionaryReplaceValue(CFMutableDictionaryRef dict, const void *key, const void *value) {
    CFIndex match;
    const CFDictionaryValueCallBacks *vcb;
    const void *newValue;
    CF_OBJC_FUNCDISPATCH2(__kCFDictionaryTypeID, void, dict, "_replaceObject:forKey:", value, key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    switch (__CFDictionaryGetType(dict)) {
    case __kCFDictionaryMutable:
    case __kCFDictionaryFixedMutable:
	break;
    default:
	CFAssert2(__CFDictionaryGetType(dict) != __kCFDictionaryImmutable, __kCFLogAssertion, "%s(): immutable dict %p passed to mutating operation", __PRETTY_FUNCTION__, dict);
	break;
    }
    if (0 == dict->_count) return;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    if (kCFNotFound == match) return;
    vcb = __CFDictionaryGetValueCallBacks(dict);
    if (vcb->retain) {
	newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))vcb->retain), __CFGetAllocator(dict), value, dict->_context);
    } else {
	newValue = value;
    }
    CF_OBJC_KVO_WILLCHANGE(dict, key);
    if (vcb->release) {
	INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))vcb->release), __CFGetAllocator(dict), dict->_values[match], dict->_context);
    }
    dict->_values[match] = newValue;
    CF_OBJC_KVO_DIDCHANGE(dict, key);
}

void CFDictionarySetValue(CFMutableDictionaryRef dict, const void *key, const void *value) {
    CFIndex match, nomatch;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    const void *newKey, *newValue;
    CF_OBJC_FUNCDISPATCH2(__kCFDictionaryTypeID, void, dict, "setObject:forKey:", value, key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    switch (__CFDictionaryGetType(dict)) {
    case __kCFDictionaryMutable:
	if (dict->_count == dict->_capacity || NULL == dict->_keys) {
	    __CFDictionaryGrow(dict, 1);
	}
	break;
    case __kCFDictionaryFixedMutable:
	break;
    default:
	CFAssert2(__CFDictionaryGetType(dict) != __kCFDictionaryImmutable, __kCFLogAssertion, "%s(): immutable dict %p passed to mutating operation", __PRETTY_FUNCTION__, dict);
	break;
    }
    __CFDictionaryFindBuckets2(dict, key, &match, &nomatch);
    vcb = __CFDictionaryGetValueCallBacks(dict);
    if (vcb->retain) {
	newValue = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))vcb->retain), __CFGetAllocator(dict), value, dict->_context);
    } else {
	newValue = value;
    }
    if (kCFNotFound != match) {
	CF_OBJC_KVO_WILLCHANGE(dict, key);
	if (vcb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))vcb->release), __CFGetAllocator(dict), dict->_values[match], dict->_context);
	}
	dict->_values[match] = newValue;
	CF_OBJC_KVO_DIDCHANGE(dict, key);
    } else {
	CFAssert3(__kCFDictionaryFixedMutable != __CFDictionaryGetType(dict) || dict->_count < dict->_capacity, __kCFLogAssertion, "%s(): capacity exceeded on fixed-capacity dict %p (capacity = %d)", __PRETTY_FUNCTION__, dict, dict->_capacity);
	cb = __CFDictionaryGetKeyCallBacks(dict);
	if (cb->retain) {
	    newKey = (void *)INVOKE_CALLBACK3(((const void *(*)(CFAllocatorRef, const void *, void *))cb->retain), __CFGetAllocator(dict), key, dict->_context);
	} else {
	    newKey = key;
	}
	if (dict->_marker == (uintptr_t)newKey || ~dict->_marker == (uintptr_t)newKey) {
	    __CFDictionaryFindNewMarker(dict);
	}
	CF_OBJC_KVO_WILLCHANGE(dict, key);
	dict->_keys[nomatch] = newKey;
	dict->_values[nomatch] = newValue;
	dict->_count++;
	CF_OBJC_KVO_DIDCHANGE(dict, key);
    }
}

void CFDictionaryRemoveValue(CFMutableDictionaryRef dict, const void *key) {
    CFIndex match;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    CF_OBJC_FUNCDISPATCH1(__kCFDictionaryTypeID, void, dict, "removeObjectForKey:", key);
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    switch (__CFDictionaryGetType(dict)) {
    case __kCFDictionaryMutable:
    case __kCFDictionaryFixedMutable:
	break;
    default:
	CFAssert2(__CFDictionaryGetType(dict) != __kCFDictionaryImmutable, __kCFLogAssertion, "%s(): immutable dict %p passed to mutating operation", __PRETTY_FUNCTION__, dict);
	break;
    }
    if (0 == dict->_count) return;
    if (__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2)) {
	match = __CFDictionaryFindBuckets1a(dict, key);
    } else {
	match = __CFDictionaryFindBuckets1b(dict, key);
    }
    if (kCFNotFound == match) return;
    if (1) {
	cb = __CFDictionaryGetKeyCallBacks(dict);
	vcb = __CFDictionaryGetValueCallBacks(dict);
	const void *oldkey = dict->_keys[match];
	CF_OBJC_KVO_WILLCHANGE(dict, oldkey);
	if (vcb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))vcb->release), __CFGetAllocator(dict), dict->_values[match], dict->_context);
	}
        dict->_keys[match] = (const void *)~dict->_marker;
	dict->_count--;
	CF_OBJC_KVO_DIDCHANGE(dict, oldkey);
	if (cb->release) {
	    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), __CFGetAllocator(dict), oldkey, dict->_context);
	}
	dict->_deletes++;
	// _CFDictionaryDecrementValue() below has this same code.
	if ((__kCFDictionaryMutable == __CFDictionaryGetType(dict)) && (dict->_bucketsNum < 4 * dict->_deletes || (512 < dict->_capacity && 3.236067 * dict->_count < dict->_capacity))) {
	    // 3.236067 == 2 * golden_mean; this comes about because we're trying to resize down
	    // when the count is less than 2 capacities smaller, but not right away when count
	    // is just less than 2 capacities smaller, because an add would then force growth;
	    // well, the ratio between one capacity and the previous is close to the golden
	    // mean currently, so (cap / m / m) would be two smaller; but so we're not close,
	    // we take the average of that and the prior cap (cap / m / m / m). Well, after one
	    // does the algebra, and uses the convenient fact that m^(x+2) = m^(x+1) + m^x if m
	    // is the golden mean, this reduces to cap / 2m for the threshold. In general, the
	    // possible threshold constant is 1 / (2 * m^k), k = 0, 1, 2, ... under this scheme.
	    // Rehash; currently only for mutable-variable dictionaries
	    __CFDictionaryGrow(dict, 0);
	} else {
	    // When the probeskip == 1 always and only, a DELETED slot followed by an EMPTY slot
	    // can be converted to an EMPTY slot.  By extension, a chain of DELETED slots followed
	    // by an EMPTY slot can be converted to EMPTY slots, which is what we do here.
	    // _CFDictionaryDecrementValue() below has this same code.
	    if (match < dict->_bucketsNum - 1 && dict->_keys[match + 1] == (const void *)dict->_marker) {
		while (0 <= match && dict->_keys[match] == (const void *)~dict->_marker) {
		    dict->_keys[match] = (const void *)dict->_marker;
		    dict->_deletes--;
		    match--;
		}
	    }
	}
    }
}

void CFDictionaryRemoveAllValues(CFMutableDictionaryRef dict) {
    const void **keys;
    const CFDictionaryKeyCallBacks *cb;
    const CFDictionaryValueCallBacks *vcb;
    CFAllocatorRef allocator;
    CFIndex idx, nbuckets;
    CF_OBJC_FUNCDISPATCH0(__kCFDictionaryTypeID, void, dict, "removeAllObjects");
    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    switch (__CFDictionaryGetType(dict)) {
    case __kCFDictionaryMutable:
    case __kCFDictionaryFixedMutable:
	break;
    default:
	CFAssert2(__CFDictionaryGetType(dict) != __kCFDictionaryImmutable, __kCFLogAssertion, "%s(): immutable dict %p passed to mutating operation", __PRETTY_FUNCTION__, dict);
	break;
    }
    if (0 == dict->_count) return;
    keys = dict->_keys;
    nbuckets = dict->_bucketsNum;
    cb = __CFDictionaryGetKeyCallBacks(dict);
    vcb = __CFDictionaryGetValueCallBacks(dict);
    allocator = __CFGetAllocator(dict);
    for (idx = 0; idx < nbuckets; idx++) {
	if (dict->_marker != (uintptr_t)keys[idx] && ~dict->_marker != (uintptr_t)keys[idx]) {
	    const void *oldkey = keys[idx];
	    CF_OBJC_KVO_WILLCHANGE(dict, oldkey);
	    if (vcb->release) {
		INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))vcb->release), allocator, dict->_values[idx], dict->_context);
	    }
	    keys[idx] = (const void *)~dict->_marker;
	    dict->_count--;
	    CF_OBJC_KVO_DIDCHANGE(dict, oldkey);
	    if (cb->release) {
		INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, const void *, void *))cb->release), allocator, oldkey, dict->_context);
	    }
	}
    }
    for (idx = 0; idx < nbuckets; idx++) {
	keys[idx] = (const void *)dict->_marker;
    }
    dict->_count = 0;
    dict->_deletes = 0;
    if ((__kCFDictionaryMutable == __CFDictionaryGetType(dict)) && (512 < dict->_capacity)) {
	__CFDictionaryGrow(dict, 256);
    }
}

void _CFDictionaryIncrementValue(CFMutableDictionaryRef dict, const void *key) {
    CFIndex match, nomatch;

    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    CFAssert1(__CFDictionaryGetType(dict) == __kCFDictionaryMutable, __kCFLogAssertion, "%s(): invalid dict type passed to increment operation", __PRETTY_FUNCTION__);
    CFAssert1(__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2), __kCFLogAssertion, "%s(): invalid dict passed to increment operation", __PRETTY_FUNCTION__);
    CFAssert1(__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 5, 4), __kCFLogAssertion, "%s(): invalid dict passed to increment operation", __PRETTY_FUNCTION__);

    match = kCFNotFound;
    if (NULL != dict->_keys) {
	__CFDictionaryFindBuckets2(dict, key, &match, &nomatch);
    }
    if (kCFNotFound != match) {
	if (dict->_values[match] != (void *)UINT_MAX) {
	    dict->_values[match] = (void *)((uintptr_t)dict->_values[match] + 1);
	}
    } else {
	if (dict->_marker == (uintptr_t)key || ~dict->_marker == (uintptr_t)key) {
	    __CFDictionaryFindNewMarker(dict);
	}
	if (dict->_count == dict->_capacity || NULL == dict->_keys) {
	    __CFDictionaryGrow(dict, 1);
	    __CFDictionaryFindBuckets2(dict, key, &match, &nomatch);
	}
	dict->_keys[nomatch] = key;
	dict->_values[nomatch] = (void *)1;
	dict->_count++;
    }
}

int _CFDictionaryDecrementValue(CFMutableDictionaryRef dict, const void *key) {
    CFIndex match;

    __CFGenericValidateType(dict, __kCFDictionaryTypeID);
    CFAssert1(__CFDictionaryGetType(dict) == __kCFDictionaryMutable, __kCFLogAssertion, "%s(): invalid dict type passed to increment operation", __PRETTY_FUNCTION__);
    CFAssert1(__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 3, 2), __kCFLogAssertion, "%s(): invalid dict passed to increment operation", __PRETTY_FUNCTION__);
    CFAssert1(__kCFDictionaryHasNullCallBacks == __CFBitfieldGetValue(((const CFRuntimeBase *)dict)->_info, 5, 4), __kCFLogAssertion, "%s(): invalid dict passed to increment operation", __PRETTY_FUNCTION__);

    if (0 == dict->_count) return -1;
    match = __CFDictionaryFindBuckets1a(dict, key);
    if (kCFNotFound == match) return -1;
    if (1 == (uintptr_t)dict->_values[match]) {
	dict->_count--;
	dict->_values[match] = 0;
        dict->_keys[match] = (const void *)~dict->_marker;
	dict->_deletes++;
	if ((__kCFDictionaryMutable == __CFDictionaryGetType(dict)) && (dict->_bucketsNum < 4 * dict->_deletes || (512 < dict->_capacity && 3.236067 * dict->_count < dict->_capacity))) {
	    __CFDictionaryGrow(dict, 0);
	} else {
	    if (match < dict->_bucketsNum - 1 && dict->_keys[match + 1] == (const void *)dict->_marker) {
		while (0 <= match && dict->_keys[match] == (const void *)~dict->_marker) {
		    dict->_keys[match] = (const void *)dict->_marker;
		    dict->_deletes--;
		    match--;
		}
	    }
	}
	return 0;
    } else if (dict->_values[match] != (void *)UINT_MAX) {
	dict->_values[match] = (void *)((uintptr_t)dict->_values[match] - 1);
    }
    return 1;
}

