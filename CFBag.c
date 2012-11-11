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
/*	CFBag.c
	Copyright 1998-2006, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
        Machine generated from Notes/HashingCode.template
*/




#include <CoreFoundation/CFBag.h>
#include "CFInternal.h"
#include <mach-o/dyld.h>

#define CFDictionary 0
#define CFSet 0
#define CFBag 0
#undef CFBag
#define CFBag 1

#if CFDictionary
const CFBagKeyCallBacks kCFTypeBagKeyCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFBagKeyCallBacks kCFCopyStringBagKeyCallBacks = {0, __CFStringCollectionCopy, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFBagValueCallBacks kCFTypeBagValueCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual};
static const CFBagKeyCallBacks __kCFNullBagKeyCallBacks = {0, NULL, NULL, NULL, NULL, NULL};
static const CFBagValueCallBacks __kCFNullBagValueCallBacks = {0, NULL, NULL, NULL, NULL};

#define CFHashRef CFDictionaryRef
#define CFMutableHashRef CFMutableDictionaryRef
#define __kCFHashTypeID __kCFDictionaryTypeID
#endif

#if CFSet
const CFBagCallBacks kCFTypeBagCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFBagCallBacks kCFCopyStringBagCallBacks = {0, __CFStringCollectionCopy, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
static const CFBagCallBacks __kCFNullBagCallBacks = {0, NULL, NULL, NULL, NULL, NULL};

#define CFBagKeyCallBacks CFBagCallBacks
#define CFBagValueCallBacks CFBagCallBacks
#define kCFTypeBagKeyCallBacks kCFTypeBagCallBacks
#define kCFTypeBagValueCallBacks kCFTypeBagCallBacks
#define __kCFNullBagKeyCallBacks __kCFNullBagCallBacks
#define __kCFNullBagValueCallBacks __kCFNullBagCallBacks

#define CFHashRef CFSetRef
#define CFMutableHashRef CFMutableSetRef
#define __kCFHashTypeID __kCFSetTypeID
#endif

#if CFBag
const CFBagCallBacks kCFTypeBagCallBacks = {0, __CFTypeCollectionRetain, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFBagCallBacks kCFCopyStringBagCallBacks = {0, __CFStringCollectionCopy, __CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
static const CFBagCallBacks __kCFNullBagCallBacks = {0, NULL, NULL, NULL, NULL, NULL};

#define CFBagKeyCallBacks CFBagCallBacks
#define CFBagValueCallBacks CFBagCallBacks
#define kCFTypeBagKeyCallBacks kCFTypeBagCallBacks
#define kCFTypeBagValueCallBacks kCFTypeBagCallBacks
#define __kCFNullBagKeyCallBacks __kCFNullBagCallBacks
#define __kCFNullBagValueCallBacks __kCFNullBagCallBacks

#define CFHashRef CFBagRef
#define CFMutableHashRef CFMutableBagRef
#define __kCFHashTypeID __kCFBagTypeID
#endif

#define GETNEWKEY(newKey, oldKey) \
        any_t (*kretain)(CFAllocatorRef, any_t, any_pointer_t) = \
          !hasBeenFinalized(hc) \
            ? (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))__CFBagGetKeyCallBacks(hc)->retain \
            : (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
        any_t newKey = kretain ? (any_t)INVOKE_CALLBACK3(kretain, allocator, (any_t)key, hc->_context) : (any_t)oldKey

#define RELEASEKEY(oldKey) \
        void (*krelease)(CFAllocatorRef, any_t, any_pointer_t) = \
          !hasBeenFinalized(hc) \
            ? (void (*)(CFAllocatorRef,any_t,any_pointer_t))__CFBagGetKeyCallBacks(hc)->release \
            : (void (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
        if (krelease) INVOKE_CALLBACK3(krelease, allocator, oldKey, hc->_context)
        
#if CFDictionary
#define GETNEWVALUE(newValue) \
        any_t (*vretain)(CFAllocatorRef, any_t, any_pointer_t) = \
          !hasBeenFinalized(hc) \
            ? (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))__CFBagGetValueCallBacks(hc)->retain \
            : (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
        any_t newValue = vretain ? (any_t)INVOKE_CALLBACK3(vretain, allocator, (any_t)value, hc->_context) : (any_t)value

#define RELEASEVALUE(oldValue) \
    void (*vrelease)(CFAllocatorRef, any_t, any_pointer_t) = \
      !hasBeenFinalized(hc) \
        ? (void (*)(CFAllocatorRef,any_t,any_pointer_t))__CFBagGetValueCallBacks(hc)->release \
        : (void (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
    if (vrelease) INVOKE_CALLBACK3(vrelease, allocator, oldValue, hc->_context)

#endif

static void __CFBagHandleOutOfMemory(CFTypeRef obj, CFIndex numBytes) {
    CFStringRef msg = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("Attempt to allocate %ld bytes for NS/CFBag failed"), numBytes);
    CFBadErrorCallBack cb = _CFGetOutOfMemoryErrorCallBack();
    if (NULL == cb || !cb(obj, CFSTR("NS/CFBag"), msg)) {
        CFLog(kCFLogLevelCritical, CFSTR("%@"), msg);
	HALT;
    }
    CFRelease(msg);
}


// Max load is 3/4 number of buckets
CF_INLINE CFIndex __CFHashRoundUpCapacity(CFIndex capacity) {
    return 3 * ((CFIndex)1 << (flsl((capacity - 1) / 3)));
}

// Returns next power of two higher than the capacity
// threshold for the given input capacity.
CF_INLINE CFIndex __CFHashNumBucketsForCapacity(CFIndex capacity) {
    return 4 * ((CFIndex)1 << (flsl((capacity - 1) / 3)));
}

enum {                /* Bits 1-0 */
    __kCFHashImmutable = 0,        /* unchangable and fixed capacity */
    __kCFHashMutable = 1,                /* changeable and variable capacity */
};

enum {                /* Bits 5-4 (value), 3-2 (key) */
    __kCFHashHasNullCallBacks = 0,
    __kCFHashHasCFTypeCallBacks = 1,
    __kCFHashHasCustomCallBacks = 3        /* callbacks are at end of header */
};

// Under GC, we fudge the key/value memory in two ways
// First, if we had null callbacks or null for both retain/release, we use unscanned memory and get
// standard 'dangling' references.
// This means that if people were doing addValue:[xxx new] and never removing, well, that doesn't work
//
// Second, if we notice standard retain/release implementations we use scanned memory, and fudge the
// standard callbacks to generally do nothing if the collection was allocated in GC memory. On special
// CF objects, however, like those used for precious resources like video-card buffers, we do indeed
// do CFRetain on input and CFRelease on output.  The tricky case is GC finalization; we need to remember
// that we did the CFReleases so that subsequent collection operations, like removal, don't double CFRelease.
// (In fact we don't really use CFRetain/CFRelease but go directly to the collector)
//

enum {
    __kCFHashFinalized =         (1 << 7),
    __kCFHashWeakKeys =          (1 << 8),
    __kCFHashWeakValues =        (1 << 9)
};

typedef uintptr_t any_t;
typedef const void * const_any_pointer_t;
typedef void * any_pointer_t;

struct __CFBag {
    CFRuntimeBase _base;
    CFIndex _count;             /* number of values */
    CFIndex _bucketsNum;        /* number of buckets */
    CFIndex _bucketsUsed;       /* number of used buckets */
    CFIndex _bucketsCap;        /* maximum number of used buckets */
    CFIndex _mutations;
    CFIndex _deletes;
    any_pointer_t _context;     /* private */
    CFOptionFlags _xflags;
    any_t _marker;
    any_t *_keys;     /* can be NULL if not allocated yet */
    any_t *_values;   /* can be NULL if not allocated yet */
};

/* Bits 1-0 of the _xflags are used for mutability variety */
/* Bits 3-2 of the _xflags are used for key callback indicator bits */
/* Bits 5-4 of the _xflags are used for value callback indicator bits */
/* Bit 6 of the _xflags is special KVO actions bit */
/* Bits 7,8,9 are GC use */

CF_INLINE bool hasBeenFinalized(CFTypeRef collection) {
    return __CFBitfieldGetValue(((const struct __CFBag *)collection)->_xflags, 7, 7) != 0;
}

CF_INLINE void markFinalized(CFTypeRef collection) {
    __CFBitfieldSetValue(((struct __CFBag *)collection)->_xflags, 7, 7, 1);
}


CF_INLINE CFIndex __CFHashGetType(CFHashRef hc) {
    return __CFBitfieldGetValue(hc->_xflags, 1, 0);
}

CF_INLINE CFIndex __CFBagGetSizeOfType(CFIndex t) {
    CFIndex size = sizeof(struct __CFBag);
    if (__CFBitfieldGetValue(t, 3, 2) == __kCFHashHasCustomCallBacks) {
        size += sizeof(CFBagKeyCallBacks);
    }
    if (__CFBitfieldGetValue(t, 5, 4) == __kCFHashHasCustomCallBacks) {
        size += sizeof(CFBagValueCallBacks);
    }
    return size;
}

CF_INLINE const CFBagKeyCallBacks *__CFBagGetKeyCallBacks(CFHashRef hc) {
    CFBagKeyCallBacks *result = NULL;
    switch (__CFBitfieldGetValue(hc->_xflags, 3, 2)) {
    case __kCFHashHasNullCallBacks:
        return &__kCFNullBagKeyCallBacks;
    case __kCFHashHasCFTypeCallBacks:
        return &kCFTypeBagKeyCallBacks;
    case __kCFHashHasCustomCallBacks:
        break;
    }
    result = (CFBagKeyCallBacks *)((uint8_t *)hc + sizeof(struct __CFBag));
    return result;
}

CF_INLINE Boolean __CFBagKeyCallBacksMatchNull(const CFBagKeyCallBacks *c) {
    return (NULL == c ||
        (c->retain == __kCFNullBagKeyCallBacks.retain &&
         c->release == __kCFNullBagKeyCallBacks.release &&
         c->copyDescription == __kCFNullBagKeyCallBacks.copyDescription &&
         c->equal == __kCFNullBagKeyCallBacks.equal &&
         c->hash == __kCFNullBagKeyCallBacks.hash));
}

CF_INLINE Boolean __CFBagKeyCallBacksMatchCFType(const CFBagKeyCallBacks *c) {
    return (&kCFTypeBagKeyCallBacks == c ||
        (c->retain == kCFTypeBagKeyCallBacks.retain &&
         c->release == kCFTypeBagKeyCallBacks.release &&
         c->copyDescription == kCFTypeBagKeyCallBacks.copyDescription &&
         c->equal == kCFTypeBagKeyCallBacks.equal &&
         c->hash == kCFTypeBagKeyCallBacks.hash));
}

CF_INLINE const CFBagValueCallBacks *__CFBagGetValueCallBacks(CFHashRef hc) {
    CFBagValueCallBacks *result = NULL;
    switch (__CFBitfieldGetValue(hc->_xflags, 5, 4)) {
    case __kCFHashHasNullCallBacks:
        return &__kCFNullBagValueCallBacks;
    case __kCFHashHasCFTypeCallBacks:
        return &kCFTypeBagValueCallBacks;
    case __kCFHashHasCustomCallBacks:
        break;
    }
    if (__CFBitfieldGetValue(hc->_xflags, 3, 2) == __kCFHashHasCustomCallBacks) {
        result = (CFBagValueCallBacks *)((uint8_t *)hc + sizeof(struct __CFBag) + sizeof(CFBagKeyCallBacks));
    } else {
        result = (CFBagValueCallBacks *)((uint8_t *)hc + sizeof(struct __CFBag));
    }
    return result;
}

CF_INLINE Boolean __CFBagValueCallBacksMatchNull(const CFBagValueCallBacks *c) {
    return (NULL == c ||
        (c->retain == __kCFNullBagValueCallBacks.retain &&
         c->release == __kCFNullBagValueCallBacks.release &&
         c->copyDescription == __kCFNullBagValueCallBacks.copyDescription &&
         c->equal == __kCFNullBagValueCallBacks.equal));
}

CF_INLINE Boolean __CFBagValueCallBacksMatchCFType(const CFBagValueCallBacks *c) {
    return (&kCFTypeBagValueCallBacks == c ||
        (c->retain == kCFTypeBagValueCallBacks.retain &&
         c->release == kCFTypeBagValueCallBacks.release &&
         c->copyDescription == kCFTypeBagValueCallBacks.copyDescription &&
         c->equal == kCFTypeBagValueCallBacks.equal));
}

CFIndex _CFBagGetKVOBit(CFHashRef hc) {
    return __CFBitfieldGetValue(hc->_xflags, 6, 6);
}

void _CFBagSetKVOBit(CFHashRef hc, CFIndex bit) {
    __CFBitfieldSetValue(((CFMutableHashRef)hc)->_xflags, 6, 6, ((uintptr_t)bit & 0x1));
}

CF_INLINE Boolean __CFBagShouldShrink(CFHashRef hc) {
    return (__kCFHashMutable == __CFHashGetType(hc)) &&
                !(CF_USING_COLLECTABLE_MEMORY && auto_zone_is_finalized(__CFCollectableZone, hc)) && /* GC:  don't shrink finalizing hcs! */
                (hc->_bucketsNum < 4 * hc->_deletes || (256 <= hc->_bucketsCap && hc-> _bucketsUsed < 3 * hc->_bucketsCap / 16));
}

CF_INLINE CFIndex __CFHashGetOccurrenceCount(CFHashRef hc, CFIndex idx) {
#if CFBag
    return hc->_values[idx];
#endif
    return 1;
}

CF_INLINE Boolean __CFHashKeyIsValue(CFHashRef hc, any_t key) {
    return (hc->_marker != key && ~hc->_marker != key) ? true : false;
}

CF_INLINE Boolean __CFHashKeyIsMagic(CFHashRef hc, any_t key) {
    return (hc->_marker == key || ~hc->_marker == key) ? true : false;
}


#if !defined(CF_OBJC_KVO_WILLCHANGE)
#define CF_OBJC_KVO_WILLCHANGE(obj, key)
#define CF_OBJC_KVO_DIDCHANGE(obj, key)
#endif

CF_INLINE uintptr_t __CFBagScrambleHash(uintptr_t k) {
#if 0
    return k;
#else
#if __LP64__
    uintptr_t a = 0x4368726973746F70ULL;
    uintptr_t b = 0x686572204B616E65ULL;
#else
    uintptr_t a = 0x4B616E65UL;
    uintptr_t b = 0x4B616E65UL;
#endif
    uintptr_t c = 1;
    a += k;
#if __LP64__
    a -= b; a -= c; a ^= (c >> 43);
    b -= c; b -= a; b ^= (a << 9);
    c -= a; c -= b; c ^= (b >> 8);
    a -= b; a -= c; a ^= (c >> 38);
    b -= c; b -= a; b ^= (a << 23);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 35);
    b -= c; b -= a; b ^= (a << 49);
    c -= a; c -= b; c ^= (b >> 11);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 18);
    c -= a; c -= b; c ^= (b >> 22);
#else
    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);
#endif
    return c;
#endif
}

static CFIndex __CFBagFindBuckets1a(CFHashRef hc, any_t key) {
    CFHashCode keyHash = (CFHashCode)key;
    keyHash = __CFBagScrambleHash(keyHash);
    any_t *keys = hc->_keys;
    any_t marker = hc->_marker;
    CFIndex probe = keyHash & (hc->_bucketsNum - 1);
    CFIndex probeskip = 1;        // See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
        any_t currKey = keys[probe];
        if (marker == currKey) {                /* empty */
            return kCFNotFound;
        } else if (~marker == currKey) {        /* deleted */
            /* do nothing */
        } else if (currKey == key) {
            return probe;
        }
        probe = probe + probeskip;
        // This alternative to probe % buckets assumes that
        // probeskip is always positive and less than the
        // number of buckets.
        if (hc->_bucketsNum <= probe) {
            probe -= hc->_bucketsNum;
        }
        if (start == probe) {
            return kCFNotFound;
        }
    }
}

static CFIndex __CFBagFindBuckets1b(CFHashRef hc, any_t key) {
    const CFBagKeyCallBacks *cb = __CFBagGetKeyCallBacks(hc);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(any_t, any_pointer_t))cb->hash), key, hc->_context) : (CFHashCode)key;
    keyHash = __CFBagScrambleHash(keyHash);
    any_t *keys = hc->_keys;
    any_t marker = hc->_marker;
    CFIndex probe = keyHash & (hc->_bucketsNum - 1);
    CFIndex probeskip = 1;        // See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
        any_t currKey = keys[probe];
        if (marker == currKey) {                /* empty */
            return kCFNotFound;
        } else if (~marker == currKey) {        /* deleted */
            /* do nothing */
        } else if (currKey == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(any_t, any_t, any_pointer_t))cb->equal, currKey, key, hc->_context))) {
            return probe;
        }
        probe = probe + probeskip;
        // This alternative to probe % buckets assumes that
        // probeskip is always positive and less than the
        // number of buckets.
        if (hc->_bucketsNum <= probe) {
            probe -= hc->_bucketsNum;
        }
        if (start == probe) {
            return kCFNotFound;
        }
    }
}

