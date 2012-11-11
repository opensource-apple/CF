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

/*	CFBasicHash.m
	Copyright (c) 2008-2009, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#import "CFBasicHash.h"
#import <CoreFoundation/CFRuntime.h>
#import <CoreFoundation/CFSet.h>
#import <Block.h>
#import <objc/objc.h>
#import <math.h>

#if DEPLOYMENT_TARGET_WINDOWS
#define __SetLastAllocationEventName(A, B) do { } while (0)
#else
#define __SetLastAllocationEventName(A, B) do { if (__CFOASafe && (A)) __CFSetLastAllocationEventName(A, B); } while (0)
#endif

#define GCRETAIN(A, B) kCFTypeSetCallBacks.retain(A, B)
#define GCRELEASE(A, B) kCFTypeSetCallBacks.release(A, B)

#define __AssignWithWriteBarrier(location, value) objc_assign_strongCast((id)value, (id *)location)

#define ENABLE_DTRACE_PROBES 0
#define ENABLE_MEMORY_COUNTERS 0


/*
// dtrace -h -s foo.d
// Note: output then changed by putting do/while around macro bodies and adding a cast of the arguments

provider Cocoa_HashTable {
        probe hash_key(unsigned long table, unsigned long key, unsigned long hash);
        probe test_equal(unsigned long table, unsigned long key1, unsigned long key2);
        probe probing_start(unsigned long table, unsigned long num_buckets);
        probe probe_empty(unsigned long table, unsigned long idx);
        probe probe_deleted(unsigned long table, unsigned long idx);
        probe probe_valid(unsigned long table, unsigned long idx);
        probe probing_end(unsigned long table, unsigned long num_probes);
        probe rehash_start(unsigned long table, unsigned long num_buckets, unsigned long total_size);
        probe rehash_end(unsigned long table, unsigned long num_buckets, unsigned long total_size);
};

#pragma D attributes Unstable/Unstable/Common provider Cocoa_HashTable provider
#pragma D attributes Private/Private/Unknown provider Cocoa_HashTable module
#pragma D attributes Private/Private/Unknown provider Cocoa_HashTable function
#pragma D attributes Unstable/Unstable/Common provider Cocoa_HashTable name
#pragma D attributes Unstable/Unstable/Common provider Cocoa_HashTable args
*/

#if ENABLE_DTRACE_PROBES

#define COCOA_HASHTABLE_STABILITY "___dtrace_stability$Cocoa_HashTable$v1$4_4_5_1_1_0_1_1_0_4_4_5_4_4_5"

#define COCOA_HASHTABLE_TYPEDEFS "___dtrace_typedefs$Cocoa_HashTable$v2"

#define COCOA_HASHTABLE_REHASH_END(arg0, arg1, arg2) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$rehash_end$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1), (unsigned long)(arg2)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_REHASH_END_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$rehash_end$v1()
#define COCOA_HASHTABLE_REHASH_START(arg0, arg1, arg2) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$rehash_start$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1), (unsigned long)(arg2)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_REHASH_START_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$rehash_start$v1()
#define COCOA_HASHTABLE_HASH_KEY(arg0, arg1, arg2) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$hash_key$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1), (unsigned long)(arg2)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_HASH_KEY_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$hash_key$v1()
#define COCOA_HASHTABLE_PROBE_DELETED(arg0, arg1) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$probe_deleted$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_PROBE_DELETED_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$probe_deleted$v1()
#define COCOA_HASHTABLE_PROBE_EMPTY(arg0, arg1) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$probe_empty$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_PROBE_EMPTY_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$probe_empty$v1()
#define COCOA_HASHTABLE_PROBE_VALID(arg0, arg1) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$probe_valid$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_PROBE_VALID_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$probe_valid$v1()
#define COCOA_HASHTABLE_PROBING_END(arg0, arg1) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$probing_end$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_PROBING_END_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$probing_end$v1()
#define COCOA_HASHTABLE_PROBING_START(arg0, arg1) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$probing_start$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_PROBING_START_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$probing_start$v1()
#define COCOA_HASHTABLE_TEST_EQUAL(arg0, arg1, arg2) \
do { \
        __asm__ volatile(".reference " COCOA_HASHTABLE_TYPEDEFS); \
        __dtrace_probe$Cocoa_HashTable$test_equal$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67((unsigned long)(arg0), (unsigned long)(arg1), (unsigned long)(arg2)); \
        __asm__ volatile(".reference " COCOA_HASHTABLE_STABILITY); \
} while (0)
#define COCOA_HASHTABLE_TEST_EQUAL_ENABLED() \
        __dtrace_isenabled$Cocoa_HashTable$test_equal$v1()

extern void __dtrace_probe$Cocoa_HashTable$rehash_end$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$rehash_end$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$rehash_start$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$rehash_start$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$hash_key$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$hash_key$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$probe_deleted$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$probe_deleted$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$probe_empty$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$probe_empty$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$probe_valid$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$probe_valid$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$probing_end$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$probing_end$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$probing_start$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$probing_start$v1(void);
extern void __dtrace_probe$Cocoa_HashTable$test_equal$v1$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67$756e7369676e6564206c6f6e67(unsigned long, unsigned long, unsigned long);
extern int __dtrace_isenabled$Cocoa_HashTable$test_equal$v1(void);

#else

#define COCOA_HASHTABLE_REHASH_END(arg0, arg1, arg2) do {} while (0)
#define COCOA_HASHTABLE_REHASH_END_ENABLED() 0
#define COCOA_HASHTABLE_REHASH_START(arg0, arg1, arg2) do {} while (0)
#define COCOA_HASHTABLE_REHASH_START_ENABLED() 0
#define COCOA_HASHTABLE_HASH_KEY(arg0, arg1, arg2) do {} while (0)
#define COCOA_HASHTABLE_HASH_KEY_ENABLED() 0
#define COCOA_HASHTABLE_PROBE_DELETED(arg0, arg1) do {} while (0)
#define COCOA_HASHTABLE_PROBE_DELETED_ENABLED() 0
#define COCOA_HASHTABLE_PROBE_EMPTY(arg0, arg1) do {} while (0)
#define COCOA_HASHTABLE_PROBE_EMPTY_ENABLED() 0
#define COCOA_HASHTABLE_PROBE_VALID(arg0, arg1) do {} while (0)
#define COCOA_HASHTABLE_PROBE_VALID_ENABLED() 0
#define COCOA_HASHTABLE_PROBING_END(arg0, arg1) do {} while (0)
#define COCOA_HASHTABLE_PROBING_END_ENABLED() 0
#define COCOA_HASHTABLE_PROBING_START(arg0, arg1) do {} while (0)
#define COCOA_HASHTABLE_PROBING_START_ENABLED() 0
#define COCOA_HASHTABLE_TEST_EQUAL(arg0, arg1, arg2) do {} while (0)
#define COCOA_HASHTABLE_TEST_EQUAL_ENABLED() 0

#endif


#if !defined(__LP64__)
#define __LP64__ 0
#endif

enum {
#if __LP64__
    __CFBasicHashMarkerShift = 40 // 64 - 24
#else
    __CFBasicHashMarkerShift = 8  // 32 - 24
#endif
};

typedef union {
    uintptr_t weak;
    id strong;
} CFBasicHashValue;

struct __CFBasicHash {
    CFRuntimeBase base;
    struct { // 128 bits
        uint64_t hash_style:2;
        uint64_t values2_offset:1;
        uint64_t keys_offset:2;
        uint64_t keys2_offset:2;
        uint64_t counts_offset:3;
        uint64_t orders_offset:3;
        uint64_t hashes_offset:3;
        uint64_t num_buckets_idx:6; /* index to number of buckets */
        uint64_t used_buckets:42;   /* number of used buckets */
        uint64_t __0:2;
        uint64_t finalized:1;
        uint64_t fast_grow:1;
        uint64_t strong_values:1;
        uint64_t strong_values2:1;
        uint64_t strong_keys:1;
        uint64_t strong_keys2:1;
        uint64_t marker:24;
        uint64_t deleted:16;
        uint64_t mutations:16;
    } bits;
    __strong const CFBasicHashCallbacks *callbacks;
    void *pointers[1];
};

CF_INLINE CFBasicHashValue *__CFBasicHashGetValues(CFBasicHashRef ht) {
    return (CFBasicHashValue *)ht->pointers[0];
}

CF_INLINE void __CFBasicHashSetValues(CFBasicHashRef ht, CFBasicHashValue *ptr) {
    __AssignWithWriteBarrier(&ht->pointers[0], ptr);
}

CF_INLINE CFBasicHashValue *__CFBasicHashGetValues2(CFBasicHashRef ht) {
    if (0 == ht->bits.values2_offset) HALT;
    return (CFBasicHashValue *)ht->pointers[ht->bits.values2_offset];
}

CF_INLINE void __CFBasicHashSetValues2(CFBasicHashRef ht, CFBasicHashValue *ptr) {
    if (0 == ht->bits.values2_offset) HALT;
    __AssignWithWriteBarrier(&ht->pointers[ht->bits.values2_offset], ptr);
}

CF_INLINE CFBasicHashValue *__CFBasicHashGetKeys(CFBasicHashRef ht) {
    if (0 == ht->bits.keys_offset) HALT;
    return (CFBasicHashValue *)ht->pointers[ht->bits.keys_offset];
}

CF_INLINE void __CFBasicHashSetKeys(CFBasicHashRef ht, CFBasicHashValue *ptr) {
    if (0 == ht->bits.keys_offset) HALT;
    __AssignWithWriteBarrier(&ht->pointers[ht->bits.keys_offset], ptr);
}

CF_INLINE CFBasicHashValue *__CFBasicHashGetKeys2(CFBasicHashRef ht) {
    if (0 == ht->bits.keys2_offset) HALT;
    return (CFBasicHashValue *)ht->pointers[ht->bits.keys2_offset];
}

CF_INLINE void __CFBasicHashSetKeys2(CFBasicHashRef ht, CFBasicHashValue *ptr) {
    if (0 == ht->bits.keys2_offset) HALT;
    __AssignWithWriteBarrier(&ht->pointers[ht->bits.keys2_offset], ptr);
}

CF_INLINE uintptr_t *__CFBasicHashGetCounts(CFBasicHashRef ht) {
    if (0 == ht->bits.counts_offset) HALT;
    return (uintptr_t *)ht->pointers[ht->bits.counts_offset];
}

CF_INLINE void __CFBasicHashSetCounts(CFBasicHashRef ht, uintptr_t *ptr) {
    if (0 == ht->bits.counts_offset) HALT;
    __AssignWithWriteBarrier(&ht->pointers[ht->bits.counts_offset], ptr);
}

CF_INLINE uintptr_t *__CFBasicHashGetOrders(CFBasicHashRef ht) {
    if (0 == ht->bits.orders_offset) HALT;
    return (uintptr_t *)ht->pointers[ht->bits.orders_offset];
}

CF_INLINE void __CFBasicHashSetOrders(CFBasicHashRef ht, uintptr_t *ptr) {
    if (0 == ht->bits.orders_offset) HALT;
    __AssignWithWriteBarrier(&ht->pointers[ht->bits.orders_offset], ptr);
}

CF_INLINE uintptr_t *__CFBasicHashGetHashes(CFBasicHashRef ht) {
    if (0 == ht->bits.hashes_offset) HALT;
    return (uintptr_t *)ht->pointers[ht->bits.hashes_offset];
}

CF_INLINE void __CFBasicHashSetHashes(CFBasicHashRef ht, uintptr_t *ptr) {
    if (0 == ht->bits.hashes_offset) HALT;
    __AssignWithWriteBarrier(&ht->pointers[ht->bits.hashes_offset], ptr);
}

