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

/*	CFBag.c
	Copyright 1998-2008, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
	Machine generated from Notes/HashingCode.template
*/





#include <CoreFoundation/CFBag.h>
#include "CFInternal.h"
#include "CFBasicHash.h"
#include <CoreFoundation/CFString.h>

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
#define CFHashKeyCallBacks CFBagKeyCallBacks
#define CFHashValueCallBacks CFBagValueCallBacks
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
#define CFHashKeyCallBacks CFBagCallBacks
#define CFHashValueCallBacks CFBagCallBacks
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
#define CFHashKeyCallBacks CFBagCallBacks
#define CFHashValueCallBacks CFBagCallBacks
#endif


typedef uintptr_t any_t;
typedef const void * const_any_pointer_t;
typedef void * any_pointer_t;

static Boolean __CFBagEqual(CFTypeRef cf1, CFTypeRef cf2) {
    return __CFBasicHashEqual((CFBasicHashRef)cf1, (CFBasicHashRef)cf2);
}

static CFHashCode __CFBagHash(CFTypeRef cf) {
    return __CFBasicHashHash((CFBasicHashRef)cf);
}

static CFStringRef __CFBagCopyDescription(CFTypeRef cf) {
    return __CFBasicHashCopyDescription((CFBasicHashRef)cf);
}

