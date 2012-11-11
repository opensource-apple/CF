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
/*	CFStringEncodingConverter.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Aki Inoue
*/

#include "CFInternal.h"
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
#include "CFUniChar.h"
#include "CFUtilities.h"
#include "CFUnicodeDecomposition.h"
#include "CFStringEncodingConverterExt.h"
#include "CFStringEncodingConverterPriv.h"
#include <stdlib.h>
#if !defined(__MACOS8__)
#ifdef __WIN32__
#include <windows.h>
#else // Mach, HP-UX, Solaris
#include <pthread.h>
#endif
#endif __MACOS8__


/* Macros
*/
#define TO_BYTE(conv,flags,chars,numChars,bytes,max,used) (conv->_toBytes ? conv->toBytes(conv,flags,chars,numChars,bytes,max,used) : ((CFStringEncodingToBytesProc)conv->toBytes)(flags,chars,numChars,bytes,max,used))
#define TO_UNICODE(conv,flags,bytes,numBytes,chars,max,used) (conv->_toUnicode ?  (flags & (kCFStringEncodingUseCanonical|kCFStringEncodingUseHFSPlusCanonical) ? conv->toCanonicalUnicode(conv,flags,bytes,numBytes,chars,max,used) : conv->toUnicode(conv,flags,bytes,numBytes,chars,max,used)) : ((CFStringEncodingToUnicodeProc)conv->toUnicode)(flags,bytes,numBytes,chars,max,used))

#define LineSeparator 0x2028
#define ParagraphSeparator 0x2029
#define ASCIINewLine 0x0a
#define kSurrogateHighStart 0xD800
#define kSurrogateHighEnd 0xDBFF
#define kSurrogateLowStart 0xDC00
#define kSurrogateLowEnd 0xDFFF