static uintptr_t __CFBasicHashNullCallback(CFBasicHashRef ht, uint8_t op, uintptr_t a1, uintptr_t a2, const CFBasicHashCallbacks *cb) {
    switch (op) {
    case kCFBasicHashCallbackOpCopyCallbacks: return (uintptr_t)&CFBasicHashNullCallbacks;
    case kCFBasicHashCallbackOpFreeCallbacks: return 0;
    case kCFBasicHashCallbackOpRetainValue:
    case kCFBasicHashCallbackOpRetainValue2:
    case kCFBasicHashCallbackOpRetainKey:
    case kCFBasicHashCallbackOpRetainKey2:   return a1;
    case kCFBasicHashCallbackOpReleaseValue:
    case kCFBasicHashCallbackOpReleaseValue2:
    case kCFBasicHashCallbackOpReleaseKey:
    case kCFBasicHashCallbackOpReleaseKey2:  return 0;
    case kCFBasicHashCallbackOpValueEqual:
    case kCFBasicHashCallbackOpValue2Equal:
    case kCFBasicHashCallbackOpKeyEqual:
    case kCFBasicHashCallbackOpKey2Equal:    return a1 == a2;
    case kCFBasicHashCallbackOpHashKey:
    case kCFBasicHashCallbackOpHashKey2:     return a1;
    case kCFBasicHashCallbackOpDescribeValue:
    case kCFBasicHashCallbackOpDescribeValue2:
    case kCFBasicHashCallbackOpDescribeKey:
    case kCFBasicHashCallbackOpDescribeKey2: return (uintptr_t)CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<%p>"), (void *)a1);
    }
    return 0;
}

static uintptr_t __CFBasicHashStandardCallback(CFBasicHashRef ht, uint8_t op, uintptr_t a1, uintptr_t a2, const CFBasicHashCallbacks *cb) {
    switch (op) {
    case kCFBasicHashCallbackOpCopyCallbacks: return (uintptr_t)&CFBasicHashStandardCallbacks;
    case kCFBasicHashCallbackOpFreeCallbacks: return 0;
    case kCFBasicHashCallbackOpRetainValue:   return (ht->bits.strong_values) ? (uintptr_t)GCRETAIN(kCFAllocatorSystemDefault, (CFTypeRef)a1) : (uintptr_t)CFRetain((CFTypeRef)a1);
    case kCFBasicHashCallbackOpRetainValue2:  return (ht->bits.strong_values2) ? (uintptr_t)GCRETAIN(kCFAllocatorSystemDefault, (CFTypeRef)a1) : (uintptr_t)CFRetain((CFTypeRef)a1);
    case kCFBasicHashCallbackOpRetainKey:     return (ht->bits.strong_keys) ? (uintptr_t)GCRETAIN(kCFAllocatorSystemDefault, (CFTypeRef)a1) : (uintptr_t)CFRetain((CFTypeRef)a1);
    case kCFBasicHashCallbackOpRetainKey2:    return (ht->bits.strong_keys2) ? (uintptr_t)GCRETAIN(kCFAllocatorSystemDefault, (CFTypeRef)a1) : (uintptr_t)CFRetain((CFTypeRef)a1);
    case kCFBasicHashCallbackOpReleaseValue:  if (ht->bits.strong_values) GCRELEASE(kCFAllocatorSystemDefault, (CFTypeRef)a1); else CFRelease((CFTypeRef)a1); return 0;
    case kCFBasicHashCallbackOpReleaseValue2: if (ht->bits.strong_values2) GCRELEASE(kCFAllocatorSystemDefault, (CFTypeRef)a1); else CFRelease((CFTypeRef)a1); return 0;
    case kCFBasicHashCallbackOpReleaseKey:    if (ht->bits.strong_keys) GCRELEASE(kCFAllocatorSystemDefault, (CFTypeRef)a1); else CFRelease((CFTypeRef)a1); return 0;
    case kCFBasicHashCallbackOpReleaseKey2:   if (ht->bits.strong_keys2) GCRELEASE(kCFAllocatorSystemDefault, (CFTypeRef)a1); else CFRelease((CFTypeRef)a1); return 0;
    case kCFBasicHashCallbackOpValueEqual:
    case kCFBasicHashCallbackOpValue2Equal:
    case kCFBasicHashCallbackOpKeyEqual:
    case kCFBasicHashCallbackOpKey2Equal:     return CFEqual((CFTypeRef)a1, (CFTypeRef)a2);
    case kCFBasicHashCallbackOpHashKey:
    case kCFBasicHashCallbackOpHashKey2:      return (uintptr_t)CFHash((CFTypeRef)a1);
    case kCFBasicHashCallbackOpDescribeValue:
    case kCFBasicHashCallbackOpDescribeValue2:
    case kCFBasicHashCallbackOpDescribeKey:
    case kCFBasicHashCallbackOpDescribeKey2:  return (uintptr_t)CFCopyDescription((CFTypeRef)a1);
    }
    return 0;
}

__private_extern__ const CFBasicHashCallbacks CFBasicHashNullCallbacks = {__CFBasicHashNullCallback};
__private_extern__ const CFBasicHashCallbacks CFBasicHashStandardCallbacks = {__CFBasicHashStandardCallback};

CF_INLINE uintptr_t __CFBasicHashImportValue(CFBasicHashRef ht, uintptr_t stack_value) {
    return ht->callbacks->func(ht, kCFBasicHashCallbackOpRetainValue, stack_value, 0, ht->callbacks);
}

CF_INLINE uintptr_t __CFBasicHashImportValue2(CFBasicHashRef ht, uintptr_t stack_value) {
    return ht->callbacks->func(ht, kCFBasicHashCallbackOpRetainValue2, stack_value, 0, ht->callbacks);
}

CF_INLINE uintptr_t __CFBasicHashImportKey(CFBasicHashRef ht, uintptr_t stack_key) {
    return ht->callbacks->func(ht, kCFBasicHashCallbackOpRetainKey, stack_key, 0, ht->callbacks);
}

CF_INLINE uintptr_t __CFBasicHashImportKey2(CFBasicHashRef ht, uintptr_t stack_key) {
    return ht->callbacks->func(ht, kCFBasicHashCallbackOpRetainKey2, stack_key, 0, ht->callbacks);
}

CF_INLINE void __CFBasicHashEjectValue(CFBasicHashRef ht, uintptr_t stack_value) {
    ht->callbacks->func(ht, kCFBasicHashCallbackOpReleaseValue, stack_value, 0, ht->callbacks);
}

CF_INLINE void __CFBasicHashEjectValue2(CFBasicHashRef ht, uintptr_t stack_value) {
    ht->callbacks->func(ht, kCFBasicHashCallbackOpReleaseValue2, stack_value, 0, ht->callbacks);
}

CF_INLINE void __CFBasicHashEjectKey(CFBasicHashRef ht, uintptr_t stack_key) {
    ht->callbacks->func(ht, kCFBasicHashCallbackOpReleaseKey, stack_key, 0, ht->callbacks);
}

CF_INLINE void __CFBasicHashEjectKey2(CFBasicHashRef ht, uintptr_t stack_key) {
    ht->callbacks->func(ht, kCFBasicHashCallbackOpReleaseKey2, stack_key, 0, ht->callbacks);
}

CF_INLINE Boolean __CFBasicHashTestEqualValue(CFBasicHashRef ht, uintptr_t stack_value_a, uintptr_t stack_value_b) {
    return (Boolean)ht->callbacks->func(ht, kCFBasicHashCallbackOpValueEqual, stack_value_a, stack_value_b, ht->callbacks);
}

CF_INLINE Boolean __CFBasicHashTestEqualValue2(CFBasicHashRef ht, uintptr_t stack_value_a, uintptr_t stack_value_b) {
    return (Boolean)ht->callbacks->func(ht, kCFBasicHashCallbackOpValue2Equal, stack_value_a, stack_value_b, ht->callbacks);
}

CF_INLINE Boolean __CFBasicHashTestEqualKey(CFBasicHashRef ht, uintptr_t stack_key_a, uintptr_t stack_key_b) {
    COCOA_HASHTABLE_TEST_EQUAL(ht, stack_key_a, stack_key_b);
    return (Boolean)ht->callbacks->func(ht, kCFBasicHashCallbackOpKeyEqual, stack_key_a, stack_key_b, ht->callbacks);
}

CF_INLINE Boolean __CFBasicHashTestEqualKey2(CFBasicHashRef ht, uintptr_t stack_key_a, uintptr_t stack_key_b) {
    COCOA_HASHTABLE_TEST_EQUAL(ht, stack_key_a, stack_key_b);
    return (Boolean)ht->callbacks->func(ht, kCFBasicHashCallbackOpKey2Equal, stack_key_a, stack_key_b, ht->callbacks);
}

CF_INLINE CFHashCode __CFBasicHashHashKey(CFBasicHashRef ht, uintptr_t stack_key) {
    CFHashCode hash_code = (CFHashCode)ht->callbacks->func(ht, kCFBasicHashCallbackOpHashKey, stack_key, 0, ht->callbacks);
    COCOA_HASHTABLE_HASH_KEY(ht, stack_key, hash_code);
    return hash_code;
}

CF_INLINE CFHashCode __CFBasicHashHashKey2(CFBasicHashRef ht, uintptr_t stack_key) {
    CFHashCode hash_code = (CFHashCode)ht->callbacks->func(ht, kCFBasicHashCallbackOpHashKey2, stack_key, 0, ht->callbacks);
    COCOA_HASHTABLE_HASH_KEY(ht, stack_key, hash_code);
    return hash_code;
}

CF_INLINE void *__CFBasicHashAllocateMemory(CFBasicHashRef ht, CFIndex count, CFIndex elem_size, Boolean strong) {
    CFAllocatorRef allocator = CFGetAllocator(ht);
    void *new_mem = NULL;
    if (CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        new_mem = auto_zone_allocate_object(auto_zone(), count * elem_size, strong ? AUTO_MEMORY_SCANNED : AUTO_UNSCANNED, false, false);
    } else {
        new_mem = CFAllocatorAllocate(allocator, count * elem_size, 0);
    }
    if (!new_mem) HALT;
    return new_mem;
}


// Prime numbers. Values above 100 have been adjusted up so that the
// malloced block size will be just below a multiple of 512; values
// above 1200 have been adjusted up to just below a multiple of 4096.
static const uintptr_t __CFBasicHashTableSizes[64] = {
    0, 3, 7, 13, 23, 41, 71, 127, 191, 251, 383, 631, 1087, 1723,
    2803, 4523, 7351, 11959, 19447, 31231, 50683, 81919, 132607,
    214519, 346607, 561109, 907759, 1468927, 2376191, 3845119,
    6221311, 10066421, 16287743, 26354171, 42641881, 68996069,
    111638519, 180634607, 292272623, 472907251,
#if __LP64__
    765180413UL, 1238087663UL, 2003267557UL, 3241355263UL, 5244622819UL,
    8485977589UL, 13730600407UL, 22216578047UL, 35947178479UL,
    58163756537UL, 94110934997UL, 152274691561UL, 246385626107UL,
    398660317687UL, 645045943807UL, 1043706260983UL, 1688752204787UL,
    2732458465769UL, 4421210670577UL, 7153669136377UL,
    11574879807461UL, 18728548943849UL, 30303428750843UL
#endif
};

// Primitive roots for the primes above
static const uintptr_t __CFBasicHashPrimitiveRoots[64] = {
    0, 2, 3, 2, 5, 6, 7, 3, 19, 6, 5, 3, 3, 3,
    2, 5, 6, 3, 3, 6, 2, 3, 3,
    3, 5, 10, 3, 3, 22, 3,
    3, 3, 5, 2, 22, 2,
    11, 5, 5, 2,
#if __LP64__
    3, 10, 2, 3, 10,
    2, 3, 5, 3,
    3, 2, 7, 2,
    3, 3, 3, 2,
    3, 5, 5,
    2, 3, 2
#endif
};

