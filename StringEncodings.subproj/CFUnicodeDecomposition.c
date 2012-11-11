/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
/*	CFUnicodeDecomposition.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Aki Inoue
*/

#include <string.h>
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFCharacterSet.h>
#include <CoreFoundation/CFUniChar.h>
#include <CoreFoundation/CFUnicodeDecomposition.h>
#include "CFInternal.h"
#include "CFUniCharPriv.h"

// Canonical Decomposition
static UTF32Char *__CFUniCharDecompositionTable = NULL;
static uint32_t __CFUniCharDecompositionTableLength = 0;
static UTF32Char *__CFUniCharMultipleDecompositionTable = NULL;

static const uint8_t *__CFUniCharDecomposableBitmapForBMP = NULL;
static const uint8_t *__CFUniCharHFSPlusDecomposableBitmapForBMP = NULL;
static const uint8_t *__CFUniCharNonBaseBitmapForBMP = NULL;

static CFSpinLock_t __CFUniCharDecompositionTableLock = 0;

static const uint8_t **__CFUniCharCombiningPriorityTable = NULL;
static uint8_t __CFUniCharCombiningPriorityTableNumPlane = 0;

static void __CFUniCharLoadDecompositionTable(void) {

    __CFSpinLock(&__CFUniCharDecompositionTableLock);

    if (NULL == __CFUniCharDecompositionTable) {
        const void *bytes = CFUniCharGetMappingData(kCFUniCharCanonicalDecompMapping);

        if (NULL == bytes) {
            __CFSpinUnlock(&__CFUniCharDecompositionTableLock);
            return;
        }

        __CFUniCharDecompositionTableLength = *(((uint32_t *)bytes)++);
        __CFUniCharDecompositionTable = (UTF32Char *)bytes;
        __CFUniCharMultipleDecompositionTable = (UTF32Char *)((intptr_t)bytes + __CFUniCharDecompositionTableLength);

        __CFUniCharDecompositionTableLength /= (sizeof(uint32_t) * 2);
        __CFUniCharDecomposableBitmapForBMP = CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, 0);
        __CFUniCharHFSPlusDecomposableBitmapForBMP = CFUniCharGetBitmapPtrForPlane(kCFUniCharHFSPlusDecomposableCharacterSet, 0);
        __CFUniCharNonBaseBitmapForBMP = CFUniCharGetBitmapPtrForPlane(kCFUniCharNonBaseCharacterSet, 0);
    }

    __CFSpinUnlock(&__CFUniCharDecompositionTableLock);
}

static CFSpinLock_t __CFUniCharCompatibilityDecompositionTableLock = 0;
static UTF32Char *__CFUniCharCompatibilityDecompositionTable = NULL;
static uint32_t __CFUniCharCompatibilityDecompositionTableLength = 0;
static UTF32Char *__CFUniCharCompatibilityMultipleDecompositionTable = NULL;

static void __CFUniCharLoadCompatibilityDecompositionTable(void) {

    __CFSpinLock(&__CFUniCharCompatibilityDecompositionTableLock);

    if (NULL == __CFUniCharCompatibilityDecompositionTable) {
        const void *bytes = CFUniCharGetMappingData(kCFUniCharCompatibilityDecompMapping);

        if (NULL == bytes) {
            __CFSpinUnlock(&__CFUniCharCompatibilityDecompositionTableLock);
            return;
        }

        __CFUniCharCompatibilityDecompositionTableLength = *(((uint32_t *)bytes)++);
        __CFUniCharCompatibilityDecompositionTable = (UTF32Char *)bytes;
        __CFUniCharCompatibilityMultipleDecompositionTable = (UTF32Char *)((intptr_t)bytes + __CFUniCharCompatibilityDecompositionTableLength);

        __CFUniCharCompatibilityDecompositionTableLength /= (sizeof(uint32_t) * 2);
    }

    __CFSpinUnlock(&__CFUniCharCompatibilityDecompositionTableLock);
}

CF_INLINE bool __CFUniCharIsDecomposableCharacterWithFlag(UTF32Char character, bool isHFSPlus) {
    return CFUniCharIsMemberOfBitmap(character, (character < 0x10000 ? (isHFSPlus ? __CFUniCharHFSPlusDecomposableBitmapForBMP : __CFUniCharDecomposableBitmapForBMP) : CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, ((character >> 16) & 0xFF))));
}