/* Mapping 128..255 to lossy ASCII
*/
static const struct {
    unsigned char chars[4];
} _toLossyASCIITable[] = {
    {{' ', 0, 0, 0}}, // NO-BREAK SPACE
    {{'!', 0, 0, 0}}, // INVERTED EXCLAMATION MARK
    {{'c', 0, 0, 0}}, // CENT SIGN
    {{'L', 0, 0, 0}}, // POUND SIGN
    {{'$', 0, 0, 0}}, // CURRENCY SIGN
    {{'Y', 0, 0, 0}}, // YEN SIGN
    {{'|', 0, 0, 0}}, // BROKEN BAR
    {{0, 0, 0, 0}}, // SECTION SIGN
    {{0, 0, 0, 0}}, // DIAERESIS
    {{'(', 'C', ')', 0}}, // COPYRIGHT SIGN
    {{'a', 0, 0, 0}}, // FEMININE ORDINAL INDICATOR
    {{'<', '<', 0, 0}}, // LEFT-POINTING DOUBLE ANGLE QUOTATION MARK
    {{0, 0, 0, 0}}, // NOT SIGN
    {{'-', 0, 0, 0}}, // SOFT HYPHEN
    {{'(', 'R', ')', 0}}, // REGISTERED SIGN
    {{0, 0, 0, 0}}, // MACRON
    {{0, 0, 0, 0}}, // DEGREE SIGN
    {{'+', '-', 0, 0}}, // PLUS-MINUS SIGN
    {{'2', 0, 0, 0}}, // SUPERSCRIPT TWO
    {{'3', 0, 0, 0}}, // SUPERSCRIPT THREE
    {{0, 0, 0, 0}}, // ACUTE ACCENT
    {{0, 0, 0, 0}}, // MICRO SIGN
    {{0, 0, 0, 0}}, // PILCROW SIGN
    {{0, 0, 0, 0}}, // MIDDLE DOT
    {{0, 0, 0, 0}}, // CEDILLA
    {{'1', 0, 0, 0}}, // SUPERSCRIPT ONE
    {{'o', 0, 0, 0}}, // MASCULINE ORDINAL INDICATOR
    {{'>', '>', 0, 0}}, // RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK
    {{'1', '/', '4', 0}}, // VULGAR FRACTION ONE QUARTER
    {{'1', '/', '2', 0}}, // VULGAR FRACTION ONE HALF
    {{'3', '/', '4', 0}}, // VULGAR FRACTION THREE QUARTERS
    {{'?', 0, 0, 0}}, // INVERTED QUESTION MARK
    {{'A', 0, 0, 0}}, // LATIN CAPITAL LETTER A WITH GRAVE
    {{'A', 0, 0, 0}}, // LATIN CAPITAL LETTER A WITH ACUTE
    {{'A', 0, 0, 0}}, // LATIN CAPITAL LETTER A WITH CIRCUMFLEX
    {{'A', 0, 0, 0}}, // LATIN CAPITAL LETTER A WITH TILDE
    {{'A', 0, 0, 0}}, // LATIN CAPITAL LETTER A WITH DIAERESIS
    {{'A', 0, 0, 0}}, // LATIN CAPITAL LETTER A WITH RING ABOVE
    {{'A', 'E', 0, 0}}, // LATIN CAPITAL LETTER AE
    {{'C', 0, 0, 0}}, // LATIN CAPITAL LETTER C WITH CEDILLA
    {{'E', 0, 0, 0}}, // LATIN CAPITAL LETTER E WITH GRAVE
    {{'E', 0, 0, 0}}, // LATIN CAPITAL LETTER E WITH ACUTE
    {{'E', 0, 0, 0}}, // LATIN CAPITAL LETTER E WITH CIRCUMFLEX
    {{'E', 0, 0, 0}}, // LATIN CAPITAL LETTER E WITH DIAERESIS
    {{'I', 0, 0, 0}}, // LATIN CAPITAL LETTER I WITH GRAVE
    {{'I', 0, 0, 0}}, // LATIN CAPITAL LETTER I WITH ACUTE
    {{'I', 0, 0, 0}}, // LATIN CAPITAL LETTER I WITH CIRCUMFLEX
    {{'I', 0, 0, 0}}, // LATIN CAPITAL LETTER I WITH DIAERESIS
    {{'T', 'H', 0, 0}}, // LATIN CAPITAL LETTER ETH (Icelandic)
    {{'N', 0, 0, 0}}, // LATIN CAPITAL LETTER N WITH TILDE
    {{'O', 0, 0, 0}}, // LATIN CAPITAL LETTER O WITH GRAVE
    {{'O', 0, 0, 0}}, // LATIN CAPITAL LETTER O WITH ACUTE
    {{'O', 0, 0, 0}}, // LATIN CAPITAL LETTER O WITH CIRCUMFLEX
    {{'O', 0, 0, 0}}, // LATIN CAPITAL LETTER O WITH TILDE
    {{'O', 0, 0, 0}}, // LATIN CAPITAL LETTER O WITH DIAERESIS
    {{'X', 0, 0, 0}}, // MULTIPLICATION SIGN
    {{'O', 0, 0, 0}}, // LATIN CAPITAL LETTER O WITH STROKE
    {{'U', 0, 0, 0}}, // LATIN CAPITAL LETTER U WITH GRAVE
    {{'U', 0, 0, 0}}, // LATIN CAPITAL LETTER U WITH ACUTE
    {{'U', 0, 0, 0}}, // LATIN CAPITAL LETTER U WITH CIRCUMFLEX
    {{'U', 0, 0, 0}}, // LATIN CAPITAL LETTER U WITH DIAERESIS
    {{'Y', 0, 0, 0}}, // LATIN CAPITAL LETTER Y WITH ACUTE
    {{'t', 'h', 0, 0}}, // LATIN CAPITAL LETTER THORN (Icelandic)
    {{'s', 0, 0, 0}}, // LATIN SMALL LETTER SHARP S (German)
    {{'a', 0, 0, 0}}, // LATIN SMALL LETTER A WITH GRAVE
    {{'a', 0, 0, 0}}, // LATIN SMALL LETTER A WITH ACUTE
    {{'a', 0, 0, 0}}, // LATIN SMALL LETTER A WITH CIRCUMFLEX
    {{'a', 0, 0, 0}}, // LATIN SMALL LETTER A WITH TILDE
    {{'a', 0, 0, 0}}, // LATIN SMALL LETTER A WITH DIAERESIS
    {{'a', 0, 0, 0}}, // LATIN SMALL LETTER A WITH RING ABOVE
    {{'a', 'e', 0, 0}}, // LATIN SMALL LETTER AE
    {{'c', 0, 0, 0}}, // LATIN SMALL LETTER C WITH CEDILLA
    {{'e', 0, 0, 0}}, // LATIN SMALL LETTER E WITH GRAVE
    {{'e', 0, 0, 0}}, // LATIN SMALL LETTER E WITH ACUTE
    {{'e', 0, 0, 0}}, // LATIN SMALL LETTER E WITH CIRCUMFLEX
    {{'e', 0, 0, 0}}, // LATIN SMALL LETTER E WITH DIAERESIS
    {{'i', 0, 0, 0}}, // LATIN SMALL LETTER I WITH GRAVE
    {{'i', 0, 0, 0}}, // LATIN SMALL LETTER I WITH ACUTE
    {{'i', 0, 0, 0}}, // LATIN SMALL LETTER I WITH CIRCUMFLEX
    {{'i', 0, 0, 0}}, // LATIN SMALL LETTER I WITH DIAERESIS
    {{'T', 'H', 0, 0}}, // LATIN SMALL LETTER ETH (Icelandic)
    {{'n', 0, 0, 0}}, // LATIN SMALL LETTER N WITH TILDE
    {{'o', 0, 0, 0}}, // LATIN SMALL LETTER O WITH GRAVE
    {{'o', 0, 0, 0}}, // LATIN SMALL LETTER O WITH ACUTE
    {{'o', 0, 0, 0}}, // LATIN SMALL LETTER O WITH CIRCUMFLEX
    {{'o', 0, 0, 0}}, // LATIN SMALL LETTER O WITH TILDE
    {{'o', 0, 0, 0}}, // LATIN SMALL LETTER O WITH DIAERESIS
    {{'/', 0, 0, 0}}, // DIVISION SIGN
    {{'o', 0, 0, 0}}, // LATIN SMALL LETTER O WITH STROKE
    {{'u', 0, 0, 0}}, // LATIN SMALL LETTER U WITH GRAVE
    {{'u', 0, 0, 0}}, // LATIN SMALL LETTER U WITH ACUTE
    {{'u', 0, 0, 0}}, // LATIN SMALL LETTER U WITH CIRCUMFLEX
    {{'u', 0, 0, 0}}, // LATIN SMALL LETTER U WITH DIAERESIS
    {{'y', 0, 0, 0}}, // LATIN SMALL LETTER Y WITH ACUTE
    {{'t', 'h', 0, 0}}, // LATIN SMALL LETTER THORN (Icelandic)
    {{'y', 0, 0, 0}}, // LATIN SMALL LETTER Y WITH DIAERESIS
};

CF_INLINE UInt32 __CFToASCIILatin1Fallback(UniChar character, UInt8 *bytes, UInt32 maxByteLen) {
    const char *losChars = (const unsigned char*)_toLossyASCIITable + (character - 0xA0) * sizeof(unsigned char[4]);
    unsigned int numBytes = 0;
    int idx, max = (maxByteLen && (maxByteLen < 4) ? maxByteLen : 4);

    for (idx = 0;idx < max;idx++) {
        if (losChars[idx]) {
            if (maxByteLen) bytes[idx] = losChars[idx];
            ++numBytes;
        } else {
            break;
        }
    }

    return numBytes;
}

