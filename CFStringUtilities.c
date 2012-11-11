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
/*	CFStringUtilities.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Aki Inoue
*/

#include "CFInternal.h"
#include "CFStringEncodingConverterExt.h"
#include "CFUniChar.h"
#include <CoreFoundation/CFStringEncodingExt.h>
#include <CoreFoundation/CFPreferences.h>
#include <limits.h>
#if (DEPLOYMENT_TARGET_MACOSX) || DEPLOYMENT_TARGET_LINUX
#include <stdlib.h>
#elif  defined(__WIN32__)
#include <stdlib.h>
#include <tchar.h>
#endif


Boolean CFStringIsEncodingAvailable(CFStringEncoding theEncoding) {
    switch (theEncoding) {
        case kCFStringEncodingASCII: // Built-in encodings
        case kCFStringEncodingMacRoman:
        case kCFStringEncodingUTF8:
        case kCFStringEncodingNonLossyASCII:
        case kCFStringEncodingWindowsLatin1:
        case kCFStringEncodingNextStepLatin:
        case kCFStringEncodingUTF16:
        case kCFStringEncodingUTF16BE:
        case kCFStringEncodingUTF16LE:
        case kCFStringEncodingUTF32:
        case kCFStringEncodingUTF32BE:
        case kCFStringEncodingUTF32LE:
            return true;

        default:
            return CFStringEncodingIsValidEncoding(theEncoding);
    }
}

const CFStringEncoding* CFStringGetListOfAvailableEncodings() {
    return (const CFStringEncoding *)CFStringEncodingListOfAvailableEncodings();
}

CFStringRef CFStringGetNameOfEncoding(CFStringEncoding theEncoding) {
    static CFMutableDictionaryRef mappingTable = NULL;
    CFStringRef theName = mappingTable ? (CFStringRef)CFDictionaryGetValue(mappingTable, (const void*)(uintptr_t)theEncoding) : NULL;

    if (!theName) {
        switch (theEncoding) {
            case kCFStringEncodingUTF8: theName = CFSTR("Unicode (UTF-8)"); break;
            case kCFStringEncodingUTF16: theName = CFSTR("Unicode (UTF-16)"); break;
            case kCFStringEncodingUTF16BE: theName = CFSTR("Unicode (UTF-16BE)"); break;
            case kCFStringEncodingUTF16LE: theName = CFSTR("Unicode (UTF-16LE)"); break;
            case kCFStringEncodingUTF32: theName = CFSTR("Unicode (UTF-32)"); break;
            case kCFStringEncodingUTF32BE: theName = CFSTR("Unicode (UTF-32BE)"); break;
            case kCFStringEncodingUTF32LE: theName = CFSTR("Unicode (UTF-32LE)"); break;
            case kCFStringEncodingNonLossyASCII: theName = CFSTR("Non-lossy ASCII"); break;
    
            default: {
                const char *encodingName = CFStringEncodingName(theEncoding);
    
                if (encodingName) {
                    theName = CFStringCreateWithCString(kCFAllocatorSystemDefault, encodingName, kCFStringEncodingASCII);
                }
            }
            break;
        }

        if (theName) {
            if (!mappingTable) mappingTable = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, (const CFDictionaryKeyCallBacks *)NULL, &kCFTypeDictionaryValueCallBacks);

            CFDictionaryAddValue(mappingTable, (const void*)(uintptr_t)theEncoding, (const void*)theName);
            CFRelease(theName);
        }
    }

    return theName;
}