CF_INLINE bool __CFUniCharIsNonBaseCharacter(UTF32Char character) {
    return CFUniCharIsMemberOfBitmap(character, (character < 0x10000 ? __CFUniCharNonBaseBitmapForBMP : CFUniCharGetBitmapPtrForPlane(kCFUniCharNonBaseCharacterSet, ((character >> 16) & 0xFF))));
}

typedef struct {
    uint32_t _key;
    uint32_t _value;
} __CFUniCharDecomposeMappings;

static uint32_t __CFUniCharGetMappedValue(const __CFUniCharDecomposeMappings *theTable, uint32_t numElem, UTF32Char character) {
    const __CFUniCharDecomposeMappings *p, *q, *divider;

    if ((character < theTable[0]._key) || (character > theTable[numElem-1]._key)) {
        return 0;
    }
    p = theTable;
    q = p + (numElem-1);
    while (p <= q) {
        divider = p + ((q - p) >> 1);	/* divide by 2 */
        if (character < divider->_key) { q = divider - 1; }
        else if (character > divider->_key) { p = divider + 1; }
        else { return divider->_value; }
    }
    return 0;
}

#define __CFUniCharGetCombiningPropertyForCharacter(character) CFUniCharGetCombiningPropertyForCharacter(character, (((character) >> 16) < __CFUniCharCombiningPriorityTableNumPlane ? __CFUniCharCombiningPriorityTable[(character) >> 16] : NULL))

static void __CFUniCharPrioritySort(UTF32Char *characters, uint32_t length) {
    uint32_t p1, p2;
    UTF32Char *ch1, *ch2;
    bool changes = true;
    UTF32Char *end = characters + length;

    if (NULL == __CFUniCharCombiningPriorityTable) {
        __CFSpinLock(&__CFUniCharDecompositionTableLock);
        if (NULL == __CFUniCharCombiningPriorityTable) {
            uint32_t numPlanes = CFUniCharGetNumberOfPlanesForUnicodePropertyData(kCFUniCharCombiningProperty);
            uint32_t idx;

            __CFUniCharCombiningPriorityTable = (const uint8_t **)CFAllocatorAllocate(NULL, sizeof(uint8_t *) * numPlanes, 0);
            for (idx = 0;idx < numPlanes;idx++) __CFUniCharCombiningPriorityTable[idx] = CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, idx);
            __CFUniCharCombiningPriorityTableNumPlane = numPlanes;
        }
        __CFSpinUnlock(&__CFUniCharDecompositionTableLock);
    }

    if (length < 2) return;

    do {
        changes = false;
        ch1 = characters; ch2 = characters + 1;
        p2 = __CFUniCharGetCombiningPropertyForCharacter(*ch1);
        while (ch2 < end) {
            p1 = p2; p2 = __CFUniCharGetCombiningPropertyForCharacter(*ch2);
            if (p1 > p2) {
                UTF32Char tmp = *ch1; *ch1 = *ch2; *ch2 = tmp;
                changes = true;
            }
            ++ch1; ++ch2;
        }
    } while (changes);
}

static uint32_t __CFUniCharRecursivelyDecomposeCharacter(UTF32Char character, UTF32Char *convertedChars, uint32_t maxBufferLength) {
    uint32_t value = __CFUniCharGetMappedValue((const __CFUniCharDecomposeMappings *)__CFUniCharDecompositionTable, __CFUniCharDecompositionTableLength, character);
    uint32_t length = CFUniCharConvertFlagToCount(value);
    UTF32Char firstChar = value & 0xFFFFFF;
    UTF32Char *mappings = (length > 1 ? __CFUniCharMultipleDecompositionTable + firstChar : &firstChar);
    uint32_t usedLength = 0;

    if (maxBufferLength < length) return 0;

    if (value & kCFUniCharRecursiveDecompositionFlag) {
        usedLength = __CFUniCharRecursivelyDecomposeCharacter(*mappings, convertedChars, maxBufferLength - length);

        --length; // Decrement for the first char
        if (!usedLength || usedLength + length > maxBufferLength) return 0;
        ++mappings;
        convertedChars += usedLength;
    }

    usedLength += length;

    while (length--) *(convertedChars++) = *(mappings++);

    return usedLength;
}
    
#define HANGUL_SBASE 0xAC00
#define HANGUL_LBASE 0x1100
#define HANGUL_VBASE 0x1161
#define HANGUL_TBASE 0x11A7
#define HANGUL_SCOUNT 11172
#define HANGUL_LCOUNT 19
#define HANGUL_VCOUNT 21
#define HANGUL_TCOUNT 28
#define HANGUL_NCOUNT (HANGUL_VCOUNT * HANGUL_TCOUNT)

