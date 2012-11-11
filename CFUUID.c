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
/*	CFUUID.c
	Copyright (c) 1999-2007 Apple Inc.  All rights reserved.
	Responsibility: Doug Davidson
*/

#include <CoreFoundation/CFUUID.h>
#include "CFInternal.h"
#if DEPLOYMENT_TARGET_MACOSX
#include <uuid/uuid.h>
#endif

static CFMutableDictionaryRef _uniquedUUIDs = NULL;
static CFSpinLock_t CFUUIDGlobalDataLock = CFSpinLockInit;

struct __CFUUID {
    CFRuntimeBase _base;
    CFUUIDBytes _bytes;
};

static Boolean __CFisEqualUUIDBytes(const void *ptr1, const void *ptr2) {
    CFUUIDBytes *p1 = (CFUUIDBytes *)ptr1;
    CFUUIDBytes *p2 = (CFUUIDBytes *)ptr2;

    return (((p1->byte0 == p2->byte0) && (p1->byte1 == p2->byte1) && (p1->byte2 == p2->byte2) && (p1->byte3 == p2->byte3) && (p1->byte4 == p2->byte4) && (p1->byte5 == p2->byte5) && (p1->byte6 == p2->byte6) && (p1->byte7 == p2->byte7) && (p1->byte8 == p2->byte8) && (p1->byte9 == p2->byte9) && (p1->byte10 == p2->byte10) && (p1->byte11 == p2->byte11) && (p1->byte12 == p2->byte12) && (p1->byte13 == p2->byte13) && (p1->byte14 == p2->byte14) && (p1->byte15 == p2->byte15)) ? true : false);
}

static CFHashCode __CFhashUUIDBytes(const void *ptr) {
    return CFHashBytes((uint8_t *)ptr, 16);
}

#import "auto_stubs.h"

#define LOCK() __CFSpinLock(&CFUUIDGlobalDataLock)
#define UNLOCK() __CFSpinUnlock(&CFUUIDGlobalDataLock)

#define MALLOC(x) CFAllocatorAllocate(kCFAllocatorSystemDefault, x, 0)
#define FREE(x) CFAllocatorDeallocate(kCFAllocatorSystemDefault, x)
#define HASH(x) CFHashBytes((uint8_t *)x, 16)


/***** end of weak set */

static void __CFUUIDAddUniqueUUID(CFUUIDRef uuid) {
    __CFSpinLock(&CFUUIDGlobalDataLock);
        
    CFDictionaryKeyCallBacks __CFUUIDBytesDictionaryKeyCallBacks = {0, NULL, NULL, NULL, __CFisEqualUUIDBytes, __CFhashUUIDBytes};
    CFDictionaryValueCallBacks __CFnonRetainedUUIDDictionaryValueCallBacks = {0, NULL, NULL, CFCopyDescription, CFEqual};

    if (_uniquedUUIDs == NULL) {
        _uniquedUUIDs = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &__CFUUIDBytesDictionaryKeyCallBacks, &__CFnonRetainedUUIDDictionaryValueCallBacks);
    }
    CFDictionarySetValue(_uniquedUUIDs, &(uuid->_bytes), uuid);
    __CFSpinUnlock(&CFUUIDGlobalDataLock);
}

static void __CFUUIDRemoveUniqueUUID(CFUUIDRef uuid) {
    __CFSpinLock(&CFUUIDGlobalDataLock);
    if (_uniquedUUIDs != NULL) {
        CFDictionaryRemoveValue(_uniquedUUIDs, &(uuid->_bytes));
    }
    __CFSpinUnlock(&CFUUIDGlobalDataLock);
}

static CFUUIDRef __CFUUIDGetUniquedUUID(CFUUIDBytes *bytes) {
    CFUUIDRef uuid = NULL;
    __CFSpinLock(&CFUUIDGlobalDataLock);
    if (_uniquedUUIDs != NULL) {
        uuid = (CFUUIDRef)CFDictionaryGetValue(_uniquedUUIDs, bytes);
    }
    __CFSpinUnlock(&CFUUIDGlobalDataLock);
    return uuid;
}

