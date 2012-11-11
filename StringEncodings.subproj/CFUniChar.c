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
/*	CFUniChar.c
	Copyright 2001-2002, Apple, Inc. All rights reserved.
	Responsibility: Aki Inoue
*/

#include <CoreFoundation/CFByteOrder.h>
#include "CFInternal.h"
#include "CFUniChar.h" 
#include "CFStringEncodingConverterExt.h"
#include "CFUnicodeDecomposition.h"
#include "CFUniCharPriv.h"
#if defined(__MACOS8__)
#include <stdio.h>
#elif defined(__WIN32__)
#include <windows.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#elif defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
#if defined(__MACH__)
#include <mach/mach.h>
#endif
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#endif

#if defined(__MACOS8__)
#define	MAXPATHLEN FILENAME_MAX
#elif defined WIN32
#define MAXPATHLEN MAX_PATH
#endif

// Memory map the file
#if !defined(__MACOS8__)

CF_INLINE void __CFUniCharCharacterSetPath(char *cpath) {
    strcpy(cpath, __kCFCharacterSetDir);
    strcat(cpath, "/CharacterSets/");
}

static bool __CFUniCharLoadBytesFromFile(const char *fileName, const void **bytes) {
#if defined(__WIN32__)
    HANDLE bitmapFileHandle;
    HANDLE mappingHandle;

    if ((bitmapFileHandle = CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE) return false;
    mappingHandle = CreateFileMapping(bitmapFileHandle, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(bitmapFileHandle);
    if (!mappingHandle) return false;

    *bytes = MapViewOfFileEx(mappingHandle, FILE_MAP_READ, 0, 0, 0, NULL);
    CloseHandle(mappingHandle);

    return (*bytes ? true : false);
#else
    struct stat statBuf;
    int fd = -1;

    if ((fd = open(fileName, O_RDONLY, 0)) < 0) return false;

#if defined(__MACH__)
    if (fstat(fd, &statBuf) < 0 || map_fd(fd, 0, (vm_offset_t *)bytes, true, (vm_size_t)statBuf.st_size)) {
        close(fd);
        return false;
    }
#else
    if (fstat(fd, &statBuf) < 0 || (*bytes = mmap(0, statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == (void *)-1) {
        close(fd);

        return false;
    }
#endif
    close(fd);

    return true;
#endif
}

static bool __CFUniCharLoadFile(const char *bitmapName, const void **bytes) {
    char cpath[MAXPATHLEN];

    __CFUniCharCharacterSetPath(cpath);
    strcat(cpath, bitmapName);

    return __CFUniCharLoadBytesFromFile(cpath, bytes);
}
#endif !defined(__MACOS8__)

// Bitmap functions
CF_INLINE bool isControl(UTF32Char theChar, uint16_t charset, const void *data) { // ISO Control
    if ((theChar <= 0x001F) || (theChar >= 0x007F && theChar <= 0x009F)) return true;
    return false;
}

CF_INLINE bool isWhitespace(UTF32Char theChar, uint16_t charset, const void *data) { // Space
    if ((theChar == 0x0020) || (theChar == 0x0009) || (theChar == 0x00A0) || (theChar == 0x1680) || (theChar >= 0x2000 && theChar <= 0x200B) || (theChar == 0x202F) || (theChar == 0x205F) || (theChar == 0x3000)) return true;
    return false;
}

CF_INLINE bool isWhitespaceAndNewLine(UTF32Char theChar, uint16_t charset, const void *data) { // White space
    if (isWhitespace(theChar, charset, data) || (theChar >= 0x000A && theChar <= 0x000D) || (theChar == 0x0085) || (theChar == 0x2028) || (theChar == 0x2029)) return true;
    return false;
}

#if defined(__MACOS8__)
/* This structure MUST match the sets in NSRulebook.h  The "__CFCSetIsMemberSet()" function is a modified version of the one in Text shlib.
*/
typedef struct _CFCharSetPrivateStruct {
    int issorted;	/* 1=sorted or 0=unsorted ; 2=is_property_table */
    int bitrange[4];	/* bitmap (each bit is a 1k range in space of 2^17) */
    int nsingles;	/* number of single elements */
    int nranges;	/* number of ranges */
    int singmin;	/* minimum single element */
    int singmax;	/* maximum single element */
    int array[1];	/* actually bunch of singles followed by ranges */
} CFCharSetPrivateStruct;

/* Membership function for complex sets
*/
CF_INLINE bool __CFCSetIsMemberSet(const CFCharSetPrivateStruct *set, UTF16Char theChar) {
    int *tmp, *tmp2;
    int i, nel;
    int *p, *q, *wari;

    if (set->issorted != 1) {
        return false;
    }
    theChar &= 0x0001FFFF;	/* range 1-131k */
    if (__CFCSetBitsInRange(theChar, set->bitrange)) {
        if (theChar >= set->singmin && theChar <= set->singmax) {
            tmp = (int *) &(set->array[0]);
            if ((nel = set->nsingles) < __kCFSetBreakeven) {
                for (i = 0; i < nel; i++) {
                    if (*tmp == theChar) return true;
                    ++tmp;
                }
            }
            else {	// this does a binary search
                p = tmp; q = tmp + (nel-1);
                while (p <= q) {
                    wari = (p + ((q-p)>>1));
                    if (theChar < *wari) q = wari - 1;
                    else if (theChar > *wari) p = wari + 1;
                    else return true;
                }
            }
        }
        tmp = (int *) &(set->array[0]) + set->nsingles;
        if ((nel = set->nranges) < __kCFSetBreakeven) {
            i = nel;
            tmp2 = tmp+1;
            while (i) {
                if (theChar <= *tmp2) {
                    if (theChar >= *tmp) return true;
                }
                tmp += 2;
                tmp2 = tmp+1;
                --i;
            }
        } else {	/* binary search the ranges */
            p = tmp; q = tmp + (2*nel-2);
            while (p <= q) {
                i = (q - p) >> 1;	/* >>1 means divide by 2 */
                wari = p + (i & 0xFFFFFFFE); /* &fffffffe make it an even num */
                if (theChar < *wari) q = wari - 2;
                else if (theChar > *(wari + 1)) p = wari + 2;
                else return true;
            }
        }
        return false;
        /* fall through & return zero */
    }
    return false;	/* not a member */
}

/* Take a private "set" structure and make a bitmap from it.  Return the bitmap.  THE CALLER MUST RELEASE THE RETURNED MEMORY as necessary.
*/

CF_INLINE void __CFCSetBitmapProcessManyCharacters(unsigned char *map, unsigned n, unsigned m) {
    unsigned tmp;
    for (tmp = n; tmp <= m; tmp++) CFUniCharAddCharacterToBitmap(tmp, map);
}

CF_INLINE void __CFCSetMakeSetBitmapFromSet(const CFCharSetPrivateStruct *theSet, uint8_t *map)
{
    int *ip;
    UTF16Char ctmp;
    int cnt;

    for (cnt = 0; cnt < theSet->nsingles; cnt++) {
        ctmp = theSet->array[cnt];
        CFUniCharAddCharacterToBitmap(tmp, map);
    }
    ip = (int *) (&(theSet->array[0]) + theSet->nsingles);
    cnt = theSet->nranges;
    while (cnt) {
        /* This could be more efficient: turn on whole bytes at a time
           when there are such cases as 8 characters in a row... */
        __CFCSetBitmapProcessManyCharacters((unsigned char *)map, *ip, *(ip+1));
        ip += 2;
        --cnt;
    }
}

extern const CFCharSetPrivateStruct *_CFdecimalDigitCharacterSetData;
extern const CFCharSetPrivateStruct *_CFletterCharacterSetData;
extern const CFCharSetPrivateStruct *_CFlowercaseLetterCharacterSetData;
extern const CFCharSetPrivateStruct *_CFuppercaseLetterCharacterSetData;
extern const CFCharSetPrivateStruct *_CFnonBaseCharacterSetData;
extern const CFCharSetPrivateStruct *_CFdecomposableCharacterSetData;
extern const CFCharSetPrivateStruct *_CFpunctuationCharacterSetData;
extern const CFCharSetPrivateStruct *_CFalphanumericCharacterSetData;
extern const CFCharSetPrivateStruct *_CFillegalCharacterSetData;
extern const CFCharSetPrivateStruct *_CFhasNonSelfLowercaseMappingData;
extern const CFCharSetPrivateStruct *_CFhasNonSelfUppercaseMappingData;
extern const CFCharSetPrivateStruct *_CFhasNonSelfTitlecaseMappingData;

#else __MACOS8__
typedef struct {
    uint32_t _numPlanes;
    const uint8_t **_planes;
} __CFUniCharBitmapData;

static char __CFUniCharUnicodeVersionString[8] = {0, 0, 0, 0, 0, 0, 0, 0};

static uint32_t __CFUniCharNumberOfBitmaps = 0;
static __CFUniCharBitmapData *__CFUniCharBitmapDataArray = NULL;

static CFSpinLock_t __CFUniCharBitmapLock = 0;

#ifndef CF_UNICHAR_BITMAP_FILE
#define CF_UNICHAR_BITMAP_FILE "CFCharacterSetBitmaps.bitmap"
#endif CF_UNICHAR_BITMAP_FILE

static bool __CFUniCharLoadBitmapData(void) {
    uint32_t headerSize;
    uint32_t bitmapSize;
    int numPlanes;
    uint8_t currentPlane;
    const void *bytes;
    const void *bitmapBase;
    const void *bitmap;
    int idx, bitmapIndex;

    __CFSpinLock(&__CFUniCharBitmapLock);

    if (__CFUniCharBitmapDataArray || !__CFUniCharLoadFile(CF_UNICHAR_BITMAP_FILE, &bytes)) {
        __CFSpinUnlock(&__CFUniCharBitmapLock);
        return false;
    }

    for (idx = 0;idx < 4 && ((const uint8_t *)bytes)[idx];idx++) {
        __CFUniCharUnicodeVersionString[idx * 2] = ((const uint8_t *)bytes)[idx];
        __CFUniCharUnicodeVersionString[idx * 2 + 1] = '.';
    }
    __CFUniCharUnicodeVersionString[(idx < 4 ? idx * 2 - 1 : 7)] = '\0';

    headerSize = CFSwapInt32BigToHost(*((uint32_t *)((char *)bytes + 4)));

    bitmapBase = (char *)bytes + headerSize;
    (char *)bytes += (sizeof(uint32_t) * 2);
    headerSize -= (sizeof(uint32_t) * 2);

    __CFUniCharNumberOfBitmaps = headerSize / (sizeof(uint32_t) * 2);

    __CFUniCharBitmapDataArray = (__CFUniCharBitmapData *)CFAllocatorAllocate(NULL, sizeof(__CFUniCharBitmapData) * __CFUniCharNumberOfBitmaps, 0);

    for (idx = 0;idx < (int)__CFUniCharNumberOfBitmaps;idx++) {
        bitmap = (char *)bitmapBase + CFSwapInt32BigToHost(*(((uint32_t *)bytes)++));
        bitmapSize = CFSwapInt32BigToHost(*(((uint32_t *)bytes)++));

        numPlanes = bitmapSize / (8 * 1024);
        numPlanes = *(const uint8_t *)((char *)bitmap + (((numPlanes - 1) * ((8 * 1024) + 1)) - 1)) + 1;
        __CFUniCharBitmapDataArray[idx]._planes = (const uint8_t **)CFAllocatorAllocate(NULL, sizeof(const void *) * numPlanes, NULL);
        __CFUniCharBitmapDataArray[idx]._numPlanes = numPlanes;

        currentPlane = 0;
        for (bitmapIndex = 0;bitmapIndex < numPlanes;bitmapIndex++) {
            if (bitmapIndex == currentPlane) {
                __CFUniCharBitmapDataArray[idx]._planes[bitmapIndex] = bitmap;
                (char *)bitmap += (8 * 1024);
                currentPlane = *(((const uint8_t *)bitmap)++);
            } else {
                __CFUniCharBitmapDataArray[idx]._planes[bitmapIndex] = NULL;
            }
        }
    }

    __CFSpinUnlock(&__CFUniCharBitmapLock);

    return true;
}

__private_extern__ const char *__CFUniCharGetUnicodeVersionString(void) {
    if (NULL == __CFUniCharBitmapDataArray) __CFUniCharLoadBitmapData();
    return __CFUniCharUnicodeVersionString;
}

#endif __MACOS8__

#define CONTROLSET_HAS_FORMATTER 1

bool CFUniCharIsMemberOf(UTF32Char theChar, uint32_t charset) {
#if CONTROLSET_HAS_FORMATTER
    if (charset == kCFUniCharControlCharacterSet) charset = kCFUniCharControlAndFormatterCharacterSet;
#endif CONTROLSET_HAS_FORMATTER

    switch (charset) {
        case kCFUniCharControlCharacterSet:
            return isControl(theChar, charset, NULL);

        case kCFUniCharWhitespaceCharacterSet:
            return isWhitespace(theChar, charset, NULL);

        case kCFUniCharWhitespaceAndNewlineCharacterSet:
            return isWhitespaceAndNewLine(theChar, charset, NULL);

#if defined(__MACOS8__)
        case kCFUniCharDecimalDigitCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFdecimalDigitCharacterSetData, theChar);
        case kCFUniCharLetterCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFletterCharacterSetData, theChar);
        case kCFUniCharLowercaseLetterCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFlowercaseLetterCharacterSetData, theChar);
        case kCFUniCharUppercaseLetterCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFuppercaseLetterCharacterSetData, theChar);
        case kCFUniCharNonBaseCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFnonBaseCharacterSetData, theChar);
        case kCFUniCharAlphaNumericCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFalphanumericCharacterSetData, theChar);
        case kCFUniCharDecomposableCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFdecomposableCharacterSetData, theChar);
        case kCFUniCharPunctuationCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFpunctuationCharacterSetData, theChar);
        case kCFUniCharIllegalCharacterSet:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFillegalCharacterSetData, theChar);
        case kCFUniCharHasNonSelfLowercaseMapping:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFhasNonSelfLowercaseMappingData, theChar);
        case kCFUniCharHasNonSelfUppercaseMapping:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFhasNonSelfUppercaseMappingData, theChar);
        case kCFUniCharHasNonSelfTitlecaseMapping:
return __CFCSetIsMemberSet((const CFCharSetPrivateStruct *)&_CFhasNonSelfTitlecaseMappingData, theChar);
        default:
            return false;
#else
        default:
            if (NULL == __CFUniCharBitmapDataArray) __CFUniCharLoadBitmapData();

            if ((charset - kCFUniCharDecimalDigitCharacterSet) < __CFUniCharNumberOfBitmaps) {
                __CFUniCharBitmapData *data = __CFUniCharBitmapDataArray + (charset - kCFUniCharDecimalDigitCharacterSet);
                uint8_t planeNo = (theChar >> 16) & 0xFF;

                // The bitmap data for kCFUniCharIllegalCharacterSet is actually LEGAL set less Plane 14 ~ 16
                if (charset == kCFUniCharIllegalCharacterSet) {
                    if (planeNo == 0x0E) { // Plane 14
                        theChar &= 0xFF;
                        return (((theChar == 0x01) || ((theChar > 0x1F) && (theChar < 0x80))) ? false : true);
                    } else if (planeNo == 0x0F || planeNo == 0x10) { // Plane 15 & 16
                        return ((theChar & 0xFF) > 0xFFFD ? true : false);
                    } else {
                        return (planeNo < data->_numPlanes && data->_planes[planeNo] ? !CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) : true);
                    }
                } else if (charset == kCFUniCharControlAndFormatterCharacterSet) {
                    if (planeNo == 0x0E) { // Plane 14
                        theChar &= 0xFF;
                        return (((theChar == 0x01) || ((theChar > 0x1F) && (theChar < 0x80))) ? true : false);
                    } else {
                        return (planeNo < data->_numPlanes && data->_planes[planeNo] ? CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) : false);
                    }
                } else {
                    return (planeNo < data->_numPlanes && data->_planes[planeNo] ? CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) : false);
                }
            }
            return false;