static UInt32 __CFDefaultToBytesFallbackProc(const UniChar *characters, UInt32 numChars, uint8_t *bytes, UInt32 maxByteLen, UInt32 *usedByteLen) {
    if (*characters < 0xA0) { // 0x80 to 0x9F maps to ASCII C0 range
        if (maxByteLen) *bytes = (UInt8)(*characters - 0x80);
        *usedByteLen = 1;
        return 1;
    } else if (*characters < 0x100) {
        *usedByteLen = __CFToASCIILatin1Fallback(*characters, bytes, maxByteLen);
        return 1;
    } else if (*characters >= kSurrogateHighStart && *characters <= kSurrogateLowEnd) {
        if (maxByteLen) *bytes = '?';
        *usedByteLen = 1;
        return (numChars > 1 && *characters <= kSurrogateLowStart && *(characters + 1) >= kSurrogateLowStart && *(characters + 1) <= kSurrogateLowEnd ? 2 : 1);
    } else if (CFUniCharIsMemberOf(*characters, kCFUniCharWhitespaceCharacterSet)) {
        if (maxByteLen) *bytes = ' ';
        *usedByteLen = 1;
        return 1;
    } else if (CFUniCharIsMemberOf(*characters, kCFUniCharWhitespaceAndNewlineCharacterSet)) {
        if (maxByteLen) *bytes = ASCIINewLine;
        *usedByteLen = 1;
        return 1;
    } else if (!CFUniCharIsMemberOf(*characters, kCFUniCharLetterCharacterSet)) {
        *usedByteLen = 0;
        return 1;
    } else if (CFUniCharIsMemberOf(*characters, kCFUniCharDecomposableCharacterSet)) {
        UTF32Char decomposed[MAX_DECOMPOSED_LENGTH];

        (void)CFUniCharDecomposeCharacter(*characters, decomposed, MAX_DECOMPOSED_LENGTH);
        if (*decomposed < 0x80) {
            if (maxByteLen) *bytes = (UInt8)(*decomposed);
            *usedByteLen = 1;
            return 1;
        } else {
            UTF16Char theChar = *decomposed;

            return __CFDefaultToBytesFallbackProc(&theChar, 1, bytes, maxByteLen, usedByteLen);
        }
    } else {
        if (maxByteLen) *bytes = '?';
        *usedByteLen = 1;
        return 1;
    }
}

static UInt32 __CFDefaultToUnicodeFallbackProc(const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    if (maxCharLen) *characters = (UniChar)'?';
    *usedCharLen = 1;
    return 1;
}

#define TO_BYTE_FALLBACK(conv,chars,numChars,bytes,max,used) (conv->toBytesFallback(chars,numChars,bytes,max,used))
#define TO_UNICODE_FALLBACK(conv,bytes,numBytes,chars,max,used) (conv->toUnicodeFallback(bytes,numBytes,chars,max,used))

#define EXTRA_BASE (0x0F00)

/* Wrapper funcs for non-standard converters
*/
static UInt32 __CFToBytesCheapEightBitWrapper(const void *converter, UInt32 flags, const UniChar *characters, UInt32 numChars, uint8_t *bytes, UInt32 maxByteLen, UInt32 *usedByteLen) {
    UInt32 processedCharLen = 0;
    UInt32 length = (maxByteLen && (maxByteLen < numChars) ? maxByteLen : numChars);
    uint8_t byte;

    while (processedCharLen < length) {
        if (!((CFStringEncodingCheapEightBitToBytesProc)((const _CFEncodingConverter*)converter)->_toBytes)(flags, characters[processedCharLen], &byte)) break;

        if (maxByteLen) bytes[processedCharLen] = byte;
        processedCharLen++;
    }

    *usedByteLen = processedCharLen;
    return processedCharLen;
}

static UInt32 __CFToUnicodeCheapEightBitWrapper(const void *converter, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    UInt32 processedByteLen = 0;
    UInt32 length = (maxCharLen && (maxCharLen < numBytes) ? maxCharLen : numBytes);
    UniChar character;

    while (processedByteLen < length) {
        if (!((CFStringEncodingCheapEightBitToUnicodeProc)((const _CFEncodingConverter*)converter)->_toUnicode)(flags, bytes[processedByteLen], &character)) break;

        if (maxCharLen) characters[processedByteLen] = character;
        processedByteLen++;
    }

    *usedCharLen = processedByteLen;
    return processedByteLen;
}

static UInt32 __CFToCanonicalUnicodeCheapEightBitWrapper(const void *converter, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    UInt32 processedByteLen = 0;
    UInt32 theUsedCharLen = 0;
    UTF32Char charBuffer[MAX_DECOMPOSED_LENGTH];
    UInt32 usedLen;
    UniChar character;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);

    while ((processedByteLen < numBytes) && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if (!((CFStringEncodingCheapEightBitToUnicodeProc)((const _CFEncodingConverter*)converter)->_toUnicode)(flags, bytes[processedByteLen], &character)) break;

        if (CFUniCharIsDecomposableCharacter(character, isHFSPlus)) {
            uint32_t idx;

            usedLen = CFUniCharDecomposeCharacter(character, charBuffer, MAX_DECOMPOSED_LENGTH);
            *usedCharLen = theUsedCharLen;

            for (idx = 0;idx < usedLen;idx++) {
                if (charBuffer[idx] > 0xFFFF) { // Non-BMP
                    if (theUsedCharLen + 2 > maxCharLen)  return processedByteLen;
                    theUsedCharLen += 2;
                    if (maxCharLen) {
                        charBuffer[idx] = charBuffer[idx] - 0x10000;
                        *(characters++) = (charBuffer[idx] >> 10) + 0xD800UL;
                        *(characters++) = (charBuffer[idx] & 0x3FF) + 0xDC00UL;
                    }
                } else {
                    if (theUsedCharLen + 1 > maxCharLen)  return processedByteLen;
                    ++theUsedCharLen;
                    *(characters++) = charBuffer[idx];
                }
            }
        } else {
            if (maxCharLen) *(characters++) = character;
            ++theUsedCharLen;
        }
        processedByteLen++;
    }

    *usedCharLen = theUsedCharLen;
    return processedByteLen;
}