static void __CFUUIDDeallocate(CFTypeRef cf) {    
    struct __CFUUID *uuid = (struct __CFUUID *)cf;
    __CFUUIDRemoveUniqueUUID(uuid);
}

static CFStringRef __CFUUIDCopyDescription(CFTypeRef cf) {
    CFStringRef uuidStr = CFUUIDCreateString(CFGetAllocator(cf), (CFUUIDRef)cf);
    CFStringRef desc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFUUID %p> %@"), cf, uuidStr);
    CFRelease(uuidStr);
    return desc;
}

static CFStringRef __CFUUIDCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    return CFUUIDCreateString(CFGetAllocator(cf), (CFUUIDRef)cf);
}

static CFTypeID __kCFUUIDTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFUUIDClass = {
    0,
    "CFUUID",
    NULL,	// init
    NULL,	// copy
    __CFUUIDDeallocate,
    NULL,	// equal
    NULL,	// hash
    __CFUUIDCopyFormattingDescription,
    __CFUUIDCopyDescription
};

__private_extern__ void __CFUUIDInitialize(void) {
    __kCFUUIDTypeID = _CFRuntimeRegisterClass(&__CFUUIDClass);
}

CFTypeID CFUUIDGetTypeID(void) {
    return __kCFUUIDTypeID;
}

static CFUUIDRef __CFUUIDCreateWithBytesPrimitive(CFAllocatorRef allocator, CFUUIDBytes bytes, Boolean isConst) {
    struct __CFUUID *uuid = (struct __CFUUID *)__CFUUIDGetUniquedUUID(&bytes);

    if (uuid == NULL) {
        size_t size;
        size = sizeof(struct __CFUUID) - sizeof(CFRuntimeBase);
        uuid = (struct __CFUUID *)_CFRuntimeCreateInstance(allocator, __kCFUUIDTypeID, size, NULL);

        if (NULL == uuid) return NULL;

        uuid->_bytes = bytes;

        __CFUUIDAddUniqueUUID(uuid);
    } else if (!isConst) {
        CFRetain(uuid);
    }
    
    return (CFUUIDRef)uuid;
}

CFUUIDRef CFUUIDCreate(CFAllocatorRef alloc) {
    /* Create a new bytes struct and then call the primitive. */
    CFUUIDBytes bytes;
    uint32_t retval = 0;
    
    __CFSpinLock(&CFUUIDGlobalDataLock);
#if (DEPLOYMENT_TARGET_MACOSX)
    static Boolean useV1UUIDs = false, checked = false;
    uuid_t uuid;
    if (!checked) {
        const char *value = getenv("CFUUIDVersionNumber");
        if (value) {
            if (1 == strtoul_l(value, NULL, 0, NULL)) useV1UUIDs = true;
        } else {
            if (!_CFExecutableLinkedOnOrAfter(CFSystemVersionTiger)) useV1UUIDs = true;
        }
        checked = true;
    }
    if (useV1UUIDs) uuid_generate_time(uuid); else uuid_generate_random(uuid);
    memcpy(&bytes, uuid, sizeof(uuid));
#else
    retval = 1;
#endif
    __CFSpinUnlock(&CFUUIDGlobalDataLock);

    return (retval == 0) ? __CFUUIDCreateWithBytesPrimitive(alloc, bytes, false) : NULL;
}