#endif
    }
}

const uint8_t *CFUniCharGetBitmapPtrForPlane(uint32_t charset, uint32_t plane) {
    if (NULL == __CFUniCharBitmapDataArray) __CFUniCharLoadBitmapData();

#if CONTROLSET_HAS_FORMATTER
    if (charset == kCFUniCharControlCharacterSet) charset = kCFUniCharControlAndFormatterCharacterSet;
#endif CONTROLSET_HAS_FORMATTER

    if (charset > kCFUniCharWhitespaceAndNewlineCharacterSet && (charset - kCFUniCharDecimalDigitCharacterSet) < __CFUniCharNumberOfBitmaps && charset != kCFUniCharIllegalCharacterSet) {
        __CFUniCharBitmapData *data = __CFUniCharBitmapDataArray + (charset - kCFUniCharDecimalDigitCharacterSet);

        return (plane < data->_numPlanes ? data->_planes[plane] : NULL);
    }
    return NULL;
}

__private_extern__ uint8_t CFUniCharGetBitmapForPlane(uint32_t charset, uint32_t plane, void *bitmap, bool isInverted) {
    const uint8_t *src = CFUniCharGetBitmapPtrForPlane(charset, plane);
    int numBytes = (8 * 1024);

    if (src) {
        if (isInverted) {
            while (numBytes-- > 0) *(((uint8_t *)bitmap)++) = ~(*(src++));
        } else {
            while (numBytes-- > 0) *(((uint8_t *)bitmap)++) = *(src++);
        }
        return kCFUniCharBitmapFilled;
    } else if (charset == kCFUniCharIllegalCharacterSet) {
        __CFUniCharBitmapData *data = __CFUniCharBitmapDataArray + (charset - kCFUniCharDecimalDigitCharacterSet);

        if (plane < data->_numPlanes && (src = data->_planes[plane])) {
            if (isInverted) {
                while (numBytes-- > 0) *(((uint8_t *)bitmap)++) = *(src++);
            } else {
                while (numBytes-- > 0) *(((uint8_t *)bitmap)++) = ~(*(src++));
            }
            return kCFUniCharBitmapFilled;
        } else if (plane == 0x0E) { // Plane 14
            int idx;
            uint8_t asciiRange = (isInverted ? (uint8_t)0xFF : (uint8_t)0);
            uint8_t otherRange = (isInverted ? (uint8_t)0 : (uint8_t)0xFF);

            *(((uint8_t *)bitmap)++) = 0x02; // UE0001 LANGUAGE TAG
            for (idx = 1;idx < numBytes;idx++) {
                *(((uint8_t *)bitmap)++) = ((idx >= (0x20 / 8) && (idx < (0x80 / 8))) ? asciiRange : otherRange);
            }
            return kCFUniCharBitmapFilled;
        } else if (plane == 0x0F || plane == 0x10) { // Plane 15 & 16
            uint32_t value = (isInverted ? 0xFFFFFFFF : 0);
            numBytes /= 4; // for 32bit

            while (numBytes-- > 0) *(((uint32_t *)bitmap)++) = value;
            *(((uint8_t *)bitmap) - 5) = (isInverted ? 0x3F : 0xC0); // 0xFFFE & 0xFFFF
            return kCFUniCharBitmapFilled;
        }
        return (isInverted ? kCFUniCharBitmapEmpty : kCFUniCharBitmapAll);
#if CONTROLSET_HAS_FORMATTER
    } else if ((charset == kCFUniCharControlCharacterSet) && (plane == 0x0E)) { // Language tags
            int idx;
            uint8_t asciiRange = (isInverted ? (uint8_t)0 : (uint8_t)0xFF);
            uint8_t otherRange = (isInverted ? (uint8_t)0xFF : (uint8_t)0);

            *(((uint8_t *)bitmap)++) = 0x02; // UE0001 LANGUAGE TAG
            for (idx = 1;idx < numBytes;idx++) {
                *(((uint8_t *)bitmap)++) = ((idx >= (0x20 / 8) && (idx < (0x80 / 8))) ? asciiRange : otherRange);
            }
            return kCFUniCharBitmapFilled;
#endif CONTROLSET_HAS_FORMATTER
    } else if (charset < kCFUniCharDecimalDigitCharacterSet) {
        if (plane) return (isInverted ? kCFUniCharBitmapAll : kCFUniCharBitmapEmpty);

        if (charset == kCFUniCharControlCharacterSet) {
            int idx;
            uint8_t nonFillValue = (isInverted ? (uint8_t)0xFF : (uint8_t)0);
            uint8_t fillValue = (isInverted ? (uint8_t)0 : (uint8_t)0xFF);
            uint8_t *bitmapP = (uint8_t *)bitmap;

            for (idx = 0;idx < numBytes;idx++) {
                *(bitmapP++) = (idx < (0x20 / 8) || (idx >= (0x80 / 8) && idx < (0xA0 / 8)) ? fillValue : nonFillValue);
            }

            // DEL
            if (isInverted) {
                CFUniCharRemoveCharacterFromBitmap(0x007F, bitmap);
            } else {
                CFUniCharAddCharacterToBitmap(0x007F, bitmap);
            }
        } else {
            uint8_t *bitmapBase = (uint8_t *)bitmap;
            int idx;
            uint8_t nonFillValue = (isInverted ? (uint8_t)0xFF : (uint8_t)0);

            while (numBytes-- > 0) *(((uint8_t *)bitmap)++) = nonFillValue;

            if (charset == kCFUniCharWhitespaceAndNewlineCharacterSet) {
                static const UniChar newlines[] = {0x000A, 0x000B, 0x000C, 0x000D, 0x0085, 0x2028, 0x2029};

                for (idx = 0;idx < (int)(sizeof(newlines) / sizeof(*newlines)); idx++) {
                    if (isInverted) {
                        CFUniCharRemoveCharacterFromBitmap(newlines[idx], bitmapBase);
                    } else {
                        CFUniCharAddCharacterToBitmap(newlines[idx], bitmapBase);
                    }
                }
            }

            if (isInverted) {
                CFUniCharRemoveCharacterFromBitmap(0x0009, bitmapBase);
                CFUniCharRemoveCharacterFromBitmap(0x0020, bitmapBase);
                CFUniCharRemoveCharacterFromBitmap(0x00A0, bitmapBase);
                CFUniCharRemoveCharacterFromBitmap(0x1680, bitmapBase);
                CFUniCharRemoveCharacterFromBitmap(0x202F, bitmapBase);
                CFUniCharRemoveCharacterFromBitmap(0x205F, bitmapBase);
                CFUniCharRemoveCharacterFromBitmap(0x3000, bitmapBase);
            } else {
                CFUniCharAddCharacterToBitmap(0x0009, bitmapBase);
                CFUniCharAddCharacterToBitmap(0x0020, bitmapBase);
                CFUniCharAddCharacterToBitmap(0x00A0, bitmapBase);
                CFUniCharAddCharacterToBitmap(0x1680, bitmapBase);
                CFUniCharAddCharacterToBitmap(0x202F, bitmapBase);
                CFUniCharAddCharacterToBitmap(0x205F, bitmapBase);
                CFUniCharAddCharacterToBitmap(0x3000, bitmapBase);
            }

            for (idx = 0x2000;idx <= 0x200B;idx++) {
                if (isInverted) {
                    CFUniCharRemoveCharacterFromBitmap(idx, bitmapBase);
                } else {
                    CFUniCharAddCharacterToBitmap(idx, bitmapBase);
                }
            }
        }
        return kCFUniCharBitmapFilled;
    }
    return (isInverted ? kCFUniCharBitmapAll : kCFUniCharBitmapEmpty);
}

