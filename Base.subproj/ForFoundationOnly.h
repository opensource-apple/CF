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
/*	ForFoundationOnly.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

#if !CF_BUILDING_CF && !NSBUILDINGFOUNDATION
    #error The header file ForFoundationOnly.h is for the exclusive use of the
    #error CoreFoundation and Foundation projects.  No other project should include it.
#endif

#if !defined(__COREFOUNDATION_FORFOUNDATIONONLY__)
#define __COREFOUNDATION_FORFOUNDATIONONLY__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFPriv.h>

// NOTE: miscellaneous declarations are at the end


// ---- CFBundle material ----------------------------------------

#include "CFBundlePriv.h"

#if defined(__cplusplus)
extern "C" {
#endif

CF_EXPORT const CFStringRef _kCFBundleExecutablePathKey;
CF_EXPORT const CFStringRef _kCFBundleInfoPlistURLKey;
CF_EXPORT const CFStringRef _kCFBundleNumericVersionKey;
CF_EXPORT const CFStringRef _kCFBundleResourcesFileMappedKey;
CF_EXPORT const CFStringRef _kCFBundleCFMLoadAsBundleKey;
CF_EXPORT const CFStringRef _kCFBundleAllowMixedLocalizationsKey;


CF_EXPORT CFArrayRef _CFBundleCopyLanguageSearchListInDirectory(CFAllocatorRef alloc, CFURLRef url, UInt8 *version);
CF_EXPORT CFArrayRef _CFBundleGetLanguageSearchList(CFBundleRef bundle);

#if defined(__cplusplus)
}
#endif


// ---- CFString material ----------------------------------------

#if defined(__cplusplus)
extern "C" {
#endif

/* Create a byte stream from a CFString backing. Can convert a string piece at a
   time into a fixed size buffer. Returns number of characters converted.
   Characters that cannot be converted to the specified encoding are represented
   with the char specified by lossByte; if 0, then lossy conversion is not allowed
   and conversion stops, returning partial results.
   generatingExternalFile indicates that any extra stuff to allow this data to be
   persistent (for instance, BOM) should be included. 
   Pass buffer==NULL if you don't care about the converted string (but just the
   convertability, or number of bytes required, indicated by usedBufLen).
   Does not zero-terminate. If you want to create Pascal or C string, allow one
   extra byte at start or end.
*/
CF_EXPORT CFIndex __CFStringEncodeByteStream(CFStringRef string, CFIndex rangeLoc, CFIndex rangeLen, Boolean generatingExternalFile, CFStringEncoding encoding, char lossByte, UInt8 *buffer, CFIndex max, CFIndex *usedBufLen);

CF_INLINE Boolean __CFStringEncodingIsSupersetOfASCII(CFStringEncoding encoding) {
    switch (encoding & 0x0000FF00) {
        case 0x0: // MacOS Script range
            return true;

        case 0x100: // Unicode range
            if (encoding == kCFStringEncodingUnicode) return false;
            return true;

        case 0x200: // ISO range
            return true;
            
        case 0x600: // National standards range
            if (encoding != kCFStringEncodingASCII) return false;
            return true;

        case 0x800: // ISO 2022 range
            return false; // It's modal encoding

        case 0xA00: // Misc standard range
            return true;

        case 0xB00:
            if (encoding == kCFStringEncodingNonLossyASCII) return false;
            return true;

        case 0xC00: // EBCDIC
            return false;

        default:
            return ((encoding & 0x0000FF00) > 0x0C00 ? false : true);
    }
}

/* Desperately using extern here */
CF_EXPORT CFStringEncoding __CFDefaultEightBitStringEncoding;
CF_EXPORT CFStringEncoding __CFStringComputeEightBitStringEncoding(void);

CF_INLINE CFStringEncoding __CFStringGetEightBitStringEncoding(void) {
    if (__CFDefaultEightBitStringEncoding == kCFStringEncodingInvalidId) __CFStringComputeEightBitStringEncoding();
    return __CFDefaultEightBitStringEncoding;
}