static UInt32 __CFToBytesStandardEightBitWrapper(const void *converter, UInt32 flags, const UniChar *characters, UInt32 numChars, uint8_t *bytes, UInt32 maxByteLen, UInt32 *usedByteLen) {
    UInt32 processedCharLen = 0;
    uint8_t byte;
    UInt32 usedLen;

    *usedByteLen = 0;

    while (numChars && (!maxByteLen || (*usedByteLen < maxByteLen))) {
        if (!(usedLen = ((CFStringEncodingStandardEightBitToBytesProc)((const _CFEncodingConverter*)converter)->_toBytes)(flags, characters, numChars, &byte))) break;

        if (maxByteLen) bytes[*usedByteLen] = byte;
        (*usedByteLen)++;
        characters += usedLen;
        numChars -= usedLen;
        processedCharLen += usedLen;
    }

    return processedCharLen;
}

static UInt32 __CFToUnicodeStandardEightBitWrapper(const void *converter, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    UInt32 processedByteLen = 0;
#if defined(__MACOS8__) || defined(__WIN32__)
    UniChar charBuffer[20]; // Dynamic stack allocation is GNU specific
#else
    UniChar charBuffer[((const _CFEncodingConverter*)converter)->maxLen];
#endif
    UInt32 usedLen;

    *usedCharLen = 0;

    while ((processedByteLen < numBytes) && (!maxCharLen || (*usedCharLen < maxCharLen))) {
        if (!(usedLen = ((CFStringEncodingCheapEightBitToUnicodeProc)((const _CFEncodingConverter*)converter)->_toUnicode)(flags, bytes[processedByteLen], charBuffer))) break;

        if (maxCharLen) {
            uint16_t idx;

            if (*usedCharLen + usedLen > maxCharLen) break;

            for (idx = 0;idx < usedLen;idx++) {
                characters[*usedCharLen + idx] = charBuffer[idx];
            }
        }
        *usedCharLen += usedLen;
        processedByteLen++;
    }

    return processedByteLen;
}

static UInt32 __CFToCanonicalUnicodeStandardEightBitWrapper(const void *converter, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    UInt32 processedByteLen = 0;
#if defined(__MACOS8__) || defined(__WIN32__)
    UniChar charBuffer[20]; // Dynamic stack allocation is GNU specific
#else
    UniChar charBuffer[((const _CFEncodingConverter*)converter)->maxLen];
#endif
    UTF32Char decompBuffer[MAX_DECOMPOSED_LENGTH];
    UInt32 usedLen;
    UInt32 decompedLen;
    UInt32 idx, decompIndex;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);
    UInt32 theUsedCharLen = 0;

    while ((processedByteLen < numBytes) && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if (!(usedLen = ((CFStringEncodingCheapEightBitToUnicodeProc)((const _CFEncodingConverter*)converter)->_toUnicode)(flags, bytes[processedByteLen], charBuffer))) break;

        for (idx = 0;idx < usedLen;idx++) {
            if (CFUniCharIsDecomposableCharacter(charBuffer[idx], isHFSPlus)) {
                decompedLen = CFUniCharDecomposeCharacter(charBuffer[idx], decompBuffer, MAX_DECOMPOSED_LENGTH);
                *usedCharLen = theUsedCharLen;

                for (decompIndex = 0;decompIndex < decompedLen;decompIndex++) {
                    if (decompBuffer[decompIndex] > 0xFFFF) { // Non-BMP
                        if (theUsedCharLen + 2 > maxCharLen)  return processedByteLen;
                        theUsedCharLen += 2;
                        if (maxCharLen) {
                            charBuffer[idx] = charBuffer[idx] - 0x10000;
                            *(characters++) = (charBuffer[idx] >> 10) + 0xD800UL;
                            *(characters++) = (charBuffer[idx] & 0x3FF) + 0xDC00UL;
                        }
                    } else {
                        if (theUsedCharLen + 1 > maxCharLen)  return processedByteLen;
                        ++theUsedCharLen;
                        *(characters++) = charBuffer[idx];
                    }
                }
            } else {
                if (maxCharLen) *(characters++) = charBuffer[idx];
                ++theUsedCharLen;
            }
        }
        processedByteLen++;
    }

    *usedCharLen = theUsedCharLen;
    return processedByteLen;
}

static UInt32 __CFToBytesCheapMultiByteWrapper(const void *converter, UInt32 flags, const UniChar *characters, UInt32 numChars, uint8_t *bytes, UInt32 maxByteLen, UInt32 *usedByteLen) {
    UInt32 processedCharLen = 0;
#if defined(__MACOS8__) || defined(__WIN32__)
    uint8_t byteBuffer[20]; // Dynamic stack allocation is GNU specific
#else
    uint8_t byteBuffer[((const _CFEncodingConverter*)converter)->maxLen];
#endif
    UInt32 usedLen;

    *usedByteLen = 0;

    while ((processedCharLen < numChars) && (!maxByteLen || (*usedByteLen < maxByteLen))) {
        if (!(usedLen = ((CFStringEncodingCheapMultiByteToBytesProc)((const _CFEncodingConverter*)converter)->_toBytes)(flags, characters[processedCharLen], byteBuffer))) break;

        if (maxByteLen) {
            uint16_t idx;

            if (*usedByteLen + usedLen > maxByteLen) break;

            for (idx = 0;idx <usedLen;idx++) {
                bytes[*usedByteLen + idx] = byteBuffer[idx];
            }
        }

        *usedByteLen += usedLen;
        processedCharLen++;
    }

    return processedCharLen;
}

static UInt32 __CFToUnicodeCheapMultiByteWrapper(const void *converter, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    UInt32 processedByteLen = 0;
    UniChar character;
    UInt32 usedLen;

    *usedCharLen = 0;

    while (numBytes && (!maxCharLen || (*usedCharLen < maxCharLen))) {
        if (!(usedLen = ((CFStringEncodingCheapMultiByteToUnicodeProc)((const _CFEncodingConverter*)converter)->_toUnicode)(flags, bytes, numBytes, &character))) break;

        if (maxCharLen) *(characters++) = character;
        (*usedCharLen)++;
        processedByteLen += usedLen;
        bytes += usedLen;
        numBytes -= usedLen;
    }

    return processedByteLen;
}