__private_extern__ uint32_t CFUniCharGetNumberOfPlanes(uint32_t charset) {
#if defined(__MACOS8__)
    return 1;
#else __MACOS8__
#if CONTROLSET_HAS_FORMATTER
    if (charset == kCFUniCharControlCharacterSet) return 15; // 0 to 14
#endif CONTROLSET_HAS_FORMATTER

    if (charset < kCFUniCharDecimalDigitCharacterSet) {
        return 1;
    } else if (charset == kCFUniCharIllegalCharacterSet) {
        return 17;
    } else {
        uint32_t numPlanes;

        if (NULL == __CFUniCharBitmapDataArray) __CFUniCharLoadBitmapData();

        numPlanes = __CFUniCharBitmapDataArray[charset - kCFUniCharDecimalDigitCharacterSet]._numPlanes;

        return numPlanes;
    }
#endif __MACOS8__
}

// Mapping data loading
static const void **__CFUniCharMappingTables = NULL;

static CFSpinLock_t __CFUniCharMappingTableLock = 0;

#if defined(__BIG_ENDIAN__)
#define MAPPING_TABLE_FILE "CFUnicodeData-B.mapping"
#else __BIG_ENDIAN__
#define MAPPING_TABLE_FILE "CFUnicodeData-L.mapping"
#endif __BIG_ENDIAN__