CF_INLINE CFIndex __CFBagFindBuckets1(CFHashRef hc, any_t key) {
    if (__kCFHashHasNullCallBacks == __CFBitfieldGetValue(hc->_xflags, 3, 2)) {
        return __CFBagFindBuckets1a(hc, key);
    }
    return __CFBagFindBuckets1b(hc, key);
}

static void __CFBagFindBuckets2(CFHashRef hc, any_t key, CFIndex *match, CFIndex *nomatch) {
    const CFBagKeyCallBacks *cb = __CFBagGetKeyCallBacks(hc);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(any_t, any_pointer_t))cb->hash), key, hc->_context) : (CFHashCode)key;
    keyHash = __CFBagScrambleHash(keyHash);
    any_t *keys = hc->_keys;
    any_t marker = hc->_marker;
    CFIndex probe = keyHash & (hc->_bucketsNum - 1);
    CFIndex probeskip = 1;        // See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    *match = kCFNotFound;
    *nomatch = kCFNotFound;
    for (;;) {
        any_t currKey = keys[probe];
        if (marker == currKey) {                /* empty */
            if (nomatch) *nomatch = probe;
            return;
        } else if (~marker == currKey) {        /* deleted */
            if (nomatch) {
                *nomatch = probe;
                nomatch = NULL;
            }
        } else if (currKey == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(any_t, any_t, any_pointer_t))cb->equal, currKey, key, hc->_context))) {
            *match = probe;
            return;
        }
        probe = probe + probeskip;
        // This alternative to probe % buckets assumes that
        // probeskip is always positive and less than the
        // number of buckets.
        if (hc->_bucketsNum <= probe) {
            probe -= hc->_bucketsNum;
        }
        if (start == probe) {
            return;
        }
    }
}

