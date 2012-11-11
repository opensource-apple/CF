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
/*	CFData.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFData.h>
#include "CFUtilities.h"
#include "CFInternal.h"
#include <string.h>

struct __CFData {
    CFRuntimeBase _base;
    CFIndex _length;	/* number of bytes */
    CFIndex _capacity;	/* maximum number of bytes */
    CFAllocatorRef _bytesDeallocator;	/* used only for immutable; if NULL, no deallocation */
    uint8_t *_bytes;
};

/* Bits 3-2 are used for mutability variation */

CF_INLINE UInt32 __CFMutableVariety(const void *cf) {
    return __CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_info, 3, 2);
}

CF_INLINE void __CFSetMutableVariety(void *cf, UInt32 v) {
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_info, 3, 2, v);
}

CF_INLINE UInt32 __CFMutableVarietyFromFlags(UInt32 flags) {
    return __CFBitfieldGetValue(flags, 1, 0);
}

#define __CFGenericValidateMutabilityFlags(flags) \
    CFAssert2(__CFMutableVarietyFromFlags(flags) != 0x2, __kCFLogAssertion, "%s(): flags 0x%x do not correctly specify the mutable variety", __PRETTY_FUNCTION__, flags);

CF_INLINE CFIndex __CFDataLength(CFDataRef data) {
    return data->_length;
}

CF_INLINE void __CFDataSetLength(CFMutableDataRef data, CFIndex v) {
    /* for a CFData, _bytesUsed == _length */
}

CF_INLINE CFIndex __CFDataCapacity(CFDataRef data) {
    return data->_capacity;
}

CF_INLINE void __CFDataSetCapacity(CFMutableDataRef data, CFIndex v) {
    /* for a CFData, _bytesNum == _capacity */
}

CF_INLINE CFIndex __CFDataNumBytesUsed(CFDataRef data) {
    return data->_length;
}

CF_INLINE void __CFDataSetNumBytesUsed(CFMutableDataRef data, CFIndex v) {
    data->_length = v;
}

CF_INLINE CFIndex __CFDataNumBytes(CFDataRef data) {
    return data->_capacity;
}

CF_INLINE void __CFDataSetNumBytes(CFMutableDataRef data, CFIndex v) {
    data->_capacity = v;
}

CF_INLINE CFIndex __CFDataRoundUpCapacity(CFIndex capacity) {
    if (capacity < 16) return 16;
// CF: quite probably, this doubling should slow as the data gets larger and larger; should not use strict doubling
    return (1 << (CFLog2(capacity) + 1));
}

CF_INLINE CFIndex __CFDataNumBytesForCapacity(CFIndex capacity) {
    return capacity;
}

#if defined(DEBUG)
CF_INLINE void __CFDataValidateRange(CFDataRef data, CFRange range, const char *func) {
    CFAssert2(0 <= range.location && range.location <= __CFDataLength(data), __kCFLogAssertion, "%s(): range.location index (%d) out of bounds", func, range.location);
    CFAssert2(0 <= range.length, __kCFLogAssertion, "%s(): length (%d) cannot be less than zero", func, range.length);
    CFAssert2(range.location + range.length <= __CFDataLength(data), __kCFLogAssertion, "%s(): ending index (%d) out of bounds", func, range.location + range.length);
}
#else
#define __CFDataValidateRange(a,r,f)
#endif

static bool __CFDataEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFDataRef data1 = (CFDataRef)cf1;
    CFDataRef data2 = (CFDataRef)cf2;
    CFIndex length;
    length = __CFDataLength(data1);
    if (length != __CFDataLength(data2)) return false;
    return 0 == memcmp(data1->_bytes, data2->_bytes, length);
}

static CFHashCode __CFDataHash(CFTypeRef cf) {
    CFDataRef data = (CFDataRef)cf;
    return CFHashBytes(data->_bytes, __CFMin(__CFDataLength(data), 16));
}