__private_extern__ const void *CFUniCharGetMappingData(uint32_t type) {

    __CFSpinLock(&__CFUniCharMappingTableLock);

    if (NULL == __CFUniCharMappingTables) {
        const void *bytes;
        const void *bodyBase;
        int headerSize;
        int idx, count;

        if (!__CFUniCharLoadFile(MAPPING_TABLE_FILE, &bytes)) {
            __CFSpinUnlock(&__CFUniCharMappingTableLock);
            return NULL;
        }

        (char *)bytes += 4; // Skip Unicode version
        headerSize = *(((uint32_t *)bytes)++);
        headerSize -= (sizeof(uint32_t) * 2);
        bodyBase = (char *)bytes + headerSize;

        count = headerSize / sizeof(uint32_t);

        __CFUniCharMappingTables = (const void **)CFAllocatorAllocate(NULL, sizeof(const void *) * count, 0);

        for (idx = 0;idx < count;idx++) {
            __CFUniCharMappingTables[idx] = (char *)bodyBase + *(((uint32_t *)bytes)++);
        }
    }

    __CFSpinUnlock(&__CFUniCharMappingTableLock);

    return __CFUniCharMappingTables[type];
}

// Case mapping functions
#define DO_SPECIAL_CASE_MAPPING 1

static uint32_t *__CFUniCharCaseMappingTableCounts = NULL;
static uint32_t **__CFUniCharCaseMappingTable = NULL;
static const uint32_t **__CFUniCharCaseMappingExtraTable = NULL;

typedef struct {
    uint32_t _key;
    uint32_t _value;
} __CFUniCharCaseMappings;

/* Binary searches CFStringEncodingUnicodeTo8BitCharMap */
static uint32_t __CFUniCharGetMappedCase(const __CFUniCharCaseMappings *theTable, uint32_t numElem, UTF32Char character) {
    const __CFUniCharCaseMappings *p, *q, *divider;

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

#define NUM_CASE_MAP_DATA (kCFUniCharCaseFold + 1)

static bool __CFUniCharLoadCaseMappingTable(void) {
    int idx;

    if (NULL == __CFUniCharMappingTables) (void)CFUniCharGetMappingData(kCFUniCharToLowercase);
    if (NULL == __CFUniCharMappingTables) return false;

    __CFSpinLock(&__CFUniCharMappingTableLock);

    if (__CFUniCharCaseMappingTableCounts) {
        __CFSpinUnlock(&__CFUniCharMappingTableLock);
        return true;
    }

    __CFUniCharCaseMappingTableCounts = (uint32_t *)CFAllocatorAllocate(NULL, sizeof(uint32_t) * NUM_CASE_MAP_DATA + sizeof(uint32_t *) * NUM_CASE_MAP_DATA * 2, 0);
    __CFUniCharCaseMappingTable = (uint32_t **)((char *)__CFUniCharCaseMappingTableCounts + sizeof(uint32_t) * NUM_CASE_MAP_DATA);
    __CFUniCharCaseMappingExtraTable = (const uint32_t **)__CFUniCharCaseMappingTable + NUM_CASE_MAP_DATA;

    for (idx = 0;idx < NUM_CASE_MAP_DATA;idx++) {
        __CFUniCharCaseMappingTableCounts[idx] = *((uint32_t *)__CFUniCharMappingTables[idx]) / (sizeof(uint32_t) * 2);
        __CFUniCharCaseMappingTable[idx] = ((uint32_t *)__CFUniCharMappingTables[idx]) + 1;
        __CFUniCharCaseMappingExtraTable[idx] = (const uint32_t *)((char *)__CFUniCharCaseMappingTable[idx] + *((uint32_t *)__CFUniCharMappingTables[idx]));
    }

    __CFSpinUnlock(&__CFUniCharMappingTableLock);
    return true;
}

#if __BIG_ENDIAN__
#define TURKISH_LANG_CODE	(0x7472) // tr
#define LITHUANIAN_LANG_CODE	(0x6C74) // lt
#define AZERI_LANG_CODE		(0x617A) // az
#else __BIG_ENDIAN__
#define TURKISH_LANG_CODE	(0x7274) // tr
#define LITHUANIAN_LANG_CODE	(0x746C) // lt
#define AZERI_LANG_CODE		(0x7A61) // az
#endif __BIG_ENDIAN__

uint32_t CFUniCharMapCaseTo(UTF32Char theChar, UTF16Char *convertedChar, uint32_t maxLength, uint32_t ctype, uint32_t flags, const uint8_t *langCode) {
    __CFUniCharBitmapData *data;
    uint8_t planeNo = (theChar >> 16) & 0xFF;

caseFoldRetry:

#if DO_SPECIAL_CASE_MAPPING
    if (flags & kCFUniCharCaseMapFinalSigma) {
        if (theChar == 0x03A3) { // Final sigma
            *convertedChar = (ctype == kCFUniCharToLowercase ? 0x03C2 : 0x03A3);
            return 1;
        }
    }

    if (langCode) {
        switch (*(uint16_t *)langCode) {
            case LITHUANIAN_LANG_CODE:
                if (theChar == 0x0307 && (flags & kCFUniCharCaseMapAfter_i)) {
                    return 0;
                } else if (ctype == kCFUniCharToLowercase) {
                    if (flags & kCFUniCharCaseMapMoreAbove) {
                        switch (theChar) {
                            case 0x0049: // LATIN CAPITAL LETTER I
                                *(convertedChar++) = 0x0069;
                                *(convertedChar++) = 0x0307;
                                return 2;

                            case 0x004A: // LATIN CAPITAL LETTER J
                                *(convertedChar++) = 0x006A;
                                *(convertedChar++) = 0x0307;
                                return 2;

                            case 0x012E: // LATIN CAPITAL LETTER I WITH OGONEK
                                *(convertedChar++) = 0x012F;
                                *(convertedChar++) = 0x0307;
                                return 2;

                            default: break;
                        }
                    }
                    switch (theChar) {
                        case 0x00CC: // LATIN CAPITAL LETTER I WITH GRAVE
                            *(convertedChar++) = 0x0069;
                            *(convertedChar++) = 0x0307;
                            *(convertedChar++) = 0x0300;
                            return 3;

                        case 0x00CD: // LATIN CAPITAL LETTER I WITH ACUTE
                            *(convertedChar++) = 0x0069;
                            *(convertedChar++) = 0x0307;
                            *(convertedChar++) = 0x0301;
                            return 3;

                        case 0x0128: // LATIN CAPITAL LETTER I WITH TILDE
                            *(convertedChar++) = 0x0069;
                            *(convertedChar++) = 0x0307;
                            *(convertedChar++) = 0x0303;
                            return 3;

                        default: break;
                    }
                }
            break;

            case TURKISH_LANG_CODE:
            case AZERI_LANG_CODE:
                if (theChar == 0x0049) { // LATIN CAPITAL LETTER I
                    *convertedChar = (ctype == kCFUniCharToLowercase  ? ((kCFUniCharCaseMapMoreAbove & flags) ? 0x0069 : 0x0131) : 0x0049);
                    return 1;
                } else if ((theChar == 0x0069) || (theChar == 0x0130)) { // LATIN SMALL LETTER I & LATIN CAPITAL LETTER I WITH DOT ABOVE
                    *convertedChar = (ctype == kCFUniCharToLowercase ? 0x0069 : 0x0130);
                    return 1;
                } else if (theChar == 0x0307 && (kCFUniCharCaseMapAfter_i & flags)) { // COMBINING DOT ABOVE AFTER_i
                    if (ctype == kCFUniCharToLowercase) {
                        return 0;
                    } else {
                        *convertedChar = 0x0307;
                        return 1;
                    }
                }
                break;

            default: break;
        }
    }
#endif DO_SPECIAL_CASE_MAPPING

    if (NULL == __CFUniCharBitmapDataArray) __CFUniCharLoadBitmapData();

    data = __CFUniCharBitmapDataArray + ((ctype + kCFUniCharHasNonSelfLowercaseCharacterSet) - kCFUniCharDecimalDigitCharacterSet);

    if (planeNo < data->_numPlanes && data->_planes[planeNo] && CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) && (__CFUniCharCaseMappingTableCounts || __CFUniCharLoadCaseMappingTable())) {
        uint32_t value = __CFUniCharGetMappedCase((const __CFUniCharCaseMappings *)__CFUniCharCaseMappingTable[ctype], __CFUniCharCaseMappingTableCounts[ctype], theChar);

        if (!value && ctype == kCFUniCharToTitlecase) {
            value = __CFUniCharGetMappedCase((const __CFUniCharCaseMappings *)__CFUniCharCaseMappingTable[kCFUniCharToUppercase], __CFUniCharCaseMappingTableCounts[kCFUniCharToUppercase], theChar);
            if (value) ctype = kCFUniCharToUppercase;
        }

        if (value) {
            int count = CFUniCharConvertFlagToCount(value);

            if (count == 1) {
                if (value & kCFUniCharNonBmpFlag) {
                    if (maxLength > 1) {
                        value = (value & 0xFFFFFF) - 0x10000;
                        *(convertedChar++) = (value >> 10) + 0xD800UL;
                        *(convertedChar++) = (value & 0x3FF) + 0xDC00UL;
                        return 2;
                    }
                } else {
                    *convertedChar = (UTF16Char)value;
                    return 1;
                }
            } else if (count < (int)maxLength) {
                const uint32_t *extraMapping = __CFUniCharCaseMappingExtraTable[ctype] + (value & 0xFFFFFF);

                if (value & kCFUniCharNonBmpFlag) {
                    int copiedLen = 0;

                    while (count-- > 0) {
                        value = *(extraMapping++);
                        if (value > 0xFFFF) {
                            if (copiedLen + 2 >= (int)maxLength) break;
                            value = (value & 0xFFFFFF) - 0x10000;
                            convertedChar[copiedLen++] = (value >> 10) + 0xD800UL;
                            convertedChar[copiedLen++] = (value & 0x3FF) + 0xDC00UL;
                        } else {
                            if (copiedLen + 1 >= (int)maxLength) break;
                            convertedChar[copiedLen++] = value;
                        }
                    }
                    if (!count) return copiedLen;
                } else {
                    int idx;

                    for (idx = 0;idx < count;idx++) *(convertedChar++) = (UTF16Char)*(extraMapping++);
                    return count;
                }
            }
        }
    } else if (ctype == kCFUniCharCaseFold) {
        ctype = kCFUniCharToLowercase;
        goto caseFoldRetry;
    }

    *convertedChar = theChar;
    return 1;
}

