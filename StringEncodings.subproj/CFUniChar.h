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
/*	CFUniChar.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFUNICHAR__)
#define __COREFOUNDATION_CFUNICHAR__ 1

#include <CoreFoundation/CFBase.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define kCFUniCharBitShiftForByte	(3)
#define kCFUniCharBitShiftForMask	(7)

CF_INLINE Boolean CFUniCharIsSurrogateHighCharacter(UniChar character) {
    return ((character >= 0xD800UL) && (character <= 0xDBFFUL) ? true : false);
}

CF_INLINE Boolean CFUniCharIsSurrogateLowCharacter(UniChar character) {
    return ((character >= 0xDC00UL) && (character <= 0xDFFFUL) ? true : false);
}

CF_INLINE UTF32Char CFUniCharGetLongCharacterForSurrogatePair(UniChar surrogateHigh, UniChar surrogateLow) {
    return ((surrogateHigh - 0xD800UL) << 10) + (surrogateLow - 0xDC00UL) + 0x0010000UL;
}

// The following values coinside TextEncodingFormat format defines in TextCommon.h
enum {
    kCFUniCharUTF16Format = 0,
    kCFUniCharUTF8Format = 2,
    kCFUniCharUTF32Format = 3
};

CF_INLINE bool CFUniCharIsMemberOfBitmap(UTF16Char theChar, const uint8_t *bitmap) {
    return (bitmap && (bitmap[(theChar) >> kCFUniCharBitShiftForByte] & (((uint32_t)1) << (theChar & kCFUniCharBitShiftForMask))) ? true : false);
}

CF_INLINE void CFUniCharAddCharacterToBitmap(UTF16Char theChar, uint8_t *bitmap) {
    bitmap[(theChar) >> kCFUniCharBitShiftForByte] |= (((uint32_t)1) << (theChar & kCFUniCharBitShiftForMask));
}

CF_INLINE void CFUniCharRemoveCharacterFromBitmap(UTF16Char theChar, uint8_t *bitmap) {
    bitmap[(theChar) >> kCFUniCharBitShiftForByte] &= ~(((uint32_t)1) << (theChar & kCFUniCharBitShiftForMask));
}

enum {
    kCFUniCharControlCharacterSet = 1,
    kCFUniCharWhitespaceCharacterSet,
    kCFUniCharWhitespaceAndNewlineCharacterSet,
    kCFUniCharDecimalDigitCharacterSet,
    kCFUniCharLetterCharacterSet,
    kCFUniCharLowercaseLetterCharacterSet,
    kCFUniCharUppercaseLetterCharacterSet,
    kCFUniCharNonBaseCharacterSet,
    kCFUniCharCanonicalDecomposableCharacterSet,
    kCFUniCharDecomposableCharacterSet = kCFUniCharCanonicalDecomposableCharacterSet,
    kCFUniCharAlphaNumericCharacterSet,
    kCFUniCharPunctuationCharacterSet,
    kCFUniCharIllegalCharacterSet,
    kCFUniCharTitlecaseLetterCharacterSet,
    kCFUniCharSymbolAndOperatorCharacterSet,
    kCFUniCharCompatibilityDecomposableCharacterSet,
    kCFUniCharHFSPlusDecomposableCharacterSet,
    kCFUniCharStrongRightToLeftCharacterSet,
    kCFUniCharHasNonSelfLowercaseCharacterSet,
    kCFUniCharHasNonSelfUppercaseCharacterSet,
    kCFUniCharHasNonSelfTitlecaseCharacterSet,
    kCFUniCharHasNonSelfCaseFoldingCharacterSet,
    kCFUniCharHasNonSelfMirrorMappingCharacterSet,
    kCFUniCharControlAndFormatterCharacterSet,
    kCFUniCharCaseIgnorableCharacterSet
};

CF_EXPORT bool CFUniCharIsMemberOf(UTF32Char theChar, uint32_t charset);

// This function returns NULL for kCFUniCharControlCharacterSet, kCFUniCharWhitespaceCharacterSet, kCFUniCharWhitespaceAndNewlineCharacterSet, & kCFUniCharIllegalCharacterSet
CF_EXPORT const uint8_t *CFUniCharGetBitmapPtrForPlane(uint32_t charset, uint32_t plane);

enum {
    kCFUniCharBitmapFilled = (uint8_t)0,
    kCFUniCharBitmapEmpty = (uint8_t)0xFF,
    kCFUniCharBitmapAll = (uint8_t)1
};

CF_EXPORT uint8_t CFUniCharGetBitmapForPlane(uint32_t charset, uint32_t plane, void *bitmap, bool isInverted);

CF_EXPORT uint32_t CFUniCharGetNumberOfPlanes(uint32_t charset);

enum {
    kCFUniCharToLowercase = 0,
    kCFUniCharToUppercase,
    kCFUniCharToTitlecase,
    kCFUniCharCaseFold
};

enum {
    kCFUniCharCaseMapFinalSigma = (1),
    kCFUniCharCaseMapAfter_i = (1 << 1),
    kCFUniCharCaseMapMoreAbove = (1 << 2)
};

CF_EXPORT uint32_t CFUniCharMapCaseTo(UTF32Char theChar, UTF16Char *convertedChar, uint32_t maxLength, uint32_t ctype, uint32_t flags, const uint8_t *langCode);

CF_EXPORT uint32_t CFUniCharGetConditionalCaseMappingFlags(UTF32Char theChar, UTF16Char *buffer, uint32_t currentIndex, uint32_t length, uint32_t type, const uint8_t *langCode, uint32_t lastFlags);

enum {
    kCFUniCharBiDiPropertyON = 0,
    kCFUniCharBiDiPropertyL,
    kCFUniCharBiDiPropertyR,
    kCFUniCharBiDiPropertyAN,
    kCFUniCharBiDiPropertyEN,
    kCFUniCharBiDiPropertyAL,
    kCFUniCharBiDiPropertyNSM,
    kCFUniCharBiDiPropertyCS,
    kCFUniCharBiDiPropertyES,
    kCFUniCharBiDiPropertyET,
    kCFUniCharBiDiPropertyBN,
    kCFUniCharBiDiPropertyS,
    kCFUniCharBiDiPropertyWS,
    kCFUniCharBiDiPropertyB,
    kCFUniCharBiDiPropertyRLO,
    kCFUniCharBiDiPropertyRLE,
    kCFUniCharBiDiPropertyLRO,
    kCFUniCharBiDiPropertyLRE,
    kCFUniCharBiDiPropertyPDF
};

enum {
    kCFUniCharCombiningProperty = 0,
    kCFUniCharBidiProperty
};

// The second arg 'bitmap' has to be the pointer to a specific plane
CF_INLINE uint8_t CFUniCharGetBidiPropertyForCharacter(UTF16Char character, const uint8_t *bitmap) {
    if (bitmap) {
        uint8_t value = bitmap[(character >> 8)];

        if (value != kCFUniCharBiDiPropertyL) {
            bitmap = bitmap + 256 + ((value - kCFUniCharBiDiPropertyPDF - 1) * 256);
            return bitmap[character % 256];
        }
    }
    return kCFUniCharBiDiPropertyL;
}

CF_INLINE uint8_t CFUniCharGetCombiningPropertyForCharacter(UTF16Char character, const uint8_t *bitmap) {
    if (bitmap) {
        uint8_t value = bitmap[(character >> 8)];

        if (value) {
            bitmap = bitmap + 256 + ((value - 1) * 256);
            return bitmap[character % 256];
        }
    }
    return 0;
}

CF_EXPORT const void *CFUniCharGetUnicodePropertyDataForPlane(uint32_t propertyType, uint32_t plane);
CF_EXPORT uint32_t CFUniCharGetNumberOfPlanesForUnicodePropertyData(uint32_t propertyType);
CF_EXPORT uint32_t CFUniCharGetUnicodeProperty(UTF32Char character, uint32_t propertyType);

CF_EXPORT bool CFUniCharFillDestinationBuffer(const UTF32Char *src, uint32_t srcLength, void **dst, uint32_t dstLength, uint32_t *filledLength, uint32_t dstFormat);

#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_CFUNICHAR__ */