static void __CFBagFindNewMarker(CFHashRef hc) {
    any_t *keys = hc->_keys;
    any_t newMarker;
    CFIndex idx, nbuckets;
    Boolean hit;

    nbuckets = hc->_bucketsNum;
    newMarker = hc->_marker;
    do {
        newMarker--;
        hit = false;
        for (idx = 0; idx < nbuckets; idx++) {
            if (newMarker == keys[idx] || ~newMarker == keys[idx]) {
                hit = true;
                break;
            }
        }
    } while (hit);
    for (idx = 0; idx < nbuckets; idx++) {
        if (hc->_marker == keys[idx]) {
            keys[idx] = newMarker;
        } else if (~hc->_marker == keys[idx]) {
            keys[idx] = ~newMarker;
        }
    }
    ((struct __CFBag *)hc)->_marker = newMarker;
}

static Boolean __CFBagEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFHashRef hc1 = (CFHashRef)cf1;
    CFHashRef hc2 = (CFHashRef)cf2;
    const CFBagKeyCallBacks *cb1, *cb2;
    const CFBagValueCallBacks *vcb1, *vcb2;
    any_t *keys;
    CFIndex idx, nbuckets;
    if (hc1 == hc2) return true;
    if (hc1->_count != hc2->_count) return false;
    cb1 = __CFBagGetKeyCallBacks(hc1);
    cb2 = __CFBagGetKeyCallBacks(hc2);
    if (cb1->equal != cb2->equal) return false;
    vcb1 = __CFBagGetValueCallBacks(hc1);
    vcb2 = __CFBagGetValueCallBacks(hc2);
    if (vcb1->equal != vcb2->equal) return false;
    if (0 == hc1->_bucketsUsed) return true; /* after function comparison! */
    keys = hc1->_keys;
    nbuckets = hc1->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
        if (hc1->_marker != keys[idx] && ~hc1->_marker != keys[idx]) {
#if CFDictionary
            const_any_pointer_t value;
            if (!CFBagGetValueIfPresent(hc2, (any_pointer_t)keys[idx], &value)) return false;
	    if (hc1->_values[idx] != (any_t)value) {
		if (NULL == vcb1->equal) return false;
	        if (!INVOKE_CALLBACK3((Boolean (*)(any_t, any_t, any_pointer_t))vcb1->equal, hc1->_values[idx], (any_t)value, hc1->_context)) return false;
            }
#endif
#if  CFSet
            const_any_pointer_t value;
            if (!CFBagGetValueIfPresent(hc2, (any_pointer_t)keys[idx], &value)) return false;
#endif
#if CFBag
            if (hc1->_values[idx] != CFBagGetCountOfValue(hc2, (any_pointer_t)keys[idx])) return false;
#endif
        }
    }
    return true;
}