enum {
     __kCFVarWidthLocalBufferSize = 1008
};

typedef struct {      /* A simple struct to maintain ASCII/Unicode versions of the same buffer. */
     union {
        UInt8 *ascii;
	UniChar *unicode;
    } chars;
    Boolean isASCII;	/* This really does mean 7-bit ASCII, not _NSDefaultCStringEncoding() */
    Boolean shouldFreeChars;	/* If the number of bytes exceeds __kCFVarWidthLocalBufferSize, bytes are allocated */
    Boolean _unused1;
    Boolean _unused2;
    CFAllocatorRef allocator;	/* Use this allocator to allocate, reallocate, and deallocate the bytes */
    UInt32 numChars;	/* This is in terms of ascii or unicode; that is, if isASCII, it is number of 7-bit chars; otherwise it is number of UniChars; note that the actual allocated space might be larger */
    UInt8 localBuffer[__kCFVarWidthLocalBufferSize];	/* private; 168 ISO2022JP chars, 504 Unicode chars, 1008 ASCII chars */
} CFVarWidthCharBuffer;


/* Convert a byte stream to ASCII (7-bit!) or Unicode, with a CFVarWidthCharBuffer struct on the stack. false return indicates an error occured during the conversion. Depending on .isASCII, follow .chars.ascii or .chars.unicode.  If .shouldFreeChars is returned as true, free the returned buffer when done with it.  If useClientsMemoryPtr is provided as non-NULL, and the provided memory can be used as is, this is set to true, and the .ascii or .unicode buffer in CFVarWidthCharBuffer is set to bytes.
!!! If the stream is Unicode and has no BOM, the data is assumed to be big endian! Could be trouble on Intel if someone didn't follow that assumption.
!!! __CFStringDecodeByteStream2() needs to be deprecated and removed post-Jaguar.
*/
CF_EXPORT Boolean __CFStringDecodeByteStream2(const UInt8 *bytes, UInt32 len, CFStringEncoding encoding, Boolean alwaysUnicode, CFVarWidthCharBuffer *buffer, Boolean *useClientsMemoryPtr);
CF_EXPORT Boolean __CFStringDecodeByteStream3(const UInt8 *bytes, UInt32 len, CFStringEncoding encoding, Boolean alwaysUnicode, CFVarWidthCharBuffer *buffer, Boolean *useClientsMemoryPtr, UInt32 converterFlags);


/* Convert single byte to Unicode; assumes one-to-one correspondence (that is, can only be used with 1-byte encodings). You can use the function if it's not NULL. The table is always safe to use; calling __CFSetCharToUniCharFunc() updates it.
*/
CF_EXPORT Boolean (*__CFCharToUniCharFunc)(UInt32 flags, UInt8 ch, UniChar *unicodeChar);
CF_EXPORT void __CFSetCharToUniCharFunc(Boolean (*func)(UInt32 flags, UInt8 ch, UniChar *unicodeChar));
CF_EXPORT UniChar __CFCharToUniCharTable[256];

/* Character class functions UnicodeData-2_1_5.txt
*/
CF_INLINE Boolean __CFIsWhitespace(UniChar theChar) {
    return ((theChar < 0x21) || (theChar > 0x7E && theChar < 0xA1) || (theChar >= 0x2000 && theChar <= 0x200B) || (theChar == 0x3000)) ? true : false;
}