static void __CFBagDeallocate(CFTypeRef cf) {
    __CFBasicHashDeallocate((CFBasicHashRef)cf);
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

CFTypeID CFBagGetTypeID(void) {
    if (_kCFRuntimeNotATypeID == __kCFBagTypeID) __kCFBagTypeID = _CFRuntimeRegisterClass(&__CFBagClass);
    return __kCFBagTypeID;
}

static uintptr_t __CFBagCallback(CFBasicHashRef ht, uint8_t op, uintptr_t a1, uintptr_t a2, CFBasicHashCallbacks *cb) {
    switch (op) {
    case kCFBasicHashCallbackOpCopyCallbacks: {
        CFBasicHashCallbacks *newcb = NULL;
        if (CF_IS_COLLECTABLE_ALLOCATOR((CFAllocatorRef)a1)) {
            newcb = (CFBasicHashCallbacks *)auto_zone_allocate_object(auto_zone(), 10 * sizeof(void *), AUTO_MEMORY_UNSCANNED, true, false);
        } else {
            newcb = (CFBasicHashCallbacks *)CFAllocatorAllocate((CFAllocatorRef)a1, 10 * sizeof(void *), 0);
        }
        if (!newcb) HALT;
        memmove(newcb, (void *)cb, 10 * sizeof(void *));
        return (uintptr_t)newcb;
    }
    case kCFBasicHashCallbackOpFreeCallbacks: {
        if (CF_IS_COLLECTABLE_ALLOCATOR((CFAllocatorRef)a1)) {
	    auto_zone_release(auto_zone(), cb);
	} else {
            CFAllocatorDeallocate((CFAllocatorRef)a1, cb);
        }
        return 0;
    }
    case kCFBasicHashCallbackOpRetainValue: {
        const_any_pointer_t (*value_retain)(CFAllocatorRef, const_any_pointer_t) = (const_any_pointer_t (*)(CFAllocatorRef, const_any_pointer_t))cb->context[0];
        if (NULL == value_retain) return a1;
        return (uintptr_t)INVOKE_CALLBACK2(value_retain, CFGetAllocator(ht), (const_any_pointer_t)a1);
    }
    case kCFBasicHashCallbackOpRetainKey: {
        const_any_pointer_t (*key_retain)(CFAllocatorRef, const_any_pointer_t) = (const_any_pointer_t (*)(CFAllocatorRef, const_any_pointer_t))cb->context[1];
        if (NULL == key_retain) return a1;
        return (uintptr_t)INVOKE_CALLBACK2(key_retain, CFGetAllocator(ht), (const_any_pointer_t)a1);
    }
    case kCFBasicHashCallbackOpReleaseValue: {
        void (*value_release)(CFAllocatorRef, const_any_pointer_t) = (void (*)(CFAllocatorRef, const_any_pointer_t))cb->context[2];
        if (NULL != value_release) INVOKE_CALLBACK2(value_release, CFGetAllocator(ht), (const_any_pointer_t)a1);
        return 0;
    }
    case kCFBasicHashCallbackOpReleaseKey: {
        void (*key_release)(CFAllocatorRef, const_any_pointer_t) = (void (*)(CFAllocatorRef, const_any_pointer_t))cb->context[3];
        if (NULL != key_release) INVOKE_CALLBACK2(key_release, CFGetAllocator(ht), (const_any_pointer_t)a1);
        return 0;
    }
    case kCFBasicHashCallbackOpValueEqual: {
        Boolean (*value_equal)(const_any_pointer_t, const_any_pointer_t) = (Boolean (*)(const_any_pointer_t, const_any_pointer_t))cb->context[4];
        if (NULL == value_equal) return (a1 == a2);
        return INVOKE_CALLBACK2(value_equal, (const_any_pointer_t)a1, (const_any_pointer_t)a2) ? 1 : 0;
    }
    case kCFBasicHashCallbackOpKeyEqual: {
        Boolean (*key_equal)(const_any_pointer_t, const_any_pointer_t) = (Boolean (*)(const_any_pointer_t, const_any_pointer_t))cb->context[5];
        if (NULL == key_equal) return (a1 == a2);
        return INVOKE_CALLBACK2(key_equal, (const_any_pointer_t)a1, (const_any_pointer_t)a2) ? 1 : 0;
    }
    case kCFBasicHashCallbackOpHashKey: {
        CFHashCode (*hash)(const_any_pointer_t) = (CFHashCode (*)(const_any_pointer_t))cb->context[6];
        if (NULL == hash) return a1;
        return (uintptr_t)INVOKE_CALLBACK1(hash, (const_any_pointer_t)a1);
    }
    case kCFBasicHashCallbackOpDescribeValue: {
        CFStringRef (*value_describe)(const_any_pointer_t) = (CFStringRef (*)(const_any_pointer_t))cb->context[7];
        if (NULL == value_describe) return (uintptr_t)CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<%p>"), (const_any_pointer_t)a1);
        return (uintptr_t)INVOKE_CALLBACK1(value_describe, (const_any_pointer_t)a1);
    }
    case kCFBasicHashCallbackOpDescribeKey: {
        CFStringRef (*key_describe)(const_any_pointer_t) = (CFStringRef (*)(const_any_pointer_t))cb->context[8];
        if (NULL == key_describe) return (uintptr_t)CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<%p>"), (const_any_pointer_t)a1);
        return (uintptr_t)INVOKE_CALLBACK1(key_describe, (const_any_pointer_t)a1);
    }
    }
    return 0;
}

static CFBasicHashRef __CFBagCreateGeneric(CFAllocatorRef allocator, const CFHashKeyCallBacks *keyCallBacks, const CFHashValueCallBacks *valueCallBacks, Boolean useValueCB) {

    CFBasicHashCallbacks *cb = NULL;
    CFOptionFlags flags = kCFBasicHashLinearHashing; // kCFBasicHashExponentialHashing
    flags |= (CFDictionary ? kCFBasicHashHasKeys : 0) | (CFBag ? kCFBasicHashHasCounts : 0);

    const_any_pointer_t (*key_retain)(CFAllocatorRef, const_any_pointer_t) = NULL;
    void (*key_release)(CFAllocatorRef, const_any_pointer_t) = NULL;
    const_any_pointer_t (*value_retain)(CFAllocatorRef, const_any_pointer_t) = NULL;
    void (*value_release)(CFAllocatorRef, const_any_pointer_t) = NULL;
    Boolean std_cb = false;
    if ((NULL == keyCallBacks || (keyCallBacks && 0 == memcmp(&__kCFNullBagKeyCallBacks, keyCallBacks, sizeof(__kCFNullBagKeyCallBacks))))
                && (!useValueCB || (NULL == valueCallBacks || (valueCallBacks && 0 == memcmp(&__kCFNullBagValueCallBacks, valueCallBacks, sizeof(__kCFNullBagValueCallBacks)))))) {
        cb = (CFBasicHashCallbacks *)& CFBasicHashNullCallbacks;
    } else if ((&kCFTypeBagKeyCallBacks == keyCallBacks || (keyCallBacks && 0 == memcmp(&kCFTypeBagKeyCallBacks, keyCallBacks, sizeof(kCFTypeBagKeyCallBacks))))
                && (!useValueCB || (&kCFTypeBagValueCallBacks == valueCallBacks || (valueCallBacks && 0 == memcmp(&kCFTypeBagValueCallBacks, valueCallBacks, sizeof(kCFTypeBagValueCallBacks)))))) {
        std_cb = true;
        cb = (CFBasicHashCallbacks *)& CFBasicHashStandardCallbacks;
    } else {
        Boolean (*key_equal)(const_any_pointer_t, const_any_pointer_t) = NULL;
        Boolean (*value_equal)(const_any_pointer_t, const_any_pointer_t) = NULL;
        CFStringRef (*key_describe)(const_any_pointer_t) = NULL;
        CFStringRef (*value_describe)(const_any_pointer_t) = NULL;
        CFHashCode (*hash_key)(const_any_pointer_t) = NULL;
        key_retain = keyCallBacks ? keyCallBacks->retain : NULL;
        key_release = keyCallBacks ? keyCallBacks->release : NULL;
        key_equal = keyCallBacks ? keyCallBacks->equal : NULL;
        key_describe = keyCallBacks ? keyCallBacks->copyDescription : NULL;
        if (useValueCB) {
            value_retain = valueCallBacks ? valueCallBacks->retain : NULL;
            value_release = valueCallBacks ? valueCallBacks->release : NULL;
            value_equal = valueCallBacks ? valueCallBacks->equal : NULL;
            value_describe = valueCallBacks ? valueCallBacks->copyDescription : NULL;
        } else {
            value_retain = key_retain;
            value_release = key_release;
            value_equal = key_equal;
            value_describe = key_describe;
        }
        hash_key = keyCallBacks ? keyCallBacks->hash : NULL;
        FAULT_CALLBACK((void **)&key_retain);
        FAULT_CALLBACK((void **)&key_release);
        FAULT_CALLBACK((void **)&value_retain);
        FAULT_CALLBACK((void **)&value_release);
        FAULT_CALLBACK((void **)&key_equal);
        FAULT_CALLBACK((void **)&value_equal);
        FAULT_CALLBACK((void **)&key_describe);
        FAULT_CALLBACK((void **)&value_describe);
        FAULT_CALLBACK((void **)&hash_key);

        CFBasicHashCallbacks *newcb = NULL;
        if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
            newcb = (CFBasicHashCallbacks *)auto_zone_allocate_object(auto_zone(), 10 * sizeof(void *), AUTO_MEMORY_UNSCANNED, true, false);
        } else {
            newcb = (CFBasicHashCallbacks *)CFAllocatorAllocate(allocator, 10 * sizeof(void *), 0);
        }
        if (!newcb) HALT;
        newcb->func = (CFBasicHashCallbackType)__CFBagCallback;
        newcb->context[0] = (uintptr_t)value_retain;
        newcb->context[1] = (uintptr_t)key_retain;
        newcb->context[2] = (uintptr_t)value_release;
        newcb->context[3] = (uintptr_t)key_release;
        newcb->context[4] = (uintptr_t)value_equal;
        newcb->context[5] = (uintptr_t)key_equal;
        newcb->context[6] = (uintptr_t)hash_key;
        newcb->context[7] = (uintptr_t)value_describe;
        newcb->context[8] = (uintptr_t)key_describe;
        cb = newcb;
    }

    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        if (std_cb || value_retain != NULL || value_release != NULL) {
            flags |= kCFBasicHashStrongValues;
        }
        if (std_cb || key_retain != NULL || key_release != NULL) {
            flags |= kCFBasicHashStrongKeys;
        }
    }

    return CFBasicHashCreate(allocator, flags, cb);
}

