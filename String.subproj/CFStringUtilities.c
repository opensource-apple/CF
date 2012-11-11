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
/*	CFStringUtilities.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Aki Inoue
*/

#include "CFInternal.h"
#include "CFStringEncodingConverterExt.h"
#include "CFUniChar.h"
#include <limits.h>
#if defined(__MACH__) || defined(__LINUX__)
#include <stdlib.h>
#elif  defined(__WIN32__)
#include <stdlib.h>
#include <tchar.h>
#endif



Boolean CFStringIsEncodingAvailable(CFStringEncoding theEncoding) {
    switch (theEncoding) {
        case kCFStringEncodingASCII: // Built-in encodings
        case kCFStringEncodingMacRoman:
        case kCFStringEncodingUnicode:
        case kCFStringEncodingUTF8:
        case kCFStringEncodingNonLossyASCII:
        case kCFStringEncodingWindowsLatin1:
        case kCFStringEncodingNextStepLatin:
            return true;

        default:
            return CFStringEncodingIsValidEncoding(theEncoding);
    }
}

const CFStringEncoding* CFStringGetListOfAvailableEncodings() {
    return CFStringEncodingListOfAvailableEncodings();
}

CFStringRef CFStringGetNameOfEncoding(CFStringEncoding theEncoding) {
    static CFMutableDictionaryRef mappingTable = NULL;
    CFStringRef theName = mappingTable ? CFDictionaryGetValue(mappingTable, (const void*)theEncoding) : NULL;

    if (!theName) {
        if (theEncoding == kCFStringEncodingUnicode) {
            theName = CFSTR("Unicode (UTF-16)");
        } else if (theEncoding == kCFStringEncodingUTF8) {
            theName = CFSTR("Unicode (UTF-8)");
        } else if (theEncoding == kCFStringEncodingNonLossyASCII) {
            theName = CFSTR("Non-lossy ASCII");
        } else {
            const uint8_t *encodingName = CFStringEncodingName(theEncoding);

            if (encodingName) {
                theName = CFStringCreateWithCString(NULL, encodingName, kCFStringEncodingASCII);
            }

            if (theName) {
                if (!mappingTable) mappingTable = CFDictionaryCreateMutable(NULL, 0, (const CFDictionaryKeyCallBacks *)NULL, &kCFTypeDictionaryValueCallBacks);

                CFDictionaryAddValue(mappingTable, (const void*)theEncoding, (const void*)theName);
                CFRelease(theName);
            }
        }
    }

    return theName;
}

