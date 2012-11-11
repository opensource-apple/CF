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
/*	CFStringEncodingConverterPriv.h
	Copyright (c) 1998-2007, Apple Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFSTRINGENCODINGCONVERTERPRIV__)
#define __COREFOUNDATION_CFSTRINGENCODINGCONVERTERPRIV__ 1

#include <CoreFoundation/CFBase.h>
#include "CFStringEncodingConverterExt.h"

#define MAX_IANA_ALIASES (4)

typedef CFIndex (*_CFToBytesProc)(const void *converter, uint32_t flags, const UniChar *characters, CFIndex numChars, uint8_t *bytes, CFIndex maxByteLen, CFIndex *usedByteLen);
typedef CFIndex (*_CFToUnicodeProc)(const void *converter, uint32_t flags, const uint8_t *bytes, CFIndex numBytes, UniChar *characters, CFIndex maxCharLen, CFIndex *usedCharLen);

typedef struct {
    _CFToBytesProc toBytes;
    _CFToUnicodeProc toUnicode;
    _CFToUnicodeProc toCanonicalUnicode;
    void *_toBytes; // original proc
    void *_toUnicode; // original proc
    uint16_t maxLen;
    uint16_t :16;
    CFStringEncodingToBytesLenProc toBytesLen;
    CFStringEncodingToUnicodeLenProc toUnicodeLen;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    CFStringEncodingToBytesPrecomposeProc toBytesPrecompose;
    CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar;
} _CFEncodingConverter;

typedef struct {
    uint32_t encoding;
    _CFEncodingConverter *converter;
    const char *encodingName;
    const char *ianaNames[MAX_IANA_ALIASES];
    const char *loadablePath;
    CFStringEncodingBootstrapProc bootstrap;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    uint32_t scriptCode;
} _CFConverterEntry;

extern const CFStringEncodingConverter __CFConverterASCII;
extern const CFStringEncodingConverter __CFConverterISOLatin1;
extern const CFStringEncodingConverter __CFConverterMacRoman;
extern const CFStringEncodingConverter __CFConverterWinLatin1;
extern const CFStringEncodingConverter __CFConverterNextStepLatin;
extern const CFStringEncodingConverter __CFConverterUTF8;


#endif /* ! __COREFOUNDATION_CFSTRINGENCODINGCONVERTERPRIV__ */