/* Primitive roots under 100 for the primes above
3: 2
7: 3 5
13: 2 6 7 11
23: 5 7 10 11 14 15 17 19 20 21
41: 6 7 11 12 13 15 17 19 22 24 26 28 29 30 34 35
71: 7 11 13 21 22 28 31 33 35 42 44 47 52 53 55 56 59 61 62 63 65 67 68 69
127: 3 6 7 12 14 23 29 39 43 45 46 48 53 55 56 57 58 65 67 78 83 85 86 91 92 93 96 97
191: 19 21 22 28 29 33 35 42 44 47 53 56 57 58 61 62 63 71 73 74 76 83 87 88 89 91 93 94 95 99
251: 6 11 14 18 19 24 26 29 30 33 34 37 42 43 44 46 53 54 55 56 57 59 61 62 70 71 72 76 77 78 82 87 90 95 96 97 98 99
383: 5 10 11 13 15 20 22 26 30 33 35 37 39 40 41 44 45 47 52 53 59 60 61 66 70 74 77 78 79 80 82 83 85 88 89 90 91 94 95 97 99
631: 3 7 12 13 14 15 19 26 51 53 54 56 59 60 61 63 65 70 75 76 87 93 95 96 99
1087: 3 10 12 13 14 20 22 24 28 29 31 38 44 45 46 51 52 53 54 58 59 61 62 63 67 74 75 76 80 89 90 92 94 96 97 99
1723: 3 12 17 18 29 30 38 45 46 48 55 63 74 75 77 78 82 83 86 88 94 95
2803: 2 11 12 18 20 21 29 30 32 34 35 38 41 46 48 50 52 56 66 67 74 78 79 80 83 84 86 91 93 94 98 99
4523: 5 6 7 15 18 20 22 24 26 31 34 41 45 54 55 57 60 65 66 70 72 74 76 77 83 85 88 93 94 96 98
7351: 6 7 12 15 17 22 28 31 35 38 44 52 54 55 56 60 65 69 71 75 96
11959: 3 6 12 24 29 33 37 39 41 47 48 51 53 57 58 59 66 67 69 73 74 75 78 82 94 96
19447: 3 5 6 7 10 12 14 20 23 24 28 29 37 39 45 46 47 51 55 56 58 65 71 73 74 75 78 79 80 82 83 90 91 92 94 96
31231: 6 7 24 29 30 33 41 43 48 52 53 54 56 57 59 60 62 65 69 70 73 75 77 83 86
50683: 2 3 12 14 17 18 20 32 33 35 39 41 45 50 55 56 57 58 61 62 65 68 69 71 72 74 75 80 84 86 88 93 95
81919: 3 12 23 24 26 30 33 43 52 53 54 57 59 60 65 66 75 84 86 87 91 92 93 96 97
132607: 3 5 6 17 19 20 21 23 24 26 29 33 34 35 38 40 42 45 48 52 54 61 62 67 71 73 75 79 82 86 89 90 92
214519: 3 7 12 15 19 24 26 28 30 33 35 38 41 52 54 56 61 65 69 70 73 77 86 87 89 93 96 97
346607: 5 10 14 15 17 19 20 21 28 30 34 38 40 41 42 45 51 55 56 57 59 60 63 65 67 68 76 77 80 82 84 89 90 91 97
561109: 10 11 18 21 26 30 33 35 38 40 41 43 46 47 50 51 53 61 62 72 73 74 84 85 91 96
907759: 3 6 12 13 17 24 26 33 34 41 47 48 52 57 61 66 68 71 75 79 82 87 89 93 94
1468927: 3 5 6 11 20 21 22 23 24 26 35 40 42 45 48 51 52 54 58 71 73 75 77 79 86 88 90 92 93 94 95
2376191: 22 29 31 33 37 43 44 47 55 58 59 62 66 77 86 87 88 93 99
3845119: 3 11 12 13 15 23 24 30 37 42 43 44 51 52 53 54 55 57 65 73 84 86 87 88 89 92 94 96 97
6221311: 3 12 13 15 21 24 30 31 33 46 54 57 61 66 67 74 82 84 87 89 91 92 96
10066421: 3 10 11 12 17 18 19 21 23 27 39 40 41 48 50 56 58 60 61 66 68 71 72 73 74 75 76 77 83 85 86 87 92 94 95 97
16287743: 5 10 15 20 30 31 35 40 43 45 47 53 55 59 60 61 62 65 70 73 79 80 85 86 89 90 93 94 95
26354171: 2 6 7 8 10 17 18 21 22 23 24 26 30 35 38 40 50 51 53 59 62 63 66 67 68 69 71 72 74 77 78 83 84 85 87 88 90 91 96 98
42641881: 22 31 38 44 46 57 59 62 67 69 73 76 77 83 92 99
68996069: 2 3 8 10 11 12 14 15 17 18 21 26 27 32 37 38 40 43 44 46 47 48 50 53 55 56 58 60 61 62 66 67 68 69 70 72 75 77 82 84 85 87 89 90 93 98 99
111638519: 11 13 17 22 26 29 33 34 39 41 44 51 52 53 55 58 59 61 65 66 67 68 71 77 78 79 82 83 85 87 88 91 97 99
180634607: 5 10 15 19 20 23 30 31 35 37 38 40 43 45 46 47 55 57 60 62 65 69 70 74 76 79 80 85 86 89 90 92 93 94
292272623: 5 10 11 13 15 20 22 23 26 30 31 33 35 39 40 44 45 46 47 52 59 60 61 62 66 67 69 70 71 77 78 79 80 83 85 88 90 91 92 93 94 95 97 99
472907251: 2 10 12 14 17 18 29 31 37 46 50 60 61 65 68 70 78 82 84 85 90 91 94 98
765180413: 3 5 11 12 14 18 20 21 23 26 27 29 30 34 35 38 39 44 45 47 48 50 51 56 57 59 62 66 67 71 72 73 74 75 77 80 82 84 85 86 89 92 93 95 97 98 99
1238087663: 10 13 14 15 20 21 23 28 38 40 41 42 43 45 46 52 55 56 57 60 63 67 69 71 76 78 80 82 84 85 86 89 90 92
2003267557: 2 13 14 18 20 22 23 24 31 32 34 37 38 41 43 47 50 54 59 60 67 69 79 80 85 87 91 93 96
3241355263: 3 6 10 11 20 21 22 24 34 42 43 45 46 48 54 57 61 65 68 70 71 75 77 78 80 83 86 87 88 92 93 94
5244622819: 10 15 17 23 29 31 35 38 40 50 57 60 65 67 68 71 73 74 75 79 90 92 94
8485977589: 2 6 10 17 18 19 22 28 30 31 32 35 37 40 47 51 52 54 57 58 59 61 65 66 76 77 79 84 85 86 88 90 93 96 98
13730600407: 3 5 10 12 19 24 33 35 40 42 43 45 46 51 54 55 65 73 75 76 78 80 82 84 87 89 92 93 94 96
22216578047: 5 10 11 15 17 20 22 30 33 34 35 40 44 45 51 59 60 61 65 66 68 70 73 77 79 80 88 90 95 99
35947178479: 3 12 14 15 22 24 29 30 38 41 44 51 54 55 56 58 63 69 70 73 76 78 89 91 95 96 97 99
58163756537: 3 5 6 7 10 11 12 14 17 20 22 23 24 27 28 31 39 40 43 44 45 46 47 48 53 54 56 57 59 61 62 63 65 68 71 73 75 78 79 80 86 87 88 89 90 91 92 94 95 96 97 99
94110934997: 2 3 5 8 11 12 14 18 19 20 21 23 26 27 29 30 32 34 35 39 41 43 44 48 51 56 59 62 66 67 71 72 74 75 76 77 79 80 84 85 92 93 94 98 99
152274691561: 7 17 26 35 37 39 41 42 43 53 56 62 63 65 67 74 82 84 85 89 93 94
246385626107UL: 2 5 6 8 11 14 15 18 20 23 24 26 29 31 32 33 34 35 37 38 42 43 44 45 50 54 56 60 61 65 67 69 71 72 77 78 80 82 83 85 87 89 92 93 94 95 96 98 99
398660317687UL: 3 5 6 7 11 13 20 24 26 28 40 44 45 48 54 56 59 63 69 75 79 85 88 89 90 93 95 99
645045943807UL: 3 5 10 12 21 22 23 24 26 35 37 40 41 44 45 47 51 52 53 59 70 75 79 85 87 92 93 95 96 97 99
1043706260983UL: 3 7 11 24 28 29 (<= 32)
1688752204787UL: 2 5 6 7 8 13 18 20 21 22 23 24 28 32 (<= 32)
2732458465769UL: 3 6 11 12 13 15 17 19 21 22 23 24 26 27 30 31 (<= 32)
4421210670577UL: 5 (<= 9)

*/

static const uintptr_t __CFBasicHashTableCapacities[64] = {
    0, 3, 6, 11, 19, 32, 52, 85, 118, 155, 237, 390, 672, 1065,
    1732, 2795, 4543, 7391, 12019, 19302, 31324, 50629, 81956,
    132580, 214215, 346784, 561026, 907847, 1468567, 2376414,
    3844982, 6221390, 10066379, 16287773, 26354132, 42641916,
    68996399, 111638327, 180634415, 292272755,
#if __LP64__
    472907503UL, 765180257UL, 1238087439UL, 2003267722UL, 3241355160UL,
    5244622578UL, 8485977737UL, 13730600347UL, 22216578100UL,
    35947178453UL, 58163756541UL, 94110935011UL, 152274691274UL,
    246385626296UL, 398660317578UL, 645045943559UL, 1043706261135UL,
    1688752204693UL, 2732458465840UL, 4421210670552UL,
    7153669136706UL, 11574879807265UL, 18728548943682UL
#endif
};

// to expose the load factor, expose this function to customization
CF_INLINE CFIndex __CFBasicHashGetCapacityForNumBuckets(CFBasicHashRef ht, CFIndex num_buckets_idx) {
    return __CFBasicHashTableCapacities[num_buckets_idx];
#if 0
    CFIndex num_buckets = __CFBasicHashTableSizes[num_buckets_idx];
    if (num_buckets_idx < 8) {
        double dep = 0.0545665730357293074; // (1 - (sqrt(5) - 1) / 2) / 7
        double factor = 1.0 - (num_buckets_idx - 1) * dep;
        return (CFIndex)floor(num_buckets * factor + 0.375); // 0.375 is intentional
    }
    double factor = 0.6180339887498948482; // (sqrt(5) - 1) / 2
    return (CFIndex)floor(num_buckets * factor + 0.5);
#endif
}

CF_INLINE CFIndex __CFBasicHashGetNumBucketsIndexForCapacity(CFBasicHashRef ht, CFIndex capacity) {
    for (CFIndex idx = 0; idx < 64; idx++) {
        if (capacity <= __CFBasicHashGetCapacityForNumBuckets(ht, idx)) return idx;
    }
    HALT;
    return 0;
}

__private_extern__ CFIndex CFBasicHashGetNumBuckets(CFBasicHashRef ht) {
    return __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
}

__private_extern__ CFIndex CFBasicHashGetCapacity(CFBasicHashRef ht) {
    return __CFBasicHashGetCapacityForNumBuckets(ht, ht->bits.num_buckets_idx);
}

// In returned struct, .count is zero if the bucket is empty or deleted,
// and the .weak_key field indicates which. .idx is either the index of
// the found bucket or the index of the bucket which should be filled by
// an add operation. For a set or multiset, the .weak_key and .weak_value
// are the same.
__private_extern__ CFBasicHashBucket CFBasicHashGetBucket(CFBasicHashRef ht, CFIndex idx) {
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashBucket result;
    result.idx = idx;
    result.weak_value = __CFBasicHashGetValues(ht)[idx].weak;
    result.weak_value2 = (0 != ht->bits.values2_offset) ? __CFBasicHashGetValues2(ht)[idx].weak : 0;
    result.weak_key = (0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht)[idx].weak : result.weak_value;
    result.weak_key2 = (0 != ht->bits.keys2_offset) ? __CFBasicHashGetKeys2(ht)[idx].weak : 0;
    result.count = (0 != ht->bits.counts_offset) ? __CFBasicHashGetCounts(ht)[idx] : ((result.weak_key == empty || result.weak_key == deleted) ? 0 : 1);
    result.order = (0 != ht->bits.orders_offset) ? __CFBasicHashGetOrders(ht)[idx] : 0;
    return result;
}

