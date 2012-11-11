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
/*	CFBinaryHeap.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFBinaryHeap.h>
#include "CFUtilities.h"
#include "CFInternal.h"

const CFBinaryHeapCallBacks kCFStringBinaryHeapCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, (CFComparisonResult (*)(const void *, const void *, void *))CFStringCompare};

struct __CFBinaryHeapBucket {
    void *_item;
};

CF_INLINE CFIndex __CFBinaryHeapRoundUpCapacity(CFIndex capacity) {
    if (capacity < 4) return 4;
    return (1 << (CFLog2(capacity) + 1));
}

CF_INLINE CFIndex __CFBinaryHeapNumBucketsForCapacity(CFIndex capacity) {
    return capacity;
}

struct __CFBinaryHeap {
    CFRuntimeBase _base;
    CFIndex _count;	/* number of objects */
    CFIndex _capacity;	/* maximum number of objects */
    CFBinaryHeapCallBacks _callbacks;
    CFBinaryHeapCompareContext _context;
    struct __CFBinaryHeapBucket *_buckets;
};

CF_INLINE CFIndex __CFBinaryHeapCount(CFBinaryHeapRef heap) {
    return heap->_count;
}

CF_INLINE void __CFBinaryHeapSetCount(CFBinaryHeapRef heap, CFIndex v) {
    /* for a CFBinaryHeap, _bucketsUsed == _count */
}

CF_INLINE CFIndex __CFBinaryHeapCapacity(CFBinaryHeapRef heap) {
    return heap->_capacity;
}

CF_INLINE void __CFBinaryHeapSetCapacity(CFBinaryHeapRef heap, CFIndex v) {
    /* for a CFBinaryHeap, _bucketsNum == _capacity */
}

CF_INLINE CFIndex __CFBinaryHeapNumBucketsUsed(CFBinaryHeapRef heap) {
    return heap->_count;
}

CF_INLINE void __CFBinaryHeapSetNumBucketsUsed(CFBinaryHeapRef heap, CFIndex v) {
    heap->_count = v;
}

CF_INLINE CFIndex __CFBinaryHeapNumBuckets(CFBinaryHeapRef heap) {
    return heap->_capacity;
}

CF_INLINE void __CFBinaryHeapSetNumBuckets(CFBinaryHeapRef heap, CFIndex v) {
    heap->_capacity = v;
}

enum {
    kCFBinaryHeapImmutable = 0x0,		/* unchangable and fixed capacity; default */
    kCFBinaryHeapMutable = 0x1,		/* changeable and variable capacity */
    kCFBinaryHeapFixedMutable = 0x3	/* changeable and fixed capacity */
};

CF_INLINE UInt32 __CFBinaryHeapMutableVariety(const void *cf) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 3, 2);
}

CF_INLINE void __CFBinaryHeapSetMutableVariety(void *cf, UInt32 v) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_info, 3, 2, v);
}

CF_INLINE UInt32 __CFBinaryHeapMutableVarietyFromFlags(UInt32 flags) {
    return __CFBitfieldGetValue(flags, 1, 0);
}

static bool __CFBinaryHeapEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFBinaryHeapRef heap1 = (CFBinaryHeapRef)cf1;
    CFBinaryHeapRef heap2 = (CFBinaryHeapRef)cf2;
    CFComparisonResult (*compare)(const void *, const void *, void *);
    CFIndex idx;
    CFIndex cnt;
    const void **list1, **list2, *buffer[256];
    cnt = __CFBinaryHeapCount(heap1);
    if (cnt != __CFBinaryHeapCount(heap2)) return false;
    compare = heap1->_callbacks.compare;
    if (compare != heap2->_callbacks.compare) return false;
    if (0 == cnt) return true;	/* after function comparison */
    list1 = (cnt <= 128) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, 2 * cnt * sizeof(void *), 0);
    if (__CFOASafe && list1 != buffer) __CFSetLastAllocationEventName(list1, "CFBinaryHeap (temp)");
    list2 = (cnt <= 128) ? buffer + 128 : list1 + cnt;
    CFBinaryHeapGetValues(heap1, list1);
    CFBinaryHeapGetValues(heap2, list2);
    for (idx = 0; idx < cnt; idx++) {
	const void *val1 = list1[idx];
	const void *val2 = list2[idx];
// CF: which context info should be passed in? both?
// CF: if the context infos are not equal, should the heaps not be equal?
	if (val1 != val2 && compare && !compare(val1, val2, heap1->_context.info)) return false;
    }
    if (list1 != buffer) CFAllocatorDeallocate(CFGetAllocator(heap1), list1);
    return true;
}