static UInt32 __CFToCanonicalUnicodeCheapMultiByteWrapper(const void *converter, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    UInt32 processedByteLen = 0;
    UTF32Char charBuffer[MAX_DECOMPOSED_LENGTH];
    UniChar character;
    UInt32 usedLen;
    UInt32 decomposedLen;
    UInt32 theUsedCharLen = 0;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);

    while (numBytes && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if (!(usedLen = ((CFStringEncodingCheapMultiByteToUnicodeProc)((const _CFEncodingConverter*)converter)->_toUnicode)(flags, bytes, numBytes, &character))) break;

        if (CFUniCharIsDecomposableCharacter(character, isHFSPlus)) {
            uint32_t idx;

            decomposedLen = CFUniCharDecomposeCharacter(character, charBuffer, MAX_DECOMPOSED_LENGTH);
            *usedCharLen = theUsedCharLen;

            for (idx = 0;idx < decomposedLen;idx++) {
                if (charBuffer[idx] > 0xFFFF) { // Non-BMP
                    if (theUsedCharLen + 2 > maxCharLen)  return processedByteLen;
                    theUsedCharLen += 2;
                    if (maxCharLen) {
                        charBuffer[idx] = charBuffer[idx] - 0x10000;
                        *(characters++) = (charBuffer[idx] >> 10) + 0xD800UL;
                        *(characters++) = (charBuffer[idx] & 0x3FF) + 0xDC00UL;
                    }
                } else {
                    if (theUsedCharLen + 1 > maxCharLen)  return processedByteLen;
                    ++theUsedCharLen;
                    *(characters++) = charBuffer[idx];
                }
            }
        } else {
            if (maxCharLen) *(characters++) = character;
            ++theUsedCharLen;
        }

        processedByteLen += usedLen;
        bytes += usedLen;
        numBytes -= usedLen;
    }
    *usedCharLen = theUsedCharLen;
    return processedByteLen;
}