#if CFDictionary
CFHashRef CFBagCreate(CFAllocatorRef allocator, const_any_pointer_t *klist, const_any_pointer_t *vlist, CFIndex numValues, const CFBagKeyCallBacks *keyCallBacks, const CFBagValueCallBacks *valueCallBacks) {
#endif
#if CFSet || CFBag
CFHashRef CFBagCreate(CFAllocatorRef allocator, const_any_pointer_t *klist, CFIndex numValues, const CFBagKeyCallBacks *keyCallBacks) {
    const_any_pointer_t *vlist = klist;
    const CFBagValueCallBacks *valueCallBacks = 0;
#endif
    CFTypeID typeID = CFBagGetTypeID();
    CFAssert2(0 <= numValues, __kCFLogAssertion, "%s(): numValues (%ld) cannot be less than zero", __PRETTY_FUNCTION__, numValues);
    CFBasicHashRef ht = __CFBagCreateGeneric(allocator, keyCallBacks, valueCallBacks, CFDictionary);
    if (0 < numValues) CFBasicHashSetCapacity(ht, numValues);
    for (CFIndex idx = 0; idx < numValues; idx++) {
        CFBasicHashAddValue(ht, (uintptr_t)klist[idx], (uintptr_t)vlist[idx]);
    }
    CFBasicHashMakeImmutable(ht);
    *(uintptr_t *)ht = __CFISAForTypeID(typeID);
    _CFRuntimeSetInstanceTypeID(ht, typeID);
    if (__CFOASafe) __CFSetLastAllocationEventName(ht, "CFBag (immutable)");
    return (CFHashRef)ht;
}

#if CFDictionary
CFMutableHashRef CFBagCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFBagKeyCallBacks *keyCallBacks, const CFBagValueCallBacks *valueCallBacks) {
#endif
#if CFSet || CFBag
CFMutableHashRef CFBagCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFBagKeyCallBacks *keyCallBacks) {
    const CFBagValueCallBacks *valueCallBacks = 0;
#endif
    CFTypeID typeID = CFBagGetTypeID();
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%ld) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    CFBasicHashRef ht = __CFBagCreateGeneric(allocator, keyCallBacks, valueCallBacks, CFDictionary);
    *(uintptr_t *)ht = __CFISAForTypeID(typeID);
    _CFRuntimeSetInstanceTypeID(ht, typeID);
    if (__CFOASafe) __CFSetLastAllocationEventName(ht, "CFBag (mutable)");
    return (CFMutableHashRef)ht;
}