// During rehashing of a mutable CFBasicHash, we know that there are no
// deleted slots and the keys have already been uniqued. When rehashing,
// if key_hash is non-0, we use it as the hash code.
static CFBasicHashBucket ___CFBasicHashFindBucket1(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t key_hash, Boolean rehashing) {
    CFHashCode hash_code = key_hash ? key_hash : __CFBasicHashHashKey(ht, stack_key);
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *keys = (0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht) : __CFBasicHashGetValues(ht);
    uintptr_t *hashes = (0 != ht->bits.hashes_offset) ? __CFBasicHashGetHashes(ht) : NULL;
    uintptr_t num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    CFBasicHashBucket result = {kCFNotFound, deleted, 0, deleted, 0, 0, 0};

    uintptr_t h1 = 0;
        // Linear probing, with c = 1
        // probe[0] = h1(k)
        // probe[i] = (h1(k) + i * c) mod num_buckets, i = 1 .. num_buckets - 1
        // h1(k) = k mod num_buckets
        h1 = hash_code % num_buckets;

    COCOA_HASHTABLE_PROBING_START(ht, num_buckets);
    uintptr_t probe = h1;
    for (CFIndex idx = 0; idx < num_buckets; idx++) {
        uintptr_t stack_curr = keys[probe].weak;
        if (stack_curr == empty) {
            COCOA_HASHTABLE_PROBE_EMPTY(ht, probe);
            if (kCFNotFound == result.idx) {
                result.idx = probe;
                result.weak_value = empty;
                result.weak_key = empty;
            }
            COCOA_HASHTABLE_PROBING_END(ht, idx + 1);
            return result;
        } else if (__builtin_expect(!rehashing, 0)) {
            if (stack_curr == deleted) {
                COCOA_HASHTABLE_PROBE_DELETED(ht, probe);
                if (kCFNotFound == result.idx) {
                    result.idx = probe;
                }
            } else {
                COCOA_HASHTABLE_PROBE_VALID(ht, probe);
                if (stack_curr == stack_key || ((!hashes || hashes[probe] == hash_code) && __CFBasicHashTestEqualKey(ht, stack_curr, stack_key))) {
                    COCOA_HASHTABLE_PROBING_END(ht, idx + 1);
                    result.idx = probe;
                    result.weak_value = (0 != ht->bits.keys_offset) ? __CFBasicHashGetValues(ht)[probe].weak : stack_curr;
                    result.weak_value2 = (0 != ht->bits.values2_offset) ? __CFBasicHashGetValues2(ht)[probe].weak : 0;
                    result.weak_key = stack_curr;
                    result.weak_key2 = (0 != ht->bits.keys2_offset) ? __CFBasicHashGetKeys2(ht)[probe].weak : 0;
                    result.count = (0 != ht->bits.counts_offset) ? __CFBasicHashGetCounts(ht)[probe] : 1;
                    result.order = (0 != ht->bits.orders_offset) ? __CFBasicHashGetOrders(ht)[probe] : 1;
                    return result;
                }
            }
        }
        // Linear probing, with c = 1
            probe += 1;
            if (__builtin_expect(num_buckets <= probe, 0)) {
                probe -= num_buckets;
            }
    }
    COCOA_HASHTABLE_PROBING_END(ht, num_buckets);
    return result; // all buckets full or deleted, return first deleted element which was found
}

static CFBasicHashBucket ___CFBasicHashFindBucket2(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t key_hash, Boolean rehashing) {
    CFHashCode hash_code = key_hash ? key_hash : __CFBasicHashHashKey(ht, stack_key);
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *keys = (0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht) : __CFBasicHashGetValues(ht);
    uintptr_t *hashes = (0 != ht->bits.hashes_offset) ? __CFBasicHashGetHashes(ht) : NULL;
    uintptr_t num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    CFBasicHashBucket result = {kCFNotFound, deleted, 0, deleted, 0, 0, 0};

    uintptr_t h1 = 0, h2 = 0;
        // Double hashing
        // probe[0] = h1(k)
        // probe[i] = (h1(k) + i * h2(k)) mod num_buckets, i = 1 .. num_buckets - 1
        // h1(k) = k mod num_buckets
        // h2(k) = floor(k / num_buckets) mod num_buckets
        h1 = hash_code % num_buckets;
        h2 = (hash_code / num_buckets) % num_buckets;
        if (0 == h2) h2 = num_buckets - 1;

    COCOA_HASHTABLE_PROBING_START(ht, num_buckets);
    uintptr_t probe = h1;
    for (CFIndex idx = 0; idx < num_buckets; idx++) {
        uintptr_t stack_curr = keys[probe].weak;
        if (stack_curr == empty) {
            COCOA_HASHTABLE_PROBE_EMPTY(ht, probe);
            if (kCFNotFound == result.idx) {
                result.idx = probe;
                result.weak_value = empty;
                result.weak_key = empty;
            }
            COCOA_HASHTABLE_PROBING_END(ht, idx + 1);
            return result;
        } else if (__builtin_expect(!rehashing, 0)) {
            if (stack_curr == deleted) {
                COCOA_HASHTABLE_PROBE_DELETED(ht, probe);
                if (kCFNotFound == result.idx) {
                    result.idx = probe;
                }
            } else {
                COCOA_HASHTABLE_PROBE_VALID(ht, probe);
                if (stack_curr == stack_key || ((!hashes || hashes[probe] == hash_code) && __CFBasicHashTestEqualKey(ht, stack_curr, stack_key))) {
                    COCOA_HASHTABLE_PROBING_END(ht, idx + 1);
                    result.idx = probe;
                    result.weak_value = (0 != ht->bits.keys_offset) ? __CFBasicHashGetValues(ht)[probe].weak : stack_curr;
                    result.weak_value2 = (0 != ht->bits.values2_offset) ? __CFBasicHashGetValues2(ht)[probe].weak : 0;
                    result.weak_key = stack_curr;
                    result.weak_key2 = (0 != ht->bits.keys2_offset) ? __CFBasicHashGetKeys2(ht)[probe].weak : 0;
                    result.count = (0 != ht->bits.counts_offset) ? __CFBasicHashGetCounts(ht)[probe] : 1;
                    result.order = (0 != ht->bits.orders_offset) ? __CFBasicHashGetOrders(ht)[probe] : 1;
                    return result;
                }
            }
        }
        // Double hashing
            probe += h2;
            if (__builtin_expect(num_buckets <= probe, 1)) {
                probe -= num_buckets;
            }
    }
    COCOA_HASHTABLE_PROBING_END(ht, num_buckets);
    return result; // all buckets full or deleted, return first deleted element which was found
}

static CFBasicHashBucket ___CFBasicHashFindBucket3(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t key_hash, Boolean rehashing) {
    CFHashCode hash_code = key_hash ? key_hash : __CFBasicHashHashKey(ht, stack_key);
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *keys = (0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht) : __CFBasicHashGetValues(ht);
    uintptr_t *hashes = (0 != ht->bits.hashes_offset) ? __CFBasicHashGetHashes(ht) : NULL;
    uintptr_t num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    CFBasicHashBucket result = {kCFNotFound, deleted, 0, deleted, 0, 0, 0};

    uintptr_t h1 = 0, h2 = 0, pr = 0;
        // Improved exponential hashing
        // probe[0] = h1(k)
        // probe[i] = (h1(k) + pr(k)^i * h2(k)) mod num_buckets, i = 1 .. num_buckets - 1
        // h1(k) = k mod num_buckets
        // h2(k) = floor(k / num_buckets) mod num_buckets
        // note: h2(k) has the effect of rotating the sequence if it is constant
        // note: pr(k) is any primitive root of num_buckets, varying this gives different sequences
        h1 = hash_code % num_buckets;
        h2 = (hash_code / num_buckets) % num_buckets;
        if (0 == h2) h2 = num_buckets - 1;
        pr = __CFBasicHashPrimitiveRoots[ht->bits.num_buckets_idx];

    COCOA_HASHTABLE_PROBING_START(ht, num_buckets);
    uintptr_t probe = h1, acc = pr;
    for (CFIndex idx = 0; idx < num_buckets; idx++) {
        uintptr_t stack_curr = keys[probe].weak;
        if (stack_curr == empty) {
            COCOA_HASHTABLE_PROBE_EMPTY(ht, probe);
            if (kCFNotFound == result.idx) {
                result.idx = probe;
                result.weak_value = empty;
                result.weak_key = empty;
            }
            COCOA_HASHTABLE_PROBING_END(ht, idx + 1);
            return result;
        } else if (__builtin_expect(!rehashing, 0)) {
            if (stack_curr == deleted) {
                COCOA_HASHTABLE_PROBE_DELETED(ht, probe);
                if (kCFNotFound == result.idx) {
                    result.idx = probe;
                }
            } else {
                COCOA_HASHTABLE_PROBE_VALID(ht, probe);
                if (stack_curr == stack_key || ((!hashes || hashes[probe] == hash_code) && __CFBasicHashTestEqualKey(ht, stack_curr, stack_key))) {
                    COCOA_HASHTABLE_PROBING_END(ht, idx + 1);
                    result.idx = probe;
                    result.weak_value = (0 != ht->bits.keys_offset) ? __CFBasicHashGetValues(ht)[probe].weak : stack_curr;
                    result.weak_value2 = (0 != ht->bits.values2_offset) ? __CFBasicHashGetValues2(ht)[probe].weak : 0;
                    result.weak_key = stack_curr;
                    result.weak_key2 = (0 != ht->bits.keys2_offset) ? __CFBasicHashGetKeys2(ht)[probe].weak : 0;
                    result.count = (0 != ht->bits.counts_offset) ? __CFBasicHashGetCounts(ht)[probe] : 1;
                    result.order = (0 != ht->bits.orders_offset) ? __CFBasicHashGetOrders(ht)[probe] : 1;
                    return result;
                }
            }
        }
        // Improved exponential hashing
            probe = h1 + h2 * acc;
            if (__builtin_expect(num_buckets <= probe, 1)) {
                probe = probe % num_buckets;
            }
            acc = acc * pr;
            if (__builtin_expect(num_buckets <= acc, 1)) {
                acc = acc % num_buckets;
            }
    }
    COCOA_HASHTABLE_PROBING_END(ht, num_buckets);
    return result; // all buckets full or deleted, return first deleted element which was found
}

CF_INLINE CFBasicHashBucket ___CFBasicHashFindBucket(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t key_hash, Boolean rehashing) {
    switch (ht->bits.hash_style) {
    case __kCFBasicHashLinearHashingValue:
        return ___CFBasicHashFindBucket1(ht, stack_key, 0, rehashing);
    case __kCFBasicHashDoubleHashingValue:
        return ___CFBasicHashFindBucket2(ht, stack_key, 0, rehashing);
    case __kCFBasicHashExponentialHashingValue:
        return ___CFBasicHashFindBucket3(ht, stack_key, 0, rehashing);
    }
    HALT;
    CFBasicHashBucket result = {kCFNotFound, 0, 0, 0};
    return result;
}

CF_INLINE CFBasicHashBucket __CFBasicHashFindBucket(CFBasicHashRef ht, uintptr_t stack_key) {
    if (0 == ht->bits.num_buckets_idx) {
        uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift);
        CFBasicHashBucket result = {kCFNotFound, empty, empty, 0};
        return result;
    }
    return ___CFBasicHashFindBucket(ht, stack_key, 0, false);
}

__private_extern__ CFBasicHashBucket CFBasicHashFindBucket(CFBasicHashRef ht, uintptr_t stack_key) {
    return __CFBasicHashFindBucket(ht, stack_key);
}

__private_extern__ CFOptionFlags CFBasicHashGetFlags(CFBasicHashRef ht) {
    CFOptionFlags flags = (ht->bits.hash_style << 13);
    if (ht->bits.strong_values) flags |= kCFBasicHashStrongValues;
    if (ht->bits.strong_values2) flags |= kCFBasicHashStrongValues2;
    if (ht->bits.strong_keys) flags |= kCFBasicHashStrongKeys;
    if (ht->bits.strong_keys2) flags |= kCFBasicHashStrongKeys2;
    if (ht->bits.fast_grow) flags |= kCFBasicHashAggressiveGrowth;
    if (ht->bits.values2_offset) flags |= kCFBasicHashHasValues2;
    if (ht->bits.keys_offset) flags |= kCFBasicHashHasKeys;
    if (ht->bits.keys2_offset) flags |= kCFBasicHashHasKeys2;
    if (ht->bits.counts_offset) flags |= kCFBasicHashHasCounts;
    if (ht->bits.orders_offset) flags |= kCFBasicHashHasOrder;
    if (ht->bits.hashes_offset) flags |= kCFBasicHashHasHashCache;
    return flags;
}

__private_extern__ CFIndex CFBasicHashGetCount(CFBasicHashRef ht) {
    if (0 != ht->bits.counts_offset) {
        CFIndex total = 0L;
        CFIndex cnt = (CFIndex)__CFBasicHashTableSizes[ht->bits.num_buckets_idx];
        uintptr_t *counts = __CFBasicHashGetCounts(ht);
        for (CFIndex idx = 0; idx < cnt; idx++) {
            total += counts[idx];
        }
        return total;
    }
    return (CFIndex)ht->bits.used_buckets;
}

__private_extern__ CFIndex CFBasicHashGetCountOfKey(CFBasicHashRef ht, uintptr_t stack_key) {
    if (0L == ht->bits.used_buckets) {
        return 0L;
    }
    return __CFBasicHashFindBucket(ht, stack_key).count;
}

