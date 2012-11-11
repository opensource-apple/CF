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
/*	CFStringEncodingConverterExt.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

#ifndef __CFSTRINGENCODINGCONVERETEREXT_H__
#define __CFSTRINGENCODINGCONVERETEREXT_H__ 1

#include "CFStringEncodingConverter.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define MAX_DECOMPOSED_LENGTH (10)

enum {
    kCFStringEncodingConverterStandard = 0,
    kCFStringEncodingConverterCheapEightBit = 1,
    kCFStringEncodingConverterStandardEightBit = 2,
    kCFStringEncodingConverterCheapMultiByte = 3,
    kCFStringEncodingConverterPlatformSpecific = 4 // Other fields are ignored
};

/* kCFStringEncodingConverterStandard */
typedef UInt32 (*CFStringEncodingToBytesProc)(UInt32 flags, const UniChar *characters, UInt32 numChars, UInt8 *bytes, UInt32 maxByteLen, UInt32 *usedByteLen);
typedef UInt32 (*CFStringEncodingToUnicodeProc)(UInt32 flags, const UInt8 *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen);
/* kCFStringEncodingConverterCheapEightBit */
typedef Boolean (*CFStringEncodingCheapEightBitToBytesProc)(UInt32 flags, UniChar character, UInt8 *byte);
typedef Boolean (*CFStringEncodingCheapEightBitToUnicodeProc)(UInt32 flags, UInt8 byte, UniChar *character);
/* kCFStringEncodingConverterStandardEightBit */
typedef UInt16 (*CFStringEncodingStandardEightBitToBytesProc)(UInt32 flags, const UniChar *characters, UInt32 numChars, UInt8 *byte);
typedef UInt16 (*CFStringEncodingStandardEightBitToUnicodeProc)(UInt32 flags, UInt8 byte, UniChar *characters);
/* kCFStringEncodingConverterCheapMultiByte */
typedef UInt16 (*CFStringEncodingCheapMultiByteToBytesProc)(UInt32 flags, UniChar character, UInt8 *bytes);
typedef UInt16 (*CFStringEncodingCheapMultiByteToUnicodeProc)(UInt32 flags, const UInt8 *bytes, UInt32 numBytes, UniChar *character);

typedef UInt32 (*CFStringEncodingToBytesLenProc)(UInt32 flags, const UniChar *characters, UInt32 numChars);
typedef UInt32 (*CFStringEncodingToUnicodeLenProc)(UInt32 flags, const UInt8 *bytes, UInt32 numBytes);

typedef UInt32 (*CFStringEncodingToBytesPrecomposeProc)(UInt32 flags, const UniChar *character, UInt32 numChars, UInt8 *bytes, UInt32 maxByteLen, UInt32 *usedByteLen);
typedef Boolean (*CFStringEncodingIsValidCombiningCharacterProc)(UniChar character);

typedef struct {
    void *toBytes;
    void *toUnicode;
    UInt16 maxBytesPerChar;
    UInt16 maxDecomposedCharLen;
    UInt8 encodingClass;
    UInt32 :24;
    CFStringEncodingToBytesLenProc toBytesLen;
    CFStringEncodingToUnicodeLenProc toUnicodeLen;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    CFStringEncodingToBytesPrecomposeProc toBytesPrecompose;
    CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar;
} CFStringEncodingConverter;


extern const CFStringEncodingConverter *CFStringEncodingGetConverter(UInt32 encoding);

enum {
    kCFStringEncodingGetConverterSelector = 0,
    kCFStringEncodingIsDecomposableCharacterSelector = 1,
    kCFStringEncodingDecomposeCharacterSelector = 2,
    kCFStringEncodingIsValidLatin1CombiningCharacterSelector = 3,
    kCFStringEncodingPrecomposeLatin1CharacterSelector = 4
};

extern const void *CFStringEncodingGetAddressForSelector(UInt32 selector);

#define BOOTSTRAPFUNC_NAME	CFStringEncodingBootstrap
typedef const CFStringEncodingConverter* (*CFStringEncodingBootstrapProc)(UInt32 encoding, const void *getSelector);

extern UInt32 CFStringEncodingGetScriptCodeForEncoding(CFStringEncoding encoding);

/* Latin precomposition */
/* This function does not precompose recursively nor to U+01E0 and U+01E1.
*/
extern Boolean CFStringEncodingIsValidCombiningCharacterForLatin1(UniChar character);
extern UniChar CFStringEncodingPrecomposeLatinCharacter(const UniChar *character, UInt32 numChars, UInt32 *usedChars);

/* Convenience functions for converter development */
typedef struct _CFStringEncodingUnicodeTo8BitCharMap {
    UniChar _u;
    UInt8 _c;
    UInt8 :8;
} CFStringEncodingUnicodeTo8BitCharMap;

/* Binary searches CFStringEncodingUnicodeTo8BitCharMap */
CF_INLINE Boolean CFStringEncodingUnicodeTo8BitEncoding(const CFStringEncodingUnicodeTo8BitCharMap *theTable, UInt32 numElem, UniChar character, UInt8 *ch) {
    const CFStringEncodingUnicodeTo8BitCharMap *p, *q, *divider;

    if ((character < theTable[0]._u) || (character > theTable[numElem-1]._u)) {
        return 0;
    }
    p = theTable;
    q = p + (numElem-1);
    while (p <= q) {
        divider = p + ((q - p) >> 1);	/* divide by 2 */
        if (character < divider->_u) { q = divider - 1; }
        else if (character > divider->_u) { p = divider + 1; }
        else { *ch = divider->_c; return 1; }
    }
    return 0;
}


#if defined(__cplusplus)
}
#endif

#endif /* __CFSTRINGENCODINGCONVERETEREXT_H__ */