UInt32 CFUniCharMapTo(UniChar theChar, UniChar *convertedChar, UInt32 maxLength, uint16_t ctype, UInt32 flags) {
    if (ctype == kCFUniCharCaseFold + 1) { // kCFUniCharDecompose
        if (CFUniCharIsDecomposableCharacter(theChar, false)) {
            UTF32Char buffer[MAX_DECOMPOSED_LENGTH];
            CFIndex usedLength = CFUniCharDecomposeCharacter(theChar, buffer, MAX_DECOMPOSED_LENGTH);
            CFIndex idx;

            for (idx = 0;idx < usedLength;idx++) *(convertedChar++) = buffer[idx];
            return usedLength;
        } else {
            *convertedChar = theChar;
            return 1;
        }
    } else {
        return CFUniCharMapCaseTo(theChar, convertedChar, maxLength, ctype, flags, NULL);
    }
}

CF_INLINE bool __CFUniCharIsMoreAbove(UTF16Char *buffer, uint32_t length) {
    UTF32Char currentChar;
    uint32_t property;

    while (length-- > 0) {
        currentChar = *(buffer)++;
        if (CFUniCharIsSurrogateHighCharacter(currentChar) && (length > 0) && CFUniCharIsSurrogateLowCharacter(*(buffer + 1))) {
            currentChar = CFUniCharGetLongCharacterForSurrogatePair(currentChar, *(buffer++));
            --length;
        }
        if (!CFUniCharIsMemberOf(currentChar, kCFUniCharNonBaseCharacterSet)) break;

        property = CFUniCharGetCombiningPropertyForCharacter(currentChar, CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16) & 0xFF));

        if (property == 230) return true; // Above priority
    }
    return false;
}