CFHashRef CFBagCreateCopy(CFAllocatorRef allocator, CFHashRef other) {
    CFTypeID typeID = CFBagGetTypeID();
    CFAssert1(other, __kCFLogAssertion, "%s(): other CFBag cannot be NULL", __PRETTY_FUNCTION__);
    __CFGenericValidateType(other, typeID);
    CFBasicHashRef ht = NULL;
    if (CF_IS_OBJC(typeID, other)) {
        CFIndex numValues = CFBagGetCount(other);
        const_any_pointer_t vbuffer[256], kbuffer[256];
        const_any_pointer_t *vlist = (numValues <= 256) ? vbuffer : (const_any_pointer_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, numValues * sizeof(const_any_pointer_t), 0);
#if CFSet || CFBag
        const_any_pointer_t *klist = vlist;
        CFBagGetValues(other, vlist);
#endif
#if CFDictionary
        const_any_pointer_t *klist = (numValues <= 256) ? kbuffer : (const_any_pointer_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, numValues * sizeof(const_any_pointer_t), 0);
        CFDictionaryGetKeysAndValues(other, klist, vlist);
#endif
        ht = __CFBagCreateGeneric(allocator, & kCFTypeBagKeyCallBacks, CFDictionary ? & kCFTypeBagValueCallBacks : NULL, CFDictionary);
        if (0 < numValues) CFBasicHashSetCapacity(ht, numValues);
        for (CFIndex idx = 0; idx < numValues; idx++) {
            CFBasicHashAddValue(ht, (uintptr_t)klist[idx], (uintptr_t)vlist[idx]);
        }
        if (klist != kbuffer && klist != vlist) CFAllocatorDeallocate(kCFAllocatorSystemDefault, klist);
        if (vlist != vbuffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, vlist);
    } else {
        ht = CFBasicHashCreateCopy(allocator, (CFBasicHashRef)other);
    }
    CFBasicHashMakeImmutable(ht);
    *(uintptr_t *)ht = __CFISAForTypeID(typeID);
    _CFRuntimeSetInstanceTypeID(ht, typeID);
    if (__CFOASafe) __CFSetLastAllocationEventName(ht, "CFBag (immutable)");
    return (CFHashRef)ht;
}

