/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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

/*	CFBasicHash.h
	Copyright (c) 2008-2009, Apple Inc. All rights reserved.
*/

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>
#include "CFInternal.h"

CF_EXTERN_C_BEGIN

struct __objcFastEnumerationStateEquivalent2 {
    unsigned long state;
    unsigned long *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
};

enum {
    __kCFBasicHashLinearHashingValue = 1,
    __kCFBasicHashDoubleHashingValue = 2,
    __kCFBasicHashExponentialHashingValue = 3,
};

enum {
    kCFBasicHashHasValues2 = (1UL << 2),
    kCFBasicHashHasKeys = (1UL << 3),
    kCFBasicHashHasKeys2 = (1UL << 4),
    kCFBasicHashHasCounts = (1UL << 5),
    kCFBasicHashHasOrder = (1UL << 6),
    kCFBasicHashHasHashCache = (1UL << 7),

    kCFBasicHashStrongValues = (1UL << 9),
    kCFBasicHashStrongValues2 = (1UL << 10),
    kCFBasicHashStrongKeys = (1UL << 11),
    kCFBasicHashStrongKeys2 = (1UL << 12),

    kCFBasicHashLinearHashing = (__kCFBasicHashLinearHashingValue << 13), // bits 13-14
    kCFBasicHashDoubleHashing = (__kCFBasicHashDoubleHashingValue << 13),
    kCFBasicHashExponentialHashing = (__kCFBasicHashExponentialHashingValue << 13),

    kCFBasicHashAggressiveGrowth = (1UL << 15),
};

// Note that for a hash table without keys, the value is treated as the key,
// and the value should be passed in as the key for operations which take a key.

typedef struct {
    CFIndex idx;
    uintptr_t weak_key;
    uintptr_t weak_key2;
    uintptr_t weak_value;
    uintptr_t weak_value2;
    uintptr_t count;
    uintptr_t order;
} CFBasicHashBucket;

typedef struct __CFBasicHash *CFBasicHashRef;

// Bit 6 in the CF_INFO_BITS of the CFRuntimeBase inside the CFBasicHashRef is the "is immutable" bit
CF_INLINE Boolean CFBasicHashIsMutable(CFBasicHashRef ht) {
    return __CFBitfieldGetValue(((CFRuntimeBase *)ht)->_cfinfo[CF_INFO_BITS], 6, 6) ? false : true;
}
CF_INLINE void CFBasicHashMakeImmutable(CFBasicHashRef ht) {
    __CFBitfieldSetValue(((CFRuntimeBase *)ht)->_cfinfo[CF_INFO_BITS], 6, 6, 1);
}


typedef struct __CFBasicHashCallbacks CFBasicHashCallbacks;

typedef uintptr_t (*CFBasicHashCallbackType)(CFBasicHashRef ht, uint8_t op, uintptr_t a1, uintptr_t a2, const CFBasicHashCallbacks *cb);

enum {
    kCFBasicHashCallbackOpCopyCallbacks = 8,	// Return new value; REQUIRED
    kCFBasicHashCallbackOpFreeCallbacks = 9,	// Return 0; REQUIRED

    kCFBasicHashCallbackOpRetainValue = 10,	// Return first arg or new value; REQUIRED
    kCFBasicHashCallbackOpRetainValue2 = 11,	// Return first arg or new value
    kCFBasicHashCallbackOpRetainKey = 12,	// Return first arg or new key; REQUIRED
    kCFBasicHashCallbackOpRetainKey2 = 13,	// Return first arg or new key
    kCFBasicHashCallbackOpReleaseValue = 14,	// Return 0; REQUIRED
    kCFBasicHashCallbackOpReleaseValue2 = 15,	// Return 0
    kCFBasicHashCallbackOpReleaseKey = 16,	// Return 0; REQUIRED
    kCFBasicHashCallbackOpReleaseKey2 = 17,	// Return 0
    kCFBasicHashCallbackOpValueEqual = 18,	// Return 0 or 1; REQUIRED
    kCFBasicHashCallbackOpValue2Equal = 19,	// Return 0 or 1
    kCFBasicHashCallbackOpKeyEqual = 20,	// Return 0 or 1; REQUIRED
    kCFBasicHashCallbackOpKey2Equal = 21,	// Return 0 or 1
    kCFBasicHashCallbackOpHashKey = 22,		// Return hash code; REQUIRED
    kCFBasicHashCallbackOpHashKey2 = 23,	// Return hash code
    kCFBasicHashCallbackOpDescribeValue = 24,	// Return retained CFStringRef; REQUIRED
    kCFBasicHashCallbackOpDescribeValue2 = 25,	// Return retained CFStringRef
    kCFBasicHashCallbackOpDescribeKey = 26,	// Return retained CFStringRef; REQUIRED
    kCFBasicHashCallbackOpDescribeKey2 = 27,	// Return retained CFStringRef
};