__private_extern__ CFIndex CFBasicHashGetCountOfValue(CFBasicHashRef ht, uintptr_t stack_value) {
    if (0L == ht->bits.used_buckets) {
        return 0L;
    }
    if (!(0 != ht->bits.keys_offset)) {
        return __CFBasicHashFindBucket(ht, stack_value).count;
    }
    __block CFIndex total = 0L;
    CFBasicHashApply(ht, ^(CFBasicHashBucket bkt) {
            if ((stack_value == bkt.weak_value) || __CFBasicHashTestEqualValue(ht, bkt.weak_value, stack_value)) total += bkt.count;
            return (Boolean)true;
        });
    return total;
}

__private_extern__ Boolean CFBasicHashesAreEqual(CFBasicHashRef ht1, CFBasicHashRef ht2) {
    CFIndex cnt1 = CFBasicHashGetCount(ht1);
    if (cnt1 != CFBasicHashGetCount(ht2)) return false;
    if (0 == cnt1) return true;
    __block Boolean equal = true;
    CFBasicHashApply(ht1, ^(CFBasicHashBucket bkt1) {
            CFBasicHashBucket bkt2 = __CFBasicHashFindBucket(ht2, bkt1.weak_key);
            if (bkt1.count != bkt2.count) {
                equal = false;
                return (Boolean)false;
            }
            if ((0 != ht1->bits.keys_offset) && (bkt1.weak_value != bkt2.weak_value) && !__CFBasicHashTestEqualValue(ht1, bkt1.weak_value, bkt2.weak_value)) {
                equal = false;
                return (Boolean)false;
            }
            return (Boolean)true;
        });
    return equal;
}

__private_extern__ void CFBasicHashApply(CFBasicHashRef ht, Boolean (^block)(CFBasicHashBucket)) {
    CFIndex used = (CFIndex)ht->bits.used_buckets, cnt = (CFIndex)__CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    for (CFIndex idx = 0; 0 < used && idx < cnt; idx++) {
        CFBasicHashBucket bkt = CFBasicHashGetBucket(ht, idx);
        if (0 < bkt.count) {
            if (!block(bkt)) {
                return;
            }
            used--;
        }
    }
}

__private_extern__ void CFBasicHashGetElements(CFBasicHashRef ht, CFIndex bufferslen, uintptr_t *weak_values, uintptr_t *weak_values2, uintptr_t *weak_keys, uintptr_t *weak_keys2) {
    CFIndex used = (CFIndex)ht->bits.used_buckets, cnt = (CFIndex)__CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *values = __CFBasicHashGetValues(ht);
    CFBasicHashValue *values2 = (0 != ht->bits.values2_offset) ? __CFBasicHashGetValues2(ht) : NULL;
    CFBasicHashValue *keys = (0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht) : NULL;
    CFBasicHashValue *keys2 = (0 != ht->bits.keys2_offset) ? __CFBasicHashGetKeys2(ht) : NULL;
    uintptr_t *counts = (0 != ht->bits.counts_offset) ? __CFBasicHashGetCounts(ht) : NULL;
    CFIndex offset = 0;
    for (CFIndex idx = 0; 0 < used && idx < cnt && offset < bufferslen; idx++) {
        uintptr_t weak_key = keys ? keys[idx].weak : values[idx].weak;
        uintptr_t count = counts ? counts[idx] : ((weak_key == empty || weak_key == deleted) ? 0 : 1);
        if (0 < count) {
            used--;
            for (CFIndex cnt = count; cnt-- && offset < bufferslen;) {
                if (weak_values) { weak_values[offset] = values[idx].weak; }
                if (weak_values2) { weak_values2[offset] = values2 ? values2[idx].weak : 0; }
                if (weak_keys) { weak_keys[offset] = weak_key; }
                if (weak_keys2) { weak_keys2[offset] = keys2 ? keys2[idx].weak : 0; }
                offset++;
            }
        }
    }
}

__private_extern__ unsigned long __CFBasicHashFastEnumeration(CFBasicHashRef ht, struct __objcFastEnumerationStateEquivalent2 *state, void *stackbuffer, unsigned long count) {
    /* copy as many as count items over */
    if (0 == state->state) {        /* first time */
        state->mutationsPtr = (unsigned long *)&ht->bits + (__LP64__ ? 1 : 3);
    }
    state->itemsPtr = (unsigned long *)stackbuffer;
    CFIndex cntx = 0;
    CFIndex used = (CFIndex)ht->bits.used_buckets, cnt = (CFIndex)__CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    for (CFIndex idx = (CFIndex)state->state; 0 < used && idx < cnt && cntx < (CFIndex)count; idx++) {
        CFBasicHashBucket bkt = CFBasicHashGetBucket(ht, idx);
        if (0 < bkt.count) {
            state->itemsPtr[cntx++] = (unsigned long)bkt.weak_key;
            used--;
        }
        state->state++;
    }
    return cntx;
}

#if ENABLE_MEMORY_COUNTERS
static volatile int64_t __CFBasicHashTotalCount = 0ULL;
static volatile int64_t __CFBasicHashTotalSize = 0ULL;
static volatile int64_t __CFBasicHashPeakCount = 0ULL;
static volatile int64_t __CFBasicHashPeakSize = 0ULL;
static volatile int32_t __CFBasicHashSizes[64] = {0};
#endif

static void __CFBasicHashDrain(CFBasicHashRef ht, Boolean forFinalization) {
#if ENABLE_MEMORY_COUNTERS
    OSAtomicAdd64Barrier(-1 * (int64_t) CFBasicHashGetSize(ht, true), & __CFBasicHashTotalSize);
#endif

    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFIndex old_num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];

    CFAllocatorRef allocator = CFGetAllocator(ht);
    Boolean nullify = (!forFinalization || !CF_IS_COLLECTABLE_ALLOCATOR(allocator));

    CFBasicHashValue *old_values = NULL, *old_values2 = NULL, *old_keys = NULL, *old_keys2 = NULL;
    uintptr_t *old_counts = NULL, *old_orders = NULL, *old_hashes = NULL;

    old_values = __CFBasicHashGetValues(ht);
    if (nullify) __CFBasicHashSetValues(ht, NULL);
    if (0 != ht->bits.values2_offset) {
        old_values2 = __CFBasicHashGetValues2(ht);
        if (nullify) __CFBasicHashSetValues2(ht, NULL);
    }
    if (0 != ht->bits.keys_offset) {
        old_keys = __CFBasicHashGetKeys(ht);
        if (nullify) __CFBasicHashSetKeys(ht, NULL);
    }
    if (0 != ht->bits.keys2_offset) {
        old_keys2 = __CFBasicHashGetKeys2(ht);
        if (nullify) __CFBasicHashSetKeys2(ht, NULL);
    }
    if (0 != ht->bits.counts_offset) {
        old_counts = __CFBasicHashGetCounts(ht);
        if (nullify) __CFBasicHashSetCounts(ht, NULL);
    }
    if (0 != ht->bits.orders_offset) {
        old_orders = __CFBasicHashGetOrders(ht);
        if (nullify) __CFBasicHashSetOrders(ht, NULL);
    }
    if (0 != ht->bits.hashes_offset) {
        old_hashes = __CFBasicHashGetHashes(ht);
        if (nullify) __CFBasicHashSetHashes(ht, NULL);
    }

    if (nullify) {
        ht->bits.mutations++;
        ht->bits.num_buckets_idx = 0;
        ht->bits.used_buckets = 0;
        ht->bits.marker = 0;
        ht->bits.deleted = 0;
    }
    
    if (ht->callbacks != &CFBasicHashNullCallbacks) {
        CFBasicHashValue *keys = old_keys ? old_keys : old_values;
        for (CFIndex idx = 0; idx < old_num_buckets; idx++) {
            uintptr_t stack_key = keys[idx].weak;
            if (stack_key != empty && stack_key != deleted) {
                __CFBasicHashEjectValue(ht, old_values[idx].weak);
                if (old_values2) {
                    __CFBasicHashEjectValue2(ht, old_values2[idx].weak);
                }
                if (old_keys) {
                    __CFBasicHashEjectKey(ht, old_keys[idx].weak);
                }
                if (old_keys2) {
                    __CFBasicHashEjectKey2(ht, old_keys2[idx].weak);
                }
            }
        }
    }

    if (forFinalization) {
        ht->callbacks->func(ht, kCFBasicHashCallbackOpFreeCallbacks, (uintptr_t)allocator, 0, ht->callbacks);
    }

    if (!CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        CFAllocatorDeallocate(allocator, old_values);
        CFAllocatorDeallocate(allocator, old_values2);
        CFAllocatorDeallocate(allocator, old_keys);
        CFAllocatorDeallocate(allocator, old_keys2);
        CFAllocatorDeallocate(allocator, old_counts);
        CFAllocatorDeallocate(allocator, old_orders);
        CFAllocatorDeallocate(allocator, old_hashes);
    }

#if ENABLE_MEMORY_COUNTERS
    int64_t size_now = OSAtomicAdd64Barrier((int64_t) CFBasicHashGetSize(ht, true), & __CFBasicHashTotalSize);
    while (__CFBasicHashPeakSize < size_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakSize, size_now, & __CFBasicHashPeakSize));
#endif
}