CFMutableHashRef CFBagCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFHashRef other) {
    CFTypeID typeID = CFBagGetTypeID();
    CFAssert1(other, __kCFLogAssertion, "%s(): other CFBag cannot be NULL", __PRETTY_FUNCTION__);
    __CFGenericValidateType(other, typeID);
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%ld) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    CFBasicHashRef ht = NULL;
    if (CF_IS_OBJC(typeID, other)) {
        CFIndex numValues = CFBagGetCount(other);
        const_any_pointer_t vbuffer[256], kbuffer[256];
        const_any_pointer_t *vlist = (numValues <= 256) ? vbuffer : (const_any_pointer_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, numValues * sizeof(const_any_pointer_t), 0);
#if CFSet || CFBag
        const_any_pointer_t *klist = vlist;
        CFBagGetValues(other, vlist);
#endif
#if CFDictionary
        const_any_pointer_t *klist = (numValues <= 256) ? kbuffer : (const_any_pointer_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, numValues * sizeof(const_any_pointer_t), 0);
        CFDictionaryGetKeysAndValues(other, klist, vlist);
#endif
        ht = __CFBagCreateGeneric(allocator, & kCFTypeBagKeyCallBacks, CFDictionary ? & kCFTypeBagValueCallBacks : NULL, CFDictionary);
        if (0 < numValues) CFBasicHashSetCapacity(ht, numValues);
        for (CFIndex idx = 0; idx < numValues; idx++) {
            CFBasicHashAddValue(ht, (uintptr_t)klist[idx], (uintptr_t)vlist[idx]);
        }
        if (klist != kbuffer && klist != vlist) CFAllocatorDeallocate(kCFAllocatorSystemDefault, klist);
        if (vlist != vbuffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, vlist);
    } else {
        ht = CFBasicHashCreateCopy(allocator, (CFBasicHashRef)other);
    }
    *(uintptr_t *)ht = __CFISAForTypeID(typeID);
    _CFRuntimeSetInstanceTypeID(ht, typeID);
    if (__CFOASafe) __CFSetLastAllocationEventName(ht, "CFBag (mutable)");
    return (CFMutableHashRef)ht;
}

CFIndex CFBagGetCount(CFHashRef hc) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH0(__kCFBagTypeID, CFIndex, hc, "count");
    if (CFSet) CF_OBJC_FUNCDISPATCH0(__kCFBagTypeID, CFIndex, hc, "count");
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return CFBasicHashGetCount((CFBasicHashRef)hc);
}

#if CFDictionary
CFIndex CFBagGetCountOfKey(CFHashRef hc, const_any_pointer_t key) {
#endif
#if CFSet || CFBag
CFIndex CFBagGetCountOfValue(CFHashRef hc, const_any_pointer_t key) {
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, CFIndex, hc, "countForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, CFIndex, hc, "countForObject:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return CFBasicHashGetCountOfKey((CFBasicHashRef)hc, (uintptr_t)key);
}

#if CFDictionary
Boolean CFBagContainsKey(CFHashRef hc, const_any_pointer_t key) {
#endif
#if CFSet || CFBag
Boolean CFBagContainsValue(CFHashRef hc, const_any_pointer_t key) {
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, char, hc, "containsKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, char, hc, "containsObject:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return (0 < CFBasicHashGetCountOfKey((CFBasicHashRef)hc, (uintptr_t)key));
}

const_any_pointer_t CFBagGetValue(CFHashRef hc, const_any_pointer_t key) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, const_any_pointer_t, hc, "objectForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, const_any_pointer_t, hc, "member:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFBasicHashBucket bkt = CFBasicHashFindBucket((CFBasicHashRef)hc, (uintptr_t)key);
    return (0 < bkt.count ? (const_any_pointer_t)bkt.weak_value : 0);
}

Boolean CFBagGetValueIfPresent(CFHashRef hc, const_any_pointer_t key, const_any_pointer_t *value) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, Boolean, hc, "_getValue:forKey:", (any_t *)value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, Boolean, hc, "_getValue:forObj:", (any_t *)value, key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFBasicHashBucket bkt = CFBasicHashFindBucket((CFBasicHashRef)hc, (uintptr_t)key);
    if (0 < bkt.count) {
        if (value) {
            if (kCFUseCollectableAllocator && (CFBasicHashGetFlags((CFBasicHashRef)hc) & kCFBasicHashStrongValues)) {
                __CFAssignWithWriteBarrier((void **)value, (void *)bkt.weak_value);
            } else {
                *value = (const_any_pointer_t)bkt.weak_value;
            }
        }
        return true;
    }
    return false;
}