uint32_t CFUniCharDecomposeCharacter(UTF32Char character, UTF32Char *convertedChars, uint32_t maxBufferLength) {
    if (NULL == __CFUniCharDecompositionTable) __CFUniCharLoadDecompositionTable();
    if (character >= HANGUL_SBASE && character <= (HANGUL_SBASE + HANGUL_SCOUNT)) {
        uint32_t length;

        character -= HANGUL_SBASE;

        length = (character % HANGUL_TCOUNT ? 3 : 2);

        if (maxBufferLength < length) return 0;

        *(convertedChars++) = character / HANGUL_NCOUNT + HANGUL_LBASE;
        *(convertedChars++) = (character % HANGUL_NCOUNT) / HANGUL_TCOUNT + HANGUL_VBASE;
        if (length > 2) *convertedChars = (character % HANGUL_TCOUNT) + HANGUL_TBASE;
        return length;
    } else {
        return __CFUniCharRecursivelyDecomposeCharacter(character, convertedChars, maxBufferLength);
    }
}

#define MAX_BUFFER_LENGTH (32)
bool CFUniCharDecompose(const UTF16Char *src, uint32_t length, uint32_t *consumedLength, void *dst, uint32_t maxLength, uint32_t *filledLength, bool needToReorder, uint32_t dstFormat, bool isHFSPlus) {
    uint32_t usedLength = 0;
    uint32_t originalLength = length;
    UTF32Char buffer[MAX_BUFFER_LENGTH];
    UTF32Char *decompBuffer = buffer;
    uint32_t decompBufferLen = MAX_BUFFER_LENGTH;
    UTF32Char currentChar;
    uint32_t idx;
    bool isDecomp = false;
    bool isNonBase = false;

    if (NULL == __CFUniCharDecompositionTable) __CFUniCharLoadDecompositionTable();

    while (length > 0) {
        currentChar = *(src++);
        --length;

        if (currentChar < 0x80) {
            if (maxLength) {
                if (usedLength < maxLength) {
                    switch (dstFormat) {
                        case kCFUniCharUTF8Format: *(((uint8_t *)dst)++) = currentChar; break;
                        case kCFUniCharUTF16Format: *(((UTF16Char *)dst)++) = currentChar; break;
                        case kCFUniCharUTF32Format: *(((UTF32Char *)dst)++) = currentChar; break;
                    }
                } else {
                    if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                    if (consumedLength) *consumedLength = originalLength - length - 1;
                    if (filledLength) *filledLength = usedLength;
                    return false;
                }
            }
            ++usedLength;
            continue;
        }

        if (CFUniCharIsSurrogateHighCharacter(currentChar) && (length > 0) && CFUniCharIsSurrogateLowCharacter(*src)) {
            currentChar = CFUniCharGetLongCharacterForSurrogatePair(currentChar, *(src++));
            --length;
        }

        isDecomp = __CFUniCharIsDecomposableCharacterWithFlag(currentChar, isHFSPlus);
        isNonBase = (needToReorder && __CFUniCharIsNonBaseCharacter(currentChar));

        if (!isDecomp || isNonBase) {
            if (isNonBase) {
                if (isDecomp) {
                    idx = CFUniCharDecomposeCharacter(currentChar, decompBuffer, MAX_BUFFER_LENGTH);
                } else {
                    idx = 1;
                    *decompBuffer = currentChar;
                }
                
                while (length > 0) {
                    if (CFUniCharIsSurrogateHighCharacter(*src) && ((length + 1) > 0) && CFUniCharIsSurrogateLowCharacter(*(src + 1))) {
                        currentChar = CFUniCharGetLongCharacterForSurrogatePair(*src, *(src + 1));
                    } else {
                        currentChar = *src;
                    }
                    if (__CFUniCharIsNonBaseCharacter(currentChar)) {
                        if (currentChar > 0xFFFF) { // Non-BMP
                            length -= 2;
                            src += 2;
                        } else {
                            --length;
                            ++src;
                        }
                        if ((idx + 1) >= decompBufferLen) {
                            UTF32Char *newBuffer;

                            decompBufferLen += MAX_BUFFER_LENGTH;
                            newBuffer = (UTF32Char *)CFAllocatorAllocate(NULL, sizeof(UTF32Char) * decompBufferLen, 0);
                            memmove(newBuffer, decompBuffer, (decompBufferLen - MAX_BUFFER_LENGTH) * sizeof(UTF32Char));
                            if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                            decompBuffer = newBuffer;
                        }

                        if (__CFUniCharIsDecomposableCharacterWithFlag(currentChar, isHFSPlus)) { // Vietnamese accent, etc.
                            idx += CFUniCharDecomposeCharacter(currentChar, decompBuffer + idx, MAX_BUFFER_LENGTH - idx);
                        } else {
                            decompBuffer[idx++] = currentChar;
                        }
                    } else {
                        break;
                    }
                }

                if (idx > 1) { // Need to reorder
                    __CFUniCharPrioritySort(decompBuffer, idx);
                }
                if (!CFUniCharFillDestinationBuffer(decompBuffer, idx, &dst, maxLength, &usedLength, dstFormat)) {
                    if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                    if (consumedLength) *consumedLength = originalLength - length;
                    if (filledLength) *filledLength = usedLength;
                    return false;
                }
            } else {
                if (dstFormat == kCFUniCharUTF32Format) {
                    ++usedLength;
                    if (maxLength) {
                        if (usedLength > maxLength) {
                            if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                            if (consumedLength) *consumedLength = originalLength - length;
                            if (filledLength) *filledLength = usedLength;
                            return false;
                        }
                        *(((UTF32Char *)dst)++) = currentChar;
                    }
                } else {
                    if (!CFUniCharFillDestinationBuffer(&currentChar, 1, &dst, maxLength, &usedLength, dstFormat)) {
                        if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                        if (consumedLength) *consumedLength = originalLength - length;
                        if (filledLength) *filledLength = usedLength;
                        return false;
                    }
                }
            }
        } else {
            if (dstFormat == kCFUniCharUTF32Format && maxLength) {
                idx = CFUniCharDecomposeCharacter(currentChar, dst, maxLength - usedLength);

                if (idx == 0) {
                    if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                    if (consumedLength) *consumedLength = originalLength - length;
                    if (filledLength) *filledLength = usedLength;
                    return false;
                } else if (needToReorder && (idx > 1)) { // Need to reorder
                    bool moreCombiningMarks = false;
                    ++((UTF32Char *)dst); --idx; ++usedLength; // Skip the base

                    while (length > 0) {
                        if (CFUniCharIsSurrogateHighCharacter(*src) && ((length + 1) > 0) && CFUniCharIsSurrogateLowCharacter(*(src + 1))) {
                            currentChar = CFUniCharGetLongCharacterForSurrogatePair(*src, *(src + 1));
                        } else {
                            currentChar = *src;
                        }
                        if (__CFUniCharIsNonBaseCharacter(currentChar)) {
                            if (currentChar > 0xFFFF) { // Non-BMP
                                length -= 2;
                                src += 2;
                            } else {
                                --length;
                                ++src;
                            }
                            if ((idx + usedLength + 1) >= maxLength) {
                                if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                                if (consumedLength) *consumedLength = originalLength - length;
                                if (filledLength) *filledLength = usedLength;
                                return false;
                            }
                            ((UTF32Char *)dst)[idx++] = currentChar;
                            moreCombiningMarks = true;
                        } else {
                            break;
                        }
                    }
                    if (moreCombiningMarks) __CFUniCharPrioritySort(((UTF32Char *)dst), idx);

                }
                usedLength += idx;
                ((UTF32Char *)dst) += idx;
            } else {
                idx = CFUniCharDecomposeCharacter(currentChar, decompBuffer, decompBufferLen);

                if (maxLength && idx + usedLength > maxLength) {
                    if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                    if (consumedLength) *consumedLength = originalLength - length;
                    if (filledLength) *filledLength = usedLength;
                    return false;
                } else if (needToReorder && (idx > 1)) { // Need to reorder
                    bool moreCombiningMarks = false;

                    while (length > 0) {
                        if (CFUniCharIsSurrogateHighCharacter(*src) && ((length + 1) > 0) && CFUniCharIsSurrogateLowCharacter(*(src + 1))) {
                            currentChar = CFUniCharGetLongCharacterForSurrogatePair(*src, *(src + 1));
                        } else {
                            currentChar = *src;
                        }
                        if (__CFUniCharIsNonBaseCharacter(currentChar)) {
                            if (currentChar > 0xFFFF) { // Non-BMP
                                length -= 2;
                                src += 2;
                            } else {
                                --length;
                                ++src;
                            }
                            if ((idx + 1) >= decompBufferLen) {
                                UTF32Char *newBuffer;

                                decompBufferLen += MAX_BUFFER_LENGTH;
                                newBuffer = (UTF32Char *)CFAllocatorAllocate(NULL, sizeof(UTF32Char) * decompBufferLen, 0);
                                memmove(newBuffer, decompBuffer, (decompBufferLen - MAX_BUFFER_LENGTH) * sizeof(UTF32Char));
                                if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                                decompBuffer = newBuffer;
                            }
                            decompBuffer[idx++] = currentChar;
                            moreCombiningMarks = true;
                        } else {
                            break;
                        }
                    }
                    if (moreCombiningMarks) __CFUniCharPrioritySort(decompBuffer + 1, idx - 1);
                }
                if (!CFUniCharFillDestinationBuffer(decompBuffer, idx, &dst, maxLength, &usedLength, dstFormat)) {
                    if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);
                    if (consumedLength) *consumedLength = originalLength - length;
                    if (filledLength) *filledLength = usedLength;
                    return false;
                }
            }
        }
    }
    if (decompBuffer != buffer) CFAllocatorDeallocate(NULL, decompBuffer);

    if (consumedLength) *consumedLength = originalLength - length;
    if (filledLength) *filledLength = usedLength;

    return true;
}