CFStringEncoding CFStringConvertIANACharSetNameToEncoding(CFStringRef  charsetName) {
    static CFMutableDictionaryRef mappingTable = NULL;
    CFStringEncoding result = kCFStringEncodingInvalidId;
    CFMutableStringRef lowerCharsetName;

    /* Check for common encodings first */
    if (CFStringCompare(charsetName, CFSTR("utf-8"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        return kCFStringEncodingUTF8;
    } else if (CFStringCompare(charsetName, CFSTR("iso-8859-1"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        return kCFStringEncodingISOLatin1;
    }

    /* Create lowercase copy */
    lowerCharsetName = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, charsetName);
    CFStringLowercase(lowerCharsetName, NULL);

    if (mappingTable == NULL) {
        CFMutableDictionaryRef table = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, (const CFDictionaryValueCallBacks *)NULL);
        const CFStringEncoding *encodings = CFStringGetListOfAvailableEncodings();

        while (*encodings != kCFStringEncodingInvalidId) {
            const char **nameList = CFStringEncodingCanonicalCharsetNames(*encodings);

            if (nameList) {
                while (*nameList) {
                    CFStringRef name = CFStringCreateWithCString(kCFAllocatorSystemDefault, *nameList++, kCFStringEncodingASCII);

                    if (name) {
                        CFDictionaryAddValue(table, (const void*)name, (const void*)(uintptr_t)*encodings);
                        CFRelease(name);
                    }
                }
            }
            encodings++;
        }
        // Adding Unicode names
        CFDictionaryAddValue(table, (const void*)CFSTR("unicode-1-1"), (const void*)kCFStringEncodingUTF16);
        CFDictionaryAddValue(table, (const void*)CFSTR("iso-10646-ucs-2"), (const void*)kCFStringEncodingUTF16);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-16"), (const void*)kCFStringEncodingUTF16);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-16be"), (const void*)kCFStringEncodingUTF16BE);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-16le"), (const void*)kCFStringEncodingUTF16LE);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-32"), (const void*)kCFStringEncodingUTF32);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-32be"), (const void*)kCFStringEncodingUTF32BE);
        CFDictionaryAddValue(table, (const void*)CFSTR("utf-32le"), (const void*)kCFStringEncodingUTF32LE);

        mappingTable = table;
    }

    if (CFDictionaryContainsKey(mappingTable, (const void*)lowerCharsetName)) {
        result = (CFStringEncoding)(uintptr_t)CFDictionaryGetValue(mappingTable, (const void*)lowerCharsetName);
    }
    
    CFRelease(lowerCharsetName);

    return result;
}