#if CFDictionary
CFIndex CFDictionaryGetCountOfValue(CFHashRef hc, const_any_pointer_t value) {
    CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, CFIndex, hc, "countForObject:", value);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return CFBasicHashGetCountOfValue((CFBasicHashRef)hc, (uintptr_t)value);
}

Boolean CFDictionaryContainsValue(CFHashRef hc, const_any_pointer_t value) {
    CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, char, hc, "containsObject:", value);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return (0 < CFBasicHashGetCountOfValue((CFBasicHashRef)hc, (uintptr_t)value));
}

CF_EXPORT Boolean CFDictionaryGetKeyIfPresent(CFHashRef hc, const_any_pointer_t key, const_any_pointer_t *actualkey) {
    CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, Boolean, hc, "getActualKey:forKey:", actualkey, key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFBasicHashBucket bkt = CFBasicHashFindBucket((CFBasicHashRef)hc, (uintptr_t)key);
    if (0 < bkt.count) {
        if (actualkey) {
            if (kCFUseCollectableAllocator && (CFBasicHashGetFlags((CFBasicHashRef)hc) & kCFBasicHashStrongKeys)) {
                __CFAssignWithWriteBarrier((void **)actualkey, (void *)bkt.weak_key);
            } else {
                *actualkey = (const_any_pointer_t)bkt.weak_key;
            }
        }
        return true;
    }
    return false;
}
#endif

#if CFDictionary
void CFBagGetKeysAndValues(CFHashRef hc, const_any_pointer_t *keybuf, const_any_pointer_t *valuebuf) {
#endif
#if CFSet || CFBag
void CFBagGetValues(CFHashRef hc, const_any_pointer_t *keybuf) {
    const_any_pointer_t *valuebuf = 0;
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, void, hc, "getObjects:andKeys:", (any_t *)valuebuf, (any_t *)keybuf);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, void, hc, "getObjects:", (any_t *)keybuf);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    if (kCFUseCollectableAllocator) {
        CFOptionFlags flags = CFBasicHashGetFlags((CFBasicHashRef)hc);
        __block const_any_pointer_t *keys = keybuf, *values = valuebuf;
        CFBasicHashApply((CFBasicHashRef)hc, ^(CFBasicHashBucket bkt) {
                for (CFIndex cnt = bkt.count; cnt--;) {
                    if (keybuf && (flags & kCFBasicHashStrongKeys)) { __CFAssignWithWriteBarrier((void **)keys, (void *)bkt.weak_key); keys++; }
                    if (keybuf && !(flags & kCFBasicHashStrongKeys)) { *keys++ = (const_any_pointer_t)bkt.weak_key; }
                    if (valuebuf && (flags & kCFBasicHashStrongValues)) { __CFAssignWithWriteBarrier((void **)values, (void *)bkt.weak_value); values++; }
                    if (valuebuf && !(flags & kCFBasicHashStrongValues)) { *values++ = (const_any_pointer_t)bkt.weak_value; }
                }
                return (Boolean)true;
            });
    } else {
        CFBasicHashGetElements((CFBasicHashRef)hc, CFBagGetCount(hc), (uintptr_t *)valuebuf, NULL, (uintptr_t *)keybuf, NULL);
    }
}