static void __CFBasicHashRehash(CFBasicHashRef ht, CFIndex newItemCount) {
#if ENABLE_MEMORY_COUNTERS
    OSAtomicAdd64Barrier(-1 * (int64_t) CFBasicHashGetSize(ht, true), & __CFBasicHashTotalSize);
    OSAtomicAdd32Barrier(-1, &__CFBasicHashSizes[ht->bits.num_buckets_idx]);
#endif

    if (COCOA_HASHTABLE_REHASH_START_ENABLED()) COCOA_HASHTABLE_REHASH_START(ht, CFBasicHashGetNumBuckets(ht), CFBasicHashGetSize(ht, true));

    CFIndex new_num_buckets_idx = ht->bits.num_buckets_idx;
    if (0 != newItemCount) {
        if (newItemCount < 0) newItemCount = 0;
        CFIndex new_capacity_req = ht->bits.used_buckets + newItemCount;
        new_num_buckets_idx = __CFBasicHashGetNumBucketsIndexForCapacity(ht, new_capacity_req);
        if (1 == newItemCount && ht->bits.fast_grow) {
            new_num_buckets_idx++;
        }
    }

    CFIndex new_num_buckets = __CFBasicHashTableSizes[new_num_buckets_idx];
    CFIndex old_num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];

    CFBasicHashValue *new_values = NULL, *new_values2 = NULL, *new_keys = NULL, *new_keys2 = NULL;
    uintptr_t *new_counts = NULL, *new_orders = NULL, *new_hashes = NULL;

    if (0 < new_num_buckets) {
        new_values = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_values);
        __SetLastAllocationEventName(new_values, "CFBasicHash (value-store)");
        if (0 != ht->bits.values2_offset) {
            new_values2 = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_values2);
            __SetLastAllocationEventName(new_values2, "CFBasicHash (value2-store)");
        }
        if (0 != ht->bits.keys_offset) {
            new_keys = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_keys);
            __SetLastAllocationEventName(new_keys, "CFBasicHash (key-store)");
        }
        if (0 != ht->bits.keys2_offset) {
            new_keys2 = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_keys2);
            __SetLastAllocationEventName(new_keys2, "CFBasicHash (key2-store)");
        }
        if (0 != ht->bits.counts_offset) {
            new_counts = (uintptr_t *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(uintptr_t), false);
            __SetLastAllocationEventName(new_counts, "CFBasicHash (count-store)");
            memset(new_counts, 0, new_num_buckets * sizeof(uintptr_t));
        }
        if (0 != ht->bits.orders_offset) {
            new_orders = (uintptr_t *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(uintptr_t), false);
            __SetLastAllocationEventName(new_orders, "CFBasicHash (order-store)");
            memset(new_orders, 0, new_num_buckets * sizeof(uintptr_t));
        }
        if (0 != ht->bits.hashes_offset) {
            new_hashes = (uintptr_t *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(uintptr_t), false);
            __SetLastAllocationEventName(new_hashes, "CFBasicHash (hash-store)");
            memset(new_hashes, 0, new_num_buckets * sizeof(uintptr_t));
        }
    }

    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    // if we knew the allocations were coming from already-cleared memory, and the marker was still 0, we could skip all this next stuff
    for (CFIndex idx = 0; idx < new_num_buckets; idx++) {
        if (ht->bits.strong_values) new_values[idx].strong = (id)empty; else new_values[idx].weak = empty;
        if (new_values2) {
            if (ht->bits.strong_values2) new_values2[idx].strong = (id)empty; else new_values2[idx].weak = empty;
        }
        if (new_keys) {
            if (ht->bits.strong_keys) new_keys[idx].strong = (id)empty; else new_keys[idx].weak = empty;
        }
        if (new_keys2) {
            if (ht->bits.strong_keys2) new_keys2[idx].strong = (id)empty; else new_keys2[idx].weak = empty;
        }
    }

    ht->bits.num_buckets_idx = new_num_buckets_idx;
    ht->bits.deleted = 0;

    CFBasicHashValue *old_values = NULL, *old_values2 = NULL, *old_keys = NULL, *old_keys2 = NULL;
    uintptr_t *old_counts = NULL, *old_orders = NULL, *old_hashes = NULL;

    old_values = __CFBasicHashGetValues(ht);
    __CFBasicHashSetValues(ht, new_values);
    if (0 != ht->bits.values2_offset) {
        old_values2 = __CFBasicHashGetValues2(ht);
        __CFBasicHashSetValues2(ht, new_values2);
    }
    if (0 != ht->bits.keys_offset) {
        old_keys = __CFBasicHashGetKeys(ht);
        __CFBasicHashSetKeys(ht, new_keys);
    }
    if (0 != ht->bits.keys2_offset) {
        old_keys2 = __CFBasicHashGetKeys2(ht);
        __CFBasicHashSetKeys2(ht, new_keys2);
    }
    if (0 != ht->bits.counts_offset) {
        old_counts = __CFBasicHashGetCounts(ht);
        __CFBasicHashSetCounts(ht, new_counts);
    }
    if (0 != ht->bits.orders_offset) {
        old_orders = __CFBasicHashGetOrders(ht);
        __CFBasicHashSetOrders(ht, new_orders);
    }
    if (0 != ht->bits.hashes_offset) {
        old_hashes = __CFBasicHashGetHashes(ht);
        __CFBasicHashSetHashes(ht, new_hashes);
    }

    if (0 < old_num_buckets) {
        CFBasicHashValue *keys = old_keys ? old_keys : old_values;
        for (CFIndex idx = 0; idx < old_num_buckets; idx++) {
            uintptr_t stack_key = keys[idx].weak;
            if (stack_key != empty && stack_key != deleted) {
                CFBasicHashBucket bkt = ___CFBasicHashFindBucket(ht, stack_key, old_hashes ? old_hashes[idx] : 0, true);
                uintptr_t stack_value = old_values[idx].weak;
                if (ht->bits.strong_values) new_values[bkt.idx].strong = (id)stack_value; else new_values[bkt.idx].weak = stack_value;
                if (old_values2) {
                    if (ht->bits.strong_values2) new_values2[bkt.idx].strong = (id)old_values2[idx].weak; else new_values2[bkt.idx].weak = old_values2[idx].weak;
                }
                if (old_keys) {
                    if (ht->bits.strong_keys) new_keys[bkt.idx].strong = (id)stack_key; else new_keys[bkt.idx].weak = stack_key;
                }
                if (old_keys2) {
                    if (ht->bits.strong_keys2) new_keys2[bkt.idx].strong = (id)old_keys2[idx].weak; else new_keys2[bkt.idx].weak = old_keys2[idx].weak;
                }
                if (old_counts) {
                    new_counts[bkt.idx] = old_counts[idx];
                }
                if (old_orders) {
                    new_orders[bkt.idx] = old_orders[idx];
                }
                if (old_hashes) {
                    new_hashes[bkt.idx] = old_hashes[idx];
                }
            }
        }
    }

    CFAllocatorRef allocator = CFGetAllocator(ht);
    if (!CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        CFAllocatorDeallocate(allocator, old_values);
        CFAllocatorDeallocate(allocator, old_values2);
        CFAllocatorDeallocate(allocator, old_keys);
        CFAllocatorDeallocate(allocator, old_keys2);
        CFAllocatorDeallocate(allocator, old_counts);
        CFAllocatorDeallocate(allocator, old_orders);
        CFAllocatorDeallocate(allocator, old_hashes);
    }

    if (COCOA_HASHTABLE_REHASH_END_ENABLED()) COCOA_HASHTABLE_REHASH_END(ht, CFBasicHashGetNumBuckets(ht), CFBasicHashGetSize(ht, true));

#if ENABLE_MEMORY_COUNTERS
    int64_t size_now = OSAtomicAdd64Barrier((int64_t) CFBasicHashGetSize(ht, true), &__CFBasicHashTotalSize);
    while (__CFBasicHashPeakSize < size_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakSize, size_now, & __CFBasicHashPeakSize));
    OSAtomicAdd32Barrier(1, &__CFBasicHashSizes[ht->bits.num_buckets_idx]);
#endif
}

__private_extern__ void CFBasicHashSetCapacity(CFBasicHashRef ht, CFIndex capacity) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    if (ht->bits.used_buckets < capacity) {
        ht->bits.mutations++;
        __CFBasicHashRehash(ht, capacity - ht->bits.used_buckets);
    }
}

static void __CFBasicHashFindNewMarker(CFBasicHashRef ht, uintptr_t stack_key) {
    uintptr_t marker = ht->bits.marker;
    uintptr_t empty = (marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *keys = (0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht) : __CFBasicHashGetValues(ht);
    Boolean strong = (0 != ht->bits.keys_offset) ? ht->bits.strong_keys : ht->bits.strong_values;
    CFIndex cnt = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    if (0 == marker) marker = 4096;

    again:;
    marker++;
    if ((1UL << 26) <= marker) HALT;
    uintptr_t new_empty = (marker << __CFBasicHashMarkerShift), new_deleted = ~new_empty;
    if (stack_key == new_empty || stack_key == new_deleted) {
        goto again;
    }
    for (CFIndex idx = 0; idx < cnt; idx++) {
        uintptr_t stack_curr = strong ? (uintptr_t)keys[idx].strong : keys[idx].weak;
        if (stack_curr == new_empty || stack_curr == new_deleted) {
            goto again;
        }
    }
    for (CFIndex idx = 0; idx < cnt; idx++) {
        uintptr_t stack_curr = strong ? (uintptr_t)keys[idx].strong : keys[idx].weak;
        if (stack_curr == empty) {
            if (strong) keys[idx].strong = (id)new_empty; else keys[idx].weak = new_empty;
        } else if (stack_curr == deleted) {
            if (strong) keys[idx].strong = (id)new_deleted; else keys[idx].weak = new_deleted;
        }
    }
    ht->bits.marker = marker;
}

static void __CFBasicHashAddValue(CFBasicHashRef ht, CFBasicHashBucket bkt, uintptr_t stack_key, uintptr_t stack_key2, uintptr_t stack_value, uintptr_t stack_value2) {
    ht->bits.mutations++;
    stack_value = __CFBasicHashImportValue(ht, stack_value);
    if (0 != ht->bits.keys_offset) {
        stack_key = __CFBasicHashImportKey(ht, stack_key);
    } else {
        stack_key = stack_value;
    }
    if (0 != ht->bits.values2_offset) {
        stack_value2 = __CFBasicHashImportValue2(ht, stack_value2);
    }
    if (0 != ht->bits.keys2_offset) {
        stack_key2 = __CFBasicHashImportKey2(ht, stack_key2);
    }
    if (CFBasicHashGetCapacity(ht) < ht->bits.used_buckets + 1) {
        __CFBasicHashRehash(ht, 1);
        bkt = ___CFBasicHashFindBucket(ht, stack_key, 0, true);
    }
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    if (bkt.weak_key == deleted) {
        ht->bits.deleted--;
    }
    if (stack_key == empty || stack_key == deleted) {
        __CFBasicHashFindNewMarker(ht, stack_key);
    }
    CFBasicHashValue *value = &(__CFBasicHashGetValues(ht)[bkt.idx]);
    if (ht->bits.strong_values) value->strong = (id)stack_value; else value->weak = stack_value;
    if (0 != ht->bits.values2_offset) {
        CFBasicHashValue *value2 = &(__CFBasicHashGetValues2(ht)[bkt.idx]);
        if (ht->bits.strong_values2) value2->strong = (id)stack_value2; else value2->weak = stack_value2;
    }
    if (0 != ht->bits.keys_offset) {
        CFBasicHashValue *key = &(__CFBasicHashGetKeys(ht)[bkt.idx]);
        if (ht->bits.strong_keys) key->strong = (id)stack_key; else key->weak = stack_key;
    }
    if (0 != ht->bits.keys2_offset) {
        CFBasicHashValue *key2 = &(__CFBasicHashGetKeys2(ht)[bkt.idx]);
        if (ht->bits.strong_keys2) key2->strong = (id)stack_key2; else key2->weak = stack_key2;
    }
    if (0 != ht->bits.counts_offset) {
        __CFBasicHashGetCounts(ht)[bkt.idx] = 1;
    }
    if (0 != ht->bits.orders_offset) {
        __CFBasicHashGetOrders(ht)[bkt.idx] = 0;
    }
    if (ht->bits.hashes_offset) {
        __CFBasicHashGetHashes(ht)[bkt.idx] = __CFBasicHashHashKey(ht, stack_key);
    }
    ht->bits.used_buckets++;
}

static void __CFBasicHashReplaceValue(CFBasicHashRef ht, CFBasicHashBucket bkt, uintptr_t stack_key, uintptr_t stack_key2, uintptr_t stack_value, uintptr_t stack_value2) {
    ht->bits.mutations++;
    stack_value = __CFBasicHashImportValue(ht, stack_value);
    if (0 != ht->bits.keys_offset) {
        stack_key = __CFBasicHashImportKey(ht, stack_key);
    } else {
        stack_key = stack_value;
    }
    if (0 != ht->bits.values2_offset) {
        stack_value2 = __CFBasicHashImportValue2(ht, stack_value2);
    }
    if (0 != ht->bits.keys2_offset) {
        stack_key2 = __CFBasicHashImportKey2(ht, stack_key2);
    }
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    if (stack_key == empty || stack_key == deleted) {
        __CFBasicHashFindNewMarker(ht, stack_key);
    }
    CFBasicHashValue *value = &(__CFBasicHashGetValues(ht)[bkt.idx]);
    uintptr_t old_value = value->weak;
    if (ht->bits.strong_values) value->strong = (id)stack_value; else value->weak = stack_value;
    __CFBasicHashEjectValue(ht, old_value);
    if (0 != ht->bits.values2_offset) {
        CFBasicHashValue *value2 = &(__CFBasicHashGetValues2(ht)[bkt.idx]);
        uintptr_t old_value2 = value2->weak;
        if (ht->bits.strong_values2) value2->strong = (id)stack_value2; else value2->weak = stack_value2;
        __CFBasicHashEjectValue2(ht, old_value2);
    }
    if (0 != ht->bits.keys_offset) {
        CFBasicHashValue *key = &(__CFBasicHashGetKeys(ht)[bkt.idx]);
        uintptr_t old_key = key->weak;
        if (ht->bits.strong_keys) key->strong = (id)stack_key; else key->weak = stack_key;
        __CFBasicHashEjectKey(ht, old_key);
    }
    if (0 != ht->bits.keys2_offset) {
        CFBasicHashValue *key2 = &(__CFBasicHashGetKeys2(ht)[bkt.idx]);
        uintptr_t old_key2 = key2->weak;
        if (ht->bits.strong_keys2) key2->strong = (id)stack_key2; else key2->weak = stack_key2;
        __CFBasicHashEjectKey2(ht, old_key2);
    }
}

static void __CFBasicHashRemoveValue(CFBasicHashRef ht, CFBasicHashBucket bkt, uintptr_t stack_key, uintptr_t stack_key2) {
    ht->bits.mutations++;
    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *value = &(__CFBasicHashGetValues(ht)[bkt.idx]);
    uintptr_t old_value = value->weak;
    if (ht->bits.strong_values) value->strong = (id)deleted; else value->weak = deleted;
    __CFBasicHashEjectValue(ht, old_value);
    if (0 != ht->bits.values2_offset) {
        CFBasicHashValue *value2 = &(__CFBasicHashGetValues2(ht)[bkt.idx]);
        uintptr_t old_value2 = value2->weak;
        if (ht->bits.strong_values2) value2->strong = (id)deleted; else value2->weak = deleted;
        __CFBasicHashEjectValue2(ht, old_value2);
    }
    if (0 != ht->bits.keys_offset) {
        CFBasicHashValue *key = &(__CFBasicHashGetKeys(ht)[bkt.idx]);
        uintptr_t old_key = key->weak;
        if (ht->bits.strong_keys) key->strong = (id)deleted; else key->weak = deleted;
        __CFBasicHashEjectKey(ht, old_key);
    }
    if (0 != ht->bits.keys2_offset) {
        CFBasicHashValue *key2 = &(__CFBasicHashGetKeys2(ht)[bkt.idx]);
        uintptr_t old_key2 = key2->weak;
        if (ht->bits.strong_keys2) key2->strong = (id)deleted; else key2->weak = deleted;
        __CFBasicHashEjectKey2(ht, old_key2);
    }
    if (0 != ht->bits.counts_offset) {
        __CFBasicHashGetCounts(ht)[bkt.idx] = 0;
    }
    if (0 != ht->bits.orders_offset) {
        __CFBasicHashGetOrders(ht)[bkt.idx] = 0;
    }
    if (ht->bits.hashes_offset) {
        __CFBasicHashGetHashes(ht)[bkt.idx] = 0;
    }
    ht->bits.used_buckets--;
    ht->bits.deleted++;
    Boolean do_shrink = false;
    if (ht->bits.fast_grow) { // == slow shrink
        do_shrink = (5 < ht->bits.num_buckets_idx && ht->bits.used_buckets < __CFBasicHashGetCapacityForNumBuckets(ht, ht->bits.num_buckets_idx - 5));
    } else {
        do_shrink = (2 < ht->bits.num_buckets_idx && ht->bits.used_buckets < __CFBasicHashGetCapacityForNumBuckets(ht, ht->bits.num_buckets_idx - 2));
    }
    if (do_shrink) {
        __CFBasicHashRehash(ht, -1);
        return;
    }
    do_shrink = (0 == ht->bits.deleted); // .deleted roll-over
    CFIndex num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    do_shrink = do_shrink || ((20 <= num_buckets) && (num_buckets / 4 <= ht->bits.deleted));
    if (do_shrink) {
        __CFBasicHashRehash(ht, 0);
    }
}

__private_extern__ void CFBasicHashAddValue(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t stack_value) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    CFBasicHashBucket bkt = __CFBasicHashFindBucket(ht, stack_key);
    if (0 < bkt.count) {
        ht->bits.mutations++;
        if (0 != ht->bits.counts_offset) {
            __CFBasicHashGetCounts(ht)[bkt.idx]++;
        }
    } else {
        __CFBasicHashAddValue(ht, bkt, stack_key, 0, stack_value, 0);
    }
}