/* static functions
*/
static _CFConverterEntry __CFConverterEntryASCII = {
    kCFStringEncodingASCII, NULL,
    "Western (ASCII)", {"us-ascii", "ascii", "iso-646-us", NULL}, NULL, NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static _CFConverterEntry __CFConverterEntryISOLatin1 = {
    kCFStringEncodingISOLatin1, NULL,
    "Western (ISO Latin 1)", {"iso-8859-1", "latin1","iso-latin-1", NULL}, NULL, NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static _CFConverterEntry __CFConverterEntryMacRoman = {
    kCFStringEncodingMacRoman, NULL,
    "Western (Mac OS Roman)", {"macintosh", "mac", "x-mac-roman", NULL}, NULL, NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static _CFConverterEntry __CFConverterEntryWinLatin1 = {
    kCFStringEncodingWindowsLatin1, NULL,
    "Western (Windows Latin 1)", {"windows-1252", "cp1252", "windows latin1", NULL}, NULL, NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static _CFConverterEntry __CFConverterEntryNextStepLatin = {
    kCFStringEncodingNextStepLatin, NULL,
    "Western (NextStep)", {"x-nextstep", NULL, NULL, NULL}, NULL, NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static _CFConverterEntry __CFConverterEntryUTF8 = {
    kCFStringEncodingUTF8, NULL,
    "UTF-8", {"utf-8", "unicode-1-1-utf8", NULL, NULL}, NULL, NULL, NULL, NULL,
    kCFStringEncodingUnicode // We use string encoding's script range here
};

CF_INLINE _CFConverterEntry *__CFStringEncodingConverterGetEntry(UInt32 encoding) {
    switch (encoding) {
        case kCFStringEncodingInvalidId:
        case kCFStringEncodingASCII:
            return &__CFConverterEntryASCII;

        case kCFStringEncodingISOLatin1:
            return &__CFConverterEntryISOLatin1;

        case kCFStringEncodingMacRoman:
            return &__CFConverterEntryMacRoman;

        case kCFStringEncodingWindowsLatin1:
            return &__CFConverterEntryWinLatin1;

        case kCFStringEncodingNextStepLatin:
            return &__CFConverterEntryNextStepLatin;

        case kCFStringEncodingUTF8:
            return &__CFConverterEntryUTF8;

        default: return NULL;
    }
}

CF_INLINE _CFEncodingConverter *__CFEncodingConverterFromDefinition(const CFStringEncodingConverter *definition) {
#define NUM_OF_ENTRIES_CYCLE (10)
    static CFSpinLock_t _indexLock = 0;
    static UInt32 _currentIndex = 0;
    static UInt32 _allocatedSize = 0;
    static _CFEncodingConverter *_allocatedEntries = NULL;
    _CFEncodingConverter *converter;


    __CFSpinLock(&_indexLock);
    if ((_currentIndex + 1) >= _allocatedSize) {
        _currentIndex = 0;
        _allocatedSize = 0;
        _allocatedEntries = NULL;
    }
    if (_allocatedEntries == NULL) { // Not allocated yet
        _allocatedEntries = (_CFEncodingConverter *)CFAllocatorAllocate(NULL, sizeof(_CFEncodingConverter) * NUM_OF_ENTRIES_CYCLE, 0);
        _allocatedSize = NUM_OF_ENTRIES_CYCLE;
        converter = &(_allocatedEntries[_currentIndex]);
    } else {
        converter = &(_allocatedEntries[++_currentIndex]);
    }
    __CFSpinUnlock(&_indexLock);

    switch (definition->encodingClass) {
        case kCFStringEncodingConverterStandard:
            converter->toBytes = definition->toBytes;
            converter->toUnicode = definition->toUnicode;
            converter->toCanonicalUnicode = definition->toUnicode;
            converter->_toBytes = NULL;
            converter->_toUnicode = NULL;
            converter->maxLen = 2;
            break;

        case kCFStringEncodingConverterCheapEightBit:
            converter->toBytes = __CFToBytesCheapEightBitWrapper;
            converter->toUnicode = __CFToUnicodeCheapEightBitWrapper;
            converter->toCanonicalUnicode = __CFToCanonicalUnicodeCheapEightBitWrapper;
            converter->_toBytes = definition->toBytes;
            converter->_toUnicode = definition->toUnicode;
            converter->maxLen = 1;
            break;

        case kCFStringEncodingConverterStandardEightBit:
            converter->toBytes = __CFToBytesStandardEightBitWrapper;
            converter->toUnicode = __CFToUnicodeStandardEightBitWrapper;
            converter->toCanonicalUnicode = __CFToCanonicalUnicodeStandardEightBitWrapper;
            converter->_toBytes = definition->toBytes;
            converter->_toUnicode = definition->toUnicode;
            converter->maxLen = definition->maxDecomposedCharLen;
            break;

        case kCFStringEncodingConverterCheapMultiByte:
            converter->toBytes = __CFToBytesCheapMultiByteWrapper;
            converter->toUnicode = __CFToUnicodeCheapMultiByteWrapper;
            converter->toCanonicalUnicode = __CFToCanonicalUnicodeCheapMultiByteWrapper;
            converter->_toBytes = definition->toBytes;
            converter->_toUnicode = definition->toUnicode;
            converter->maxLen = definition->maxBytesPerChar;
            break;

        case kCFStringEncodingConverterPlatformSpecific:
            converter->toBytes = NULL;
            converter->toUnicode = NULL;
            converter->toCanonicalUnicode = NULL;
            converter->_toBytes = NULL;
            converter->_toUnicode = NULL;
            converter->maxLen = 0;
            converter->toBytesLen = NULL;
            converter->toUnicodeLen = NULL;
            converter->toBytesFallback = NULL;
            converter->toUnicodeFallback = NULL;
            converter->toBytesPrecompose = NULL;
            converter->isValidCombiningChar = NULL;
            return converter;
            
        default: // Shouln't be here
            return NULL;
    }

    converter->toBytesLen = (definition->toBytesLen ? definition->toBytesLen : (CFStringEncodingToBytesLenProc)(UInt32)definition->maxBytesPerChar);
    converter->toUnicodeLen = (definition->toUnicodeLen ? definition->toUnicodeLen : (CFStringEncodingToUnicodeLenProc)(UInt32)definition->maxDecomposedCharLen);
    converter->toBytesFallback = (definition->toBytesFallback ? definition->toBytesFallback : __CFDefaultToBytesFallbackProc);
    converter->toUnicodeFallback = (definition->toUnicodeFallback ? definition->toUnicodeFallback : __CFDefaultToUnicodeFallbackProc);
    converter->toBytesPrecompose = (definition->toBytesPrecompose ? definition->toBytesPrecompose : NULL);
    converter->isValidCombiningChar = (definition->isValidCombiningChar ? definition->isValidCombiningChar : NULL);

    return converter;
}

CF_INLINE const CFStringEncodingConverter *__CFStringEncodingConverterGetDefinition(_CFConverterEntry *entry) {
    if (!entry) return NULL;
    
    switch (entry->encoding) {
        case kCFStringEncodingASCII:
            return &__CFConverterASCII;

        case kCFStringEncodingISOLatin1:
            return &__CFConverterISOLatin1;

        case kCFStringEncodingMacRoman:
            return &__CFConverterMacRoman;

        case kCFStringEncodingWindowsLatin1:
            return &__CFConverterWinLatin1;

        case kCFStringEncodingNextStepLatin:
            return &__CFConverterNextStepLatin;

        case kCFStringEncodingUTF8:
            return &__CFConverterUTF8;

        default:
            return NULL;
    }
}

static const _CFEncodingConverter *__CFGetConverter(UInt32 encoding) {
    _CFConverterEntry *entry = __CFStringEncodingConverterGetEntry(encoding);

    if (!entry) return NULL;

    if (!entry->converter) {
        const CFStringEncodingConverter *definition = __CFStringEncodingConverterGetDefinition(entry);

        if (definition) {
            entry->converter = __CFEncodingConverterFromDefinition(definition);
            entry->toBytesFallback = definition->toBytesFallback;
            entry->toUnicodeFallback = definition->toUnicodeFallback;
        }
    }

    return (_CFEncodingConverter *)entry->converter;
}

/* Public API
*/
UInt32 CFStringEncodingUnicodeToBytes(UInt32 encoding, UInt32 flags, const UniChar *characters, UInt32 numChars, UInt32 *usedCharLen, uint8_t *bytes, UInt32 maxByteLen, UInt32 *usedByteLen) {
    if (encoding == kCFStringEncodingUTF8) {
        static CFStringEncodingToBytesProc __CFToUTF8 = NULL;
        uint32_t convertedCharLen;
        uint32_t usedLen;


        if ((flags & kCFStringEncodingUseCanonical) || (flags & kCFStringEncodingUseHFSPlusCanonical)) {
            (void)CFUniCharDecompose(characters, numChars, &convertedCharLen, (void *)bytes, maxByteLen, &usedLen, true, kCFUniCharUTF8Format, (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false));
        } else {
            if (!__CFToUTF8) {
                const CFStringEncodingConverter *utf8Converter = CFStringEncodingGetConverter(kCFStringEncodingUTF8);
                __CFToUTF8 = (CFStringEncodingToBytesProc)utf8Converter->toBytes;
            }
            convertedCharLen = __CFToUTF8(0, characters, numChars, bytes, maxByteLen, (UInt32 *)&usedLen);
        }
        if (usedCharLen) *usedCharLen = convertedCharLen;
        if (usedByteLen) *usedByteLen = usedLen;

        if (convertedCharLen == numChars) {
            return kCFStringEncodingConversionSuccess;
        } else if (maxByteLen && (maxByteLen == usedLen)) {
            return kCFStringEncodingInsufficientOutputBufferLength;
        } else {
            return kCFStringEncodingInvalidInputStream;
        }
    } else {
        const _CFEncodingConverter *converter = __CFGetConverter(encoding);
        UInt32 usedLen = 0;
        UInt32 localUsedByteLen;
        UInt32 theUsedByteLen = 0;
        UInt32 theResult = kCFStringEncodingConversionSuccess;
        CFStringEncodingToBytesPrecomposeProc toBytesPrecompose = NULL;
        CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar = NULL;

        if (!converter) return kCFStringEncodingConverterUnavailable;

        if (flags & kCFStringEncodingSubstituteCombinings) {
            if (!(flags & kCFStringEncodingAllowLossyConversion)) isValidCombiningChar = converter->isValidCombiningChar;
       } else {
            isValidCombiningChar = converter->isValidCombiningChar;
            if (!(flags & kCFStringEncodingIgnoreCombinings)) {
                toBytesPrecompose = converter->toBytesPrecompose;
                flags |= kCFStringEncodingComposeCombinings;
            }
        }


        while ((usedLen < numChars) && (!maxByteLen || (theUsedByteLen < maxByteLen))) {
            if ((usedLen += TO_BYTE(converter, flags, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen)) < numChars) {
                UInt32 dummy;

                if (isValidCombiningChar && (usedLen > 0) && isValidCombiningChar(characters[usedLen])) {
                    if (toBytesPrecompose) {
                        UInt32 localUsedLen = usedLen;

                        while (isValidCombiningChar(characters[--usedLen]));
                        theUsedByteLen += localUsedByteLen;
                        if (converter->maxLen > 1) {
                            TO_BYTE(converter, flags, characters + usedLen, localUsedLen - usedLen, NULL, 0, &localUsedByteLen);
                            theUsedByteLen -= localUsedByteLen;
                        } else {
                            theUsedByteLen--;
                        }
                        if ((localUsedLen = toBytesPrecompose(flags, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen)) > 0) {
                            usedLen += localUsedLen;
                            if ((usedLen < numChars) && isValidCombiningChar(characters[usedLen])) { // There is a non-base char not combined remaining
                                theUsedByteLen += localUsedByteLen;
                                theResult = kCFStringEncodingInvalidInputStream;
                                break;
                            }
                        } else if (flags & kCFStringEncodingAllowLossyConversion) {
                            uint8_t lossyByte = CFStringEncodingMaskToLossyByte(flags);

                            if (lossyByte) {
								while (isValidCombiningChar(characters[++usedLen]));
                                localUsedByteLen = 1;
                                if (maxByteLen) *(bytes + theUsedByteLen) = lossyByte;
                            } else {
                                ++usedLen;
                                usedLen += TO_BYTE_FALLBACK(converter, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen);
                            }
                        } else {
                            theResult = kCFStringEncodingInvalidInputStream;
                            break;
                        }
                    } else if (maxByteLen && ((maxByteLen == theUsedByteLen + localUsedByteLen) || TO_BYTE(converter, flags, characters + usedLen, numChars - usedLen, NULL, 0, &dummy))) { // buffer was filled up
                                    theUsedByteLen += localUsedByteLen;
                                    theResult = kCFStringEncodingInsufficientOutputBufferLength;
                                    break;
                    } else if (flags & kCFStringEncodingIgnoreCombinings) {
                        while ((++usedLen < numChars) && isValidCombiningChar(characters[usedLen]));
                    } else {
                        uint8_t lossyByte = CFStringEncodingMaskToLossyByte(flags);

                        theUsedByteLen += localUsedByteLen;
                        if (lossyByte) {
                            ++usedLen;
                            localUsedByteLen = 1;
                            if (maxByteLen) *(bytes + theUsedByteLen) = lossyByte;
                        } else {
                            usedLen += TO_BYTE_FALLBACK(converter, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen);
                        }
                    }
                } else if (maxByteLen && ((maxByteLen == theUsedByteLen + localUsedByteLen) || TO_BYTE(converter, flags, characters + usedLen, numChars - usedLen, NULL, 0, &dummy))) { // buffer was filled up
                    theUsedByteLen += localUsedByteLen;

                    if (flags & kCFStringEncodingAllowLossyConversion && !CFStringEncodingMaskToLossyByte(flags)) {
                        UInt32 localUsedLen;

                        localUsedByteLen = 0;
                        while ((usedLen < numChars) && !localUsedByteLen && (localUsedLen = TO_BYTE_FALLBACK(converter, characters + usedLen, numChars - usedLen, NULL, 0, &localUsedByteLen))) usedLen += localUsedLen;
                    }
                    if (usedLen < numChars) theResult = kCFStringEncodingInsufficientOutputBufferLength;
                    break;
                } else if (flags & kCFStringEncodingAllowLossyConversion) {
                    uint8_t lossyByte = CFStringEncodingMaskToLossyByte(flags);

                    theUsedByteLen += localUsedByteLen;
                    if (lossyByte) {
                        ++usedLen;
                        localUsedByteLen = 1;
                        if (maxByteLen) *(bytes + theUsedByteLen) = lossyByte;
                    } else {
                        usedLen += TO_BYTE_FALLBACK(converter, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen);
                    }
                } else {
                    theUsedByteLen += localUsedByteLen;
                    theResult = kCFStringEncodingInvalidInputStream;
                    break;
                }
            }
            theUsedByteLen += localUsedByteLen;
        }

        if (usedLen < numChars && maxByteLen && theResult == kCFStringEncodingConversionSuccess) {
            if (flags & kCFStringEncodingAllowLossyConversion && !CFStringEncodingMaskToLossyByte(flags)) {
                UInt32 localUsedLen;

                localUsedByteLen = 0;
                while ((usedLen < numChars) && !localUsedByteLen && (localUsedLen = TO_BYTE_FALLBACK(converter, characters + usedLen, numChars - usedLen, NULL, 0, &localUsedByteLen))) usedLen += localUsedLen;
            }
            if (usedLen < numChars) theResult = kCFStringEncodingInsufficientOutputBufferLength;
        }
        if (usedByteLen) *usedByteLen = theUsedByteLen;
        if (usedCharLen) *usedCharLen = usedLen;

        return theResult;
    }
}

UInt32 CFStringEncodingBytesToUnicode(UInt32 encoding, UInt32 flags, const uint8_t *bytes, UInt32 numBytes, UInt32 *usedByteLen, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen) {
    const _CFEncodingConverter *converter = __CFGetConverter(encoding);
    UInt32 usedLen = 0;
    UInt32 theUsedCharLen = 0;
    UInt32 localUsedCharLen;
    UInt32 theResult = kCFStringEncodingConversionSuccess;

    if (!converter) return kCFStringEncodingConverterUnavailable;


    while ((usedLen < numBytes) && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if ((usedLen += TO_UNICODE(converter, flags, bytes + usedLen, numBytes - usedLen, characters + theUsedCharLen, (maxCharLen ? maxCharLen - theUsedCharLen : 0), &localUsedCharLen)) < numBytes) {
            UInt32 tempUsedCharLen;

            if (maxCharLen && ((maxCharLen == theUsedCharLen + localUsedCharLen) || ((flags & (kCFStringEncodingUseCanonical|kCFStringEncodingUseHFSPlusCanonical)) && TO_UNICODE(converter, flags, bytes + usedLen, numBytes - usedLen, NULL, 0, &tempUsedCharLen)))) { // buffer was filled up
                theUsedCharLen += localUsedCharLen;
                theResult = kCFStringEncodingInsufficientOutputBufferLength;
                break;
            } else if (flags & kCFStringEncodingAllowLossyConversion) {
                theUsedCharLen += localUsedCharLen;
                usedLen += TO_UNICODE_FALLBACK(converter, bytes + usedLen, numBytes - usedLen, characters + theUsedCharLen, (maxCharLen ? maxCharLen - theUsedCharLen : 0), &localUsedCharLen);
            } else {
                theUsedCharLen += localUsedCharLen;
                theResult = kCFStringEncodingInvalidInputStream;
                break;
            }
        }
        theUsedCharLen += localUsedCharLen;
    }

    if (usedLen < numBytes && maxCharLen && theResult == kCFStringEncodingConversionSuccess) {
        theResult = kCFStringEncodingInsufficientOutputBufferLength;
    }
    if (usedCharLen) *usedCharLen = theUsedCharLen;
    if (usedByteLen) *usedByteLen = usedLen;

    return theResult;
}

__private_extern__ Boolean CFStringEncodingIsValidEncoding(UInt32 encoding) {
    return (CFStringEncodingGetConverter(encoding) ? true : false);
}

__private_extern__ const char *CFStringEncodingName(UInt32 encoding) {
    _CFConverterEntry *entry = __CFStringEncodingConverterGetEntry(encoding);
    if (entry) return entry->encodingName;
    return NULL;
}

__private_extern__ const char **CFStringEncodingCanonicalCharsetNames(UInt32 encoding) {
    _CFConverterEntry *entry = __CFStringEncodingConverterGetEntry(encoding);
    if (entry) return entry->ianaNames;
    return NULL;
}

__private_extern__ UInt32 CFStringEncodingGetScriptCodeForEncoding(CFStringEncoding encoding) {
    _CFConverterEntry *entry = __CFStringEncodingConverterGetEntry(encoding);

    return (entry ? entry->scriptCode : (encoding == kCFStringEncodingUnicode ? kCFStringEncodingUnicode : (encoding < 0xFF ? encoding : kCFStringEncodingInvalidId)));
}

__private_extern__ UInt32 CFStringEncodingCharLengthForBytes(UInt32 encoding, UInt32 flags, const uint8_t *bytes, UInt32 numBytes) {
    const _CFEncodingConverter *converter = __CFGetConverter(encoding);

    if (converter) {
        UInt32 switchVal = (UInt32)(converter->toUnicodeLen);

            if (switchVal < 0xFFFF)
            return switchVal * numBytes;
        else
            return converter->toUnicodeLen(flags, bytes, numBytes);
    }

    return 0;
}

__private_extern__ UInt32 CFStringEncodingByteLengthForCharacters(UInt32 encoding, UInt32 flags, const UniChar *characters, UInt32 numChars) {
    const _CFEncodingConverter *converter = __CFGetConverter(encoding);

    if (converter) {
        UInt32 switchVal = (UInt32)(converter->toBytesLen);

            if (switchVal < 0xFFFF)
            return switchVal * numChars;
        else
            return converter->toBytesLen(flags, characters, numChars);
    }

    return 0;
}

__private_extern__ void CFStringEncodingRegisterFallbackProcedures(UInt32 encoding, CFStringEncodingToBytesFallbackProc toBytes, CFStringEncodingToUnicodeFallbackProc toUnicode) {
    _CFConverterEntry *entry = __CFStringEncodingConverterGetEntry(encoding);

    if (entry && __CFGetConverter(encoding)) {
        ((_CFEncodingConverter*)entry->converter)->toBytesFallback = (toBytes ? toBytes : entry->toBytesFallback);
        ((_CFEncodingConverter*)entry->converter)->toUnicodeFallback = (toUnicode ? toUnicode : entry->toUnicodeFallback);
    }
}

__private_extern__ const CFStringEncodingConverter *CFStringEncodingGetConverter(UInt32 encoding) {
    return __CFStringEncodingConverterGetDefinition(__CFStringEncodingConverterGetEntry(encoding));
}

static const UInt32 __CFBuiltinEncodings[] = {
    kCFStringEncodingMacRoman,
    kCFStringEncodingWindowsLatin1,
    kCFStringEncodingISOLatin1,
    kCFStringEncodingNextStepLatin,
    kCFStringEncodingASCII,
    kCFStringEncodingUTF8,
    /* These two are available only in CFString-level */
    kCFStringEncodingUnicode,
    kCFStringEncodingNonLossyASCII,
    kCFStringEncodingInvalidId,
};


__private_extern__ const UInt32 *CFStringEncodingListOfAvailableEncodings(void) {
    return __CFBuiltinEncodings;
}