void CFBagApplyFunction(CFHashRef hc, CFBagApplierFunction applier, any_pointer_t context) {
    FAULT_CALLBACK((void **)&(applier));
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, void, hc, "_apply:context:", applier, context);
    if (CFSet) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, void, hc, "_applyValues:context:", applier, context);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFBasicHashApply((CFBasicHashRef)hc, ^(CFBasicHashBucket bkt) {
#if CFDictionary
            INVOKE_CALLBACK3(applier, (const_any_pointer_t)bkt.weak_key, (const_any_pointer_t)bkt.weak_value, context);
#endif
#if CFSet
            INVOKE_CALLBACK2(applier, (const_any_pointer_t)bkt.weak_value, context);
#endif
#if CFBag
            for (CFIndex cnt = bkt.count; cnt--;) {
                INVOKE_CALLBACK2(applier, (const_any_pointer_t)bkt.weak_value, context);
            }
#endif
            return (Boolean)true;
        });
}

// This function is for Foundation's benefit; no one else should use it.
CF_EXPORT unsigned long _CFBagFastEnumeration(CFHashRef hc, struct __objcFastEnumerationStateEquivalent *state, void *stackbuffer, unsigned long count) {
    if (CF_IS_OBJC(__kCFBagTypeID, hc)) return 0;
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return __CFBasicHashFastEnumeration((CFBasicHashRef)hc, (struct __objcFastEnumerationStateEquivalent2 *)state, stackbuffer, count);
}

// This function is for Foundation's benefit; no one else should use it.
CF_EXPORT Boolean _CFBagIsMutable(CFHashRef hc) {
    if (CF_IS_OBJC(__kCFBagTypeID, hc)) return false;
    __CFGenericValidateType(hc, __kCFBagTypeID);
    return CFBasicHashIsMutable((CFBasicHashRef)hc);
}