__private_extern__ void CFBasicHashReplaceValue(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t stack_value) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    CFBasicHashBucket bkt = __CFBasicHashFindBucket(ht, stack_key);
    if (0 < bkt.count) {
        __CFBasicHashReplaceValue(ht, bkt, stack_key, 0, stack_value, 0);
    }
}

__private_extern__ void CFBasicHashSetValue(CFBasicHashRef ht, uintptr_t stack_key, uintptr_t stack_value) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    CFBasicHashBucket bkt = __CFBasicHashFindBucket(ht, stack_key);
    if (0 < bkt.count) {
        __CFBasicHashReplaceValue(ht, bkt, stack_key, 0, stack_value, 0);
    } else {
        __CFBasicHashAddValue(ht, bkt, stack_key, 0, stack_value, 0);
    }
}

__private_extern__ CFIndex CFBasicHashRemoveValue(CFBasicHashRef ht, uintptr_t stack_key) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    CFBasicHashBucket bkt = __CFBasicHashFindBucket(ht, stack_key);
    if (1 < bkt.count) {
        ht->bits.mutations++;
        if (0 != ht->bits.counts_offset) {
            __CFBasicHashGetCounts(ht)[bkt.idx]--;
        }
    } else if (0 < bkt.count) {
        __CFBasicHashRemoveValue(ht, bkt, stack_key, 0);
    }
    return bkt.count;
}

__private_extern__ void CFBasicHashRemoveAllValues(CFBasicHashRef ht) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    if (0 == ht->bits.num_buckets_idx) return;
    __CFBasicHashDrain(ht, false);
}

__private_extern__ size_t CFBasicHashGetSize(CFBasicHashRef ht, Boolean total) {
    size_t size = sizeof(struct __CFBasicHash);
    if (0 != ht->bits.values2_offset) size += sizeof(CFBasicHashValue *);
    if (0 != ht->bits.keys_offset) size += sizeof(CFBasicHashValue *);
    if (0 != ht->bits.keys2_offset) size += sizeof(CFBasicHashValue *);
    if (0 != ht->bits.counts_offset) size += sizeof(uintptr_t *);
    if (0 != ht->bits.orders_offset) size += sizeof(uintptr_t *);
    if (0 != ht->bits.hashes_offset) size += sizeof(uintptr_t *);
    if (total) {
        CFIndex num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
        if (0 < num_buckets) {
            size += malloc_size(__CFBasicHashGetValues(ht));
            if (0 != ht->bits.values2_offset) size += malloc_size(__CFBasicHashGetValues2(ht));
            if (0 != ht->bits.keys_offset) size += malloc_size(__CFBasicHashGetKeys(ht));
            if (0 != ht->bits.keys2_offset) size += malloc_size(__CFBasicHashGetKeys2(ht));
            if (0 != ht->bits.counts_offset) size += malloc_size(__CFBasicHashGetCounts(ht));
            if (0 != ht->bits.orders_offset) size += malloc_size(__CFBasicHashGetOrders(ht));
            if (0 != ht->bits.hashes_offset) size += malloc_size(__CFBasicHashGetHashes(ht));
            size += malloc_size((void *)ht->callbacks);
        }
    }
    return size;
}

__private_extern__ CFStringRef CFBasicHashCopyDescription(CFBasicHashRef ht, Boolean detailed, CFStringRef prefix, CFStringRef entryPrefix, Boolean describeElements) {
    CFMutableStringRef result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(result, NULL, CFSTR("%@{type = %s %s%s, count = %ld,\n"), prefix, (CFBasicHashIsMutable(ht) ? "mutable" : "immutable"), ((0 != ht->bits.counts_offset) ? "multi" : ""), ((0 != ht->bits.keys_offset) ? "dict" : "set"), CFBasicHashGetCount(ht));
    if (detailed) {
        const char *cb_type = "custom";
        if (&CFBasicHashNullCallbacks == ht->callbacks) {
            cb_type = "null";
        } else if (&CFBasicHashStandardCallbacks == ht->callbacks) {
            cb_type = "standard";
        }
        CFStringAppendFormat(result, NULL, CFSTR("%@hash cache = %s, strong values = %s, strong keys = %s, cb = %s,\n"), prefix, ((0 != ht->bits.hashes_offset) ? "yes" : "no"), (ht->bits.strong_values ? "yes" : "no"), (ht->bits.strong_keys ? "yes" : "no"), cb_type);
        CFStringAppendFormat(result, NULL, CFSTR("%@num bucket index = %d, num buckets = %ld, capacity = %ld, num buckets used = %ld,\n"), prefix, ht->bits.num_buckets_idx, CFBasicHashGetNumBuckets(ht), CFBasicHashGetCapacity(ht), ht->bits.used_buckets);
        uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
        CFStringAppendFormat(result, NULL, CFSTR("%@empty marker = 0x%lx, deleted marker = 0x%lx, finalized = %s,\n"), prefix, empty, deleted, (ht->bits.finalized ? "yes" : "no"));
        CFStringAppendFormat(result, NULL, CFSTR("%@num mutations = %ld, num deleted = %ld, size = %ld, total size = %ld,\n"), prefix, ht->bits.mutations, ht->bits.deleted, CFBasicHashGetSize(ht, false), CFBasicHashGetSize(ht, true));
        CFStringAppendFormat(result, NULL, CFSTR("%@values ptr = %p, keys ptr = %p, counts ptr = %p, hashes ptr = %p,\n"), prefix, __CFBasicHashGetValues(ht), ((0 != ht->bits.keys_offset) ? __CFBasicHashGetKeys(ht) : NULL), ((0 != ht->bits.counts_offset) ? __CFBasicHashGetCounts(ht) : NULL), ((0 != ht->bits.hashes_offset) ? __CFBasicHashGetHashes(ht) : NULL));
    }
    CFStringAppendFormat(result, NULL, CFSTR("%@entries =>\n"), prefix);
    CFBasicHashApply(ht, ^(CFBasicHashBucket bkt) {
            CFStringRef vDesc = NULL, kDesc = NULL;
            CFBasicHashCallbackType cb = ht->callbacks->func;
            if (!describeElements) cb = __CFBasicHashNullCallback;
            vDesc = (CFStringRef)cb(ht, kCFBasicHashCallbackOpDescribeValue, bkt.weak_value, 0, ht->callbacks);
            if (0 != ht->bits.keys_offset) {
                kDesc = (CFStringRef)cb(ht, kCFBasicHashCallbackOpDescribeKey, bkt.weak_key, 0, ht->callbacks);
            }
            if ((0 != ht->bits.keys_offset) && (0 != ht->bits.counts_offset)) {
                CFStringAppendFormat(result, NULL, CFSTR("%@%ld : %@ = %@ (%ld)\n"), entryPrefix, bkt.idx, kDesc, vDesc, bkt.count);
            } else if (0 != ht->bits.keys_offset) {
                CFStringAppendFormat(result, NULL, CFSTR("%@%ld : %@ = %@\n"), entryPrefix, bkt.idx, kDesc, vDesc);
            } else if (0 != ht->bits.counts_offset) {
                CFStringAppendFormat(result, NULL, CFSTR("%@%ld : %@ (%ld)\n"), entryPrefix, bkt.idx, vDesc, bkt.count);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("%@%ld : %@\n"), entryPrefix, bkt.idx, vDesc);
            }
            if (kDesc) CFRelease(kDesc);
            if (vDesc) CFRelease(vDesc);
            return (Boolean)true;
        });
    CFStringAppendFormat(result, NULL, CFSTR("%@}\n"), prefix);
    return result;
}

__private_extern__ void CFBasicHashShow(CFBasicHashRef ht) {
    CFStringRef str = CFBasicHashCopyDescription(ht, true, CFSTR(""), CFSTR("\t"), false);
    CFShow(str);
    CFRelease(str);
}

__private_extern__ Boolean __CFBasicHashEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFBasicHashRef ht1 = (CFBasicHashRef)cf1;
    CFBasicHashRef ht2 = (CFBasicHashRef)cf2;
//#warning this used to require that the key and value equal callbacks were pointer identical
    return CFBasicHashesAreEqual(ht1, ht2);
}

__private_extern__ CFHashCode __CFBasicHashHash(CFTypeRef cf) {
    CFBasicHashRef ht = (CFBasicHashRef)cf;
    return CFBasicHashGetCount(ht);
}

__private_extern__ CFStringRef __CFBasicHashCopyDescription(CFTypeRef cf) {
    CFBasicHashRef ht = (CFBasicHashRef)cf;
    CFStringRef desc = CFBasicHashCopyDescription(ht, false, CFSTR(""), CFSTR("\t"), true);
    CFStringRef result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFBasicHash %p [%p]>%@"), cf, CFGetAllocator(cf), desc);
    CFRelease(desc);
    return result;
}

__private_extern__ void __CFBasicHashDeallocate(CFTypeRef cf) {
    CFBasicHashRef ht = (CFBasicHashRef)cf;
    if (ht->bits.finalized) HALT;
    ht->bits.finalized = 1;
    __CFBasicHashDrain(ht, true);
#if ENABLE_MEMORY_COUNTERS
    OSAtomicAdd64Barrier(-1, &__CFBasicHashTotalCount);
    OSAtomicAdd32Barrier(-1, &__CFBasicHashSizes[ht->bits.num_buckets_idx]);
#endif
}

static CFTypeID __kCFBasicHashTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFBasicHashClass = {
    _kCFRuntimeScannedObject,
    "CFBasicHash",
    NULL,        // init
    NULL,        // copy
    __CFBasicHashDeallocate,
    __CFBasicHashEqual,
    __CFBasicHashHash,
    NULL,        //
    __CFBasicHashCopyDescription
};