static CFHashCode __CFBagHash(CFTypeRef cf) {
    CFHashRef hc = (CFHashRef)cf;
    return hc->_count;
}

static CFStringRef __CFBagCopyDescription(CFTypeRef cf) {
    CFHashRef hc = (CFHashRef)cf;
    CFAllocatorRef allocator;
    const CFBagKeyCallBacks *cb;
    const CFBagValueCallBacks *vcb;
    any_t *keys;
    CFIndex idx, nbuckets;
    CFMutableStringRef result;
    cb = __CFBagGetKeyCallBacks(hc);
    vcb = __CFBagGetValueCallBacks(hc);
    keys = hc->_keys;
    nbuckets = hc->_bucketsNum;
    allocator = CFGetAllocator(hc);
    result = CFStringCreateMutable(allocator, 0);
    const char *type = "?";
    switch (__CFHashGetType(hc)) {
    case __kCFHashImmutable: type = "immutable"; break;
    case __kCFHashMutable: type = "mutable"; break;
    }
    CFStringAppendFormat(result, NULL, CFSTR("<CFBag %p [%p]>{type = %s, count = %u, capacity = %u, pairs = (\n"), cf, allocator, type, hc->_count, hc->_bucketsCap);
    for (idx = 0; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            CFStringRef kDesc = NULL, vDesc = NULL;
            if (NULL != cb->copyDescription) {
                kDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(any_t, any_pointer_t))cb->copyDescription), keys[idx], hc->_context);
            }
            if (NULL != vcb->copyDescription) {
                vDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(any_t, any_pointer_t))vcb->copyDescription), hc->_values[idx], hc->_context);
            }
#if CFDictionary
            if (NULL != kDesc && NULL != vDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ = %@\n"), idx, kDesc, vDesc);
                CFRelease(kDesc);
                CFRelease(vDesc);
            } else if (NULL != kDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ = <%p>\n"), idx, kDesc, hc->_values[idx]);
                CFRelease(kDesc);
            } else if (NULL != vDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> = %@\n"), idx, keys[idx], vDesc);
                CFRelease(vDesc);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> = <%p>\n"), idx, keys[idx], hc->_values[idx]);
            }
#endif
#if CFSet
            if (NULL != kDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@\n"), idx, kDesc);
                CFRelease(kDesc);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p>\n"), idx, keys[idx]);
            }
#endif
#if CFBag
            if (NULL != kDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ (%ld)\n"), idx, kDesc, hc->_values[idx]);
                CFRelease(kDesc);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> (%ld)\n"), idx, keys[idx], hc->_values[idx]);
            }
#endif
        }
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static void __CFBagDeallocate(CFTypeRef cf) {
    CFMutableHashRef hc = (CFMutableHashRef)cf;
    CFAllocatorRef allocator = __CFGetAllocator(hc);
    const CFBagKeyCallBacks *cb = __CFBagGetKeyCallBacks(hc);
    const CFBagValueCallBacks *vcb = __CFBagGetValueCallBacks(hc);

    // mark now in case any callout somehow tries to add an entry back in
    markFinalized(cf);
    if (vcb->release || cb->release) {
        any_t *keys = hc->_keys;
        CFIndex idx, nbuckets = hc->_bucketsNum;
        for (idx = 0; idx < nbuckets; idx++) {
            any_t oldkey = keys[idx];
            if (hc->_marker != oldkey && ~hc->_marker != oldkey) {
                if (vcb->release) {
                    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, any_t, any_pointer_t))vcb->release), allocator, hc->_values[idx], hc->_context);
                }
                if (cb->release) {
                    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, any_t, any_pointer_t))cb->release), allocator, oldkey, hc->_context);
                }
            }
        }
    }

    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        // return early so that contents are preserved after finalization
        return;
    }
    
    _CFAllocatorDeallocateGC(allocator, hc->_keys);
#if CFDictionary || CFBag
    _CFAllocatorDeallocateGC(allocator, hc->_values);
#endif
    hc->_keys = NULL;
    hc->_values = NULL;
    hc->_count = 0;  // GC: also zero count, so the hc will appear empty.
    hc->_bucketsUsed = 0;
    hc->_bucketsNum = 0;
}

static CFTypeID __kCFBagTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFBagClass = {
    _kCFRuntimeScannedObject,
    "CFBag",
    NULL,        // init
    NULL,        // copy
    __CFBagDeallocate,
    __CFBagEqual,
    __CFBagHash,
    NULL,        //
    __CFBagCopyDescription
};

__private_extern__ void __CFBagInitialize(void) {
    __kCFHashTypeID = _CFRuntimeRegisterClass(&__CFBagClass);
}

CFTypeID CFBagGetTypeID(void) {
    return __kCFHashTypeID;
}

static CFMutableHashRef __CFBagInit(CFAllocatorRef allocator, CFOptionFlags flags, CFIndex capacity, const CFBagKeyCallBacks *keyCallBacks
#if CFDictionary
, const CFBagValueCallBacks *valueCallBacks
#endif
) {
    struct __CFBag *hc;
    CFIndex size;
    __CFBitfieldSetValue(flags, 31, 2, 0);
    CFOptionFlags xflags = 0;
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        // preserve NULL for key or value CB, otherwise fix up.
        if (!keyCallBacks || (keyCallBacks->retain == NULL && keyCallBacks->release == NULL)) {
            xflags = __kCFHashWeakKeys;
        }
#if CFDictionary
        if (!valueCallBacks || (valueCallBacks->retain == NULL && valueCallBacks->release == NULL)) {
            xflags |= __kCFHashWeakValues;
        }
#endif
#if CFBag
        xflags |= __kCFHashWeakValues;
#endif
    }
    if (__CFBagKeyCallBacksMatchNull(keyCallBacks)) {
        __CFBitfieldSetValue(flags, 3, 2, __kCFHashHasNullCallBacks);
    } else if (__CFBagKeyCallBacksMatchCFType(keyCallBacks)) {
        __CFBitfieldSetValue(flags, 3, 2, __kCFHashHasCFTypeCallBacks);
    } else {
        __CFBitfieldSetValue(flags, 3, 2, __kCFHashHasCustomCallBacks);
    }
#if CFDictionary
    if (__CFBagValueCallBacksMatchNull(valueCallBacks)) {
        __CFBitfieldSetValue(flags, 5, 4, __kCFHashHasNullCallBacks);
    } else if (__CFBagValueCallBacksMatchCFType(valueCallBacks)) {
        __CFBitfieldSetValue(flags, 5, 4, __kCFHashHasCFTypeCallBacks);
    } else {
        __CFBitfieldSetValue(flags, 5, 4, __kCFHashHasCustomCallBacks);
    }