#define MAX_COMP_DECOMP_LEN (32)

static uint32_t __CFUniCharRecursivelyCompatibilityDecomposeCharacter(UTF32Char character, UTF32Char *convertedChars) {
    uint32_t value = __CFUniCharGetMappedValue((const __CFUniCharDecomposeMappings *)__CFUniCharCompatibilityDecompositionTable, __CFUniCharCompatibilityDecompositionTableLength, character);
    uint32_t length = CFUniCharConvertFlagToCount(value);
    UTF32Char firstChar = value & 0xFFFFFF;
    const UTF32Char *mappings = (length > 1 ? __CFUniCharCompatibilityMultipleDecompositionTable + firstChar : &firstChar);
    uint32_t usedLength = length;
    UTF32Char currentChar;
    uint32_t currentLength;

    while (length-- > 0) {
        currentChar = *(mappings++);
        if (__CFUniCharIsDecomposableCharacterWithFlag(currentChar, false)) {
            currentLength = __CFUniCharRecursivelyDecomposeCharacter(currentChar, convertedChars, MAX_COMP_DECOMP_LEN - length);
            convertedChars += currentLength;
            usedLength += (currentLength - 1);
        } else if (CFUniCharIsMemberOf(currentChar, kCFUniCharCompatibilityDecomposableCharacterSet)) {
            currentLength = __CFUniCharRecursivelyCompatibilityDecomposeCharacter(currentChar, convertedChars);
            convertedChars += currentLength;
            usedLength += (currentLength - 1);
        } else {
            *(convertedChars++) = currentChar;
        }
    }

    return usedLength;
}