CFUUIDRef CFUUIDCreateWithBytes(CFAllocatorRef alloc, uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4, uint8_t byte5, uint8_t byte6, uint8_t byte7, uint8_t byte8, uint8_t byte9, uint8_t byte10, uint8_t byte11, uint8_t byte12, uint8_t byte13, uint8_t byte14, uint8_t byte15) {
    CFUUIDBytes bytes;
    // CodeWarrior can't handle the structure assignment of bytes, so we must explode this - REW, 10/8/99
    bytes.byte0 = byte0;
    bytes.byte1 = byte1;
    bytes.byte2 = byte2;
    bytes.byte3 = byte3;
    bytes.byte4 = byte4;
    bytes.byte5 = byte5;
    bytes.byte6 = byte6;
    bytes.byte7 = byte7;
    bytes.byte8 = byte8;
    bytes.byte9 = byte9;
    bytes.byte10 = byte10;
    bytes.byte11 = byte11;
    bytes.byte12 = byte12;
    bytes.byte13 = byte13;
    bytes.byte14 = byte14;
    bytes.byte15 = byte15;

    return __CFUUIDCreateWithBytesPrimitive(alloc, bytes, false);
}

static void _intToHexChars(UInt32 in, UniChar *out, int digits) {
    int shift;
    UInt32 d;

    while (--digits >= 0) {
        shift = digits << 2;
        d = 0x0FL & (in >> shift);
        if (d <= 9) {
            *out++ = (UniChar)'0' + d;
        } else {
            *out++ = (UniChar)'A' + (d - 10);
        }
    }
}

static uint8_t _byteFromHexChars(UniChar *in) {
    uint8_t result = 0;
    UniChar c;
    uint8_t d;
    CFIndex i;

    for (i=0; i<2; i++) {
        c = in[i];
        if ((c >= (UniChar)'0') && (c <= (UniChar)'9')) {
            d = c - (UniChar)'0';
        } else if ((c >= (UniChar)'a') && (c <= (UniChar)'f')) {
            d = c - ((UniChar)'a' - 10);
        } else if ((c >= (UniChar)'A') && (c <= (UniChar)'F')) {
            d = c - ((UniChar)'A' - 10);
        } else {
            return 0;
        }
        result = (result << 4) | d;
    }
    
    return result;
}

CF_INLINE Boolean _isHexChar(UniChar c) {
    return ((((c >= (UniChar)'0') && (c <= (UniChar)'9')) || ((c >= (UniChar)'a') && (c <= (UniChar)'f')) || ((c >= (UniChar)'A') && (c <= (UniChar)'F'))) ? true : false);
}

#define READ_A_BYTE(into) if (i+1 < len) { \
    (into) = _byteFromHexChars(&(chars[i])); \
        i+=2; \
}

CFUUIDRef CFUUIDCreateFromString(CFAllocatorRef alloc, CFStringRef uuidStr) {
    /* Parse the string into a bytes struct and then call the primitive. */
    CFUUIDBytes bytes;
    UniChar chars[100];
    CFIndex len;
    CFIndex i = 0;
    
    if (uuidStr == NULL) return NULL;

    len = CFStringGetLength(uuidStr);
    if (len > 100) {
        len = 100;
    } else if (len == 0) {
        return NULL;
    }
    CFStringGetCharacters(uuidStr, CFRangeMake(0, len), chars);
    memset((void *)&bytes, 0, sizeof(bytes));

    /* Skip initial random stuff */
    while (!_isHexChar(chars[i]) && (i < len)) {
        i++;
    }

    READ_A_BYTE(bytes.byte0);
    READ_A_BYTE(bytes.byte1);
    READ_A_BYTE(bytes.byte2);
    READ_A_BYTE(bytes.byte3);

    i++;

    READ_A_BYTE(bytes.byte4);
    READ_A_BYTE(bytes.byte5);

    i++;

    READ_A_BYTE(bytes.byte6);
    READ_A_BYTE(bytes.byte7);

    i++;

    READ_A_BYTE(bytes.byte8);
    READ_A_BYTE(bytes.byte9);

    i++;

    READ_A_BYTE(bytes.byte10);
    READ_A_BYTE(bytes.byte11);
    READ_A_BYTE(bytes.byte12);
    READ_A_BYTE(bytes.byte13);
    READ_A_BYTE(bytes.byte14);
    READ_A_BYTE(bytes.byte15);

    return __CFUUIDCreateWithBytesPrimitive(alloc, bytes, false);
}