static CFStringRef __CFDataCopyDescription(CFTypeRef cf) {
    CFDataRef data = (CFDataRef)cf;
    CFMutableStringRef result;
    CFIndex idx;
    CFIndex len;
    const uint8_t *bytes;
    len = __CFDataLength(data);
    bytes = data->_bytes;
    result = CFStringCreateMutable(CFGetAllocator(data), 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFData %p [%p]>{length = %u, capacity = %u, bytes = 0x"), cf, CFGetAllocator(data), len, __CFDataCapacity(data));
    if (24 < len) {
        for (idx = 0; idx < 16; idx += 4) {
	    CFStringAppendFormat(result, NULL, CFSTR("%02x%02x%02x%02x"), bytes[idx], bytes[idx + 1], bytes[idx + 2], bytes[idx + 3]);
	}
        CFStringAppend(result, CFSTR(" ... "));
        for (idx = len - 8; idx < len; idx += 4) {
	    CFStringAppendFormat(result, NULL, CFSTR("%02x%02x%02x%02x"), bytes[idx], bytes[idx + 1], bytes[idx + 2], bytes[idx + 3]);
	}
    } else {
        for (idx = 0; idx < len; idx++) {
	    CFStringAppendFormat(result, NULL, CFSTR("%02x"), bytes[idx]);
	}
    }
    CFStringAppend(result, CFSTR("}"));
    return result;
}

enum {
    kCFImmutable = 0x0,		/* unchangable and fixed capacity; default */
    kCFMutable = 0x1,		/* changeable and variable capacity */
    kCFFixedMutable = 0x3	/* changeable and fixed capacity */
};

static void __CFDataDeallocate(CFTypeRef cf) {
    CFMutableDataRef data = (CFMutableDataRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(data);
    switch (__CFMutableVariety(data)) {
    case kCFMutable:
	CFAllocatorDeallocate(allocator, data->_bytes);
	break;
    case kCFFixedMutable:
	break;
    case kCFImmutable:
	if (NULL != data->_bytesDeallocator) {
	    CFAllocatorDeallocate(data->_bytesDeallocator, data->_bytes);
	    CFRelease(data->_bytesDeallocator);
	}
	break;
    }
}

static CFTypeID __kCFDataTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFDataClass = {
    0,
    "CFData",
    NULL,	// init
    NULL,	// copy
    __CFDataDeallocate,
    (void *)__CFDataEqual,
    __CFDataHash,
    NULL,	// 
    __CFDataCopyDescription
};

__private_extern__ void __CFDataInitialize(void) {
    __kCFDataTypeID = _CFRuntimeRegisterClass(&__CFDataClass);
}

CFTypeID CFDataGetTypeID(void) {
    return __kCFDataTypeID;
}

// NULL bytesDeallocator to this function does not mean the default allocator, it means
// that there should be no deallocator, and the bytes should be copied.
static CFMutableDataRef __CFDataInit(CFAllocatorRef allocator, CFOptionFlags flags, CFIndex capacity, const uint8_t *bytes, CFIndex length, CFAllocatorRef bytesDeallocator) {
    CFMutableDataRef memory;
    CFIndex size;
    __CFGenericValidateMutabilityFlags(flags);
    CFAssert2(0 <= capacity, __kCFLogAssertion, "%s(): capacity (%d) cannot be less than zero", __PRETTY_FUNCTION__, capacity);
    CFAssert3(kCFFixedMutable != __CFMutableVarietyFromFlags(flags) || length <= capacity, __kCFLogAssertion, "%s(): for kCFFixedMutable type, capacity (%d) must be greater than or equal to number of initial elements (%d)", __PRETTY_FUNCTION__, capacity, length);
    CFAssert2(0 <= length, __kCFLogAssertion, "%s(): length (%d) cannot be less than zero", __PRETTY_FUNCTION__, length);
    size = sizeof(struct __CFData) - sizeof(CFRuntimeBase);
    if (__CFMutableVarietyFromFlags(flags) != kCFMutable && (bytesDeallocator == NULL)) {
	size += sizeof(uint8_t) * __CFDataNumBytesForCapacity(capacity);
    }
    if (__CFMutableVarietyFromFlags(flags) != kCFMutable) {
	size += sizeof(uint8_t) * 15;	// for 16-byte alignment fixup
    }
    memory = (CFMutableDataRef)_CFRuntimeCreateInstance(allocator, __kCFDataTypeID, size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    __CFDataSetNumBytesUsed(memory, 0);
    __CFDataSetLength(memory, 0);
    switch (__CFMutableVarietyFromFlags(flags)) {
    case kCFMutable:
	__CFDataSetCapacity(memory, __CFDataRoundUpCapacity(1));
	__CFDataSetNumBytes(memory, __CFDataNumBytesForCapacity(__CFDataRoundUpCapacity(1)));
	// assume that allocators give 16-byte aligned memory back -- it is their responsibility
	memory->_bytes = CFAllocatorAllocate(allocator, __CFDataNumBytes(memory) * sizeof(uint8_t), 0);
	if (__CFOASafe) __CFSetLastAllocationEventName(memory->_bytes, "CFData (store)");
	if (NULL == memory->_bytes) {
	    CFRelease(memory);
	    return NULL;
	}
	memory->_bytesDeallocator = NULL;
	__CFSetMutableVariety(memory, kCFMutable);
	CFDataReplaceBytes(memory, CFRangeMake(0, 0), bytes, length);
	break;
    case kCFFixedMutable:
	/* Don't round up capacity */
	__CFDataSetCapacity(memory, capacity);
	__CFDataSetNumBytes(memory, __CFDataNumBytesForCapacity(capacity));
	memory->_bytes = (uint8_t *)((uintptr_t)((int8_t *)memory + sizeof(struct __CFData) + 15) & ~0xF);	// 16-byte align
	memory->_bytesDeallocator = NULL;
	__CFSetMutableVariety(memory, kCFFixedMutable);
	CFDataReplaceBytes(memory, CFRangeMake(0, 0), bytes, length);
	break;
    case kCFImmutable:
	/* Don't round up capacity */
	__CFDataSetCapacity(memory, capacity);
	__CFDataSetNumBytes(memory, __CFDataNumBytesForCapacity(capacity));
	if (bytesDeallocator != NULL) {
	    memory->_bytes = (uint8_t *)bytes;
	    memory->_bytesDeallocator = CFRetain(bytesDeallocator);
	    __CFDataSetNumBytesUsed(memory, length);
	    __CFDataSetLength(memory, length);
	} else {
	    memory->_bytes = (uint8_t *)((uintptr_t)((int8_t *)memory + sizeof(struct __CFData) + 15) & ~0xF);	// 16-byte align
	    memory->_bytesDeallocator = NULL;
	    __CFSetMutableVariety(memory, kCFFixedMutable);
	    CFDataReplaceBytes(memory, CFRangeMake(0, 0), bytes, length);
	}
	break;
    }
    __CFSetMutableVariety(memory, __CFMutableVarietyFromFlags(flags));
    return memory;
}

CFDataRef CFDataCreate(CFAllocatorRef allocator, const uint8_t *bytes, CFIndex length) {
    return __CFDataInit(allocator, kCFImmutable, length, bytes, length, NULL);
}

CFDataRef CFDataCreateWithBytesNoCopy(CFAllocatorRef allocator, const uint8_t *bytes, CFIndex length, CFAllocatorRef bytesDeallocator) {
    CFAssert1((0 == length || bytes != NULL), __kCFLogAssertion, "%s(): bytes pointer cannot be NULL if length is non-zero", __PRETTY_FUNCTION__);
    if (NULL == bytesDeallocator) bytesDeallocator = __CFGetDefaultAllocator();
    if (!_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	bytesDeallocator = NULL;
    }
    return __CFDataInit(allocator, kCFImmutable, length, bytes, length, bytesDeallocator);
}

CFDataRef CFDataCreateCopy(CFAllocatorRef allocator, CFDataRef data) {
    CFIndex length = CFDataGetLength(data);
    return __CFDataInit(allocator, kCFImmutable, length, CFDataGetBytePtr(data), length, NULL);
}

CFMutableDataRef CFDataCreateMutable(CFAllocatorRef allocator, CFIndex capacity) {
    return __CFDataInit(allocator, (0 == capacity) ? kCFMutable : kCFFixedMutable, capacity, NULL, 0, NULL);
}

CFMutableDataRef CFDataCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFDataRef data) {
    return __CFDataInit(allocator, (0 == capacity) ? kCFMutable : kCFFixedMutable, capacity, CFDataGetBytePtr(data), CFDataGetLength(data), NULL);
}

CFIndex CFDataGetLength(CFDataRef data) {
    CF_OBJC_FUNCDISPATCH0(__kCFDataTypeID, CFIndex, data, "length");
    __CFGenericValidateType(data, __kCFDataTypeID);
    return __CFDataLength(data);
}

const uint8_t *CFDataGetBytePtr(CFDataRef data) {
    CF_OBJC_FUNCDISPATCH0(__kCFDataTypeID, const uint8_t *, data, "bytes");
    __CFGenericValidateType(data, __kCFDataTypeID);
    return data->_bytes;
}

uint8_t *CFDataGetMutableBytePtr(CFMutableDataRef data) {
    CF_OBJC_FUNCDISPATCH0(__kCFDataTypeID, uint8_t *, data, "mutableBytes");
    CFAssert1(__CFMutableVariety(data) == kCFMutable || __CFMutableVariety(data) == kCFFixedMutable, __kCFLogAssertion, "%s(): data is immutable", __PRETTY_FUNCTION__);
    return data->_bytes;
}

void CFDataGetBytes(CFDataRef data, CFRange range, uint8_t *buffer) {
    CF_OBJC_FUNCDISPATCH2(__kCFDataTypeID, void, data, "getBytes:range:", buffer, range);
    memmove(buffer, data->_bytes + range.location, range.length);
}

static void __CFDataGrow(CFMutableDataRef data, CFIndex numNewValues) {
    CFIndex oldLength = __CFDataLength(data);
    CFIndex capacity = __CFDataRoundUpCapacity(oldLength + numNewValues);
    __CFDataSetCapacity(data, capacity);
    __CFDataSetNumBytes(data, __CFDataNumBytesForCapacity(capacity));
    data->_bytes = CFAllocatorReallocate(CFGetAllocator(data), data->_bytes, __CFDataNumBytes(data) * sizeof(uint8_t), 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(data->_bytes, "CFData (store)");
    if (NULL == data->_bytes) HALT;
}

void CFDataSetLength(CFMutableDataRef data, CFIndex length) {
    CFIndex len;
    CF_OBJC_FUNCDISPATCH1(__kCFDataTypeID, void, data, "setLength:", length);
    CFAssert1(__CFMutableVariety(data) == kCFMutable || __CFMutableVariety(data) == kCFFixedMutable, __kCFLogAssertion, "%s(): data is immutable", __PRETTY_FUNCTION__);
    len = __CFDataLength(data);
    switch (__CFMutableVariety(data)) {
    case kCFMutable:
	if (len < length) {
// CF: should only grow when new length exceeds current capacity, not whenever it exceeds the current length
	    __CFDataGrow(data, length - len);
	}
	break;
    case kCFFixedMutable:
	CFAssert1(length <= __CFDataCapacity(data), __kCFLogAssertion, "%s(): fixed-capacity data is full", __PRETTY_FUNCTION__);
	break;
    }
    if (len < length) {
	memset(data->_bytes + len, 0, length - len);
    }
    __CFDataSetLength(data, length);
    __CFDataSetNumBytesUsed(data, length);
}

void CFDataIncreaseLength(CFMutableDataRef data, CFIndex extraLength) {
    CF_OBJC_FUNCDISPATCH1(__kCFDataTypeID, void, data, "increaseLengthBy:", extraLength);
    CFAssert1(__CFMutableVariety(data) == kCFMutable || __CFMutableVariety(data) == kCFFixedMutable, __kCFLogAssertion, "%s(): data is immutable", __PRETTY_FUNCTION__);
    CFDataSetLength(data, __CFDataLength(data) + extraLength);
}

void CFDataAppendBytes(CFMutableDataRef data, const uint8_t *bytes, CFIndex length) {
    CF_OBJC_FUNCDISPATCH2(__kCFDataTypeID, void, data, "appendBytes:length:", bytes, length);
    CFAssert1(__CFMutableVariety(data) == kCFMutable || __CFMutableVariety(data) == kCFFixedMutable, __kCFLogAssertion, "%s(): data is immutable", __PRETTY_FUNCTION__);
    CFDataReplaceBytes(data, CFRangeMake(__CFDataLength(data), 0), bytes, length); 
}

void CFDataDeleteBytes(CFMutableDataRef data, CFRange range) {
    CF_OBJC_FUNCDISPATCH3(__kCFDataTypeID, void, data, "replaceBytesInRange:withBytes:length:", range, NULL, 0);
    CFAssert1(__CFMutableVariety(data) == kCFMutable || __CFMutableVariety(data) == kCFFixedMutable, __kCFLogAssertion, "%s(): data is immutable", __PRETTY_FUNCTION__);
    CFDataReplaceBytes(data, range, NULL, 0); 
}

void CFDataReplaceBytes(CFMutableDataRef data, CFRange range, const uint8_t *newBytes, CFIndex newLength) {
    CFIndex len;
    CF_OBJC_FUNCDISPATCH3(__kCFDataTypeID, void, data, "replaceBytesInRange:withBytes:length:", range, newBytes, newLength);
    __CFGenericValidateType(data, __kCFDataTypeID);
    __CFDataValidateRange(data, range, __PRETTY_FUNCTION__);
    CFAssert1(__CFMutableVariety(data) == kCFMutable || __CFMutableVariety(data) == kCFFixedMutable, __kCFLogAssertion, "%s(): data is immutable", __PRETTY_FUNCTION__);
    CFAssert2(0 <= newLength, __kCFLogAssertion, "%s(): newLength (%d) cannot be less than zero", __PRETTY_FUNCTION__, newLength);
    len = __CFDataLength(data);
    switch (__CFMutableVariety(data)) {
    case kCFMutable:
	if (range.length < newLength && __CFDataNumBytes(data) < len - range.length + newLength) {
	    __CFDataGrow(data, newLength - range.length);
	}
	break;
    case kCFFixedMutable:
	CFAssert1(len - range.length + newLength <= __CFDataCapacity(data), __kCFLogAssertion, "%s(): fixed-capacity data is full", __PRETTY_FUNCTION__);
	break;
    }
    if (newLength != range.length && range.location + range.length < len) {
        memmove(data->_bytes + range.location + newLength, data->_bytes + range.location + range.length, (len - range.location - range.length) * sizeof(uint8_t));
    }
    if (0 < newLength) {
        memmove(data->_bytes + range.location, newBytes, newLength * sizeof(uint8_t));
    }
    __CFDataSetNumBytesUsed(data, (len - range.length + newLength));
    __CFDataSetLength(data, (len - range.length + newLength));
}

#undef __CFDataValidateRange
#undef __CFGenericValidateMutabilityFlags