#endif
    size = __CFBagGetSizeOfType(flags) - sizeof(CFRuntimeBase);
    hc = (struct __CFBag *)_CFRuntimeCreateInstance(allocator, __kCFHashTypeID, size, NULL);
    if (NULL == hc) {
        return NULL;
    }
    hc->_count = 0;
    hc->_bucketsUsed = 0;
    hc->_marker = (any_t)0xa1b1c1d3;
    hc->_context = NULL;
    hc->_deletes = 0;
    hc->_mutations = 1;
    hc->_xflags = xflags | flags;
    switch (__CFBitfieldGetValue(flags, 1, 0)) {
    case __kCFHashImmutable:
        if (__CFOASafe) __CFSetLastAllocationEventName(hc, "CFBag (immutable)");
        break;
    case __kCFHashMutable:
        if (__CFOASafe) __CFSetLastAllocationEventName(hc, "CFBag (mutable-variable)");
        break;
    }
    hc->_bucketsCap = __CFHashRoundUpCapacity(1);
    hc->_bucketsNum = 0;
    hc->_keys = NULL;
    hc->_values = NULL;
    if (__kCFHashHasCustomCallBacks == __CFBitfieldGetValue(flags, 3, 2)) {
        CFBagKeyCallBacks *cb = (CFBagKeyCallBacks *)__CFBagGetKeyCallBacks((CFHashRef)hc);
        *cb = *keyCallBacks;
        FAULT_CALLBACK((void **)&(cb->retain));
        FAULT_CALLBACK((void **)&(cb->release));
        FAULT_CALLBACK((void **)&(cb->copyDescription));
        FAULT_CALLBACK((void **)&(cb->equal));
        FAULT_CALLBACK((void **)&(cb->hash));
    }
#if CFDictionary
    if (__kCFHashHasCustomCallBacks == __CFBitfieldGetValue(flags, 5, 4)) {
        CFBagValueCallBacks *vcb = (CFBagValueCallBacks *)__CFBagGetValueCallBacks((CFHashRef)hc);
        *vcb = *valueCallBacks;
        FAULT_CALLBACK((void **)&(vcb->retain));
        FAULT_CALLBACK((void **)&(vcb->release));
        FAULT_CALLBACK((void **)&(vcb->copyDescription));
        FAULT_CALLBACK((void **)&(vcb->equal));
    }
#endif
    return hc;
}

#if CFDictionary
CFHashRef CFBagCreate(CFAllocatorRef allocator, const_any_pointer_t *keys, const_any_pointer_t *values, CFIndex numValues, const CFBagKeyCallBacks *keyCallBacks, const CFBagValueCallBacks *valueCallBacks) {
#endif
#if CFSet || CFBag
CFHashRef CFBagCreate(CFAllocatorRef allocator, const_any_pointer_t *keys, CFIndex numValues, const CFBagKeyCallBacks *keyCallBacks) {
#endif
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%ld) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
#if CFDictionary
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashImmutable, numValues, keyCallBacks, valueCallBacks);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashImmutable, numValues, keyCallBacks);
#endif
    __CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashMutable);
    for (CFIndex idx = 0; idx < numValues; idx++) {
#if CFDictionary
        CFBagAddValue(hc, keys[idx], values[idx]);
#endif
#if CFSet || CFBag
        CFBagAddValue(hc, keys[idx]);
#endif
    }
    __CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashImmutable);
    return (CFHashRef)hc;
}

#if CFDictionary
CFMutableHashRef CFBagCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFBagKeyCallBacks *keyCallBacks, const CFBagValueCallBacks *valueCallBacks) {
#endif
#if CFSet || CFBag
CFMutableHashRef CFBagCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFBagKeyCallBacks *keyCallBacks) {
#endif
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%ld) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
#if CFDictionary
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashMutable, capacity, keyCallBacks, valueCallBacks);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashMutable, capacity, keyCallBacks);
#endif
    return hc;
}

#if CFDictionary || CFSet
// does not have Add semantics for Bag; it has Set semantics ... is that best?
static void __CFBagGrow(CFMutableHashRef hc, CFIndex numNewValues);

// This creates a hc which is for CFTypes or NSObjects, with a CFRetain style ownership transfer;
// the hc does not take a retain (since it claims 1), and the caller does not need to release the inserted objects (since we do it).
// The incoming objects must also be collectable if allocated out of a collectable allocator - and are neither released nor retained.
#if CFDictionary
CFHashRef _CFBagCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const_any_pointer_t *keys, const_any_pointer_t *values, CFIndex numValues) {
#endif
#if CFSet || CFBag
CFHashRef _CFBagCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const_any_pointer_t *keys, CFIndex numValues) {
#endif
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%ld) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
#if CFDictionary
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashMutable, numValues, &kCFTypeBagKeyCallBacks, &kCFTypeBagValueCallBacks);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashMutable, numValues, &kCFTypeBagKeyCallBacks);
#endif
    __CFBagGrow(hc, numValues);
    for (CFIndex idx = 0; idx < numValues; idx++) {
        CFIndex match, nomatch;
        __CFBagFindBuckets2(hc, (any_t)keys[idx], &match, &nomatch);
        if (kCFNotFound == match) {
            CFAllocatorRef allocator = __CFGetAllocator(hc);
            any_t newKey = (any_t)keys[idx];
            if (__CFHashKeyIsMagic(hc, newKey)) {
                __CFBagFindNewMarker(hc);
            }
            if (hc->_keys[nomatch] == ~hc->_marker) {
                hc->_deletes--;
            }
            CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
            CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[nomatch], newKey);
#if CFDictionary
            any_t newValue = (any_t)values[idx];
            CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
            CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[nomatch], newValue);
#endif
#if CFBag
            hc->_values[nomatch] = 1;
#endif
            hc->_bucketsUsed++;
            hc->_count++;
        } else {
            CFAllocatorRef allocator = __CFGetAllocator(hc);
#if CFSet || CFBag
            any_t oldKey = hc->_keys[match];
            any_t newKey = (any_t)keys[idx];
            CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
            CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], ~hc->_marker);
            if (__CFHashKeyIsMagic(hc, newKey)) {
                __CFBagFindNewMarker(hc);
            }
            CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], newKey);
            RELEASEKEY(oldKey);
#endif
#if CFDictionary
            any_t oldValue = hc->_values[match];
            any_t newValue = (any_t)values[idx];
            CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
            CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[match], newValue);
            RELEASEVALUE(oldValue);
#endif
        }
    }
    if (!isMutable) __CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashImmutable);
    return (CFHashRef)hc;
}
#endif

CFHashRef CFBagCreateCopy(CFAllocatorRef allocator, CFHashRef other) {
    CFMutableHashRef hc = CFBagCreateMutableCopy(allocator, CFBagGetCount(other), other);
    __CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashImmutable);
    if (__CFOASafe) __CFSetLastAllocationEventName(hc, "CFBag (immutable)");
    return hc;
}