static UInt32 __CFBinaryHeapHash(CFTypeRef cf) {
    CFBinaryHeapRef heap = (CFBinaryHeapRef)cf;
    return __CFBinaryHeapCount(heap);
}

static CFStringRef __CFBinaryHeapCopyDescription(CFTypeRef cf) {
    CFBinaryHeapRef heap = (CFBinaryHeapRef)cf;
    CFMutableStringRef result;
    CFIndex idx;
    CFIndex cnt;
    const void **list, *buffer[256];
    cnt = __CFBinaryHeapCount(heap);
    result = CFStringCreateMutable(CFGetAllocator(heap), 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFBinaryHeap %p [%p]>{count = %u, capacity = %u, objects = (\n"), cf, CFGetAllocator(heap), cnt, __CFBinaryHeapCapacity(heap));
    list = (cnt <= 128) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0);
    if (__CFOASafe && list != buffer) __CFSetLastAllocationEventName(list, "CFBinaryHeap (temp)");
    CFBinaryHeapGetValues(heap, list);
    for (idx = 0; idx < cnt; idx++) {
	CFStringRef desc = NULL;
	const void *item = list[idx];
	if (NULL != heap->_callbacks.copyDescription) {
	    desc = heap->_callbacks.copyDescription(item);
	}
	if (NULL != desc) {
	    CFStringAppendFormat(result, NULL, CFSTR("\t%u : %s\n"), idx, desc);
	    CFRelease(desc);
	} else {
	    CFStringAppendFormat(result, NULL, CFSTR("\t%u : <0x%x>\n"), idx, (UInt32)item);
	}
    }
    CFStringAppend(result, CFSTR(")}"));
    if (list != buffer) CFAllocatorDeallocate(CFGetAllocator(heap), list);
    return result;
}

static void __CFBinaryHeapDeallocate(CFTypeRef cf) {
    CFBinaryHeapRef heap = (CFBinaryHeapRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(heap);
// CF: should make the heap mutable here first, a la CFArrayDeallocate
    CFBinaryHeapRemoveAllValues(heap);
// CF: does not release the context info
    if (__CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapMutable) {
	CFAllocatorDeallocate(allocator, heap->_buckets);
    }
}

static CFTypeID __kCFBinaryHeapTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFBinaryHeapClass = {
    0,
    "CFBinaryHeap",
    NULL,	// init
    NULL,	// copy
    __CFBinaryHeapDeallocate,
    (void *)__CFBinaryHeapEqual,
    __CFBinaryHeapHash,
    NULL,	// 
    __CFBinaryHeapCopyDescription
};

__private_extern__ void __CFBinaryHeapInitialize(void) {
    __kCFBinaryHeapTypeID = _CFRuntimeRegisterClass(&__CFBinaryHeapClass);
}

CFTypeID CFBinaryHeapGetTypeID(void) {
    return __kCFBinaryHeapTypeID;
}

static CFBinaryHeapRef __CFBinaryHeapInit(CFAllocatorRef allocator, UInt32 flags, CFIndex capacity, const void **values, CFIndex numValues, const CFBinaryHeapCallBacks *callBacks, const CFBinaryHeapCompareContext *compareContext) {
// CF: does not copy the compareContext argument into the object
    CFBinaryHeapRef memory;
    CFIndex idx;
    CFIndex size;

    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%d) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    CFAssert3(kCFBinaryHeapFixedMutable != __CFBinaryHeapMutableVarietyFromFlags(flags) || numValues <= capacity, __kCFLogAssertion, "%s(): for fixed-mutable type, capacity (%d) must be greater than or equal to number of initial elements (%d)", __PRETTY_FUNCTION__, capacity, numValues);
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%d) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
    size = sizeof(struct __CFBinaryHeap) - sizeof(CFRuntimeBase);
    if (__CFBinaryHeapMutableVarietyFromFlags(flags) != kCFBinaryHeapMutable)
	size += sizeof(struct __CFBinaryHeapBucket) * __CFBinaryHeapNumBucketsForCapacity(capacity);
    memory = (CFBinaryHeapRef)_CFRuntimeCreateInstance(allocator, __kCFBinaryHeapTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    switch (__CFBinaryHeapMutableVarietyFromFlags(flags)) {
    case kCFBinaryHeapMutable:
	__CFBinaryHeapSetCapacity(memory, __CFBinaryHeapRoundUpCapacity(1));
	__CFBinaryHeapSetNumBuckets(memory, __CFBinaryHeapNumBucketsForCapacity(__CFBinaryHeapRoundUpCapacity(1)));
	memory->_buckets = CFAllocatorAllocate(allocator, __CFBinaryHeapNumBuckets(memory) * sizeof(struct __CFBinaryHeapBucket), 0);
	if (__CFOASafe) __CFSetLastAllocationEventName(memory->_buckets, "CFBinaryHeap (store)");
	if (NULL == memory->_buckets) {
	    CFRelease(memory);
	    return NULL;
	}
	break;
    case kCFBinaryHeapFixedMutable:
    case kCFBinaryHeapImmutable:
	/* Don't round up capacity */
	__CFBinaryHeapSetCapacity(memory, capacity);
	__CFBinaryHeapSetNumBuckets(memory, __CFBinaryHeapNumBucketsForCapacity(capacity));
	memory->_buckets = (struct __CFBinaryHeapBucket *)((int8_t *)memory + sizeof(struct __CFBinaryHeap));
	break;
    }
    __CFBinaryHeapSetNumBucketsUsed(memory, 0);
    __CFBinaryHeapSetCount(memory, 0);
    if (NULL != callBacks) {
	memory->_callbacks.retain = callBacks->retain;
	memory->_callbacks.release = callBacks->release;
	memory->_callbacks.copyDescription = callBacks->copyDescription;
	memory->_callbacks.compare = callBacks->compare;
    } else {
	memory->_callbacks.retain = 0;
	memory->_callbacks.release = 0;
	memory->_callbacks.copyDescription = 0;
	memory->_callbacks.compare = 0;
    }
    if (__CFBinaryHeapMutableVarietyFromFlags(flags) != kCFBinaryHeapMutable) {
        __CFBinaryHeapSetMutableVariety(memory, kCFBinaryHeapFixedMutable);
    } else {
        __CFBinaryHeapSetMutableVariety(memory, kCFBinaryHeapMutable);
    }
    for (idx = 0; idx < numValues; idx++) {
	CFBinaryHeapAddValue(memory, values[idx]);
    }
    __CFBinaryHeapSetMutableVariety(memory, __CFBinaryHeapMutableVarietyFromFlags(flags));
    return memory;
}

CFBinaryHeapRef CFBinaryHeapCreate(CFAllocatorRef allocator, CFIndex capacity, const CFBinaryHeapCallBacks *callBacks, const CFBinaryHeapCompareContext *compareContext) {
   return __CFBinaryHeapInit(allocator, (0 == capacity) ? kCFBinaryHeapMutable : kCFBinaryHeapFixedMutable, capacity, NULL, 0, callBacks, compareContext);
}

CFBinaryHeapRef CFBinaryHeapCreateCopy(CFAllocatorRef allocator, CFIndex capacity, CFBinaryHeapRef heap) {
   __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    return __CFBinaryHeapInit(allocator, (0 == capacity) ? kCFBinaryHeapMutable : kCFBinaryHeapFixedMutable, capacity, (const void **)heap->_buckets, __CFBinaryHeapCount(heap), &(heap->_callbacks), &(heap->_context));
}

CFIndex CFBinaryHeapGetCount(CFBinaryHeapRef heap) {
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    return __CFBinaryHeapCount(heap);
}

CFIndex CFBinaryHeapGetCountOfValue(CFBinaryHeapRef heap, const void *value) {
    CFComparisonResult (*compare)(const void *, const void *, void *);
    CFIndex idx;
    CFIndex cnt = 0, length;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    compare = heap->_callbacks.compare;
    length = __CFBinaryHeapCount(heap);
    for (idx = 0; idx < length; idx++) {
	const void *item = heap->_buckets[idx]._item;
	if (value == item || (compare && kCFCompareEqualTo == compare(value, item, heap->_context.info))) {
	    cnt++;
	}
    }
    return cnt;
}

Boolean CFBinaryHeapContainsValue(CFBinaryHeapRef heap, const void *value) {
    CFComparisonResult (*compare)(const void *, const void *, void *);
    CFIndex idx;
    CFIndex length;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    compare = heap->_callbacks.compare;
    length = __CFBinaryHeapCount(heap);
    for (idx = 0; idx < length; idx++) {
	const void *item = heap->_buckets[idx]._item;
	if (value == item || (compare && kCFCompareEqualTo == compare(value, item, heap->_context.info))) {
	    return true;
	}
    }
    return false;
}

const void *CFBinaryHeapGetMinimum(CFBinaryHeapRef heap) {
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    CFAssert1(0 < __CFBinaryHeapCount(heap), __kCFLogAssertion, "%s(): binary heap is empty", __PRETTY_FUNCTION__);
    return (0 < __CFBinaryHeapCount(heap)) ? heap->_buckets[0]._item : NULL;
}

Boolean CFBinaryHeapGetMinimumIfPresent(CFBinaryHeapRef heap, const void **value) {
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    if (0 == __CFBinaryHeapCount(heap)) return false;
    if (NULL != value) *value = heap->_buckets[0]._item;
    return true;
}

void CFBinaryHeapGetValues(CFBinaryHeapRef heap, const void **values) {
    CFBinaryHeapRef heapCopy;
    CFIndex idx;
    CFIndex cnt;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    CFAssert1(NULL != values, __kCFLogAssertion, "%s(): pointer to values may not be NULL", __PRETTY_FUNCTION__);
    cnt = __CFBinaryHeapCount(heap);
    if (0 == cnt) return;
    heapCopy = CFBinaryHeapCreateCopy(CFGetAllocator(heap), cnt, heap);
    idx = 0;
    while (0 < __CFBinaryHeapCount(heapCopy)) {
	const void *value = CFBinaryHeapGetMinimum(heapCopy);
	CFBinaryHeapRemoveMinimumValue(heapCopy);
	values[idx++] = value;
    }
    CFRelease(heapCopy);
}

void CFBinaryHeapApplyFunction(CFBinaryHeapRef heap, CFBinaryHeapApplierFunction applier, void *context) {
    CFBinaryHeapRef heapCopy;
    CFIndex cnt;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    CFAssert1(NULL != applier, __kCFLogAssertion, "%s(): pointer to applier function may not be NULL", __PRETTY_FUNCTION__);
    cnt = __CFBinaryHeapCount(heap);
    if (0 == cnt) return;
    heapCopy = CFBinaryHeapCreateCopy(CFGetAllocator(heap), cnt, heap);
    while (0 < __CFBinaryHeapCount(heapCopy)) {
	const void *value = CFBinaryHeapGetMinimum(heapCopy);
	CFBinaryHeapRemoveMinimumValue(heapCopy);
	applier(value, context);
    }
    CFRelease(heapCopy);
}

static void __CFBinaryHeapGrow(CFBinaryHeapRef heap, CFIndex numNewValues) {
    CFIndex oldCount = __CFBinaryHeapCount(heap);
    CFIndex capacity = __CFBinaryHeapRoundUpCapacity(oldCount + numNewValues);
    __CFBinaryHeapSetCapacity(heap, capacity);
    __CFBinaryHeapSetNumBuckets(heap, __CFBinaryHeapNumBucketsForCapacity(capacity));
    heap->_buckets = CFAllocatorReallocate(CFGetAllocator(heap), heap->_buckets, __CFBinaryHeapNumBuckets(heap) * sizeof(struct __CFBinaryHeapBucket), 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(heap->_buckets, "CFBinaryHeap (store)");
    if (NULL == heap->_buckets) HALT;
}

void CFBinaryHeapAddValue(CFBinaryHeapRef heap, const void *value) {
    CFIndex idx, pidx;
    CFIndex cnt;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    CFAssert1(__CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapMutable || __CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapFixedMutable, __kCFLogAssertion, "%s(): binary heap is immutable", __PRETTY_FUNCTION__);
    switch (__CFBinaryHeapMutableVariety(heap)) {
    case kCFBinaryHeapMutable:
	if (__CFBinaryHeapNumBucketsUsed(heap) == __CFBinaryHeapCapacity(heap))
	    __CFBinaryHeapGrow(heap, 1);
	break;
    case kCFBinaryHeapFixedMutable:
	CFAssert1(__CFBinaryHeapCount(heap) < __CFBinaryHeapCapacity(heap), __kCFLogAssertion, "%s(): fixed-capacity binary heap is full", __PRETTY_FUNCTION__);
	break;
    }
    cnt = __CFBinaryHeapCount(heap);
    idx = cnt;
    __CFBinaryHeapSetNumBucketsUsed(heap, cnt + 1);
    __CFBinaryHeapSetCount(heap, cnt + 1);
    pidx = (idx - 1) >> 1;
    while (0 < idx) {
	void *item = heap->_buckets[pidx]._item;
	if (kCFCompareGreaterThan != heap->_callbacks.compare(item, value, heap->_context.info)) break;
	heap->_buckets[idx]._item = item;
	idx = pidx;
	pidx = (idx - 1) >> 1;
    }
    if (heap->_callbacks.retain) {
	heap->_buckets[idx]._item = (void *)heap->_callbacks.retain(CFGetAllocator(heap), (void *)value);
    } else {
	heap->_buckets[idx]._item = (void *)value;
    }
}

void CFBinaryHeapRemoveMinimumValue(CFBinaryHeapRef heap) {
    void *val;
    CFIndex idx, cidx;
    CFIndex cnt;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    CFAssert1(__CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapMutable || __CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapFixedMutable, __kCFLogAssertion, "%s(): binary heap is immutable", __PRETTY_FUNCTION__);
    cnt = __CFBinaryHeapCount(heap);
    if (0 == cnt) return;
    idx = 0;
    __CFBinaryHeapSetNumBucketsUsed(heap, cnt - 1);
    __CFBinaryHeapSetCount(heap, cnt - 1);
    if (heap->_callbacks.release)
	heap->_callbacks.release(CFGetAllocator(heap), heap->_buckets[idx]._item);
    val = heap->_buckets[cnt - 1]._item;
    cidx = (idx << 1) + 1;
    while (cidx < __CFBinaryHeapCount(heap)) {
	void *item = heap->_buckets[cidx]._item;
	if (cidx + 1 < __CFBinaryHeapCount(heap)) {
	    void *item2 = heap->_buckets[cidx + 1]._item;
	    if (kCFCompareGreaterThan == heap->_callbacks.compare(item, item2, heap->_context.info)) {
		cidx++;
		item = item2;
	    }
	}
	if (kCFCompareGreaterThan == heap->_callbacks.compare(item, val, heap->_context.info)) break;
	heap->_buckets[idx]._item = item;
	idx = cidx;
	cidx = (idx << 1) + 1;
    }
    heap->_buckets[idx]._item = val;
}

void CFBinaryHeapRemoveAllValues(CFBinaryHeapRef heap) {
    CFIndex idx;
    CFIndex cnt;
    __CFGenericValidateType(heap, __kCFBinaryHeapTypeID);
    CFAssert1(__CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapMutable || __CFBinaryHeapMutableVariety(heap) == kCFBinaryHeapFixedMutable, __kCFLogAssertion, "%s(): binary heap is immutable", __PRETTY_FUNCTION__);
    cnt = __CFBinaryHeapCount(heap);
    if (heap->_callbacks.release)
	for (idx = 0; idx < cnt; idx++)
	    heap->_callbacks.release(CFGetAllocator(heap), heap->_buckets[idx]._item);
    __CFBinaryHeapSetNumBucketsUsed(heap, 0);
    __CFBinaryHeapSetCount(heap, 0);
}

