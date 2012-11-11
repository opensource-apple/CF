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
/*	CFStringEncodingConverterPriv.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFSTRINGENCODINGCONVERTERPRIV__)
#define __COREFOUNDATION_CFSTRINGENCODINGCONVERTERPRIV__ 1

#include <CoreFoundation/CFBase.h>
#include "CFStringEncodingConverterExt.h" 

#define MAX_IANA_ALIASES (4)

typedef UInt32 (*_CFToBytesProc)(const void *converter, UInt32 flags, const UniChar *characters, UInt32 numChars, UInt8 *bytes, UInt32 maxByteLen, UInt32 *usedByteLen);
typedef UInt32 (*_CFToUnicodeProc)(const void *converter, UInt32 flags, const UInt8 *bytes, UInt32 numBytes, UniChar *characters, UInt32 maxCharLen, UInt32 *usedCharLen);

typedef struct {
    _CFToBytesProc toBytes;
    _CFToUnicodeProc toUnicode;
    _CFToUnicodeProc toCanonicalUnicode;
    void *_toBytes; // original proc
    void *_toUnicode; // original proc
    UInt16 maxLen;
    UInt16 :16;
    CFStringEncodingToBytesLenProc toBytesLen;
    CFStringEncodingToUnicodeLenProc toUnicodeLen;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    CFStringEncodingToBytesPrecomposeProc toBytesPrecompose;
    CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar;
} _CFEncodingConverter;

typedef struct {
    UInt32 encoding;
    _CFEncodingConverter *converter;
    const char *encodingName;
    const char *ianaNames[MAX_IANA_ALIASES];
    const char *loadablePath;
    CFStringEncodingBootstrapProc bootstrap;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    UInt32 scriptCode;
} _CFConverterEntry;

extern CFStringEncodingConverter __CFConverterASCII;
extern CFStringEncodingConverter __CFConverterISOLatin1;
extern CFStringEncodingConverter __CFConverterMacRoman;
extern CFStringEncodingConverter __CFConverterWinLatin1;
extern CFStringEncodingConverter __CFConverterNextStepLatin;
extern CFStringEncodingConverter __CFConverterUTF8;


#endif /* ! __COREFOUNDATION_CFSTRINGENCODINGCONVERTERPRIV__ */