CFMutableHashRef CFBagCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFHashRef other) {
    CFIndex numValues = CFBagGetCount(other);
    const_any_pointer_t *list, buffer[256];
    list = (numValues <= 256) ? buffer : (const_any_pointer_t *)CFAllocatorAllocate(allocator, numValues * sizeof(const_any_pointer_t), 0);
    if (list != buffer && __CFOASafe) __CFSetLastAllocationEventName(list, "CFBag (temp)");
#if CFDictionary
    const_any_pointer_t *vlist, vbuffer[256];
    vlist = (numValues <= 256) ? vbuffer : (const_any_pointer_t *)CFAllocatorAllocate(allocator, numValues * sizeof(const_any_pointer_t), 0);
    if (vlist != vbuffer && __CFOASafe) __CFSetLastAllocationEventName(vlist, "CFBag (temp)");
#endif
#if CFSet || CFBag
    CFBagGetValues(other, list);
#endif
#if CFDictionary
    CFBagGetKeysAndValues(other, list, vlist);
#endif
    const CFBagKeyCallBacks *kcb;
    const CFBagValueCallBacks *vcb;
    if (CF_IS_OBJC(__kCFHashTypeID, other)) {
        kcb = &kCFTypeBagKeyCallBacks;
        vcb = &kCFTypeBagValueCallBacks;
    } else {
        kcb = __CFBagGetKeyCallBacks(other);
        vcb = __CFBagGetValueCallBacks(other);
    }
#if CFDictionary
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashMutable, capacity, kcb, vcb);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __CFBagInit(allocator, __kCFHashMutable, capacity, kcb);
#endif
    if (0 == capacity) _CFBagSetCapacity(hc, numValues);
    for (CFIndex idx = 0; idx < numValues; idx++) {
#if CFDictionary
        CFBagAddValue(hc, list[idx], vlist[idx]);
#endif
#if CFSet || CFBag
        CFBagAddValue(hc, list[idx]);
#endif
    }
    if (list != buffer) CFAllocatorDeallocate(allocator, list);
#if CFDictionary
    if (vlist != vbuffer) CFAllocatorDeallocate(allocator, vlist);
#endif
    return hc;
}

// Used by NSHashTables/NSMapTables and KVO
void _CFBagSetContext(CFHashRef hc, any_pointer_t context) {
    __CFGenericValidateType(hc, __kCFHashTypeID);
    CF_WRITE_BARRIER_BASE_ASSIGN(__CFGetAllocator(hc), hc, hc->_context, context);
}

any_pointer_t _CFBagGetContext(CFHashRef hc) {
    __CFGenericValidateType(hc, __kCFHashTypeID);
    return hc->_context;
}

CFIndex CFBagGetCount(CFHashRef hc) {
    if (CFDictionary || CFSet) CF_OBJC_FUNCDISPATCH0(__kCFHashTypeID, CFIndex, hc, "count");
    __CFGenericValidateType(hc, __kCFHashTypeID);
    return hc->_count;
}

#if CFDictionary
CFIndex CFBagGetCountOfKey(CFHashRef hc, const_any_pointer_t key) {
#endif
#if CFSet || CFBag
CFIndex CFBagGetCountOfValue(CFHashRef hc, const_any_pointer_t key) {
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, CFIndex, hc, "countForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, CFIndex, hc, "countForObject:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return 0;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    return (kCFNotFound != match ? __CFHashGetOccurrenceCount(hc, match) : 0);
}

#if CFDictionary
Boolean CFBagContainsKey(CFHashRef hc, const_any_pointer_t key) {
#endif
#if CFSet || CFBag
Boolean CFBagContainsValue(CFHashRef hc, const_any_pointer_t key) {
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, char, hc, "containsKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, char, hc, "containsObject:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    return (kCFNotFound != match ? true : false);
}

#if CFDictionary
CFIndex CFBagGetCountOfValue(CFHashRef hc, const_any_pointer_t value) {
    CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, CFIndex, hc, "countForObject:", value);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return 0;
    any_t *keys = hc->_keys;
    Boolean (*equal)(any_t, any_t, any_pointer_t) = (Boolean (*)(any_t, any_t, any_pointer_t))__CFBagGetValueCallBacks(hc)->equal;
    CFIndex cnt = 0;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            if ((hc->_values[idx] == (any_t)value) || (equal && INVOKE_CALLBACK3(equal, hc->_values[idx], (any_t)value, hc->_context))) {
                cnt++;
            }
        }
    }
    return cnt;
}

Boolean CFBagContainsValue(CFHashRef hc, const_any_pointer_t value) {
    CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, char, hc, "containsObject:", value);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    any_t *keys = hc->_keys;
    Boolean (*equal)(any_t, any_t, any_pointer_t) = (Boolean (*)(any_t, any_t, any_pointer_t))__CFBagGetValueCallBacks(hc)->equal;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            if ((hc->_values[idx] == (any_t)value) || (equal && INVOKE_CALLBACK3(equal, hc->_values[idx], (any_t)value, hc->_context))) {
                return true;
            }
        }
    }
    return false;
}
#endif

const_any_pointer_t CFBagGetValue(CFHashRef hc, const_any_pointer_t key) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, const_any_pointer_t, hc, "objectForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, const_any_pointer_t, hc, "member:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return 0;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    return (kCFNotFound != match ? (const_any_pointer_t)(CFDictionary ? hc->_values[match] : hc->_keys[match]) : 0);
}

Boolean CFBagGetValueIfPresent(CFHashRef hc, const_any_pointer_t key, const_any_pointer_t *value) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, Boolean, hc, "_getValue:forKey:", (any_t *)value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, Boolean, hc, "_getValue:forObj:", (any_t *)value, key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    return (kCFNotFound != match ? ((value ? __CFObjCStrongAssign((const_any_pointer_t)(CFDictionary ? hc->_values[match] : hc->_keys[match]), value) : 0), true) : false);
}

#if CFDictionary
Boolean CFBagGetKeyIfPresent(CFHashRef hc, const_any_pointer_t key, const_any_pointer_t *actualkey) {
    CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, Boolean, hc, "getActualKey:forKey:", actualkey, key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    return (kCFNotFound != match ? ((actualkey ? __CFObjCStrongAssign((const_any_pointer_t)hc->_keys[match], actualkey) : NULL), true) : false);
}
#endif

#if CFDictionary
void CFBagGetKeysAndValues(CFHashRef hc, const_any_pointer_t *keybuf, const_any_pointer_t *valuebuf) {
#endif
#if CFSet || CFBag
void CFBagGetValues(CFHashRef hc, const_any_pointer_t *keybuf) {
    const_any_pointer_t *valuebuf = 0;
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, void, hc, "getObjects:andKeys:", (any_t *)valuebuf, (any_t *)keybuf);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, void, hc, "getObjects:", (any_t *)keybuf);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    if (CF_USING_COLLECTABLE_MEMORY) {
        // GC: speculatively issue a write-barrier on the copied to buffers
        __CFObjCWriteBarrierRange(keybuf, hc->_count * sizeof(any_t));
        __CFObjCWriteBarrierRange(valuebuf, hc->_count * sizeof(any_t));
    }
    any_t *keys = hc->_keys;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            for (CFIndex cnt = __CFHashGetOccurrenceCount(hc, idx); cnt--;) {
                if (keybuf) *keybuf++ = (const_any_pointer_t)keys[idx];
                if (valuebuf) *valuebuf++ = (const_any_pointer_t)hc->_values[idx];
            }
        }
    }
}

#if CFDictionary || CFSet
unsigned long _CFBagFastEnumeration(CFHashRef hc, struct __objcFastEnumerationStateEquivalent *state, void *stackbuffer, unsigned long count) {
    /* copy as many as count items over */
    if (0 == state->state) {        /* first time */
        state->mutationsPtr = (unsigned long *)&hc->_mutations;
    }
    state->itemsPtr = (unsigned long *)stackbuffer;
    CFIndex cnt = 0;
    any_t *keys = hc->_keys;
    for (CFIndex idx = (CFIndex)state->state, nbuckets = hc->_bucketsNum; idx < nbuckets && cnt < (CFIndex)count; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            state->itemsPtr[cnt++] = (unsigned long)keys[idx];
        }
        state->state++;
    }
    return cnt;
}
#endif