/* Same as CFStringGetCharacterFromInlineBuffer() but returns 0xFFFF on out of bounds access
*/
CF_INLINE UniChar __CFStringGetCharacterFromInlineBufferAux(CFStringInlineBuffer *buf, CFIndex idx) {
    if (buf->directBuffer) {
	if (idx < 0 || idx >= buf->rangeToBuffer.length) return 0xFFFF;
        return buf->directBuffer[idx + buf->rangeToBuffer.location];
    }
    if (idx >= buf->bufferedRangeEnd || idx < buf->bufferedRangeStart) {
	if (idx < 0 || idx >= buf->rangeToBuffer.length) return 0xFFFF;
	if ((buf->bufferedRangeStart = idx - 4) < 0) buf->bufferedRangeStart = 0;
	buf->bufferedRangeEnd = buf->bufferedRangeStart + __kCFStringInlineBufferLength;
	if (buf->bufferedRangeEnd > buf->rangeToBuffer.length) buf->bufferedRangeEnd = buf->rangeToBuffer.length;
	CFStringGetCharacters(buf->theString, CFRangeMake(buf->rangeToBuffer.location + buf->bufferedRangeStart, buf->bufferedRangeEnd - buf->bufferedRangeStart), buf->buffer);
    }
    return buf->buffer[idx - buf->bufferedRangeStart];
}

/* Same as CFStringGetCharacterFromInlineBuffer(), but without the bounds checking (will return garbage or crash)
*/
CF_INLINE UniChar __CFStringGetCharacterFromInlineBufferQuick(CFStringInlineBuffer *buf, CFIndex idx) {
    if (buf->directBuffer) return buf->directBuffer[idx + buf->rangeToBuffer.location];
    if (idx >= buf->bufferedRangeEnd || idx < buf->bufferedRangeStart) {
	if ((buf->bufferedRangeStart = idx - 4) < 0) buf->bufferedRangeStart = 0;
	buf->bufferedRangeEnd = buf->bufferedRangeStart + __kCFStringInlineBufferLength;
	if (buf->bufferedRangeEnd > buf->rangeToBuffer.length) buf->bufferedRangeEnd = buf->rangeToBuffer.length;
	CFStringGetCharacters(buf->theString, CFRangeMake(buf->rangeToBuffer.location + buf->bufferedRangeStart, buf->bufferedRangeEnd - buf->bufferedRangeStart), buf->buffer);
    }
    return buf->buffer[idx - buf->bufferedRangeStart];
}


/* These two allow specifying an alternate description function (instead of CFCopyDescription); used by NSString
*/
CF_EXPORT void _CFStringAppendFormatAndArgumentsAux(CFMutableStringRef outputString, CFStringRef (*copyDescFunc)(void *, CFDictionaryRef), CFDictionaryRef formatOptions, CFStringRef formatString, va_list args);
CF_EXPORT CFStringRef  _CFStringCreateWithFormatAndArgumentsAux(CFAllocatorRef alloc, CFStringRef (*copyDescFunc)(void *, CFDictionaryRef), CFDictionaryRef formatOptions, CFStringRef format, va_list arguments);

enum {_CFStringErrNone = 0, _CFStringErrNotMutable = 1, _CFStringErrNilArg = 2, _CFStringErrBounds = 3};

CF_EXPORT Boolean __CFStringNoteErrors(void);		// Should string errors raise?

#if defined(__cplusplus)
}
#endif


// ---- Binary plist material ----------------------------------------

typedef const struct __CFKeyedArchiverUID * CFKeyedArchiverUIDRef;
extern CFTypeID _CFKeyedArchiverUIDGetTypeID(void);
extern CFKeyedArchiverUIDRef _CFKeyedArchiverUIDCreate(CFAllocatorRef allocator, uint32_t value);
extern uint32_t _CFKeyedArchiverUIDGetValue(CFKeyedArchiverUIDRef uid);


enum {
    kCFBinaryPlistMarkerNull = 0x00,
    kCFBinaryPlistMarkerFalse = 0x08,
    kCFBinaryPlistMarkerTrue = 0x09,
    kCFBinaryPlistMarkerFill = 0x0F,
    kCFBinaryPlistMarkerInt = 0x10,
    kCFBinaryPlistMarkerReal = 0x20,
    kCFBinaryPlistMarkerDate = 0x33,
    kCFBinaryPlistMarkerData = 0x40,
    kCFBinaryPlistMarkerASCIIString = 0x50,
    kCFBinaryPlistMarkerUnicode16String = 0x60,
    kCFBinaryPlistMarkerUID = 0x80,
    kCFBinaryPlistMarkerArray = 0xA0,
    kCFBinaryPlistMarkerDict = 0xD0
};