CFStringEncoding CFStringConvertIANACharSetNameToEncoding(CFStringRef  charsetName) {
    static CFMutableDictionaryRef mappingTable = NULL;
    CFStringEncoding result = kCFStringEncodingInvalidId;
    CFMutableStringRef lowerCharsetName = CFStringCreateMutableCopy(NULL, 0, charsetName);

    /* Create lowercase copy */
    CFStringLowercase(lowerCharsetName, NULL);

    /* Check for common encodings first */
    if (CFStringCompare(lowerCharsetName, CFSTR("utf-8"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        CFRelease(lowerCharsetName);
        return kCFStringEncodingUTF8;
    } else if (CFStringCompare(lowerCharsetName, CFSTR("iso-8859-1"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        CFRelease(lowerCharsetName);
        return kCFStringEncodingISOLatin1;
    } else if (CFStringCompare(lowerCharsetName, CFSTR("utf-16be"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        CFRelease(lowerCharsetName);
        return kCFStringEncodingUnicode;
    }

    if (mappingTable == NULL) {
        CFMutableDictionaryRef table = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, (const CFDictionaryValueCallBacks *)NULL);
        const CFStringEncoding *encodings = CFStringGetListOfAvailableEncodings();

        while (*encodings != kCFStringEncodingInvalidId) {
            const char **nameList = CFStringEncodingCanonicalCharsetNames(*encodings);

            if (nameList) {
                while (*nameList) {
                    CFStringRef name = CFStringCreateWithCString(NULL, *nameList++, kCFStringEncodingASCII);

                    if (name) {
                        CFDictionaryAddValue(table, (const void*)name, (const void*)*encodings);
                        CFRelease(name);
                    }
                }
            }
            encodings++;
        }
        // Adding Unicode (UCS-2) names
        CFDictionaryAddValue(table, (const void*)CFSTR("unicode-1-1"), (const void*)kCFStringEncodingUnicode);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-16"), (const void*)kCFStringEncodingUnicode);
        CFDictionaryAddValue(table, (const void*)CFSTR("iso-10646-ucs-2"), (const void*)kCFStringEncodingUnicode);

        mappingTable = table;
    }

    if (CFDictionaryContainsKey(mappingTable, (const void*)lowerCharsetName)) {
        result = (CFStringEncoding)CFDictionaryGetValue(mappingTable, (const void*)lowerCharsetName);
    }
    
    CFRelease(lowerCharsetName);

    return result;
}

CFStringRef CFStringConvertEncodingToIANACharSetName(CFStringEncoding encoding) {
    static CFMutableDictionaryRef mappingTable = NULL;
    CFStringRef theName = mappingTable ? (CFStringRef)CFDictionaryGetValue(mappingTable, (const void*)encoding) : NULL;

    if (!theName) {
        if (encoding == kCFStringEncodingUnicode) {
            theName = CFSTR("UTF-16BE");
        } else {
            const char **nameList = CFStringEncodingCanonicalCharsetNames(encoding);

            if (nameList && *nameList) {
                CFMutableStringRef upperCaseName;

                theName = CFStringCreateWithCString(NULL, *nameList, kCFStringEncodingASCII);
                if (theName) {
                    upperCaseName = CFStringCreateMutableCopy(NULL, 0, theName);
                    CFStringUppercase(upperCaseName, 0);
                    CFRelease(theName);
                    theName = upperCaseName;
                }
            }
       }

        if (theName) {
            if (!mappingTable) mappingTable = CFDictionaryCreateMutable(NULL, 0, (const CFDictionaryKeyCallBacks *)NULL, &kCFTypeDictionaryValueCallBacks);

            CFDictionaryAddValue(mappingTable, (const void*)encoding, (const void*)theName);
            CFRelease(theName);
        }
    }

    return theName;
}

enum {
    NSASCIIStringEncoding = 1,		/* 0..127 only */
    NSNEXTSTEPStringEncoding = 2,
    NSJapaneseEUCStringEncoding = 3,
    NSUTF8StringEncoding = 4,
    NSISOLatin1StringEncoding = 5,
    NSSymbolStringEncoding = 6,
    NSNonLossyASCIIStringEncoding = 7,
    NSShiftJISStringEncoding = 8,
    NSISOLatin2StringEncoding = 9,
    NSUnicodeStringEncoding = 10,
    NSWindowsCP1251StringEncoding = 11,    /* Cyrillic; same as AdobeStandardCyrillic */
    NSWindowsCP1252StringEncoding = 12,    /* WinLatin1 */
    NSWindowsCP1253StringEncoding = 13,    /* Greek */
    NSWindowsCP1254StringEncoding = 14,    /* Turkish */
    NSWindowsCP1250StringEncoding = 15,    /* WinLatin2 */
    NSISO2022JPStringEncoding = 21,         /* ISO 2022 Japanese encoding for e-mail */
    NSMacOSRomanStringEncoding = 30,

    NSProprietaryStringEncoding = 65536    /* Installation-specific encoding */
};

#define NSENCODING_MASK (1 << 31)

UInt32 CFStringConvertEncodingToNSStringEncoding(CFStringEncoding theEncoding) {
    if (theEncoding == kCFStringEncodingUTF8) {
        return NSUTF8StringEncoding;
    } else {
        theEncoding &= 0xFFF;
    }
    switch (theEncoding) {
        case kCFStringEncodingASCII: return NSASCIIStringEncoding;
        case kCFStringEncodingNextStepLatin: return NSNEXTSTEPStringEncoding;
        case kCFStringEncodingISOLatin1: return NSISOLatin1StringEncoding;
        case kCFStringEncodingNonLossyASCII: return NSNonLossyASCIIStringEncoding;
        case kCFStringEncodingUnicode: return NSUnicodeStringEncoding;
        case kCFStringEncodingWindowsLatin1: return NSWindowsCP1252StringEncoding;
        case kCFStringEncodingMacRoman: return NSMacOSRomanStringEncoding;
        default:
            return NSENCODING_MASK | theEncoding;
    }
}

CFStringEncoding CFStringConvertNSStringEncodingToEncoding(UInt32 theEncoding) {
    switch (theEncoding) {
        case NSASCIIStringEncoding: return kCFStringEncodingASCII;
        case NSNEXTSTEPStringEncoding: return kCFStringEncodingNextStepLatin;
        case NSUTF8StringEncoding: return kCFStringEncodingUTF8;
        case NSISOLatin1StringEncoding: return kCFStringEncodingISOLatin1;
        case NSNonLossyASCIIStringEncoding: return kCFStringEncodingNonLossyASCII;
        case NSUnicodeStringEncoding: return kCFStringEncodingUnicode;
        case NSWindowsCP1252StringEncoding: return kCFStringEncodingWindowsLatin1;
        case NSMacOSRomanStringEncoding: return kCFStringEncodingMacRoman;
        default:
            return ((theEncoding & NSENCODING_MASK) ? theEncoding & ~NSENCODING_MASK : kCFStringEncodingInvalidId);
    }
}

#define MACCODEPAGE_BASE (10000)
#define ISO8859CODEPAGE_BASE (28590)

static const uint16_t _CFToDOSCodePageList[] = {
    437, -1, -1, -1, -1, 737, 775, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x400
    850, 851, 852, 855, 857, 860, 861, 862, 863, 864, 865, 866, 869, 874, -1, 01, // 0x410
    932, 936, 949 , 950, // 0x420
};

static const uint16_t _CFToWindowsCodePageList[] = {
    1252, 1250, 1251, 1253, 1254, 1255, 1256, 1257, 1361,
};

UInt32 CFStringConvertEncodingToWindowsCodepage(CFStringEncoding theEncoding) {
    if (theEncoding == kCFStringEncodingUTF8) {
        return 65001;
    } else {
        theEncoding &= 0xFFF;
    }
    return kCFStringEncodingInvalidId;
}

static const struct {
    uint16_t acp;
    uint16_t encoding;
} _CFACPToCFTable[] = {
    {437,0x0400},
    {737,0x0405},
    {775,0x0406},
    {850,0x0410},
    {851,0x0411},
    {852,0x0412},
    {855,0x0413},
    {857,0x0414},
    {860,0x0415},
    {861,0x0416},
    {862,0x0417},
    {863,0x0418},
    {864,0x0419},
    {865,0x041A},
    {866,0x041B},
    {869,0x041C},
    {874,0x041D},
    {932,0x0420},
    {936,0x0421},
    {949,0x0422},
    {950,0x0423},
    {1250,0x0501},
    {1251,0x0502},
    {1252,0x0500},
    {1253,0x0503},
    {1254,0x0504},
    {1255,0x0505},
    {1256,0x0506},
    {1257,0x0507},
    {1361,0x0510},
    {0xFFFF,0xFFFF},
};

static SInt32 bsearchEncoding(unsigned short target) {
    const unsigned int *start, *end, *divider;
    unsigned int size = sizeof(_CFACPToCFTable) / sizeof(UInt32);

    start = (const unsigned int*)_CFACPToCFTable; end = (const unsigned int*)_CFACPToCFTable + (size - 1);
    while (start <= end) {
        divider = start + ((end - start) / 2);

        if (*(const unsigned short*)divider == target) return *((const unsigned short*)divider + 1);
        else if (*(const unsigned short*)divider > target) end = divider - 1;
        else if (*(const unsigned short*)(divider + 1) > target) return *((const unsigned short*)divider + 1);
        else start = divider + 1;
    }
    return (kCFStringEncodingInvalidId);
}

CFStringEncoding CFStringConvertWindowsCodepageToEncoding(UInt32 theEncoding) {
    if (theEncoding == 0 || theEncoding == 1) { // ID for default (system) codepage
        return CFStringGetSystemEncoding();
    } else if (theEncoding < MACCODEPAGE_BASE) { // MS CodePage
        return bsearchEncoding(theEncoding);
    } else if (theEncoding < 20000) { // MAC ScriptCode
        return theEncoding - MACCODEPAGE_BASE;
    } else if ((theEncoding - ISO8859CODEPAGE_BASE) < 10) { // ISO8859 range
        return (theEncoding - ISO8859CODEPAGE_BASE) + 0x200;
    } else {
        switch (theEncoding) {
            case 65001: return kCFStringEncodingUTF8;
            case 20127: return kCFStringEncodingASCII;
            default: return kCFStringEncodingInvalidId;
        }
    }
}

CFStringEncoding CFStringGetMostCompatibleMacStringEncoding(CFStringEncoding encoding) {
    CFStringEncoding macEncoding;

    macEncoding = CFStringEncodingGetScriptCodeForEncoding(encoding);

    return macEncoding;
}