CF_INLINE void __CFUniCharMoveBufferFromEnd(UTF32Char *convertedChars, uint32_t length, uint32_t delta) {
    const UTF32Char *limit = convertedChars;
    UTF32Char *dstP;

    convertedChars += length;
    dstP = convertedChars + delta;

    while (convertedChars > limit) *(--dstP) = *(--convertedChars);
}

__private_extern__ uint32_t CFUniCharCompatibilityDecompose(UTF32Char *convertedChars, uint32_t length, uint32_t maxBufferLength) {
    UTF32Char currentChar;
    UTF32Char buffer[MAX_COMP_DECOMP_LEN];
    const UTF32Char *bufferP;
    const UTF32Char *limit = convertedChars + length;
    uint32_t filledLength;

    if (NULL == __CFUniCharCompatibilityDecompositionTable) __CFUniCharLoadCompatibilityDecompositionTable();

    while (convertedChars < limit) {
        currentChar = *convertedChars;

        if (CFUniCharIsMemberOf(currentChar, kCFUniCharCompatibilityDecomposableCharacterSet)) {
            filledLength = __CFUniCharRecursivelyCompatibilityDecomposeCharacter(currentChar, buffer);

            if (filledLength + length - 1 > maxBufferLength) return 0;

            if (filledLength > 1) __CFUniCharMoveBufferFromEnd(convertedChars + 1, limit - convertedChars - 1, filledLength - 1);

            bufferP = buffer;
            length += (filledLength - 1);
            while (filledLength-- > 0) *(convertedChars++) = *(bufferP++);
        } else {
            ++convertedChars;
        }
    }
    
    return length;
}

CF_EXPORT void CFUniCharPrioritySort(UTF32Char *characters, uint32_t length) {
    __CFUniCharPrioritySort(characters, length);
}