CFStringRef CFStringConvertEncodingToIANACharSetName(CFStringEncoding encoding) {
    static CFMutableDictionaryRef mappingTable = NULL;
    CFStringRef theName = mappingTable ? (CFStringRef)CFDictionaryGetValue(mappingTable, (const void*)(uintptr_t)encoding) : NULL;

    if (!theName) {
        switch (encoding) {
            case kCFStringEncodingUTF16: theName = CFSTR("UTF-16"); break;
            case kCFStringEncodingUTF16BE: theName = CFSTR("UTF-16BE"); break;
            case kCFStringEncodingUTF16LE: theName = CFSTR("UTF-16LE"); break;
            case kCFStringEncodingUTF32: theName = CFSTR("UTF-32"); break;
            case kCFStringEncodingUTF32BE: theName = CFSTR("UTF-32BE"); break;
            case kCFStringEncodingUTF32LE: theName = CFSTR("UTF-32LE"); break;
    
    
            default: {
                const char **nameList = CFStringEncodingCanonicalCharsetNames(encoding);
    
                if (nameList && *nameList) {
                    CFMutableStringRef upperCaseName;
    
                    theName = CFStringCreateWithCString(kCFAllocatorSystemDefault, *nameList, kCFStringEncodingASCII);
                    if (theName) {
                        upperCaseName = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, theName);
                        CFStringUppercase(upperCaseName, 0);
                        CFRelease(theName);
                        theName = upperCaseName;
                    }
                }
            }
            break;
        }

        if (theName) {
            if (!mappingTable) mappingTable = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, (const CFDictionaryKeyCallBacks *)NULL, &kCFTypeDictionaryValueCallBacks);

            CFDictionaryAddValue(mappingTable, (const void*)(uintptr_t)encoding, (const void*)theName);
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

unsigned long CFStringConvertEncodingToNSStringEncoding(CFStringEncoding theEncoding) {
    switch (theEncoding & 0xFFF) {
        case kCFStringEncodingASCII: return NSASCIIStringEncoding;
        case kCFStringEncodingNextStepLatin: return NSNEXTSTEPStringEncoding;
        case kCFStringEncodingISOLatin1: return NSISOLatin1StringEncoding;
        case kCFStringEncodingNonLossyASCII: return NSNonLossyASCIIStringEncoding;
        case kCFStringEncodingWindowsLatin1: return NSWindowsCP1252StringEncoding;
        case kCFStringEncodingMacRoman: return NSMacOSRomanStringEncoding;
#if DEPLOYMENT_TARGET_MACOSX
        case kCFStringEncodingEUC_JP: return NSJapaneseEUCStringEncoding;
        case kCFStringEncodingMacSymbol: return NSSymbolStringEncoding;
        case kCFStringEncodingDOSJapanese: return NSShiftJISStringEncoding;
        case kCFStringEncodingISOLatin2: return NSISOLatin2StringEncoding;
        case kCFStringEncodingWindowsCyrillic: return NSWindowsCP1251StringEncoding;
        case kCFStringEncodingWindowsGreek: return NSWindowsCP1253StringEncoding;
        case kCFStringEncodingWindowsLatin5: return NSWindowsCP1254StringEncoding;
        case kCFStringEncodingWindowsLatin2: return NSWindowsCP1250StringEncoding;
        case kCFStringEncodingISO_2022_JP: return NSISO2022JPStringEncoding;
#endif
#if DEPLOYMENT_TARGET_MACOSX
        case kCFStringEncodingUnicode:
            if (theEncoding == kCFStringEncodingUTF16) return NSUnicodeStringEncoding;
            else if (theEncoding == kCFStringEncodingUTF8) return NSUTF8StringEncoding;
#endif
            /* fall-through for other encoding schemes */

        default:
            return NSENCODING_MASK | theEncoding;
    }
}

CFStringEncoding CFStringConvertNSStringEncodingToEncoding(unsigned long theEncoding) {
    switch (theEncoding) {
        case NSASCIIStringEncoding: return kCFStringEncodingASCII;
        case NSNEXTSTEPStringEncoding: return kCFStringEncodingNextStepLatin;
        case NSUTF8StringEncoding: return kCFStringEncodingUTF8;
        case NSISOLatin1StringEncoding: return kCFStringEncodingISOLatin1;
        case NSNonLossyASCIIStringEncoding: return kCFStringEncodingNonLossyASCII;
        case NSUnicodeStringEncoding: return kCFStringEncodingUTF16;
        case NSWindowsCP1252StringEncoding: return kCFStringEncodingWindowsLatin1;
        case NSMacOSRomanStringEncoding: return kCFStringEncodingMacRoman;
#if DEPLOYMENT_TARGET_MACOSX
        case NSSymbolStringEncoding: return kCFStringEncodingMacSymbol;
        case NSJapaneseEUCStringEncoding: return kCFStringEncodingEUC_JP;
        case NSShiftJISStringEncoding: return kCFStringEncodingDOSJapanese;
        case NSISO2022JPStringEncoding: return kCFStringEncodingISO_2022_JP;
        case NSISOLatin2StringEncoding: return kCFStringEncodingISOLatin2;
        case NSWindowsCP1251StringEncoding: return kCFStringEncodingWindowsCyrillic;
        case NSWindowsCP1253StringEncoding: return kCFStringEncodingWindowsGreek;
        case NSWindowsCP1254StringEncoding: return kCFStringEncodingWindowsLatin5;
        case NSWindowsCP1250StringEncoding: return kCFStringEncodingWindowsLatin2;
#endif
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
    1252, 1250, 1251, 1253, 1254, 1255, 1256, 1257, 1258,
};

static const uint16_t _CFEUCToCodePage[] = { // 0x900
    51932, 51936, 51950, 51949,
};

UInt32 CFStringConvertEncodingToWindowsCodepage(CFStringEncoding theEncoding) {
#if DEPLOYMENT_TARGET_MACOSX
    CFStringEncoding encodingBase = theEncoding & 0x0FFF;
#endif

    switch (theEncoding & 0x0F00) {
#if DEPLOYMENT_TARGET_MACOSX
    case 0: // Mac OS script
        if (encodingBase <= kCFStringEncodingMacCentralEurRoman) {
            return MACCODEPAGE_BASE + encodingBase;
        } else if (encodingBase == kCFStringEncodingMacTurkish) {
            return 10081;
        } else if (encodingBase == kCFStringEncodingMacCroatian) {
            return 10082;
        } else if (encodingBase == kCFStringEncodingMacIcelandic) {
            return 10079;
        }
        break;
#endif

    case 0x100: // Unicode
        switch (theEncoding) {
        case kCFStringEncodingUTF8: return 65001;
        case kCFStringEncodingUTF16: return 1200;
        case kCFStringEncodingUTF16BE: return 1201;
        case kCFStringEncodingUTF32: return 65005;
        case kCFStringEncodingUTF32BE: return 65006;
        }
        break;

#if (DEPLOYMENT_TARGET_MACOSX) 
    case 0x0200: // ISO 8859 series
        if (encodingBase <= kCFStringEncodingISOLatin10) return ISO8859CODEPAGE_BASE + (encodingBase - 0x200);
        break;

    case 0x0400: // DOS codepage
        if (encodingBase <= kCFStringEncodingDOSChineseTrad) return _CFToDOSCodePageList[encodingBase - 0x400]; 
        break;

    case 0x0500: // ANSI (Windows) codepage
        if (encodingBase <= kCFStringEncodingWindowsVietnamese) return _CFToWindowsCodePageList[theEncoding - 0x500];
        else if (encodingBase == kCFStringEncodingWindowsKoreanJohab) return 1361;
        break;

    case 0x600: // National standards
        if (encodingBase == kCFStringEncodingASCII) return 20127;
        else if (encodingBase == kCFStringEncodingGB_18030_2000) return 54936;
        break;

    case 0x0800: // ISO 2022 series
        switch (encodingBase) {
        case kCFStringEncodingISO_2022_JP: return 50220;
        case kCFStringEncodingISO_2022_CN: return 50227;
        case kCFStringEncodingISO_2022_KR: return 50225;
        }
        break;

    case 0x0900: // EUC series
        if (encodingBase <= kCFStringEncodingEUC_KR) return _CFEUCToCodePage[encodingBase - 0x0900];
        break;


    case 0x0A00: // Misc encodings
        switch (encodingBase) {
        case kCFStringEncodingKOI8_R: return 20866;
        case kCFStringEncodingHZ_GB_2312: return 52936;
        case kCFStringEncodingKOI8_U: return 21866;
        }
        break;

    case 0x0C00: // IBM EBCDIC encodings
        if (encodingBase == kCFStringEncodingEBCDIC_CP037) return 37;
        break;
#endif
    }

    return kCFStringEncodingInvalidId;
}

#if DEPLOYMENT_TARGET_MACOSX
static const struct {
    uint16_t acp;
    uint16_t encoding;
} _CFACPToCFTable[] = {
    {37, kCFStringEncodingEBCDIC_CP037},
    {437, kCFStringEncodingDOSLatinUS},
    {737, kCFStringEncodingDOSGreek},
    {775, kCFStringEncodingDOSBalticRim},
    {850, kCFStringEncodingDOSLatin1},
    {851, kCFStringEncodingDOSGreek1},
    {852, kCFStringEncodingDOSLatin2},
    {855, kCFStringEncodingDOSCyrillic},
    {857, kCFStringEncodingDOSTurkish},
    {860, kCFStringEncodingDOSPortuguese},
    {861, kCFStringEncodingDOSIcelandic},
    {862, kCFStringEncodingDOSHebrew},
    {863, kCFStringEncodingDOSCanadianFrench},
    {864, kCFStringEncodingDOSArabic},
    {865, kCFStringEncodingDOSNordic},
    {866, kCFStringEncodingDOSRussian},
    {869, kCFStringEncodingDOSGreek2},
    {874, kCFStringEncodingDOSThai},
    {932, kCFStringEncodingDOSJapanese},
    {936, kCFStringEncodingDOSChineseSimplif},
    {949, kCFStringEncodingDOSKorean},
    {950, kCFStringEncodingDOSChineseTrad},
    {1250, kCFStringEncodingWindowsLatin2},
    {1251, kCFStringEncodingWindowsCyrillic},
    {1252, kCFStringEncodingWindowsLatin1},
    {1253, kCFStringEncodingWindowsGreek},
    {1254, kCFStringEncodingWindowsLatin5},
    {1255, kCFStringEncodingWindowsHebrew},
    {1256, kCFStringEncodingWindowsArabic},
    {1257, kCFStringEncodingWindowsBalticRim},
    {1258, kCFStringEncodingWindowsVietnamese},
    {1361, kCFStringEncodingWindowsKoreanJohab},
    {20127, kCFStringEncodingASCII},
    {20866, kCFStringEncodingKOI8_R},
    {21866, kCFStringEncodingKOI8_U},
    {50220, kCFStringEncodingISO_2022_JP},
    {50225, kCFStringEncodingISO_2022_KR},
    {50227, kCFStringEncodingISO_2022_CN},
    {51932, kCFStringEncodingEUC_JP},
    {51936, kCFStringEncodingEUC_CN},
    {51949, kCFStringEncodingEUC_KR},
    {51950, kCFStringEncodingEUC_TW},
    {52936, kCFStringEncodingHZ_GB_2312},
    {54936, kCFStringEncodingGB_18030_2000},
};

static SInt32 bsearchEncoding(uint16_t target) {
    const unsigned int *start, *end, *divider;
    unsigned int size = sizeof(_CFACPToCFTable) / sizeof(UInt32);

    start = (const unsigned int*)_CFACPToCFTable; end = (const unsigned int*)_CFACPToCFTable + (size - 1);
    while (start <= end) {
        divider = start + ((end - start) / 2);

        if (*(const uint16_t*)divider == target) return *((const uint16_t*)divider + 1);
        else if (*(const uint16_t*)divider > target) end = divider - 1;
        else if (*(const uint16_t*)(divider + 1) > target) return *((const uint16_t*)divider + 1);
        else start = divider + 1;
    }
    return (kCFStringEncodingInvalidId);
}
#endif

CFStringEncoding CFStringConvertWindowsCodepageToEncoding(UInt32 theEncoding) {
    if (theEncoding == 0 || theEncoding == 1) { // ID for default (system) codepage
        return CFStringGetSystemEncoding();
    } else if ((theEncoding >= MACCODEPAGE_BASE) && (theEncoding < 20000)) { // Mac script
        if (theEncoding <= 10029) return theEncoding - MACCODEPAGE_BASE; // up to Mac Central European
#if (DEPLOYMENT_TARGET_MACOSX) 
        else if (theEncoding == 10079) return kCFStringEncodingMacIcelandic;
        else if (theEncoding == 10081) return kCFStringEncodingMacTurkish;
        else if (theEncoding == 10082) return kCFStringEncodingMacCroatian;
#endif
    } else if ((theEncoding >= ISO8859CODEPAGE_BASE) && (theEncoding <= 28605)) { // ISO 8859
        return (theEncoding - ISO8859CODEPAGE_BASE) + 0x200;
    } else if (theEncoding == 65001) { // UTF-8
        return kCFStringEncodingUTF8;
    } else if (theEncoding == 12000) { // UTF-16
        return kCFStringEncodingUTF16;
    } else if (theEncoding == 12001) { // UTF-16BE
        return kCFStringEncodingUTF16BE;
    } else if (theEncoding == 65005) { // UTF-32
        return kCFStringEncodingUTF32;
    } else if (theEncoding == 65006) { // UTF-32BE
        return kCFStringEncodingUTF32BE;
    } else {
#if DEPLOYMENT_TARGET_MACOSX
        return bsearchEncoding(theEncoding);
#endif
    }

    return kCFStringEncodingInvalidId;
}

CFStringEncoding CFStringGetMostCompatibleMacStringEncoding(CFStringEncoding encoding) {
    CFStringEncoding macEncoding;

    macEncoding = CFStringEncodingGetScriptCodeForEncoding(encoding);

    return macEncoding;
}