CFStringRef CFUUIDCreateString(CFAllocatorRef alloc, CFUUIDRef uuid) {
    CFMutableStringRef str = CFStringCreateMutable(alloc, 0);
    UniChar buff[12];

    // First segment (4 bytes, 8 digits + 1 dash)
    _intToHexChars(uuid->_bytes.byte0, buff, 2);
    _intToHexChars(uuid->_bytes.byte1, &(buff[2]), 2);
    _intToHexChars(uuid->_bytes.byte2, &(buff[4]), 2);
    _intToHexChars(uuid->_bytes.byte3, &(buff[6]), 2);
    buff[8] = (UniChar)'-';
    CFStringAppendCharacters(str, buff, 9);

    // Second segment (2 bytes, 4 digits + 1 dash)
    _intToHexChars(uuid->_bytes.byte4, buff, 2);
    _intToHexChars(uuid->_bytes.byte5, &(buff[2]), 2);
    buff[4] = (UniChar)'-';
    CFStringAppendCharacters(str, buff, 5);

    // Third segment (2 bytes, 4 digits + 1 dash)
    _intToHexChars(uuid->_bytes.byte6, buff, 2);
    _intToHexChars(uuid->_bytes.byte7, &(buff[2]), 2);
    buff[4] = (UniChar)'-';
    CFStringAppendCharacters(str, buff, 5);

    // Fourth segment (2 bytes, 4 digits + 1 dash)
    _intToHexChars(uuid->_bytes.byte8, buff, 2);
    _intToHexChars(uuid->_bytes.byte9, &(buff[2]), 2);
    buff[4] = (UniChar)'-';
    CFStringAppendCharacters(str, buff, 5);

    // Fifth segment (6 bytes, 12 digits)
    _intToHexChars(uuid->_bytes.byte10, buff, 2);
    _intToHexChars(uuid->_bytes.byte11, &(buff[2]), 2);
    _intToHexChars(uuid->_bytes.byte12, &(buff[4]), 2);
    _intToHexChars(uuid->_bytes.byte13, &(buff[6]), 2);
    _intToHexChars(uuid->_bytes.byte14, &(buff[8]), 2);
    _intToHexChars(uuid->_bytes.byte15, &(buff[10]), 2);
    CFStringAppendCharacters(str, buff, 12);

    return str;
}

CFUUIDRef CFUUIDGetConstantUUIDWithBytes(CFAllocatorRef alloc, uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4, uint8_t byte5, uint8_t byte6, uint8_t byte7, uint8_t byte8, uint8_t byte9, uint8_t byte10, uint8_t byte11, uint8_t byte12, uint8_t byte13, uint8_t byte14, uint8_t byte15) {
    CFUUIDBytes bytes;
    // CodeWarrior can't handle the structure assignment of bytes, so we must explode this - REW, 10/8/99
    bytes.byte0 = byte0;
    bytes.byte1 = byte1;
    bytes.byte2 = byte2;
    bytes.byte3 = byte3;
    bytes.byte4 = byte4;
    bytes.byte5 = byte5;
    bytes.byte6 = byte6;
    bytes.byte7 = byte7;
    bytes.byte8 = byte8;
    bytes.byte9 = byte9;
    bytes.byte10 = byte10;
    bytes.byte11 = byte11;
    bytes.byte12 = byte12;
    bytes.byte13 = byte13;
    bytes.byte14 = byte14;
    bytes.byte15 = byte15;

    return __CFUUIDCreateWithBytesPrimitive(alloc, bytes, true);
}

CFUUIDBytes CFUUIDGetUUIDBytes(CFUUIDRef uuid) {
    return uuid->_bytes;
}

CF_EXPORT CFUUIDRef CFUUIDCreateFromUUIDBytes(CFAllocatorRef alloc, CFUUIDBytes bytes) {
    return __CFUUIDCreateWithBytesPrimitive(alloc, bytes, false);
}

#undef READ_A_BYTE