CF_INLINE bool __CFUniCharIsAfter_i(UTF16Char *buffer, uint32_t length) {
    UTF32Char currentChar = 0;
    uint32_t property;
    UTF32Char decomposed[MAX_DECOMPOSED_LENGTH];
    uint32_t decompLength;
    uint32_t idx;

    if (length < 1) return 0;

    buffer += length;
    while (length-- > 1) {
        currentChar = *(--buffer);
        if (CFUniCharIsSurrogateLowCharacter(currentChar)) {
            if ((length > 1) && CFUniCharIsSurrogateHighCharacter(*(buffer - 1))) {
                currentChar = CFUniCharGetLongCharacterForSurrogatePair(*(--buffer), currentChar);
                --length;
            } else {
                break;
            }
        }
        if (!CFUniCharIsMemberOf(currentChar, kCFUniCharNonBaseCharacterSet)) break;

        property = CFUniCharGetCombiningPropertyForCharacter(currentChar, CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16) & 0xFF));

        if (property == 230) return false; // Above priority
    }
    if (length == 0) {
        currentChar = *(--buffer);
    } else if (CFUniCharIsSurrogateLowCharacter(currentChar) && CFUniCharIsSurrogateHighCharacter(*(--buffer))) {
        currentChar = CFUniCharGetLongCharacterForSurrogatePair(*buffer, currentChar);
    }

    decompLength = CFUniCharDecomposeCharacter(currentChar, decomposed, MAX_DECOMPOSED_LENGTH);
    currentChar = *decomposed;


    for (idx = 1;idx < decompLength;idx++) {
        currentChar = decomposed[idx];
        property = CFUniCharGetCombiningPropertyForCharacter(currentChar, CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16) & 0xFF));

        if (property == 230) return false; // Above priority
    }
    return true;
}

__private_extern__ uint32_t CFUniCharGetConditionalCaseMappingFlags(UTF32Char theChar, UTF16Char *buffer, uint32_t currentIndex, uint32_t length, uint32_t type, const uint8_t *langCode, uint32_t lastFlags) {
    if (theChar == 0x03A3) { // GREEK CAPITAL LETTER SIGMA
        if ((type == kCFUniCharToLowercase) && (currentIndex > 0)) {
            UTF16Char *start = buffer;
            UTF16Char *end = buffer + length;
            UTF32Char otherChar;

            // First check if we're after a cased character
            buffer += (currentIndex - 1);
            while (start <= buffer) {
                otherChar = *(buffer--);
                if (CFUniCharIsSurrogateLowCharacter(otherChar) && (start <= buffer) && CFUniCharIsSurrogateHighCharacter(*buffer)) {
                    otherChar = CFUniCharGetLongCharacterForSurrogatePair(*(buffer--), otherChar);
                }
                if (!CFUniCharIsMemberOf(otherChar, kCFUniCharCaseIgnorableCharacterSet)) {
                    if (!CFUniCharIsMemberOf(otherChar, kCFUniCharUppercaseLetterCharacterSet) && !CFUniCharIsMemberOf(otherChar, kCFUniCharLowercaseLetterCharacterSet)) return 0; // Uppercase set contains titlecase
                    break;
                }
            }

            // Next check if we're before a cased character
            buffer = start + currentIndex + 1;
            while (buffer < end) {
                otherChar = *(buffer++);
                if (CFUniCharIsSurrogateHighCharacter(otherChar) && (buffer < end) && CFUniCharIsSurrogateLowCharacter(*buffer)) {
                    otherChar = CFUniCharGetLongCharacterForSurrogatePair(otherChar, *(buffer++));
                }
                if (!CFUniCharIsMemberOf(otherChar, kCFUniCharCaseIgnorableCharacterSet)) {
                    if (CFUniCharIsMemberOf(otherChar, kCFUniCharUppercaseLetterCharacterSet) || CFUniCharIsMemberOf(otherChar, kCFUniCharLowercaseLetterCharacterSet)) return 0; // Uppercase set contains titlecase
                    break;
                }
            }
            return kCFUniCharCaseMapFinalSigma;
        }
    } else if (langCode) {
        if (*((const uint16_t *)langCode) == LITHUANIAN_LANG_CODE) {
            if ((theChar == 0x0307) && ((kCFUniCharCaseMapAfter_i|kCFUniCharCaseMapMoreAbove) & lastFlags) == (kCFUniCharCaseMapAfter_i|kCFUniCharCaseMapMoreAbove)) {
                return (__CFUniCharIsAfter_i(buffer, currentIndex) ? kCFUniCharCaseMapAfter_i : 0);
            } else if (type == kCFUniCharToLowercase) {
                if ((theChar == 0x0049) || (theChar == 0x004A) || (theChar == 0x012E)) {
                    return (__CFUniCharIsMoreAbove(buffer + (++currentIndex), length - currentIndex) ? kCFUniCharCaseMapMoreAbove : 0);
                }
            } else if ((theChar == 'i') || (theChar == 'j')) {
                return (__CFUniCharIsMoreAbove(buffer + (++currentIndex), length - currentIndex) ? (kCFUniCharCaseMapAfter_i|kCFUniCharCaseMapMoreAbove) : 0);
            }
        } else if ((*((const uint16_t *)langCode) == TURKISH_LANG_CODE) || (*((const uint16_t *)langCode) == AZERI_LANG_CODE)) {
            if (type == kCFUniCharToLowercase) {
                if (theChar == 0x0307) {
                    return (kCFUniCharCaseMapMoreAbove & lastFlags ? kCFUniCharCaseMapAfter_i : 0);
                } else if (theChar == 0x0049) {
                    return (((++currentIndex < length) && (buffer[currentIndex] == 0x0307)) ? kCFUniCharCaseMapMoreAbove : 0);
                }
            }
        }
    }
    return 0;
}

// Unicode property database
static __CFUniCharBitmapData *__CFUniCharUnicodePropertyTable = NULL;

static CFSpinLock_t __CFUniCharPropTableLock = 0;

#define PROP_DB_FILE "CFUniCharPropertyDatabase.data"