void CFBagApplyFunction(CFHashRef hc, CFBagApplierFunction applier, any_pointer_t context) {
    FAULT_CALLBACK((void **)&(applier));
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, void, hc, "_apply:context:", applier, context);
    if (CFSet) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, void, hc, "_applyValues:context:", applier, context);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    any_t *keys = hc->_keys;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            for (CFIndex cnt = __CFHashGetOccurrenceCount(hc, idx); cnt--;) {
#if CFDictionary
                INVOKE_CALLBACK3(applier, (const_any_pointer_t)keys[idx], (const_any_pointer_t)hc->_values[idx], context);
#endif
#if CFSet || CFBag
                INVOKE_CALLBACK2(applier, (const_any_pointer_t)keys[idx], context);
#endif
            }
        }
    }
}

static void __CFBagGrow(CFMutableHashRef hc, CFIndex numNewValues) {
    any_t *oldkeys = hc->_keys;
    any_t *oldvalues = hc->_values;
    CFIndex nbuckets = hc->_bucketsNum;
    hc->_bucketsCap = __CFHashRoundUpCapacity(hc->_bucketsUsed + numNewValues);
    hc->_bucketsNum = __CFHashNumBucketsForCapacity(hc->_bucketsCap);
    hc->_deletes = 0;
    CFAllocatorRef allocator = __CFGetAllocator(hc);
    CFOptionFlags weakOrStrong = (hc->_xflags & __kCFHashWeakKeys) ? 0 : __kCFAllocatorGCScannedMemory;
    any_t *mem = (any_t *)_CFAllocatorAllocateGC(allocator, hc->_bucketsNum * sizeof(any_t), weakOrStrong);
    if (NULL == mem) __CFBagHandleOutOfMemory(hc, hc->_bucketsNum * sizeof(any_t));
    if (__CFOASafe) __CFSetLastAllocationEventName(mem, "CFBag (key-store)");
    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, hc, hc->_keys, mem);
    CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;  // GC: avoids write-barrier in weak case.
    any_t *keysBase = mem;
#if CFDictionary || CFBag
    weakOrStrong = (hc->_xflags & __kCFHashWeakValues) ? 0 : __kCFAllocatorGCScannedMemory;
    mem = (any_t *)_CFAllocatorAllocateGC(allocator, hc->_bucketsNum * sizeof(any_t), weakOrStrong);
    if (NULL == mem) __CFBagHandleOutOfMemory(hc, hc->_bucketsNum * sizeof(any_t));
    if (__CFOASafe) __CFSetLastAllocationEventName(mem, "CFBag (value-store)");
    CF_WRITE_BARRIER_BASE_ASSIGN(allocator, hc, hc->_values, mem);
#endif
#if CFDictionary
    CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator; // GC: avoids write-barrier in weak case.
    any_t *valuesBase = mem;
#endif
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        hc->_keys[idx] = hc->_marker;
#if CFDictionary || CFBag
        hc->_values[idx] = 0;
#endif
    }
    if (NULL == oldkeys) return;
    for (CFIndex idx = 0; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, oldkeys[idx])) {
            CFIndex match, nomatch;
            __CFBagFindBuckets2(hc, oldkeys[idx], &match, &nomatch);
            CFAssert3(kCFNotFound == match, __kCFLogAssertion, "%s(): two values (%p, %p) now hash to the same slot; mutable value changed while in table or hash value is not immutable", __PRETTY_FUNCTION__, oldkeys[idx], hc->_keys[match]);
            if (kCFNotFound != nomatch) {
                CF_WRITE_BARRIER_BASE_ASSIGN(keysAllocator, keysBase, hc->_keys[nomatch], oldkeys[idx]);
#if CFDictionary
                CF_WRITE_BARRIER_BASE_ASSIGN(valuesAllocator, valuesBase, hc->_values[nomatch], oldvalues[idx]);
#endif
#if CFBag
                hc->_values[nomatch] = oldvalues[idx];
#endif
            }
        }
    }
    _CFAllocatorDeallocateGC(allocator, oldkeys);
    _CFAllocatorDeallocateGC(allocator, oldvalues);
}

// This function is for Foundation's benefit; no one else should use it.
void _CFBagSetCapacity(CFMutableHashRef hc, CFIndex cap) {
    if (CF_IS_OBJC(__kCFHashTypeID, hc)) return;
    __CFGenericValidateType(hc, __kCFHashTypeID);
    CFAssert1(__CFHashGetType(hc) != __kCFHashImmutable, __kCFLogAssertion, "%s(): collection is immutable", __PRETTY_FUNCTION__);
    CFAssert3(hc->_bucketsUsed <= cap, __kCFLogAssertion, "%s(): desired capacity (%ld) is less than bucket count (%ld)", __PRETTY_FUNCTION__, cap, hc->_bucketsUsed);
    __CFBagGrow(hc, cap - hc->_bucketsUsed);
}


#if CFDictionary
void CFBagAddValue(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void CFBagAddValue(CFMutableHashRef hc, const_any_pointer_t key) {
    #define value 0
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, void, hc, "_addObject:forKey:", value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, void, hc, "addObject:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        if (hc->_bucketsUsed == hc->_bucketsCap || NULL == hc->_keys) {
            __CFBagGrow(hc, 1);
        }
        break;
    default:
        CFAssert2(__CFHashGetType(hc) != __kCFHashImmutable, __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
        break;
    }
    hc->_mutations++;
    CFIndex match, nomatch;
    __CFBagFindBuckets2(hc, (any_t)key, &match, &nomatch);
    if (kCFNotFound != match) {
#if CFBag
        CF_OBJC_KVO_WILLCHANGE(hc, hc->_keys[match]);
        hc->_values[match]++;
        hc->_count++;
        CF_OBJC_KVO_DIDCHANGE(hc, hc->_keys[match]);
#endif
    } else {
        CFAllocatorRef allocator = __CFGetAllocator(hc);
        GETNEWKEY(newKey, key);
#if CFDictionary
        GETNEWVALUE(newValue);
#endif
        if (__CFHashKeyIsMagic(hc, newKey)) {
            __CFBagFindNewMarker(hc);
        }
        if (hc->_keys[nomatch] == ~hc->_marker) {
            hc->_deletes--;
        }
        CF_OBJC_KVO_WILLCHANGE(hc, key);
        CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[nomatch], newKey);
#if CFDictionary
        CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[nomatch], newValue);
#endif
#if CFBag
        hc->_values[nomatch] = 1;
#endif
        hc->_bucketsUsed++;
        hc->_count++;
        CF_OBJC_KVO_DIDCHANGE(hc, key);
    }
}