struct __CFBasicHashCallbacks {
    CFBasicHashCallbackType func; // must not be NULL
    uintptr_t context[0]; // variable size; any pointers in here must remain valid as long as the CFBasicHash
};

extern const CFBasicHashCallbacks CFBasicHashNullCallbacks;
extern const CFBasicHashCallbacks CFBasicHashStandardCallbacks;


CFOptionFlags CFBasicHashGetFlags(CFBasicHashRef ht);
CFIndex CFBasicHashGetNumBuckets(CFBasicHashRef ht);
CFIndex CFBasicHashGetCapacity(CFBasicHashRef ht);
void CFBasicHashSetCapacity(CFBasicHashRef ht, CFIndex capacity);

CFIndex CFBasicHashGetCount(CFBasicHashRef ht);
CFBasicHashBucket CFBasicHashGetBucket(CFBasicHashRef ht, CFIndex idx);
CFBasicHashBucket CFBasicHashFindBucket(CFBasicHashRef ht, uintptr_t stack_key);
CFIndex CFBasicHashGetCountOfKey(CFBasicHashRef ht, uintptr_t stack_key);
CFIndex CFBasicHashGetCountOfValue(CFBasicHashRef ht, uintptr_t stack_value);
Boolean CFBasicHashesAreEqual(CFBasicHashRef ht1, CFBasicHashRef ht2);
void CFBasicHashApply(CFBasicHashRef ht, Boolean (^block)(CFBasicHashBucket));
void CFBasicHashGetElements(CFBasicHashRef ht, CFIndex bufferslen, uintptr_t *weak_values, uintptr_t *weak_values2, uintptr_t *weak_keys, uintptr_t *weak_keys2);

void CFBasicHashAddValue(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t stack_value);
void CFBasicHashReplaceValue(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t stack_value);
void CFBasicHashSetValue(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t stack_value);
CFIndex CFBasicHashRemoveValue(CFBasicHashRef ht, uintptr_t stack_key);
void CFBasicHashRemoveAllValues(CFBasicHashRef ht);

size_t CFBasicHashGetSize(CFBasicHashRef ht, Boolean total);

CFStringRef CFBasicHashCopyDescription(CFBasicHashRef ht, Boolean detailed, CFStringRef linePrefix, CFStringRef entryLinePrefix, Boolean describeElements);

CFTypeID CFBasicHashGetTypeID(void);

extern Boolean __CFBasicHashEqual(CFTypeRef cf1, CFTypeRef cf2);
extern CFHashCode __CFBasicHashHash(CFTypeRef cf);
extern CFStringRef __CFBasicHashCopyDescription(CFTypeRef cf);
extern void __CFBasicHashDeallocate(CFTypeRef cf);
extern unsigned long __CFBasicHashFastEnumeration(CFBasicHashRef ht, struct __objcFastEnumerationStateEquivalent2 *state, void *stackbuffer, unsigned long count);

// creation functions create mutable CFBasicHashRefs
CFBasicHashRef CFBasicHashCreate(CFAllocatorRef allocator, CFOptionFlags flags, const CFBasicHashCallbacks *cb);
CFBasicHashRef CFBasicHashCreateCopy(CFAllocatorRef allocator, CFBasicHashRef ht);


CF_EXTERN_C_END