const void *CFUniCharGetUnicodePropertyDataForPlane(uint32_t propertyType, uint32_t plane) {

    __CFSpinLock(&__CFUniCharPropTableLock);

    if (NULL == __CFUniCharUnicodePropertyTable) {
        const void *bytes;
        const void *bodyBase;
        const void *planeBase;
        int headerSize;
        int idx, count;
        int planeIndex, planeCount;
        int planeSize;

        if (!__CFUniCharLoadFile(PROP_DB_FILE, &bytes)) {
            __CFSpinUnlock(&__CFUniCharPropTableLock);
            return NULL;
        }

        (char *)bytes += 4; // Skip Unicode version
        headerSize = CFSwapInt32BigToHost(*(((uint32_t *)bytes)++));
        headerSize -= (sizeof(uint32_t) * 2);
        bodyBase = (char *)bytes + headerSize;

        count = headerSize / sizeof(uint32_t);

        __CFUniCharUnicodePropertyTable = (__CFUniCharBitmapData *)CFAllocatorAllocate(NULL, sizeof(__CFUniCharBitmapData) * count, 0);

        for (idx = 0;idx < count;idx++) {
            planeCount = *((const uint8_t *)bodyBase);
            (char *)planeBase = (char *)bodyBase + planeCount + (planeCount % 4 ? 4 - (planeCount % 4) : 0);
            __CFUniCharUnicodePropertyTable[idx]._planes = (const uint8_t **)CFAllocatorAllocate(NULL, sizeof(const void *) * planeCount, 0);

            for (planeIndex = 0;planeIndex < planeCount;planeIndex++) {
                if ((planeSize = ((const uint8_t *)bodyBase)[planeIndex + 1])) {
                    __CFUniCharUnicodePropertyTable[idx]._planes[planeIndex] = planeBase;
                    (char *)planeBase += (planeSize * 256);
                } else {
                    __CFUniCharUnicodePropertyTable[idx]._planes[planeIndex] = NULL;
                }
            }

            __CFUniCharUnicodePropertyTable[idx]._numPlanes = planeCount;
            (char *)bodyBase += (CFSwapInt32BigToHost(*(((uint32_t *)bytes)++)));
        }
    }

    __CFSpinUnlock(&__CFUniCharPropTableLock);

    return (plane < __CFUniCharUnicodePropertyTable[propertyType]._numPlanes ? __CFUniCharUnicodePropertyTable[propertyType]._planes[plane] : NULL);
}

__private_extern__ uint32_t CFUniCharGetNumberOfPlanesForUnicodePropertyData(uint32_t propertyType) {
    (void)CFUniCharGetUnicodePropertyDataForPlane(propertyType, 0);
    return __CFUniCharUnicodePropertyTable[propertyType]._numPlanes;
}

__private_extern__ uint32_t CFUniCharGetUnicodeProperty(UTF32Char character, uint32_t propertyType) {
    if (propertyType == kCFUniCharCombiningProperty) {
        return CFUniCharGetCombiningPropertyForCharacter(character, CFUniCharGetUnicodePropertyDataForPlane(propertyType, (character >> 16) & 0xFF));
    } else if (propertyType == kCFUniCharBidiProperty) {
        return CFUniCharGetBidiPropertyForCharacter(character, CFUniCharGetUnicodePropertyDataForPlane(propertyType, (character >> 16) & 0xFF));
    } else {
        return 0;
    }
}



/*
    The UTF8 conversion in the following function is derived from ConvertUTF.c
*/
/*
 * Copyright 2001 Unicode, Inc.
 * 
 * Disclaimer
 * 
 * This source code is provided as is by Unicode, Inc. No claims are
 * made as to fitness for any particular purpose. No warranties of any
 * kind are expressed or implied. The recipient agrees to determine
 * applicability of information provided. If this file has been
 * purchased on magnetic or optical media from Unicode, Inc., the
 * sole remedy for any claim will be exchange of defective media
 * within 90 days of receipt.
 * 
 * Limitations on Rights to Redistribute This Code
 * 
 * Unicode, Inc. hereby grants the right to freely use the information
 * supplied in this file in the creation of products supporting the
 * Unicode Standard, and to make copies of this file in any form
 * for internal or external distribution as long as this notice
 * remains attached.
 */
#define UNI_REPLACEMENT_CHAR (0x0000FFFDUL)

bool CFUniCharFillDestinationBuffer(const UTF32Char *src, uint32_t srcLength, void **dst, uint32_t dstLength, uint32_t *filledLength, uint32_t dstFormat) {
    UTF32Char currentChar;
    uint32_t usedLength = *filledLength;

    if (dstFormat == kCFUniCharUTF16Format) {
        UTF16Char *dstBuffer = (UTF16Char *)*dst;

        while (srcLength-- > 0) {
            currentChar = *(src++);

            if (currentChar > 0xFFFF) { // Non-BMP
                usedLength += 2;
                if (dstLength) {
                    if (usedLength > dstLength) return false;
                    currentChar -= 0x10000;
                    *(dstBuffer++) = (UTF16Char)((currentChar >> 10) + 0xD800UL);
                    *(dstBuffer++) = (UTF16Char)((currentChar & 0x3FF) + 0xDC00UL);
                }
            } else {
                ++usedLength;
                if (dstLength) {
                    if (usedLength > dstLength) return false;
                    *(dstBuffer++) = (UTF16Char)currentChar;
                }
            }
        }

        *dst = dstBuffer;
    } else if (dstFormat == kCFUniCharUTF8Format) {
        uint8_t *dstBuffer = (uint8_t *)*dst;
        uint16_t bytesToWrite = 0;
        const UTF32Char byteMask = 0xBF;
        const UTF32Char byteMark = 0x80; 
        static const uint8_t firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

        while (srcLength-- > 0) {
            currentChar = *(src++);

            /* Figure out how many bytes the result will require */
            if (currentChar < (UTF32Char)0x80) {
                bytesToWrite = 1;
            } else if (currentChar < (UTF32Char)0x800) {
                bytesToWrite = 2;
            } else if (currentChar < (UTF32Char)0x10000) {
                bytesToWrite = 3;
            } else if (currentChar < (UTF32Char)0x200000) {
                bytesToWrite = 4;
            } else {
                bytesToWrite = 2;
                currentChar = UNI_REPLACEMENT_CHAR;
            }

            usedLength += bytesToWrite;

            if (dstLength) {
                if (usedLength > dstLength) return false;

                dstBuffer += bytesToWrite;
                switch (bytesToWrite) {	/* note: everything falls through. */
                    case 4:	*--dstBuffer = (currentChar | byteMark) & byteMask; currentChar >>= 6;
                    case 3:	*--dstBuffer = (currentChar | byteMark) & byteMask; currentChar >>= 6;
                    case 2:	*--dstBuffer = (currentChar | byteMark) & byteMask; currentChar >>= 6;
                    case 1:	*--dstBuffer =  currentChar | firstByteMark[bytesToWrite];
                }
                dstBuffer += bytesToWrite;
            }
        }

        *dst = dstBuffer;
    } else {
        UTF32Char *dstBuffer = (UTF32Char *)*dst;

        while (srcLength-- > 0) {
            currentChar = *(src++);

            ++usedLength;
            if (dstLength) {
                if (usedLength > dstLength) return false;
                *(dstBuffer++) = currentChar;
            }
        }

        *dst = dstBuffer;
    }

    *filledLength = usedLength;

    return true;
}