#if CFDictionary
void CFBagReplaceValue(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void CFBagReplaceValue(CFMutableHashRef hc, const_any_pointer_t key) {
    #define value 0
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, void, hc, "_replaceObject:forKey:", value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, void, hc, "_replaceObject:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        break;
    default:
        CFAssert2(__CFHashGetType(hc) != __kCFHashImmutable, __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
        break;
    }
    hc->_mutations++;
    if (0 == hc->_bucketsUsed) return;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    if (kCFNotFound == match) return;
    CFAllocatorRef allocator = __CFGetAllocator(hc);
#if CFSet || CFBag
    GETNEWKEY(newKey, key);
#endif
#if CFDictionary
    GETNEWVALUE(newValue);
#endif
    any_t oldKey = hc->_keys[match];
    CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
#if CFSet || CFBag
    CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
    CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], ~hc->_marker);
    if (__CFHashKeyIsMagic(hc, newKey)) {
        __CFBagFindNewMarker(hc);
    }
    CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], newKey);
#endif
#if CFDictionary
    any_t oldValue = hc->_values[match];
    CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
    CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[match], newValue);
#endif
    CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
#if CFSet || CFBag
    RELEASEKEY(oldKey);
#endif
#if CFDictionary
    RELEASEVALUE(oldValue);
#endif
}

#if CFDictionary
void CFBagSetValue(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void CFBagSetValue(CFMutableHashRef hc, const_any_pointer_t key) {
    #define value 0
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFHashTypeID, void, hc, "setObject:forKey:", value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, void, hc, "_setObject:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        if (hc->_bucketsUsed == hc->_bucketsCap || NULL == hc->_keys) {
            __CFBagGrow(hc, 1);
        }
        break;
    default:
        CFAssert2(__CFHashGetType(hc) != __kCFHashImmutable, __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
        break;
    }
    hc->_mutations++;
    CFIndex match, nomatch;
    __CFBagFindBuckets2(hc, (any_t)key, &match, &nomatch);
    if (kCFNotFound == match) {
        CFAllocatorRef allocator = __CFGetAllocator(hc);
        GETNEWKEY(newKey, key);
#if CFDictionary
        GETNEWVALUE(newValue);
#endif
        if (__CFHashKeyIsMagic(hc, newKey)) {
            __CFBagFindNewMarker(hc);
        }
        if (hc->_keys[nomatch] == ~hc->_marker) {
            hc->_deletes--;
        }
        CF_OBJC_KVO_WILLCHANGE(hc, key);
        CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[nomatch], newKey);
#if CFDictionary
        CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[nomatch], newValue);
#endif
#if CFBag
        hc->_values[nomatch] = 1;
#endif
        hc->_bucketsUsed++;
        hc->_count++;
        CF_OBJC_KVO_DIDCHANGE(hc, key);
    } else {
        CFAllocatorRef allocator = __CFGetAllocator(hc);
#if CFSet || CFBag
        GETNEWKEY(newKey, key);
#endif
#if CFDictionary
        GETNEWVALUE(newValue);
#endif
        any_t oldKey = hc->_keys[match];
        CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
#if CFSet || CFBag
        CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], ~hc->_marker);
        if (__CFHashKeyIsMagic(hc, newKey)) {
            __CFBagFindNewMarker(hc);
        }
        CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], newKey);
#endif
#if CFDictionary
        any_t oldValue = hc->_values[match];
        CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[match], newValue);
#endif
        CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
#if CFSet || CFBag
        RELEASEKEY(oldKey);
#endif
#if CFDictionary
        RELEASEVALUE(oldValue);
#endif
    }
}

void CFBagRemoveValue(CFMutableHashRef hc, const_any_pointer_t key) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, void, hc, "removeObjectForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFHashTypeID, void, hc, "removeObject:", key);
    __CFGenericValidateType(hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        break;
    default:
        CFAssert2(__CFHashGetType(hc) != __kCFHashImmutable, __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
        break;
    }
    hc->_mutations++;
    if (0 == hc->_bucketsUsed) return;
    CFIndex match = __CFBagFindBuckets1(hc, (any_t)key);
    if (kCFNotFound == match) return;
    if (1 < __CFHashGetOccurrenceCount(hc, match)) {
#if CFBag
        CF_OBJC_KVO_WILLCHANGE(hc, hc->_keys[match]);
        hc->_values[match]--;
        hc->_count--;
        CF_OBJC_KVO_DIDCHANGE(hc, hc->_keys[match]);
#endif
    } else {
        CFAllocatorRef allocator = __CFGetAllocator(hc);
        any_t oldKey = hc->_keys[match];
        CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
        CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[match], ~hc->_marker);
#if CFDictionary
        any_t oldValue = hc->_values[match];
        CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
        CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[match], 0);
#endif
#if CFBag
        hc->_values[match] = 0;
#endif
        hc->_count--;
        hc->_bucketsUsed--;
        hc->_deletes++;
        CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
        RELEASEKEY(oldKey);
#if CFDictionary
        RELEASEVALUE(oldValue);
#endif
        if (__CFBagShouldShrink(hc)) {
            __CFBagGrow(hc, 0);
        } else {
            // When the probeskip == 1 always and only, a DELETED slot followed by an EMPTY slot
            // can be converted to an EMPTY slot.  By extension, a chain of DELETED slots followed
            // by an EMPTY slot can be converted to EMPTY slots, which is what we do here.
            if (match < hc->_bucketsNum - 1 && hc->_keys[match + 1] == hc->_marker) {
                while (0 <= match && hc->_keys[match] == ~hc->_marker) {
                    hc->_keys[match] = hc->_marker;
                    hc->_deletes--;
                    match--;
                }
            }
        }
    }
}

void CFBagRemoveAllValues(CFMutableHashRef hc) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH0(__kCFHashTypeID, void, hc, "removeAllObjects");
    if (CFSet) CF_OBJC_FUNCDISPATCH0(__kCFHashTypeID, void, hc, "removeAllObjects");
    __CFGenericValidateType(hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        break;
    default:
        CFAssert2(__CFHashGetType(hc) != __kCFHashImmutable, __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
        break;
    }
    hc->_mutations++;
    if (0 == hc->_bucketsUsed) return;
    CFAllocatorRef allocator = __CFGetAllocator(hc);
    any_t *keys = hc->_keys;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            any_t oldKey = keys[idx];
            CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
#if CFDictionary || CFSet
            hc->_count--;
#endif
#if CFBag
            hc->_count -= hc->_values[idx];
#endif
            CFAllocatorRef keysAllocator = (hc->_xflags & __kCFHashWeakKeys) ? kCFAllocatorNull : allocator;
            CF_WRITE_BARRIER_ASSIGN(keysAllocator, hc->_keys[idx], ~hc->_marker);
#if CFDictionary
            any_t oldValue = hc->_values[idx];
            CFAllocatorRef valuesAllocator = (hc->_xflags & __kCFHashWeakValues) ? kCFAllocatorNull : allocator;
            CF_WRITE_BARRIER_ASSIGN(valuesAllocator, hc->_values[idx], 0);
#endif
#if CFBag
            hc->_values[idx] = 0;
#endif
            hc->_bucketsUsed--;
            hc->_deletes++;
            CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
            RELEASEKEY(oldKey);
#if CFDictionary
            RELEASEVALUE(oldValue);
#endif
        }
    }
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        keys[idx] = hc->_marker;
    }
    hc->_deletes = 0;
    hc->_bucketsUsed = 0;
    hc->_count = 0;
    if (__CFBagShouldShrink(hc) && (256 <= hc->_bucketsCap)) {
        __CFBagGrow(hc, 128);
    }
}

#undef CF_OBJC_KVO_WILLCHANGE
#undef CF_OBJC_KVO_DIDCHANGE