// This function is for Foundation's benefit; no one else should use it.
CF_EXPORT void _CFBagSetCapacity(CFMutableHashRef hc, CFIndex cap) {
    if (CF_IS_OBJC(__kCFBagTypeID, hc)) return;
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFAssert2(CFBasicHashIsMutable((CFBasicHashRef)hc), __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
    CFAssert3(CFBagGetCount(hc) <= cap, __kCFLogAssertion, "%s(): desired capacity (%ld) is less than count (%ld)", __PRETTY_FUNCTION__, cap, CFBagGetCount(hc));
    CFBasicHashSetCapacity((CFBasicHashRef)hc, cap);
}

CF_INLINE CFIndex __CFBagGetKVOBit(CFHashRef hc) {
    return __CFBitfieldGetValue(((CFRuntimeBase *)hc)->_cfinfo[CF_INFO_BITS], 0, 0);
}

CF_INLINE void __CFBagSetKVOBit(CFHashRef hc, CFIndex bit) {
    __CFBitfieldSetValue(((CFRuntimeBase *)hc)->_cfinfo[CF_INFO_BITS], 0, 0, ((uintptr_t)bit & 0x1));
}

// This function is for Foundation's benefit; no one else should use it.
CF_EXPORT CFIndex _CFBagGetKVOBit(CFHashRef hc) {
    return __CFBagGetKVOBit(hc);
}

// This function is for Foundation's benefit; no one else should use it.
CF_EXPORT void _CFBagSetKVOBit(CFHashRef hc, CFIndex bit) {
    __CFBagSetKVOBit(hc, bit);
}


#if !defined(CF_OBJC_KVO_WILLCHANGE)
#define CF_OBJC_KVO_WILLCHANGE(obj, key)
#define CF_OBJC_KVO_DIDCHANGE(obj, key)
#define CF_OBJC_KVO_WILLCHANGEALL(obj)
#define CF_OBJC_KVO_DIDCHANGEALL(obj)
#endif

#if CFDictionary
void CFBagAddValue(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void CFBagAddValue(CFMutableHashRef hc, const_any_pointer_t key) {
    const_any_pointer_t value = key;
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, void, hc, "addObject:forKey:", value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, void, hc, "addObject:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFAssert2(CFBasicHashIsMutable((CFBasicHashRef)hc), __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
    if (!CFBasicHashIsMutable((CFBasicHashRef)hc)) {
        CFLog(3, CFSTR("%s(): immutable collection %p given to mutating function"), __PRETTY_FUNCTION__, hc);
    }
    CF_OBJC_KVO_WILLCHANGE(hc, key);
    CFBasicHashAddValue((CFBasicHashRef)hc, (uintptr_t)key, (uintptr_t)value);
    CF_OBJC_KVO_DIDCHANGE(hc, key);
}

#if CFDictionary
void CFBagReplaceValue(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void CFBagReplaceValue(CFMutableHashRef hc, const_any_pointer_t key) {
    const_any_pointer_t value = key;
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, void, hc, "replaceObject:forKey:", value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, void, hc, "replaceObject:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFAssert2(CFBasicHashIsMutable((CFBasicHashRef)hc), __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
    if (!CFBasicHashIsMutable((CFBasicHashRef)hc)) {
        CFLog(3, CFSTR("%s(): immutable collection %p given to mutating function"), __PRETTY_FUNCTION__, hc);
    }
    CF_OBJC_KVO_WILLCHANGE(hc, key);
    CFBasicHashReplaceValue((CFBasicHashRef)hc, (uintptr_t)key, (uintptr_t)value);
    CF_OBJC_KVO_DIDCHANGE(hc, key);
}

#if CFDictionary
void CFBagSetValue(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void CFBagSetValue(CFMutableHashRef hc, const_any_pointer_t key) {
    const_any_pointer_t value = key;
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH2(__kCFBagTypeID, void, hc, "setObject:forKey:", value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, void, hc, "_setObject:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFAssert2(CFBasicHashIsMutable((CFBasicHashRef)hc), __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
    if (!CFBasicHashIsMutable((CFBasicHashRef)hc)) {
        CFLog(3, CFSTR("%s(): immutable collection %p given to mutating function"), __PRETTY_FUNCTION__, hc);
    }
    CF_OBJC_KVO_WILLCHANGE(hc, key);
//#warning this for a dictionary used to not replace the key
    CFBasicHashSetValue((CFBasicHashRef)hc, (uintptr_t)key, (uintptr_t)value);
    CF_OBJC_KVO_DIDCHANGE(hc, key);
}

void CFBagRemoveValue(CFMutableHashRef hc, const_any_pointer_t key) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, void, hc, "removeObjectForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH1(__kCFBagTypeID, void, hc, "removeObject:", key);
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFAssert2(CFBasicHashIsMutable((CFBasicHashRef)hc), __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
    if (!CFBasicHashIsMutable((CFBasicHashRef)hc)) {
        CFLog(3, CFSTR("%s(): immutable collection %p given to mutating function"), __PRETTY_FUNCTION__, hc);
    }
    CF_OBJC_KVO_WILLCHANGE(hc, key);
    CFBasicHashRemoveValue((CFBasicHashRef)hc, (uintptr_t)key);
    CF_OBJC_KVO_DIDCHANGE(hc, key);
}

void CFBagRemoveAllValues(CFMutableHashRef hc) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH0(__kCFBagTypeID, void, hc, "removeAllObjects");
    if (CFSet) CF_OBJC_FUNCDISPATCH0(__kCFBagTypeID, void, hc, "removeAllObjects");
    __CFGenericValidateType(hc, __kCFBagTypeID);
    CFAssert2(CFBasicHashIsMutable((CFBasicHashRef)hc), __kCFLogAssertion, "%s(): immutable collection %p passed to mutating operation", __PRETTY_FUNCTION__, hc);
    if (!CFBasicHashIsMutable((CFBasicHashRef)hc)) {
        CFLog(3, CFSTR("%s(): immutable collection %p given to mutating function"), __PRETTY_FUNCTION__, hc);
    }
    CF_OBJC_KVO_WILLCHANGEALL(hc);
    CFBasicHashRemoveAllValues((CFBasicHashRef)hc);
    CF_OBJC_KVO_DIDCHANGEALL(hc);
}

#undef CF_OBJC_KVO_WILLCHANGE
#undef CF_OBJC_KVO_DIDCHANGE
#undef CF_OBJC_KVO_WILLCHANGEALL
#undef CF_OBJC_KVO_DIDCHANGEALL