__private_extern__ CFTypeID CFBasicHashGetTypeID(void) {
    if (_kCFRuntimeNotATypeID == __kCFBasicHashTypeID) __kCFBasicHashTypeID = _CFRuntimeRegisterClass(&__CFBasicHashClass);
    return __kCFBasicHashTypeID;
}

CFBasicHashRef CFBasicHashCreate(CFAllocatorRef allocator, CFOptionFlags flags, const CFBasicHashCallbacks *cb) {
    size_t size = sizeof(struct __CFBasicHash) - sizeof(CFRuntimeBase);
    if (flags & kCFBasicHashHasValues2) size += sizeof(CFBasicHashValue *); // values2
    if (flags & kCFBasicHashHasKeys) size += sizeof(CFBasicHashValue *); // keys
    if (flags & kCFBasicHashHasKeys2) size += sizeof(CFBasicHashValue *); // keys2
    if (flags & kCFBasicHashHasCounts) size += sizeof(uintptr_t *); // counts
    if (flags & kCFBasicHashHasOrder) size += sizeof(uintptr_t *); // order
    if (flags & kCFBasicHashHasHashCache) size += sizeof(uintptr_t *); // hashes
    CFBasicHashRef ht = (CFBasicHashRef)_CFRuntimeCreateInstance(allocator, CFBasicHashGetTypeID(), size, NULL);
    if (NULL == ht) HALT;

    ht->bits.finalized = 0;
    ht->bits.strong_values = (flags & kCFBasicHashStrongValues) ? 1 : 0;
    ht->bits.strong_values2 = 0;
    ht->bits.strong_keys = (flags & kCFBasicHashStrongKeys) ? 1 : 0;
    ht->bits.strong_keys2 = 0;
    ht->bits.hash_style = (flags >> 13) & 0x3;
    ht->bits.fast_grow = (flags & kCFBasicHashAggressiveGrowth) ? 1 : 0;
    ht->bits.__0 = 0;
    ht->bits.num_buckets_idx = 0;
    ht->bits.used_buckets = 0;
    ht->bits.marker = 0;
    ht->bits.deleted = 0;
    ht->bits.mutations = 1;

    uint64_t offset = 1;
    ht->bits.values2_offset = 0;
    ht->bits.keys_offset = (flags & kCFBasicHashHasKeys) ? offset++ : 0;
    ht->bits.keys2_offset = 0;
    ht->bits.counts_offset = (flags & kCFBasicHashHasCounts) ? offset++ : 0;
    ht->bits.orders_offset = 0;
    ht->bits.hashes_offset = (flags & kCFBasicHashHasHashCache) ? offset++ : 0;

    __AssignWithWriteBarrier(&ht->callbacks, cb);
    for (CFIndex idx = 0; idx < offset; idx++) {
        ht->pointers[idx] = NULL;
    }

#if ENABLE_MEMORY_COUNTERS
    int64_t size_now = OSAtomicAdd64Barrier((int64_t) CFBasicHashGetSize(ht, true), & __CFBasicHashTotalSize);
    while (__CFBasicHashPeakSize < size_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakSize, size_now, & __CFBasicHashPeakSize));
    int64_t count_now = OSAtomicAdd64Barrier(1, & __CFBasicHashTotalCount);
    while (__CFBasicHashPeakCount < count_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakCount, count_now, & __CFBasicHashPeakCount));
    OSAtomicAdd32Barrier(1, &__CFBasicHashSizes[ht->bits.num_buckets_idx]);
#endif

    return ht;
}

CFBasicHashRef CFBasicHashCreateCopy(CFAllocatorRef allocator, CFBasicHashRef src_ht) {
    size_t size = CFBasicHashGetSize(src_ht, false) - sizeof(CFRuntimeBase);
    CFBasicHashRef ht = (CFBasicHashRef)_CFRuntimeCreateInstance(allocator, CFBasicHashGetTypeID(), size, NULL);
    if (NULL == ht) HALT;

    memmove((uint8_t *)ht + sizeof(CFRuntimeBase), (uint8_t *)src_ht + sizeof(CFRuntimeBase), sizeof(ht->bits));
    if (kCFUseCollectableAllocator && !CF_IS_COLLECTABLE_ALLOCATOR(allocator)) {
        ht->bits.strong_values = 0;
        ht->bits.strong_values2 = 0;
        ht->bits.strong_keys = 0;
        ht->bits.strong_keys2 = 0;
    }
    ht->bits.finalized = 0;
    ht->bits.mutations = 1;
    __AssignWithWriteBarrier(&ht->callbacks, src_ht->callbacks->func(ht, kCFBasicHashCallbackOpCopyCallbacks, (uintptr_t)allocator, 0, src_ht->callbacks));
    if (NULL == ht->callbacks) HALT;

    CFIndex new_num_buckets = __CFBasicHashTableSizes[ht->bits.num_buckets_idx];
    if (0 == new_num_buckets) {
#if ENABLE_MEMORY_COUNTERS
        int64_t size_now = OSAtomicAdd64Barrier((int64_t) CFBasicHashGetSize(ht, true), & __CFBasicHashTotalSize);
        while (__CFBasicHashPeakSize < size_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakSize, size_now, & __CFBasicHashPeakSize));
        int64_t count_now = OSAtomicAdd64Barrier(1, & __CFBasicHashTotalCount);
        while (__CFBasicHashPeakCount < count_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakCount, count_now, & __CFBasicHashPeakCount));
        OSAtomicAdd32Barrier(1, &__CFBasicHashSizes[ht->bits.num_buckets_idx]);
#endif
        return ht;
    }

    CFBasicHashValue *new_values = NULL, *new_values2 = NULL, *new_keys = NULL, *new_keys2 = NULL;
    uintptr_t *new_counts = NULL, *new_orders = NULL, *new_hashes = NULL;

    if (0 < new_num_buckets) {
        new_values = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_values);
        __SetLastAllocationEventName(new_values, "CFBasicHash (value-store)");
        if (0 != ht->bits.values2_offset) {
            new_values2 = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_values2);
            __SetLastAllocationEventName(new_values2, "CFBasicHash (value2-store)");
        }
        if (0 != ht->bits.keys_offset) {
            new_keys = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_keys);
            __SetLastAllocationEventName(new_keys, "CFBasicHash (key-store)");
        }
        if (0 != ht->bits.keys2_offset) {
            new_keys2 = (CFBasicHashValue *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(CFBasicHashValue), ht->bits.strong_keys2);
            __SetLastAllocationEventName(new_keys2, "CFBasicHash (key2-store)");
        }
        if (0 != ht->bits.counts_offset) {
            new_counts = (uintptr_t *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(uintptr_t), false);
            __SetLastAllocationEventName(new_counts, "CFBasicHash (count-store)");
        }
        if (0 != ht->bits.orders_offset) {
            new_orders = (uintptr_t *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(uintptr_t), false);
            __SetLastAllocationEventName(new_orders, "CFBasicHash (order-store)");
        }
        if (0 != ht->bits.hashes_offset) {
            new_hashes = (uintptr_t *)__CFBasicHashAllocateMemory(ht, new_num_buckets, sizeof(uintptr_t), false);
            __SetLastAllocationEventName(new_hashes, "CFBasicHash (hash-store)");
        }
    }

    uintptr_t empty = ((uintptr_t)ht->bits.marker << __CFBasicHashMarkerShift), deleted = ~empty;
    CFBasicHashValue *old_values = NULL, *old_values2 = NULL, *old_keys = NULL, *old_keys2 = NULL;
    uintptr_t *old_counts = NULL, *old_orders = NULL, *old_hashes = NULL;
    old_values = __CFBasicHashGetValues(src_ht);
    if (0 != src_ht->bits.values2_offset) {
        old_values2 = __CFBasicHashGetValues2(src_ht);
    }
    if (0 != src_ht->bits.keys_offset) {
        old_keys = __CFBasicHashGetKeys(src_ht);
    }
    if (0 != src_ht->bits.keys2_offset) {
        old_keys2 = __CFBasicHashGetKeys2(src_ht);
    }
    if (0 != src_ht->bits.counts_offset) {
        old_counts = __CFBasicHashGetCounts(src_ht);
    }
    if (0 != src_ht->bits.orders_offset) {
        old_orders = __CFBasicHashGetOrders(src_ht);
    }
    if (0 != src_ht->bits.hashes_offset) {
        old_hashes = __CFBasicHashGetHashes(src_ht);
    }

    CFBasicHashValue *keys = old_keys ? old_keys : old_values;
    for (CFIndex idx = 0; idx < new_num_buckets; idx++) {
        uintptr_t stack_key = keys[idx].weak;
        if (stack_key != empty && stack_key != deleted) {
            uintptr_t stack_value = __CFBasicHashImportValue(ht, old_values[idx].weak);
            if (ht->bits.strong_values) new_values[idx].strong = (id)stack_value; else new_values[idx].weak = stack_value;
            if (new_values2) {
                uintptr_t stack_value2 = __CFBasicHashImportValue2(ht, old_values2[idx].weak);
                if (ht->bits.strong_values2) new_values2[idx].strong = (id)stack_value2; else new_values2[idx].weak = stack_value2;
            }
            if (new_keys) {
                uintptr_t stack_key = __CFBasicHashImportKey(ht, old_keys[idx].weak);
                if (ht->bits.strong_keys) new_keys[idx].strong = (id)stack_key; else new_keys[idx].weak = stack_key;
            }
            if (new_keys2) {
                uintptr_t stack_key2 = __CFBasicHashImportKey2(ht, old_keys2[idx].weak);
                if (ht->bits.strong_keys2) new_keys2[idx].strong = (id)stack_key2; else new_keys2[idx].weak = stack_key2;
            }
        } else {
            if (ht->bits.strong_values) new_values[idx].strong = (id)stack_key; else new_values[idx].weak = stack_key;
            if (new_values2) {
                if (ht->bits.strong_values2) new_values2[idx].strong = (id)stack_key; else new_values2[idx].weak = stack_key;
            }
            if (new_keys) {
                if (ht->bits.strong_keys) new_keys[idx].strong = (id)stack_key; else new_keys[idx].weak = stack_key;
            }
            if (new_keys2) {
                if (ht->bits.strong_keys2) new_keys2[idx].strong = (id)stack_key; else new_keys2[idx].weak = stack_key;
            }
        }
    }
    if (new_counts) memmove(new_counts, old_counts, new_num_buckets * sizeof(uintptr_t));
    if (new_orders) memmove(new_orders, old_orders, new_num_buckets * sizeof(uintptr_t));
    if (new_hashes) memmove(new_hashes, old_hashes, new_num_buckets * sizeof(uintptr_t));

    __CFBasicHashSetValues(ht, new_values);
    if (new_values2) {
        __CFBasicHashSetValues2(ht, new_values2);
    }
    if (new_keys) {
        __CFBasicHashSetKeys(ht, new_keys);
    }
    if (new_keys2) {
        __CFBasicHashSetKeys2(ht, new_keys2);
    }
    if (new_counts) {
        __CFBasicHashSetCounts(ht, new_counts);
    }
    if (new_orders) {
        __CFBasicHashSetOrders(ht, new_orders);
    }
    if (new_hashes) {
        __CFBasicHashSetHashes(ht, new_hashes);
    }

#if ENABLE_MEMORY_COUNTERS
    int64_t size_now = OSAtomicAdd64Barrier((int64_t) CFBasicHashGetSize(ht, true), & __CFBasicHashTotalSize);
    while (__CFBasicHashPeakSize < size_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakSize, size_now, & __CFBasicHashPeakSize));
    int64_t count_now = OSAtomicAdd64Barrier(1, & __CFBasicHashTotalCount);
    while (__CFBasicHashPeakCount < count_now && !OSAtomicCompareAndSwap64Barrier(__CFBasicHashPeakCount, count_now, & __CFBasicHashPeakCount));
    OSAtomicAdd32Barrier(1, &__CFBasicHashSizes[ht->bits.num_buckets_idx]);
#endif

    return ht;
}

void _CFbhx588461(CFBasicHashRef ht, Boolean growth) {
    if (!CFBasicHashIsMutable(ht)) HALT;
    if (ht->bits.finalized) HALT;
    ht->bits.fast_grow = growth ? 1 : 0;
}