typedef struct {
    uint8_t	_magic[6];
    uint8_t	_version[2];
} CFBinaryPlistHeader;

typedef struct {
    uint8_t	_unused[6];
    uint8_t	_offsetIntSize;
    uint8_t	_objectRefSize;
    uint64_t	_numObjects;
    uint64_t	_topObject;
    uint64_t	_offsetTableOffset;
} CFBinaryPlistTrailer;


// ---- Miscellaneous material ----------------------------------------

#include <CoreFoundation/CFBag.h>
#include <CoreFoundation/CFSet.h>
#include <math.h>

#if defined(__cplusplus)
extern "C" {
#endif

CF_EXPORT CFTypeID CFTypeGetTypeID(void);

CF_EXPORT void _CFArraySetCapacity(CFMutableArrayRef array, CFIndex cap);
CF_EXPORT void _CFBagSetCapacity(CFMutableBagRef bag, CFIndex cap);
CF_EXPORT void _CFDictionarySetCapacity(CFMutableDictionaryRef dict, CFIndex cap);
CF_EXPORT void _CFSetSetCapacity(CFMutableSetRef set, CFIndex cap);

CF_EXPORT void _CFArrayReplaceValues(CFMutableArrayRef array, CFRange range, const void **newValues, CFIndex newCount);


/* For use by NSNumber and CFNumber.
  Hashing algorithm for CFNumber:
  M = Max CFHashCode (assumed to be unsigned)
  For positive integral values: N mod M
  For negative integral values: (-N) mod M
  For floating point numbers that are not integral: hash(integral part) + hash(float part * M)
*/
CF_INLINE CFHashCode _CFHashInt(int i) {
    return (i > 0) ? (CFHashCode)(i) : (CFHashCode)(-i);
}

CF_INLINE CFHashCode _CFHashDouble(double d) {
    double dInt;
    if (d < 0) d = -d;
    dInt = rint(d);
    return (CFHashCode)(fmod(dInt, (double)0xFFFFFFFF) + ((d - dInt) * 0xFFFFFFFF));
}


typedef void (*CFRunLoopPerformCallBack)(void *info);


#if defined(__MACH__)
#include <mach/mach_time.h>
CF_INLINE UInt64 __CFReadTSR(void) {
    return mach_absolute_time();
}
#else
CF_INLINE UInt64 __CFReadTSR(void) {
    union {
	UInt64 time64;
	UInt32 word[2];
    } now;
#if defined(__i386__)
    /* Read from Pentium and Pentium Pro 64-bit timestamp counter. */
    /* The counter is set to 0 at processor reset and increments on */
    /* every clock cycle. */
    __asm__ volatile("rdtsc" : : : "eax", "edx");
    __asm__ volatile("movl %%eax,%0" : "=m" (now.word[0]) : : "eax");
    __asm__ volatile("movl %%edx,%0" : "=m" (now.word[1]) : : "edx");
#elif defined(__ppc__)
    /* Read from PowerPC 64-bit time base register. The increment */
    /* rate of the time base is implementation-dependent, but is */
    /* 1/4th the bus clock cycle on 603/604/750 processors. */
    UInt32 t3;
    do {
	__asm__ volatile("mftbu %0" : "=r" (now.word[0]));
	__asm__ volatile("mftb %0" : "=r" (now.word[1]));
	__asm__ volatile("mftbu %0" : "=r" (t3));
    } while (now.word[0] != t3);
#else
// ??? Do not know how to read a time stamp register on this architecture
    now.time64 = (uint64_t)0;
#endif
    return now.time64;
}
#endif

#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_FORFOUNDATIONONLY__ */

