/*
 * Copyright (c) 2012 Apple Inc. All rights reserved.
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

/*	CFPropertyList.c
	Copyright (c) 1999-2011, Apple Inc. All rights reserved.
	Responsibility: Tony Parker
*/

#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFError.h>
#include <CoreFoundation/CFError_Private.h>
#include <CoreFoundation/CFPriv.h>
#include <CoreFoundation/CFStringEncodingConverter.h>
#include <CoreFoundation/CFInternal.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS
#include <CoreFoundation/CFStream.h>
#endif
#include <CoreFoundation/CFCalendar.h>
#include "CFLocaleInternal.h"
#include <limits.h>
#include <float.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

#define PLIST_IX    0
#define ARRAY_IX    1
#define DICT_IX     2
#define KEY_IX      3
#define STRING_IX   4
#define DATA_IX     5
#define DATE_IX     6
#define REAL_IX     7
#define INTEGER_IX  8
#define TRUE_IX     9
#define FALSE_IX    10
#define DOCTYPE_IX  11
#define CDSECT_IX   12

#define PLIST_TAG_LENGTH	5
#define ARRAY_TAG_LENGTH	5
#define DICT_TAG_LENGTH		4
#define KEY_TAG_LENGTH		3
#define STRING_TAG_LENGTH	6
#define DATA_TAG_LENGTH		4
#define DATE_TAG_LENGTH		4
#define REAL_TAG_LENGTH		4
#define INTEGER_TAG_LENGTH	7
#define TRUE_TAG_LENGTH		4
#define FALSE_TAG_LENGTH	5
#define DOCTYPE_TAG_LENGTH	7
#define CDSECT_TAG_LENGTH	9

#if DEPLOYMENT_TARGET_MACOSX
// Set for some exceptional circumstances, like running out of memory
extern char * __crashreporter_info__;

#define out_of_memory_warning() \
    do { \
        __crashreporter_info__ = "CFPropertyList ran out of memory while attempting to allocate temporary storage."; \
    } while (0)
#else
#define out_of_memory_warning() do {} while (0)
#endif

#if !defined(new_cftype_array)
#define new_cftype_array(N, C) \
    size_t N ## _count__ = (C); \
    if (N ## _count__ > LONG_MAX / sizeof(CFTypeRef)) { \
        out_of_memory_warning(); \
        HALT; \
    } \
    Boolean N ## _is_stack__ = (N ## _count__ <= 256); \
    if (N ## _count__ == 0) N ## _count__ = 1; \
    STACK_BUFFER_DECL(CFTypeRef, N ## _buffer__, N ## _is_stack__ ? N ## _count__ : 1); \
    if (N ## _is_stack__) memset(N ## _buffer__, 0, N ## _count__ * sizeof(CFTypeRef)); \
    CFTypeRef * N = N ## _is_stack__ ? N ## _buffer__ : (CFTypeRef *)CFAllocatorAllocate(kCFAllocatorSystemDefaultGCRefZero, (N ## _count__) * sizeof(CFTypeRef), __kCFAllocatorGCScannedMemory); \
    if (! N) { \
        out_of_memory_warning(); \
        HALT; \
    } \
    do {} while (0)
#endif

#if !defined(free_cftype_array)
#define free_cftype_array(N) \
    if (! N ## _is_stack__) { \
        if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFAllocatorDeallocate(kCFAllocatorSystemDefault, N); \
    } \
    do {} while (0)
#endif

// Used to reference an old-style plist parser error inside of a more general XML error
#define CFPropertyListOldStyleParserErrorKey     CFSTR("kCFPropertyListOldStyleParsingError")

// don't allow _CFKeyedArchiverUID here
#define __CFAssertIsPList(cf) CFAssert2(CFGetTypeID(cf) == CFStringGetTypeID() || CFGetTypeID(cf) == CFArrayGetTypeID() || CFGetTypeID(cf) == CFBooleanGetTypeID() || CFGetTypeID(cf) == CFNumberGetTypeID() || CFGetTypeID(cf) == CFDictionaryGetTypeID() || CFGetTypeID(cf) == CFDateGetTypeID() || CFGetTypeID(cf) == CFDataGetTypeID(), __kCFLogAssertion, "%s(): %p not of a property list type", __PRETTY_FUNCTION__, cf);

static bool __CFPropertyListIsValidAux(CFPropertyListRef plist, bool recursive, CFMutableSetRef set, CFPropertyListFormat format, CFStringRef *error);

static CFTypeID stringtype = -1, datatype = -1, numbertype = -1, datetype = -1;
static CFTypeID booltype = -1, nulltype = -1, dicttype = -1, arraytype = -1, settype = -1;

static void initStatics() {
    if ((CFTypeID)-1 == stringtype) {
        stringtype = CFStringGetTypeID();
    }
    if ((CFTypeID)-1 == datatype) {
        datatype = CFDataGetTypeID();
    }
    if ((CFTypeID)-1 == numbertype) {
        numbertype = CFNumberGetTypeID();
    }
    if ((CFTypeID)-1 == booltype) {
        booltype = CFBooleanGetTypeID();
    }
    if ((CFTypeID)-1 == datetype) {
        datetype = CFDateGetTypeID();
    }
    if ((CFTypeID)-1 == dicttype) {
        dicttype = CFDictionaryGetTypeID();
    }
    if ((CFTypeID)-1 == arraytype) {
        arraytype = CFArrayGetTypeID();
    }
    if ((CFTypeID)-1 == settype) {
        settype = CFSetGetTypeID();
    }
    if ((CFTypeID)-1 == nulltype) {
        nulltype = CFNullGetTypeID();
    }
}

__private_extern__ CFErrorRef __CFPropertyListCreateError(CFIndex code, CFStringRef debugString, ...) {    
    va_list argList;        
    CFErrorRef error = NULL;
    
    if (debugString != NULL) {
        CFStringRef debugMessage = NULL;
        va_start(argList, debugString);
        debugMessage = CFStringCreateWithFormatAndArguments(kCFAllocatorSystemDefault, NULL, debugString, argList);
        va_end(argList);
    
        CFMutableDictionaryRef userInfo = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); 
        CFDictionarySetValue(userInfo, kCFErrorDebugDescriptionKey, debugMessage);
                
        error = CFErrorCreate(kCFAllocatorSystemDefault, kCFErrorDomainCocoa, code, userInfo);
        
        CFRelease(debugMessage);
        CFRelease(userInfo);
    } else {
        error = CFErrorCreate(kCFAllocatorSystemDefault, kCFErrorDomainCocoa, code, NULL);
    }
    
    return error;
}

CFStringRef __CFPropertyListCopyErrorDebugDescription(CFErrorRef error) {
    CFStringRef result = NULL;
    if (error) {
        CFDictionaryRef userInfo = CFErrorCopyUserInfo(error);
        result = CFStringCreateCopy(kCFAllocatorSystemDefault, (CFStringRef)CFDictionaryGetValue(userInfo, kCFErrorDebugDescriptionKey));
        CFRelease(userInfo);
    }
    return result;
}


struct context {
    bool answer;
    CFMutableSetRef set;
    CFPropertyListFormat format;
    CFStringRef *error;
};

static void __CFPropertyListIsArrayPlistAux(const void *value, void *context) {
    struct context *ctx = (struct context *)context;
    if (!ctx->answer) return;
    if (!value && !*(ctx->error)) {
	*(ctx->error) = (CFStringRef)CFRetain(CFSTR("property list arrays cannot contain NULL"));
    }
    ctx->answer = value && __CFPropertyListIsValidAux(value, true, ctx->set, ctx->format, ctx->error);
}

static void __CFPropertyListIsDictPlistAux(const void *key, const void *value, void *context) {
    struct context *ctx = (struct context *)context;
    if (!ctx->answer) return;
    if (!key && !*(ctx->error)) *(ctx->error) = (CFStringRef)CFRetain(CFSTR("property list dictionaries cannot contain NULL keys"));
    if (!value && !*(ctx->error)) *(ctx->error) = (CFStringRef)CFRetain(CFSTR("property list dictionaries cannot contain NULL values"));
    if (stringtype != CFGetTypeID(key) && !*(ctx->error)) {
	CFStringRef desc = CFCopyTypeIDDescription(CFGetTypeID(key));
	*(ctx->error) = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("property list dictionaries may only have keys which are CFStrings, not '%@'"), desc);
	CFRelease(desc);
    }
    ctx->answer = key && value && (stringtype == CFGetTypeID(key)) && __CFPropertyListIsValidAux(value, true, ctx->set, ctx->format, ctx->error);
}

static bool __CFPropertyListIsValidAux(CFPropertyListRef plist, bool recursive, CFMutableSetRef set, CFPropertyListFormat format, CFStringRef *error) {
    CFTypeID type;
    if (!plist) {
	*error = (CFStringRef)CFRetain(CFSTR("property lists cannot contain NULL"));
    	return false;
    }
    type = CFGetTypeID(plist);    
    if (stringtype == type) return true;
    if (datatype == type) return true;
    if (kCFPropertyListOpenStepFormat != format) {
	if (booltype == type) return true;
	if (numbertype == type) return true;
	if (datetype == type) return true;        
	if (_CFKeyedArchiverUIDGetTypeID() == type) return true;
    }
    if (!recursive && arraytype == type) return true;
    if (!recursive && dicttype == type) return true;
    // at any one invocation of this function, set should contain the objects in the "path" down to this object
    if (CFSetContainsValue(set, plist)) {
	*error = (CFStringRef)CFRetain(CFSTR("property lists cannot contain recursive container references"));
	return false;
    }
    if (arraytype == type) {
	struct context ctx = {true, set, format, error}; 
	CFSetAddValue(set, plist);
	CFArrayApplyFunction((CFArrayRef)plist, CFRangeMake(0, CFArrayGetCount((CFArrayRef)plist)), __CFPropertyListIsArrayPlistAux, &ctx);
	CFSetRemoveValue(set, plist);
	return ctx.answer;
    }
    if (dicttype == type) {
	struct context ctx = {true, set, format, error}; 
	CFSetAddValue(set, plist);
	CFDictionaryApplyFunction((CFDictionaryRef)plist, __CFPropertyListIsDictPlistAux, &ctx);
	CFSetRemoveValue(set, plist);
	return ctx.answer;
    }

    CFStringRef desc = CFCopyTypeIDDescription(type);
    *error = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("property lists cannot contain objects of type '%@'"), desc);
    CFRelease(desc);

    return false;
}

Boolean CFPropertyListIsValid(CFPropertyListRef plist, CFPropertyListFormat format) {
    initStatics();
    CFAssert1(plist != NULL, __kCFLogAssertion, "%s(): NULL is not a property list", __PRETTY_FUNCTION__);
    CFMutableSetRef set = CFSetCreateMutable(kCFAllocatorSystemDefaultGCRefZero, 0, NULL);
    CFStringRef error = NULL;
    bool result = __CFPropertyListIsValidAux(plist, true, set, format, &error);
    if (error) {
#if defined(DEBUG)
	CFLog(kCFLogLevelWarning, CFSTR("CFPropertyListIsValid(): %@"), error);
#endif
	CFRelease(error);
    }
    if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(set);
    return result;
}

static Boolean _CFPropertyListIsValidWithErrorString(CFPropertyListRef plist, CFPropertyListFormat format, CFStringRef *error) {
    initStatics();
    CFAssert1(plist != NULL, __kCFLogAssertion, "%s(): NULL is not a property list", __PRETTY_FUNCTION__);
    CFMutableSetRef set = CFSetCreateMutable(kCFAllocatorSystemDefaultGCRefZero, 0, NULL);
    bool result = __CFPropertyListIsValidAux(plist, true, set, format, error);
    if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(set);
    return result;
}

static const UniChar CFXMLPlistTags[13][10]= {
{'p', 'l', 'i', 's', 't',   '\0', '\0', '\0', '\0', '\0'},
{'a', 'r', 'r', 'a', 'y',   '\0', '\0', '\0', '\0', '\0'},
{'d', 'i', 'c', 't',  '\0', '\0', '\0', '\0', '\0', '\0'},
{'k', 'e', 'y', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
{'s', 't', 'r', 'i', 'n', 'g',    '\0', '\0', '\0', '\0'},
{'d', 'a', 't', 'a',  '\0', '\0', '\0', '\0', '\0', '\0'},
{'d', 'a', 't', 'e',  '\0', '\0', '\0', '\0', '\0', '\0'},
{'r', 'e', 'a', 'l',  '\0', '\0', '\0', '\0', '\0', '\0'},
{'i', 'n', 't', 'e', 'g', 'e', 'r',     '\0', '\0', '\0'},
{'t', 'r', 'u', 'e',  '\0', '\0', '\0', '\0', '\0', '\0'},
{'f', 'a', 'l', 's', 'e',   '\0', '\0', '\0', '\0', '\0'},
{'D', 'O', 'C', 'T', 'Y', 'P', 'E',     '\0', '\0', '\0'},
{'<', '!', '[', 'C', 'D', 'A', 'T', 'A', '[',       '\0'}
};

typedef struct {
    const UniChar *begin; // first character of the XML to be parsed
    const UniChar *curr;  // current parse location
    const UniChar *end;   // the first character _after_ the end of the XML
    CFErrorRef error;
    CFAllocatorRef allocator;
    UInt32 mutabilityOption;
    CFMutableSetRef stringSet;  // set of all strings involved in this parse; allows us to share non-mutable strings in the returned plist
    Boolean allowNewTypes; // Whether to allow the new types supported by XML property lists, but not by the old, OPENSTEP ASCII property lists (CFNumber, CFBoolean, CFDate)
    char _padding[3];
} _CFXMLPlistParseInfo;

static CFTypeRef parseOldStylePropertyListOrStringsFile(_CFXMLPlistParseInfo *pInfo);

CF_INLINE void __CFPListRelease(CFTypeRef cf, _CFXMLPlistParseInfo *pInfo) {
    if (cf && !_CFAllocatorIsGCRefZero(pInfo->allocator)) CFRelease(cf);
}


// The following set of _plist... functions append various things to a mutable data which is in UTF8 encoding. These are pretty general. Assumption is call characters and CFStrings can be converted to UTF8 and appeneded.

// Null-terminated, ASCII or UTF8 string
//
static void _plistAppendUTF8CString(CFMutableDataRef mData, const char *cString) {
    CFDataAppendBytes (mData, (const UInt8 *)cString, strlen(cString));
}

// UniChars
//
static void _plistAppendCharacters(CFMutableDataRef mData, const UniChar *chars, CFIndex length) {
    CFIndex curLoc = 0;

    do {	// Flush out ASCII chars, BUFLEN at a time
        #define BUFLEN 400
	UInt8 buf[BUFLEN], *bufPtr = buf;
	CFIndex cnt = 0;
        while (cnt < length && (cnt - curLoc < BUFLEN) && (chars[cnt] < 128)) *bufPtr++ = (UInt8)(chars[cnt++]);
        if (cnt > curLoc) {	// Flush any ASCII bytes
            CFDataAppendBytes(mData, buf, cnt - curLoc);
            curLoc = cnt;
        }
    } while (curLoc < length && (chars[curLoc] < 128));	// We will exit out of here when we run out of chars or hit a non-ASCII char

    if (curLoc < length) {	// Now deal with non-ASCII chars
        CFDataRef data = NULL;
        CFStringRef str = NULL;
        if ((str = CFStringCreateWithCharactersNoCopy(kCFAllocatorSystemDefault, chars + curLoc, length - curLoc, kCFAllocatorNull))) {
            if ((data = CFStringCreateExternalRepresentation(kCFAllocatorSystemDefault, str, kCFStringEncodingUTF8, 0))) {
                CFDataAppendBytes (mData, CFDataGetBytePtr(data), CFDataGetLength(data));
                CFRelease(data);
            }
            CFRelease(str);
        }
        CFAssert1(str && data, __kCFLogAssertion, "%s(): Error writing plist", __PRETTY_FUNCTION__); 
    }
}

// Append CFString
//
static void _plistAppendString(CFMutableDataRef mData, CFStringRef str) {
    const UniChar *chars;
    const char *cStr;
    CFDataRef data;
    if ((chars = CFStringGetCharactersPtr(str))) {
        _plistAppendCharacters(mData, chars, CFStringGetLength(str));
    } else if ((cStr = CFStringGetCStringPtr(str, kCFStringEncodingASCII)) || (cStr = CFStringGetCStringPtr(str, kCFStringEncodingUTF8))) {
        _plistAppendUTF8CString(mData, cStr);
    } else if ((data = CFStringCreateExternalRepresentation(kCFAllocatorSystemDefault, str, kCFStringEncodingUTF8, 0))) {
        CFDataAppendBytes (mData, CFDataGetBytePtr(data), CFDataGetLength(data));
        CFRelease(data);
    } else {
	CFAssert1(TRUE, __kCFLogAssertion, "%s(): Error in plist writing", __PRETTY_FUNCTION__);
    }
}


// Append CFString-style format + arguments
//
static void _plistAppendFormat(CFMutableDataRef mData, CFStringRef format, ...) {
    CFStringRef fStr; 
    va_list argList;

    va_start(argList, format);
    fStr = CFStringCreateWithFormatAndArguments(kCFAllocatorSystemDefault, NULL, format, argList);
    va_end(argList);

    CFAssert1(fStr, __kCFLogAssertion, "%s(): Error writing plist", __PRETTY_FUNCTION__);
    _plistAppendString(mData, fStr);
    CFRelease(fStr);
}



static void _appendIndents(CFIndex numIndents, CFMutableDataRef str) {
#define NUMTABS 4
    static const UniChar tabs[NUMTABS] = {'\t','\t','\t','\t'};
    for (; numIndents > 0; numIndents -= NUMTABS) _plistAppendCharacters(str, tabs, (numIndents >= NUMTABS) ? NUMTABS : numIndents);
}

/* Append the escaped version of origStr to mStr.
*/
static void _appendEscapedString(CFStringRef origStr, CFMutableDataRef mStr) {
#define BUFSIZE 64
    CFIndex i, length = CFStringGetLength(origStr);
    CFIndex bufCnt = 0;
    UniChar buf[BUFSIZE];
    CFStringInlineBuffer inlineBuffer;

    CFStringInitInlineBuffer(origStr, &inlineBuffer, CFRangeMake(0, length));

    for (i = 0; i < length; i ++) {
	UniChar ch = __CFStringGetCharacterFromInlineBufferQuick(&inlineBuffer, i);
	if (CFStringIsSurrogateHighCharacter(ch) && (bufCnt + 2 >= BUFSIZE)) {
	    // flush the buffer first so we have room for a low/high pair and do not split them
	    _plistAppendCharacters(mStr, buf, bufCnt);
	    bufCnt = 0;
	}
	
        switch(ch) {
            case '<':
		if (bufCnt) _plistAppendCharacters(mStr, buf, bufCnt);
		bufCnt = 0;
	  	_plistAppendUTF8CString(mStr, "&lt;");
                break;
            case '>':
		if (bufCnt) _plistAppendCharacters(mStr, buf, bufCnt);
		bufCnt = 0;
	  	_plistAppendUTF8CString(mStr, "&gt;");
                break;
            case '&':
		if (bufCnt) _plistAppendCharacters(mStr, buf, bufCnt);
		bufCnt = 0;
	  	_plistAppendUTF8CString(mStr, "&amp;");
                break;
            default:
		buf[bufCnt++] = ch;
		if (bufCnt == BUFSIZE) {
		    _plistAppendCharacters(mStr, buf, bufCnt);
		    bufCnt = 0;
		}
		break;
        }
    }
    if (bufCnt) _plistAppendCharacters(mStr, buf, bufCnt);
}



/* Base-64 encoding/decoding */

/* The base-64 encoding packs three 8-bit bytes into four 7-bit ASCII
 * characters.  If the number of bytes in the original data isn't divisable
 * by three, "=" characters are used to pad the encoded data.  The complete
 * set of characters used in base-64 are:
 *
 *      'A'..'Z' => 00..25
 *      'a'..'z' => 26..51
 *      '0'..'9' => 52..61
 *      '+'      => 62
 *      '/'      => 63
 *      '='      => pad
 */

// Write the inputData to the mData using Base 64 encoding

static void _XMLPlistAppendDataUsingBase64(CFMutableDataRef mData, CFDataRef inputData, CFIndex indent) {
    static const char __CFPLDataEncodeTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    #define MAXLINELEN 76
    char buf[MAXLINELEN + 4 + 2];	// For the slop and carriage return and terminating NULL
    
    const uint8_t *bytes = CFDataGetBytePtr(inputData);
    CFIndex length = CFDataGetLength(inputData);
    CFIndex i, pos;
    const uint8_t *p;

    if (indent > 8) indent = 8; // refuse to indent more than 64 characters

    pos = 0;		// position within buf

    for (i = 0, p = bytes; i < length; i++, p++) {
        /* 3 bytes are encoded as 4 */
        switch (i % 3) {
            case 0:
                buf[pos++] = __CFPLDataEncodeTable [ ((p[0] >> 2) & 0x3f)];
                break;
            case 1:
                buf[pos++] = __CFPLDataEncodeTable [ ((((p[-1] << 8) | p[0]) >> 4) & 0x3f)];
                break;
            case 2:
                buf[pos++] = __CFPLDataEncodeTable [ ((((p[-1] << 8) | p[0]) >> 6) & 0x3f)];
                buf[pos++] = __CFPLDataEncodeTable [ (p[0] & 0x3f)];
                break;
        }
        /* Flush the line out every 76 (or fewer) chars --- indents count against the line length*/
        if (pos >= MAXLINELEN - 8 * indent) {
            buf[pos++] = '\n';
            buf[pos++] = 0;
            _appendIndents(indent, mData);
            _plistAppendUTF8CString(mData, buf);
            pos = 0;
        }
    }
        
    switch (i % 3) {
	case 0:
            break;
	case 1:
            buf[pos++] = __CFPLDataEncodeTable [ ((p[-1] << 4) & 0x30)];
            buf[pos++] = '=';
            buf[pos++] = '=';
            break;
	case 2:
            buf[pos++] =  __CFPLDataEncodeTable [ ((p[-1] << 2) & 0x3c)];
            buf[pos++] = '=';
            break;
    }
    
    if (pos > 0) {
        buf[pos++] = '\n';
        buf[pos++] = 0;
        _appendIndents(indent, mData);
        _plistAppendUTF8CString(mData, buf);
    }
}

extern CFStringRef __CFNumberCopyFormattingDescriptionAsFloat64(CFTypeRef cf);

static void _CFAppendXML0(CFTypeRef object, UInt32 indentation, CFMutableDataRef xmlString) {
    UInt32 typeID = CFGetTypeID(object);
    _appendIndents(indentation, xmlString);
    if (typeID == stringtype) {
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[STRING_IX], STRING_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">");
	_appendEscapedString((CFStringRef)object, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[STRING_IX], STRING_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == arraytype) {
        UInt32 i, count = CFArrayGetCount((CFArrayRef)object);
        if (count == 0) {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, "/>\n");
            return;
        }
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
        for (i = 0; i < count; i ++) {
            _CFAppendXML0(CFArrayGetValueAtIndex((CFArrayRef)object, i), indentation+1, xmlString);
        }
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == dicttype) {
        UInt32 i, count = CFDictionaryGetCount((CFDictionaryRef)object);
        CFMutableArrayRef keyArray;
        if (count == 0) {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, "/>\n");
            return;
        }
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
        new_cftype_array(keys, count);
        CFDictionaryGetKeysAndValues((CFDictionaryRef)object, keys, NULL);
        keyArray = CFArrayCreateMutable(kCFAllocatorSystemDefault, count, &kCFTypeArrayCallBacks);
        CFArrayReplaceValues(keyArray, CFRangeMake(0, 0), keys, count);
        CFArraySortValues(keyArray, CFRangeMake(0, count), (CFComparatorFunction)CFStringCompare, NULL);
        CFArrayGetValues(keyArray, CFRangeMake(0, count), keys);
        CFRelease(keyArray);
        for (i = 0; i < count; i ++) {
            CFTypeRef key = keys[i];
            _appendIndents(indentation+1, xmlString);
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[KEY_IX], KEY_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">");
	    _appendEscapedString((CFStringRef)key, xmlString);
            _plistAppendUTF8CString(xmlString, "</");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[KEY_IX], KEY_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">\n");
            _CFAppendXML0(CFDictionaryGetValue((CFDictionaryRef)object, key), indentation+1, xmlString);
        }
        free_cftype_array(keys);
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == datatype) {
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
        _XMLPlistAppendDataUsingBase64(xmlString, (CFDataRef)object, indentation);       
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == datetype) {
        // YYYY '-' MM '-' DD 'T' hh ':' mm ':' ss 'Z'
	int32_t y = 0, M = 0, d = 0, H = 0, m = 0, s = 0;
#if 1
        CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(CFDateGetAbsoluteTime((CFDateRef)object), NULL);
	y = date.year;
	M = date.month;
	d = date.day;
	H = date.hour;
	m = date.minute;
	s = (int32_t)date.second;
#else
	CFCalendarRef calendar = CFCalendarCreateWithIdentifier(kCFAllocatorSystemDefault, kCFCalendarIdentifierGregorian);
	CFTimeZoneRef tz = CFTimeZoneCreateWithName(kCFAllocatorSystemDefault, CFSTR("GMT"), true);
	CFCalendarSetTimeZone(calendar, tz);
	CFCalendarDecomposeAbsoluteTime(calendar, CFDateGetAbsoluteTime((CFDateRef)object), (const uint8_t *)"yMdHms", &y, &M, &d, &H, &m, &s);
	CFRelease(calendar);
	CFRelease(tz);
#endif
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">");
        _plistAppendFormat(xmlString, CFSTR("%04d-%02d-%02dT%02d:%02d:%02dZ"), y, M, d, H, m, s);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == numbertype) {
        if (CFNumberIsFloatType((CFNumberRef)object)) {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">");
                CFStringRef s = __CFNumberCopyFormattingDescriptionAsFloat64(object);
                _plistAppendString(xmlString, s);
                CFRelease(s);
            _plistAppendUTF8CString(xmlString, "</");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">\n");
        } else {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[INTEGER_IX], INTEGER_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">");

            _plistAppendFormat(xmlString, CFSTR("%@"), object);

            _plistAppendUTF8CString(xmlString, "</");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[INTEGER_IX], INTEGER_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">\n");
        }
    } else if (typeID == booltype) {
        if (CFBooleanGetValue((CFBooleanRef)object)) {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[TRUE_IX], TRUE_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, "/>\n");
        } else {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[FALSE_IX], FALSE_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, "/>\n");
        }
    }
}

static void _CFGenerateXMLPropertyListToData(CFMutableDataRef xml, CFTypeRef propertyList) {
    _plistAppendUTF8CString(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE ");
    _plistAppendCharacters(xml, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH);
    _plistAppendUTF8CString(xml, " PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<");
    _plistAppendCharacters(xml, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH);
    _plistAppendUTF8CString(xml, " version=\"1.0\">\n");

    _CFAppendXML0(propertyList, 0, xml);

    _plistAppendUTF8CString(xml, "</");
    _plistAppendCharacters(xml, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH);
    _plistAppendUTF8CString(xml, ">\n");
}

CFDataRef _CFPropertyListCreateXMLData(CFAllocatorRef allocator, CFPropertyListRef propertyList, Boolean checkValidPlist) {
    initStatics();
    CFMutableDataRef xml;
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL property list", __PRETTY_FUNCTION__);
    if (checkValidPlist && !CFPropertyListIsValid(propertyList, kCFPropertyListXMLFormat_v1_0)) {
        __CFAssertIsPList(propertyList);
        return NULL;
    }
    xml = CFDataCreateMutable(allocator, 0);
    _CFGenerateXMLPropertyListToData(xml, propertyList);
    return xml;
}

CFDataRef CFPropertyListCreateXMLData(CFAllocatorRef allocator, CFPropertyListRef propertyList) {
    return _CFPropertyListCreateXMLData(allocator, propertyList, true);
}

CF_EXPORT CFDataRef _CFPropertyListCreateXMLDataWithExtras(CFAllocatorRef allocator, CFPropertyListRef propertyList) {
    return _CFPropertyListCreateXMLData(allocator, propertyList, false);
}

// ========================================================================

//
// ------------------------- Reading plists ------------------
// 

static void skipInlineDTD(_CFXMLPlistParseInfo *pInfo);
static CFTypeRef parseXMLElement(_CFXMLPlistParseInfo *pInfo, Boolean *isKey);

// warning: doesn't have a good idea of Unicode line separators
static UInt32 lineNumber(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *p = pInfo->begin;
    UInt32 count = 1;
    while (p < pInfo->curr) {
        if (*p == '\r') {
            count ++;
            if (*(p + 1) == '\n')
                p ++;
        } else if (*p == '\n') {
            count ++;
        }
        p ++;
    }
    return count;
}

// warning: doesn't have a good idea of Unicode white space
CF_INLINE void skipWhitespace(_CFXMLPlistParseInfo *pInfo) {
    while (pInfo->curr < pInfo->end) {
        switch (*(pInfo->curr)) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                pInfo->curr ++;
                continue;
            default:
                return;
        }
    }
}

/* All of these advance to the end of the given construct and return a pointer to the first character beyond the construct.  If the construct doesn't parse properly, NULL is returned. */

// pInfo should be just past "<!--"
static void skipXMLComment(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *p = pInfo->curr;
    const UniChar *end = pInfo->end - 3; // Need at least 3 characters to compare against
    while (p < end) {
        if (*p == '-' && *(p+1) == '-' && *(p+2) == '>') {
            pInfo->curr = p+3;
            return;
        }
        p ++; 
    }
    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unterminated comment started on line %d"), lineNumber(pInfo));
}

// stringToMatch and buf must both be of at least len
static Boolean matchString(const UniChar *buf, const UniChar *stringToMatch, UInt32 len) {
    switch (len) {
	case 10: if (buf[9] != stringToMatch[9]) return false;
	case 9: if (buf[8] != stringToMatch[8]) return false;
	case 8: if (buf[7] != stringToMatch[7]) return false;
	case 7: if (buf[6] != stringToMatch[6]) return false;
	case 6: if (buf[5] != stringToMatch[5]) return false;
	case 5: if (buf[4] != stringToMatch[4]) return false;
	case 4: if (buf[3] != stringToMatch[3]) return false;
	case 3: if (buf[2] != stringToMatch[2]) return false;
	case 2: if (buf[1] != stringToMatch[1]) return false;
	case 1: if (buf[0] != stringToMatch[0]) return false;
	case 0: return true;
    }
    return false; // internal error
}

// pInfo should be set to the first character after "<?"
static void skipXMLProcessingInstruction(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *begin = pInfo->curr, *end = pInfo->end - 2; // Looking for "?>" so we need at least 2 characters
    while (pInfo->curr < end) {
        if (*(pInfo->curr) == '?' && *(pInfo->curr+1) == '>') {
            pInfo->curr += 2;
            return;
        }
        pInfo->curr ++; 
    }
    pInfo->curr = begin;
    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing the processing instruction begun on line %d"), lineNumber(pInfo));
}

// first character should be immediately after the "<!"
static void skipDTD(_CFXMLPlistParseInfo *pInfo) {
    // First pass "DOCTYPE"
    if (pInfo->end - pInfo->curr < DOCTYPE_TAG_LENGTH || !matchString(pInfo->curr, CFXMLPlistTags[DOCTYPE_IX], DOCTYPE_TAG_LENGTH)) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Malformed DTD on line %d"), lineNumber(pInfo));
        return;
    }
    pInfo->curr += DOCTYPE_TAG_LENGTH;
    skipWhitespace(pInfo);

    // Look for either the beginning of a complex DTD or the end of the DOCTYPE structure
    while (pInfo->curr < pInfo->end) {
        UniChar ch = *(pInfo->curr);
        if (ch == '[') break; // inline DTD
        if (ch == '>') {  // End of the DTD
            pInfo->curr ++;
            return;
        }
        pInfo->curr ++;
    }
    if (pInfo->curr == pInfo->end) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing DTD"));
        return;
    }

    // *Sigh* Must parse in-line DTD
    skipInlineDTD(pInfo);
    if (pInfo->error)  return;
    skipWhitespace(pInfo);
    if (pInfo->error) return;
    if (pInfo->curr < pInfo->end) {
        if (*(pInfo->curr) == '>') {
            pInfo->curr ++;
        } else {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c on line %d while parsing DTD"), *(pInfo->curr), lineNumber(pInfo));
        }
    } else {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing DTD"));
    }
}

static void skipPERef(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *p = pInfo->curr;
    while (p < pInfo->end) {
        if (*p == ';') {
            pInfo->curr = p+1;
            return;
        }
        p ++;
    }
    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing percent-escape sequence begun on line %d"), lineNumber(pInfo));
}

// First character should be just past '['
static void skipInlineDTD(_CFXMLPlistParseInfo *pInfo) {
    while (!pInfo->error && pInfo->curr < pInfo->end) {
        UniChar ch;
        skipWhitespace(pInfo);
        ch = *pInfo->curr;
        if (ch == '%') {
            pInfo->curr ++;
            skipPERef(pInfo);
        } else if (ch == '<') {
            pInfo->curr ++;
            if (pInfo->curr >= pInfo->end) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing inline DTD"));
                return;
            }
            ch = *(pInfo->curr);
            if (ch == '?') {
                pInfo->curr ++;
                skipXMLProcessingInstruction(pInfo);
            } else if (ch == '!') {
                if (pInfo->curr + 2 < pInfo->end && (*(pInfo->curr+1) == '-' && *(pInfo->curr+2) == '-')) {
                    pInfo->curr += 3;
                    skipXMLComment(pInfo);
                } else {
                    // Skip the myriad of DTD declarations of the form "<!string" ... ">"
                    pInfo->curr ++; // Past both '<' and '!'
                    while (pInfo->curr < pInfo->end) {
                        if (*(pInfo->curr) == '>') break;
                        pInfo->curr ++;
                    }
                    if (*(pInfo->curr) != '>') {
                        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing inline DTD"));
                        return;
                    }
                    pInfo->curr ++;
                }
            } else {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c on line %d while parsing inline DTD"), ch, lineNumber(pInfo));
                return;
            }
        } else if (ch == ']') {
            pInfo->curr ++;
            return;
        } else {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c on line %d while parsing inline DTD"), ch, lineNumber(pInfo));
            return;
        }
    }
    if (!pInfo->error) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF while parsing inline DTD"));
    }
}

/* A bit wasteful to do everything with unichars (since we know all the characters we're going to see are 7-bit ASCII), but since our data is coming from or going to a CFString, this prevents the extra cost of converting formats. */

static const signed char __CFPLDataDecodeTable[128] = {
    /* 000 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* 010 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* 020 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* 030 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* ' ' */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* '(' */ -1, -1, -1, 62, -1, -1, -1, 63,
    /* '0' */ 52, 53, 54, 55, 56, 57, 58, 59,
    /* '8' */ 60, 61, -1, -1, -1,  0, -1, -1,
    /* '@' */ -1,  0,  1,  2,  3,  4,  5,  6,
    /* 'H' */  7,  8,  9, 10, 11, 12, 13, 14,
    /* 'P' */ 15, 16, 17, 18, 19, 20, 21, 22,
    /* 'X' */ 23, 24, 25, -1, -1, -1, -1, -1,
    /* '`' */ -1, 26, 27, 28, 29, 30, 31, 32,
    /* 'h' */ 33, 34, 35, 36, 37, 38, 39, 40,
    /* 'p' */ 41, 42, 43, 44, 45, 46, 47, 48,
    /* 'x' */ 49, 50, 51, -1, -1, -1, -1, -1
};

static CFDataRef __CFPLDataDecode(_CFXMLPlistParseInfo *pInfo, Boolean isMutable) {
    int tmpbufpos = 0;
    int tmpbuflen = 256;
    uint8_t *tmpbuf;
    int numeq = 0;
    int acc = 0;
    int cntr = 0;

    tmpbuf = (uint8_t *)CFAllocatorAllocate(pInfo->allocator, tmpbuflen, 0);
    for (; pInfo->curr < pInfo->end; pInfo->curr++) {
        UniChar c = *(pInfo->curr);
        if (c == '<') {
            break;
	}
        if ('=' == c) {
            numeq++;
        } else if (!isspace(c)) {
            numeq = 0;
        }
        if (__CFPLDataDecodeTable[c] < 0)
            continue;
        cntr++;
        acc <<= 6;
        acc += __CFPLDataDecodeTable[c];
        if (0 == (cntr & 0x3)) {
            if (tmpbuflen <= tmpbufpos + 2) {
		if (tmpbuflen < 256 * 1024) {
		    tmpbuflen *= 4;
		} else if (tmpbuflen < 16 * 1024 * 1024) {
		    tmpbuflen *= 2;
		} else {
		    // once in this stage, this will be really slow
		    // and really potentially fragment memory
		    tmpbuflen += 256 * 1024;
		}
                tmpbuf = (uint8_t *)CFAllocatorReallocate(pInfo->allocator, tmpbuf, tmpbuflen, 0);
		if (!tmpbuf) HALT;
            }
            tmpbuf[tmpbufpos++] = (acc >> 16) & 0xff;
            if (numeq < 2)
                tmpbuf[tmpbufpos++] = (acc >> 8) & 0xff;
            if (numeq < 1)
                tmpbuf[tmpbufpos++] = acc & 0xff;
        }
    }
    if (isMutable) {
        CFMutableDataRef result = CFDataCreateMutable(pInfo->allocator, 0);
        CFDataAppendBytes(result, tmpbuf, tmpbufpos);
	CFAllocatorDeallocate(pInfo->allocator, tmpbuf);
        return result;
    } else {
        return CFDataCreateWithBytesNoCopy(pInfo->allocator, tmpbuf, tmpbufpos, pInfo->allocator);
    }
}

// content ::== (element | CharData | Reference | CDSect | PI | Comment)*
// In the context of a plist, CharData, Reference and CDSect are not legal (they all resolve to strings).  Skipping whitespace, then, the next character should be '<'.  From there, we figure out which of the three remaining cases we have (element, PI, or Comment).
static CFTypeRef getContentObject(_CFXMLPlistParseInfo *pInfo, Boolean *isKey) {
    if (isKey) *isKey = false;
    while (!pInfo->error && pInfo->curr < pInfo->end) {
        skipWhitespace(pInfo);
        if (pInfo->curr >= pInfo->end) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
            return NULL;
        }
        if (*(pInfo->curr) != '<') {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c on line %d"), *(pInfo->curr), lineNumber(pInfo));
            return NULL;
        }
        pInfo->curr ++;
        if (pInfo->curr >= pInfo->end) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
            return NULL;
        }
        switch (*(pInfo->curr)) {
            case '?':
                // Processing instruction
                skipXMLProcessingInstruction(pInfo);
                break;
            case '!':
                // Could be a comment
                if (pInfo->curr+2 >= pInfo->end) {
                    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
                    return NULL;
                }
                if (*(pInfo->curr+1) == '-' && *(pInfo->curr+2) == '-') {
                    pInfo->curr += 2;
                    skipXMLComment(pInfo);
                } else {
                    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
                    return NULL;
                }
                break;
            case '/':
                // Whoops!  Looks like we got to the end tag for the element whose content we're parsing
                pInfo->curr --; // Back off to the '<'
                return NULL;
            default:
                // Should be an element
                return parseXMLElement(pInfo, isKey);
        }
    }
    // Do not set the error string here; if it wasn't already set by one of the recursive parsing calls, the caller will quickly detect the failure (b/c pInfo->curr >= pInfo->end) and provide a more useful one of the form "end tag for <blah> not found"
    return NULL;
}

static void _catFromMarkToBuf(const UniChar *mark, const UniChar *buf, CFMutableStringRef *string, _CFXMLPlistParseInfo *pInfo) {
    if (!(*string)) {
        *string = CFStringCreateMutable(pInfo->allocator, 0);
    }
    CFStringAppendCharacters(*string, mark, buf-mark);
}

static void parseCDSect_pl(_CFXMLPlistParseInfo *pInfo, CFMutableStringRef string) {
    const UniChar *end, *begin;
    if (pInfo->end - pInfo->curr < CDSECT_TAG_LENGTH) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
        return;
    }
    if (!matchString(pInfo->curr, CFXMLPlistTags[CDSECT_IX], CDSECT_TAG_LENGTH)) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered improper CDATA opening at line %d"), lineNumber(pInfo));
        return;
    }
    pInfo->curr += CDSECT_TAG_LENGTH;
    begin = pInfo->curr; // Marks the first character of the CDATA content
    end = pInfo->end-2; // So we can safely look 2 characters beyond p
    while (pInfo->curr < end) {
        if (*(pInfo->curr) == ']' && *(pInfo->curr+1) == ']' && *(pInfo->curr+2) == '>') {
           // Found the end!
            CFStringAppendCharacters(string, begin, pInfo->curr-begin);
            pInfo->curr += 3;
            return;
        }
        pInfo->curr ++;
    }
    // Never found the end mark
    pInfo->curr = begin;
    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Could not find end of CDATA started on line %d"), lineNumber(pInfo));
}

// Only legal references are {lt, gt, amp, apos, quote, #ddd, #xAAA}
static void parseEntityReference_pl(_CFXMLPlistParseInfo *pInfo, CFMutableStringRef string) {
    int len;
    UniChar ch;
    pInfo->curr ++; // move past the '&';
    len = pInfo->end - pInfo->curr; // how many characters we can safely scan
    if (len < 1) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
        return;
    }
    switch (*(pInfo->curr)) {
        case 'l':  // "lt"
            if (len >= 3 && *(pInfo->curr+1) == 't' && *(pInfo->curr+2) == ';') {
                ch = '<';
                pInfo->curr += 3;
                break;
            }
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case 'g': // "gt"
            if (len >= 3 && *(pInfo->curr+1) == 't' && *(pInfo->curr+2) == ';') {
                ch = '>';
                pInfo->curr += 3;
                break;
            }
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case 'a': // "apos" or "amp"
            if (len < 4) {   // Not enough characters for either conversion
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
                return;
            }
            if (*(pInfo->curr+1) == 'm') {
                // "amp"
                if (*(pInfo->curr+2) == 'p' && *(pInfo->curr+3) == ';') {
                    ch = '&';
                    pInfo->curr += 4;
                    break;
                }
            } else if (*(pInfo->curr+1) == 'p') {
                // "apos"
                if (len > 4 && *(pInfo->curr+2) == 'o' && *(pInfo->curr+3) == 's' && *(pInfo->curr+4) == ';') {
                    ch = '\'';
                    pInfo->curr += 5;
                    break;
                }
            }
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case 'q':  // "quote"
            if (len >= 5 && *(pInfo->curr+1) == 'u' && *(pInfo->curr+2) == 'o' && *(pInfo->curr+3) == 't' && *(pInfo->curr+4) == ';') {
                ch = '\"';
                pInfo->curr += 5;
                break;
            }
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case '#':
        {
            uint16_t num = 0;
            Boolean isHex = false;
            if ( len < 4) {  // Not enough characters to make it all fit!  Need at least "&#d;"
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
                return;
            }
            pInfo->curr ++;
            if (*(pInfo->curr) == 'x') {
                isHex = true;
                pInfo->curr ++;
            }
            while (pInfo->curr < pInfo->end) {
                ch = *(pInfo->curr);
                pInfo->curr ++;
                if (ch == ';') {
                    CFStringAppendCharacters(string, &num, 1);
                    return;
                }
                if (!isHex) num = num*10;
                else num = num << 4;
                if (ch <= '9' && ch >= '0') {
                    num += (ch - '0');
                } else if (!isHex) {
                    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c at line %d"), ch, lineNumber(pInfo));
                    return;
                } else if (ch >= 'a' && ch <= 'f') {
                    num += 10 + (ch - 'a');
                } else if (ch >= 'A' && ch <= 'F') {
                    num += 10 + (ch - 'A');
                } else {
                    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c at line %d"), ch, lineNumber(pInfo));
                    return;                    
                }
            }
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
            return;
        }
        default:
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
    }
    CFStringAppendCharacters(string, &ch, 1);
}

extern void _CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex len);

static CFStringRef _uniqueStringForString(_CFXMLPlistParseInfo *pInfo, CFStringRef stringToUnique) {
    CFStringRef uniqued = (CFStringRef)CFSetGetValue(pInfo->stringSet, stringToUnique);
    if (!uniqued) {
        uniqued = (CFStringRef)__CFStringCollectionCopy(pInfo->allocator, stringToUnique);
        CFSetAddValue(pInfo->stringSet, uniqued);
        __CFTypeCollectionRelease(pInfo->allocator, uniqued);
    }
    if (uniqued && !_CFAllocatorIsGCRefZero(pInfo->allocator)) CFRetain(uniqued);
    return uniqued;
}

static CFStringRef _uniqueStringForCharacters(_CFXMLPlistParseInfo *pInfo, const UniChar *base, CFIndex length) {
    if (0 == length) return !_CFAllocatorIsGCRefZero(pInfo->allocator) ? (CFStringRef)CFRetain(CFSTR("")) : CFSTR("");
    // This is to avoid having to promote the buffers of all the strings compared against
    // during the set probe; if a Unicode string is passed in, that's what happens.
    CFStringRef stringToUnique = NULL;
    Boolean use_stack = (length < 2048);
    STACK_BUFFER_DECL(uint8_t, buffer, use_stack ? length + 1 : 1);
    uint8_t *ascii = use_stack ? buffer : (uint8_t *)CFAllocatorAllocate(kCFAllocatorSystemDefault, length + 1, 0);
    for (CFIndex idx = 0; idx < length; idx++) {
        UniChar ch = base[idx];
	if (ch < 0x80) {
	    ascii[idx] = (uint8_t)ch;
        } else {
	    stringToUnique = CFStringCreateWithCharacters(pInfo->allocator, base, length);
	    break;
	}
    }
    if (!stringToUnique) {
        ascii[length] = '\0';
        stringToUnique = CFStringCreateWithBytes(pInfo->allocator, ascii, length, kCFStringEncodingASCII, false);
    }
    if (ascii != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, ascii);
    CFStringRef uniqued = (CFStringRef)CFSetGetValue(pInfo->stringSet, stringToUnique);
    if (!uniqued) {
        CFSetAddValue(pInfo->stringSet, stringToUnique);
	uniqued = stringToUnique;
    }
    __CFPListRelease(stringToUnique, pInfo);
    if (uniqued && !_CFAllocatorIsGCRefZero(pInfo->allocator)) CFRetain(uniqued);
    return uniqued;
}

// String could be comprised of characters, CDSects, or references to one of the "well-known" entities ('<', '>', '&', ''', '"')
// returns a retained object in *string.
static CFStringRef getString(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *mark = pInfo->curr; // At any time in the while loop below, the characters between mark and p have not yet been added to *string
    CFMutableStringRef string = NULL;
    while (!pInfo->error && pInfo->curr < pInfo->end) {
        UniChar ch = *(pInfo->curr);
        if (ch == '<') {
	    if (pInfo->curr + 1 >= pInfo->end) break;
            // Could be a CDSect; could be the end of the string
            if (*(pInfo->curr+1) != '!') break; // End of the string
            _catFromMarkToBuf(mark, pInfo->curr, &string, pInfo);
            parseCDSect_pl(pInfo, string);
            mark = pInfo->curr;
        } else if (ch == '&') {
            _catFromMarkToBuf(mark, pInfo->curr, &string, pInfo);
            parseEntityReference_pl(pInfo, string);
            mark = pInfo->curr;
        } else {
            pInfo->curr ++;
        }
    }

    if (pInfo->error) {
        __CFPListRelease(string, pInfo);
        return NULL;
    }
    if (!string) {
        if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
            CFStringRef uniqueString = _uniqueStringForCharacters(pInfo, mark, pInfo->curr-mark);
            return uniqueString;
        } else {
            string = CFStringCreateMutable(pInfo->allocator, 0);
            CFStringAppendCharacters(string, mark, pInfo->curr - mark);
            return string;
        }
    }
    _catFromMarkToBuf(mark, pInfo->curr, &string, pInfo);
    if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
        CFStringRef uniqueString = _uniqueStringForString(pInfo, string);
        __CFPListRelease(string, pInfo);
        return uniqueString;
    }
    return string;
}

static Boolean checkForCloseTag(_CFXMLPlistParseInfo *pInfo, const UniChar *tag, CFIndex tagLen) {
    if (pInfo->end - pInfo->curr < tagLen + 3) {
        if (!pInfo->error) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
        }
        return false;
    }
    if (*(pInfo->curr) != '<' || *(++pInfo->curr) != '/') {
        if (!pInfo->error) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c on line %d"), *(pInfo->curr), lineNumber(pInfo));
        }
        return false;
    }
    pInfo->curr ++;
    if (!matchString(pInfo->curr, tag, tagLen)) {
        CFStringRef str = CFStringCreateWithCharactersNoCopy(kCFAllocatorSystemDefault, tag, tagLen, kCFAllocatorNull);
        if (!pInfo->error) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Close tag on line %d does not match open tag %@"), lineNumber(pInfo), str);
        }
        CFRelease(str);
        return false;
    }
    pInfo->curr += tagLen;
    skipWhitespace(pInfo);
    if (pInfo->curr == pInfo->end) {
        if (!pInfo->error) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
        }
        return false;
    }
    if (*(pInfo->curr) != '>') {
        if (!pInfo->error) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected character %c on line %d"), *(pInfo->curr), lineNumber(pInfo));
        }
        return false;
    }
    pInfo->curr ++;
    return true;
}

// pInfo should be set to the first content character of the <plist>
static CFTypeRef parsePListTag(_CFXMLPlistParseInfo *pInfo) {
    CFTypeRef result, tmp = NULL;
    const UniChar *save;
    result = getContentObject(pInfo, NULL);
    if (!result) {
        if (!pInfo->error) pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty plist tag"));
        return NULL;
    }
    save = pInfo->curr; // Save this in case the next step fails
    tmp = getContentObject(pInfo, NULL);
    if (tmp) {
        // Got an extra object
        __CFPListRelease(tmp, pInfo);
        __CFPListRelease(result, pInfo);
        pInfo->curr = save;
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected element at line %d (plist can only include one object)"), lineNumber(pInfo));
        return NULL;
    }
    if (pInfo->error) {
        // Parse failed catastrophically
        __CFPListRelease(result, pInfo);
        return NULL;
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH)) {
        return result;
    }
    __CFPListRelease(result, pInfo);
    return NULL;
}

static int allowImmutableCollections = -1;

static void checkImmutableCollections(void) {
    allowImmutableCollections = (NULL == __CFgetenv("CFPropertyListAllowImmutableCollections")) ? 0 : 1;
}

static CFTypeRef parseArrayTag(_CFXMLPlistParseInfo *pInfo) {
    CFMutableArrayRef array = CFArrayCreateMutable(pInfo->allocator, 0, &kCFTypeArrayCallBacks);
    CFTypeRef tmp = getContentObject(pInfo, NULL);
    while (tmp) {
        CFArrayAppendValue(array, tmp);
        __CFPListRelease(tmp, pInfo);
        tmp = getContentObject(pInfo, NULL);
    }
    if (pInfo->error) { // getContentObject encountered a parse error
        __CFPListRelease(array, pInfo);
        return NULL;
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH)) {
	if (-1 == allowImmutableCollections) checkImmutableCollections();
	if (1 == allowImmutableCollections) {
	    if (pInfo->mutabilityOption == kCFPropertyListImmutable) {
		CFArrayRef newArray = CFArrayCreateCopy(pInfo->allocator, array);
		__CFPListRelease(array, pInfo);
		array = (CFMutableArrayRef)newArray;
	    }
	}
	return array;
    }
    __CFPListRelease(array, pInfo);
    return NULL;
}

static CFTypeRef parseDictTag(_CFXMLPlistParseInfo *pInfo) {
    CFMutableDictionaryRef dict = NULL;
    CFTypeRef key=NULL, value=NULL;
    Boolean gotKey;
    const UniChar *base = pInfo->curr;
    key = getContentObject(pInfo, &gotKey);
    while (key) {
        if (!gotKey) {
            __CFPListRelease(key, pInfo);
            __CFPListRelease(dict, pInfo);
            pInfo->curr = base;
            if (!pInfo->error) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Found non-key inside <dict> at line %d"), lineNumber(pInfo));
            }
            return NULL;
        }
        value = getContentObject(pInfo, NULL);
        if (!value) {
            __CFPListRelease(key, pInfo);
            __CFPListRelease(dict, pInfo);
            if (!pInfo->error) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Value missing for key inside <dict> at line %d"), lineNumber(pInfo));
            }
            return NULL;
        }
	if (NULL == dict) {
	    dict = CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	    _CFDictionarySetCapacity(dict, 10);
	}
        CFDictionarySetValue(dict, key, value);
        __CFPListRelease(key, pInfo);
        key = NULL;
        __CFPListRelease(value, pInfo);
        value = NULL;
        base = pInfo->curr;
        key = getContentObject(pInfo, &gotKey);
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH)) {
	if (NULL == dict) {
	    if (pInfo->mutabilityOption == kCFPropertyListImmutable) {
		dict = (CFMutableDictionaryRef)CFDictionaryCreate(pInfo->allocator, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	    } else {
		dict = CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	    }
	} else {
	    CFIndex cnt = CFDictionaryGetCount(dict);
	    if (1 == cnt) {
		CFTypeRef val = CFDictionaryGetValue(dict, CFSTR("CF$UID"));
		if (val && CFGetTypeID(val) == numbertype) {
		    CFTypeRef uid;
		    uint32_t v;
		    CFNumberGetValue((CFNumberRef)val, kCFNumberSInt32Type, &v);
		    uid = (CFTypeRef)_CFKeyedArchiverUIDCreate(pInfo->allocator, v);
		    __CFPListRelease(dict, pInfo);
		    return uid;
		}
	    }
	    if (-1 == allowImmutableCollections) checkImmutableCollections();
	    if (1 == allowImmutableCollections) {
		if (pInfo->mutabilityOption == kCFPropertyListImmutable) {
		    CFDictionaryRef newDict = CFDictionaryCreateCopy(pInfo->allocator, dict);
		    __CFPListRelease(dict, pInfo);
		    dict = (CFMutableDictionaryRef)newDict;
		}
	    }
	}
        return dict;
    }
    __CFPListRelease(dict, pInfo);
    return NULL;
}

static CFTypeRef parseDataTag(_CFXMLPlistParseInfo *pInfo) {
    CFDataRef result;
    const UniChar *base = pInfo->curr;
    result = __CFPLDataDecode(pInfo, pInfo->mutabilityOption == kCFPropertyListMutableContainersAndLeaves);
    if (!result) {
        pInfo->curr = base;
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Could not interpret <data> at line %d (should be base64-encoded)"), lineNumber(pInfo));
        return NULL;
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH)) return result;
    __CFPListRelease(result, pInfo);
    return NULL;
}

CF_INLINE Boolean read2DigitNumber(_CFXMLPlistParseInfo *pInfo, int32_t *result) {
    UniChar ch1, ch2;
    if (pInfo->curr + 2 >= pInfo->end) return false;
    ch1 = *pInfo->curr;
    ch2 = *(pInfo->curr + 1);
    pInfo->curr += 2;
    if (!isdigit(ch1) || !isdigit(ch2)) return false;
    *result = (ch1 - '0')*10 + (ch2 - '0');
    return true;
}

// YYYY '-' MM '-' DD 'T' hh ':' mm ':' ss 'Z'
static CFTypeRef parseDateTag(_CFXMLPlistParseInfo *pInfo) {
    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int32_t num = 0;
    Boolean badForm = false;
    Boolean yearIsNegative = false;
    
    if (pInfo->curr < pInfo->end && *pInfo->curr == '-') {
        yearIsNegative = true;
        pInfo->curr++;
    }
    
    while (pInfo->curr < pInfo->end && isdigit(*pInfo->curr)) {
        year = 10*year + (*pInfo->curr) - '0';
        pInfo->curr ++;
    }
    if (pInfo->curr >= pInfo->end || *pInfo->curr != '-') {
        badForm = true;
    } else {
        pInfo->curr ++;
    }

    if (!badForm && read2DigitNumber(pInfo, &month) && pInfo->curr < pInfo->end && *pInfo->curr == '-') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &day) && pInfo->curr < pInfo->end && *pInfo->curr == 'T') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &hour) && pInfo->curr < pInfo->end && *pInfo->curr == ':') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &minute) && pInfo->curr < pInfo->end && *pInfo->curr == ':') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &num) && pInfo->curr < pInfo->end && *pInfo->curr == 'Z') {
        second = num;
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (badForm) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Could not interpret <date> at line %d"), lineNumber(pInfo));
        return NULL;
    }
    if (!checkForCloseTag(pInfo, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH)) return NULL;

    CFAbsoluteTime at = 0.0;
#if 1
    CFGregorianDate date = {yearIsNegative ? -year : year, month, day, hour, minute, second};
    at = CFGregorianDateGetAbsoluteTime(date, NULL);
#else
    CFCalendarRef calendar = CFCalendarCreateWithIdentifier(kCFAllocatorSystemDefault, kCFCalendarIdentifierGregorian);
    CFTimeZoneRef tz = CFTimeZoneCreateWithName(kCFAllocatorSystemDefault, CFSTR("GMT"), true);
    CFCalendarSetTimeZone(calendar, tz);
    CFCalendarComposeAbsoluteTime(calendar, &at, (const uint8_t *)"yMdHms", year, month, day, hour, minute, second);
    CFRelease(calendar);
    CFRelease(tz);
#endif
    return CFDateCreate(pInfo->allocator, at);
}

static CFTypeRef parseRealTag(_CFXMLPlistParseInfo *pInfo) {
    CFStringRef str = getString(pInfo);
    SInt32 idx, len;
    double val;
    CFNumberRef result;
    CFStringInlineBuffer buf;
    if (!str) {
        if (!pInfo->error) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty <real> on line %d"), lineNumber(pInfo));
        }
        return NULL;
    }
    
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("nan"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberNaN) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("+infinity"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("-infinity"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberNegativeInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("infinity"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("-inf"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberNegativeInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("inf"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("+inf"), kCFCompareCaseInsensitive)) {
	    __CFPListRelease(str, pInfo);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}

    len = CFStringGetLength(str);
    CFStringInitInlineBuffer(str, &buf, CFRangeMake(0, len));
    idx = 0;
    if (!__CFStringScanDouble(&buf, NULL, &idx, &val) || idx != len) {
        __CFPListRelease(str, pInfo);
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered misformatted real on line %d"), lineNumber(pInfo));
        return NULL;
    }
    __CFPListRelease(str, pInfo);
    result = CFNumberCreate(pInfo->allocator, kCFNumberDoubleType, &val);
    if (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) return result;
    __CFPListRelease(result, pInfo);
    return NULL;
}

#define GET_CH	if (pInfo->curr == pInfo->end) {	\
			pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Premature end of file after <integer> on line %d"), lineNumber(pInfo)); \
			return NULL;			\
		}					\
		ch = *(pInfo->curr)

typedef struct {
    int64_t high;
    uint64_t low;
} CFSInt128Struct;

enum {
    kCFNumberSInt128Type = 17
};

static CFTypeRef parseIntegerTag(_CFXMLPlistParseInfo *pInfo) {
    bool isHex = false, isNeg = false, hadLeadingZero = false;
    UniChar ch = 0;
    
    // decimal_constant         S*(-|+)?S*[0-9]+		(S == space)
    // hex_constant		S*(-|+)?S*0[xX][0-9a-fA-F]+	(S == space)
    
    while (pInfo->curr < pInfo->end && __CFIsWhitespace(*(pInfo->curr))) pInfo->curr++;
    GET_CH;
    if ('<' == ch) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty <integer> on line %d"), lineNumber(pInfo));
        return NULL;
    }
    if ('-' == ch || '+' == ch) {
	isNeg = ('-' == ch);
	pInfo->curr++;
	while (pInfo->curr < pInfo->end && __CFIsWhitespace(*(pInfo->curr))) pInfo->curr++;
    }
    GET_CH;
    if ('0' == ch) {
	if (pInfo->curr + 1 < pInfo->end && ('x' == *(pInfo->curr + 1) || 'X' == *(pInfo->curr + 1))) {
	    pInfo->curr++;
	    isHex = true;
	} else {
	    hadLeadingZero = true;
	}
	pInfo->curr++;
    }
    GET_CH;
    while ('0' == ch) {
	hadLeadingZero = true;
	pInfo->curr++;
	GET_CH;
    }
    if ('<' == ch && hadLeadingZero) {	// nothing but zeros
	int32_t val = 0;
        if (!checkForCloseTag(pInfo, CFXMLPlistTags[INTEGER_IX], INTEGER_TAG_LENGTH)) {
	    // checkForCloseTag() sets error string
	    return NULL;
        }
	return CFNumberCreate(pInfo->allocator, kCFNumberSInt32Type, &val);
    }
    if ('<' == ch) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Incomplete <integer> on line %d"), lineNumber(pInfo));
	return NULL;
    }
    uint64_t value = 0;
    uint32_t multiplier = (isHex ? 16 : 10);
    while ('<' != ch) {
	uint32_t new_digit = 0;
	switch (ch) {
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                new_digit = (ch - '0');
                break;
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                new_digit = (ch - 'a' + 10);
                break;
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                new_digit = (ch - 'A' + 10);
                break;
            default:	// other character
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unknown character '%c' (0x%x) in <integer> on line %d"), ch, ch, lineNumber(pInfo));
                return NULL;
	}
	if (!isHex && new_digit > 9) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Hex digit in non-hex <integer> on line %d"), lineNumber(pInfo));
	    return NULL;
	}
	if (UINT64_MAX / multiplier < value) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Integer overflow in <integer> on line %d"), lineNumber(pInfo));
	    return NULL;
	}
	value = multiplier * value;
	if (UINT64_MAX - new_digit < value) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Integer overflow in <integer> on line %d"), lineNumber(pInfo));
	    return NULL;
	}
	value = value + new_digit;
	if (isNeg && (uint64_t)INT64_MAX + 1 < value) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Integer underflow in <integer> on line %d"), lineNumber(pInfo));
	    return NULL;
	}
	pInfo->curr++;
	GET_CH;
    }
    if (!checkForCloseTag(pInfo, CFXMLPlistTags[INTEGER_IX], INTEGER_TAG_LENGTH)) {
	// checkForCloseTag() sets error string
	return NULL;
    }
    if (isNeg || value <= INT64_MAX) {
	int64_t v = value;
	if (isNeg) v = -v;	// no-op if INT64_MIN
        return CFNumberCreate(pInfo->allocator, kCFNumberSInt64Type, &v);
    }
    CFSInt128Struct val;
    val.high = 0;
    val.low = value;
    return CFNumberCreate(pInfo->allocator, kCFNumberSInt128Type, &val);
}

#undef GET_CH

// Returned object is retained; caller must free.  pInfo->curr expected to point to the first character after the '<'
static CFTypeRef parseXMLElement(_CFXMLPlistParseInfo *pInfo, Boolean *isKey) {
    const UniChar *marker = pInfo->curr;
    int markerLength = -1;
    Boolean isEmpty;
    int markerIx = -1;
    
    if (isKey) *isKey = false;
    while (pInfo->curr < pInfo->end) {
        UniChar ch = *(pInfo->curr);
        if (ch == ' ' || ch ==  '\t' || ch == '\n' || ch =='\r') {
            if (markerLength == -1) markerLength = pInfo->curr - marker;
        } else if (ch == '>') {
            break;
        }
        pInfo->curr ++;
    }
    if (pInfo->curr >= pInfo->end) return NULL;
    isEmpty = (*(pInfo->curr-1) == '/');
    if (markerLength == -1)
        markerLength = pInfo->curr - (isEmpty ? 1 : 0) - marker;
    pInfo->curr ++; // Advance past '>'
    if (markerLength == 0) {
        // Back up to the beginning of the marker
        pInfo->curr = marker;
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Malformed tag on line %d"), lineNumber(pInfo));
        return NULL;
    }
    switch (*marker) {
        case 'a':   // Array
            if (markerLength == ARRAY_TAG_LENGTH && matchString(marker, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH))
                markerIx = ARRAY_IX;
            break;
        case 'd': // Dictionary, data, or date; Fortunately, they all have the same marker length....
            if (markerLength != DICT_TAG_LENGTH)
                break;
            if (matchString(marker, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH))
                markerIx = DICT_IX;
            else if (matchString(marker, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH))
                markerIx = DATA_IX;
            else if (matchString(marker, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH))
                markerIx = DATE_IX;
            break;
        case 'f': // false (boolean)
            if (markerLength == FALSE_TAG_LENGTH && matchString(marker, CFXMLPlistTags[FALSE_IX], FALSE_TAG_LENGTH)) {
                markerIx = FALSE_IX;
            }
            break;
        case 'i': // integer
            if (markerLength == INTEGER_TAG_LENGTH && matchString(marker, CFXMLPlistTags[INTEGER_IX], INTEGER_TAG_LENGTH))
                markerIx = INTEGER_IX;
            break;
        case 'k': // Key of a dictionary
            if (markerLength == KEY_TAG_LENGTH && matchString(marker, CFXMLPlistTags[KEY_IX], KEY_TAG_LENGTH)) {
                markerIx = KEY_IX;
                if (isKey) *isKey = true;
            }
            break;
        case 'p': // Plist
            if (markerLength == PLIST_TAG_LENGTH && matchString(marker, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH))
                markerIx = PLIST_IX;
            break;
        case 'r': // real
            if (markerLength == REAL_TAG_LENGTH && matchString(marker, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH))
                markerIx = REAL_IX;
            break;
        case 's': // String
            if (markerLength == STRING_TAG_LENGTH && matchString(marker, CFXMLPlistTags[STRING_IX], STRING_TAG_LENGTH))
                markerIx = STRING_IX;
            break;
        case 't': // true (boolean)
            if (markerLength == TRUE_TAG_LENGTH && matchString(marker, CFXMLPlistTags[TRUE_IX], TRUE_TAG_LENGTH))
                markerIx = TRUE_IX;
            break;
    }

    if (!pInfo->allowNewTypes && markerIx != PLIST_IX && markerIx != ARRAY_IX && markerIx != DICT_IX && markerIx != STRING_IX && markerIx != KEY_IX && markerIx != DATA_IX) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered new tag when expecting only old-style property list objects"));
        return NULL;
    }

    switch (markerIx) {
        case PLIST_IX:
            if (isEmpty) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty plist tag"));
                return NULL;
            }
            return parsePListTag(pInfo);
        case ARRAY_IX: 
            if (isEmpty) {
                return pInfo->mutabilityOption == kCFPropertyListImmutable ?  CFArrayCreate(pInfo->allocator, NULL, 0, &kCFTypeArrayCallBacks) : CFArrayCreateMutable(pInfo->allocator, 0, &kCFTypeArrayCallBacks);
            } else {
                return parseArrayTag(pInfo);
            }
        case DICT_IX:
            if (isEmpty) {
                if (pInfo->mutabilityOption == kCFPropertyListImmutable) {
                    return CFDictionaryCreate(pInfo->allocator, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                } else {
                    return CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                }
            } else {
                return parseDictTag(pInfo);
            }
        case KEY_IX:
        case STRING_IX:
        {
            CFStringRef str;
            int tagLen = (markerIx == KEY_IX) ? KEY_TAG_LENGTH : STRING_TAG_LENGTH;
            if (isEmpty) {
                return pInfo->mutabilityOption == kCFPropertyListMutableContainersAndLeaves ? CFStringCreateMutable(pInfo->allocator, 0) : CFStringCreateWithCharacters(pInfo->allocator, NULL, 0);
            }
            str = getString(pInfo);
            if (!str) return NULL; // getString will already have set the error string
            if (!checkForCloseTag(pInfo, CFXMLPlistTags[markerIx], tagLen)) {
                __CFPListRelease(str, pInfo);
                return NULL;
            }
            return str;
        }
        case DATA_IX:
            if (isEmpty) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty <data> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseDataTag(pInfo);
            }
        case DATE_IX:
            if (isEmpty) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty <date> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseDateTag(pInfo);
            }
        case TRUE_IX:
            if (!isEmpty) {
		if (!checkForCloseTag(pInfo, CFXMLPlistTags[TRUE_IX], TRUE_TAG_LENGTH)) return NULL;
	    }
	    return CFRetain(kCFBooleanTrue);
        case FALSE_IX:
            if (!isEmpty) {
		if (!checkForCloseTag(pInfo, CFXMLPlistTags[FALSE_IX], FALSE_TAG_LENGTH)) return NULL;
	    }
            return CFRetain(kCFBooleanFalse);
        case REAL_IX:
            if (isEmpty) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty <real> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseRealTag(pInfo);
            }
        case INTEGER_IX:
            if (isEmpty) {
                pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered empty <integer> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseIntegerTag(pInfo);
            }
        default:  {
            CFStringRef markerStr = CFStringCreateWithCharacters(pInfo->allocator, marker, markerLength);
            pInfo->curr = marker;
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown tag %@ on line %d"), markerStr, lineNumber(pInfo));
            __CFPListRelease(markerStr, pInfo);
            return NULL;
        }
    }
}

static CFTypeRef parseXMLPropertyList(_CFXMLPlistParseInfo *pInfo) {
    while (!pInfo->error && pInfo->curr < pInfo->end) {
        UniChar ch;
        skipWhitespace(pInfo);
        if (pInfo->curr+1 >= pInfo->end) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("No XML content found"));
            return NULL;
        }
        if (*(pInfo->curr) != '<') {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unexpected character %c at line %d"), *(pInfo->curr), lineNumber(pInfo));
            return NULL;
        }
        ch = *(++ pInfo->curr);
        if (ch == '!') {
            // Comment or DTD
            ++ pInfo->curr;
            if (pInfo->curr+1 < pInfo->end && *pInfo->curr == '-' && *(pInfo->curr+1) == '-') {
                // Comment
                pInfo->curr += 2;
                skipXMLComment(pInfo);
            } else {
                skipDTD(pInfo);
            }
        } else if (ch == '?') {
            // Processing instruction
            pInfo->curr++;
            skipXMLProcessingInstruction(pInfo);
        } else {
            // Tag or malformed
            return parseXMLElement(pInfo, NULL);
            // Note we do not verify that there was only one element, so a file that has garbage after the first element will nonetheless successfully parse
        }
    }
    // Should never get here
    if (!(pInfo->error)) {
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unexpected EOF"));
    }
    return NULL;
}

static CFStringEncoding encodingForXMLData(CFDataRef data, CFErrorRef *error) {
    const uint8_t *bytes = (uint8_t *)CFDataGetBytePtr(data);
    UInt32 length = CFDataGetLength(data);
    const uint8_t *idx, *end;
    char quote;
    
    // Check for the byte order mark first
    if (length > 2 &&
        ((*bytes == 0xFF && *(bytes+1) == 0xFE) ||
         (*bytes == 0xFE && *(bytes+1) == 0xFF) ||
         *bytes == 0x00 || *(bytes+1) == 0x00)) // This clause checks for a Unicode sequence lacking the byte order mark; technically an error, but this check is recommended by the XML spec
        return kCFStringEncodingUnicode;
    
    // Scan for the <?xml.... ?> opening
    if (length < 5 || strncmp((char const *) bytes, "<?xml", 5) != 0) return kCFStringEncodingUTF8;
    idx = bytes + 5;
    end = bytes + length;
    // Found "<?xml"; now we scan for "encoding"
    while (idx < end) {
        uint8_t ch = *idx;
        const uint8_t *scan;
        if ( ch == '?' || ch == '>') return kCFStringEncodingUTF8;
        idx ++;
        scan = idx;
	if (idx + 8 >= end) {
	    if (error) *error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("End of buffer while looking for encoding name"));
	    return 0;
	}
        if (ch == 'e' && *scan++ == 'n' && *scan++ == 'c' && *scan++ == 'o' && *scan++ == 'd' && *scan++ == 'i'
            && *scan++ == 'n' && *scan++ == 'g' && *scan++ == '=') {
            idx = scan;
            break;
        }
    }
    if (idx >= end) return kCFStringEncodingUTF8;
    quote = *idx;
    if (quote != '\'' && quote != '\"') return kCFStringEncodingUTF8;
    else {
        CFStringRef encodingName;
        const uint8_t *base = idx+1; // Move past the quote character
        CFStringEncoding enc;
        UInt32 len;
        idx ++;
        while (idx < end && *idx != quote) idx ++;
        if (idx >= end) return kCFStringEncodingUTF8;
        len = idx - base;
        if (len == 5 && (*base == 'u' || *base == 'U') && (base[1] == 't' || base[1] == 'T') && (base[2] == 'f' || base[2] == 'F') && (base[3] == '-') && (base[4] == '8'))
            return kCFStringEncodingUTF8;
        encodingName = CFStringCreateWithBytes(kCFAllocatorSystemDefault, base, len, kCFStringEncodingISOLatin1, false);
        enc = CFStringConvertIANACharSetNameToEncoding(encodingName);
        if (enc != kCFStringEncodingInvalidId) {
            CFRelease(encodingName);
            return enc;
        }

        if (error) {
            *error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Encountered unknown encoding (%@)"), encodingName);
            CFRelease(encodingName);
        }
        return 0;
    }
}

bool __CFTryParseBinaryPlist(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags option, CFPropertyListRef *plist, CFStringRef *errorString);
unsigned long _CFPropertyListAllowNonUTF8 = 0;

#define SAVE_PLISTS 0

#if SAVE_PLISTS
static Boolean __savePlistData(CFDataRef data, CFOptionFlags opt) {
    uint8_t pn[2048];
    uint8_t fn[2048];
    uint32_t pnlen = sizeof(pn);
    uint8_t *pnp = NULL;
    if (0 == _NSGetExecutablePath((char *)pn, &pnlen)) {
	pnp = strrchr((char *)pn, '/');
    }
    if (!pnp) {
	pnp = pn;
    } else {
	pnp++;
    }
    if (0 == strcmp((char *)pnp, "parse_plists")) return true;
    CFUUIDRef r = CFUUIDCreate(kCFAllocatorSystemDefault);
    CFStringRef s = CFUUIDCreateString(kCFAllocatorSystemDefault, r);
    CFStringRef p = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("/tmp/plists/%s#%@#0x%x"), pnp, s, opt);
    _CFStringGetFileSystemRepresentation(p, fn, sizeof(fn));
    CFRelease(r);
    CFRelease(s);
    CFRelease(p);
    int fd = open((const char *)fn, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd < 0) return false;
    int len = CFDataGetLength(data);
    int w = write(fd, CFDataGetBytePtr(data), len);
    fsync(fd);
    close(fd);
    if (w != len) return false;
    return true;
}
#endif


CFTypeRef _CFPropertyListCreateFromXMLStringError(CFAllocatorRef allocator, CFStringRef xmlString, CFOptionFlags option, CFErrorRef *error, Boolean allowNewTypes, CFPropertyListFormat *format) {
    initStatics();
    
    CFAssert1(xmlString != NULL, __kCFLogAssertion, "%s(): NULL string not allowed", __PRETTY_FUNCTION__);
    CFAssert2(option == kCFPropertyListImmutable || option == kCFPropertyListMutableContainers || option == kCFPropertyListMutableContainersAndLeaves, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, option);
    
    UInt32 length;
    Boolean createdBuffer = false;
    length = xmlString ? CFStringGetLength(xmlString) : 0;
    
    if (!length) {
        if (error) {
            *error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Conversion of string failed. The string is empty."));
        }
        return NULL;
    }
    
    _CFXMLPlistParseInfo pInfoBuf;
    _CFXMLPlistParseInfo *pInfo = &pInfoBuf;
    CFTypeRef result;
    
    // Ensure that xmlString is not collected while we are using it
    CFRetain(xmlString);
    UniChar *buf = (UniChar *)CFStringGetCharactersPtr(xmlString);
    
    if (!buf) {
        buf = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, length * sizeof(UniChar), 0);
        if (!buf) {
            out_of_memory_warning();
            return NULL;
        }
        CFStringGetCharacters(xmlString, CFRangeMake(0, length), buf);
        createdBuffer = true;
    }
    pInfo->begin = buf;
    pInfo->end = buf+length;
    pInfo->curr = buf;
    pInfo->allocator = allocator;
    pInfo->error = NULL;
    pInfo->stringSet = CFSetCreateMutable(allocator, 0, &kCFTypeSetCallBacks);
    _CFSetSetCapacity(pInfo->stringSet, CFStringGetLength(xmlString) / 250); // avoid lots of rehashes, may waste some memory; simple heuristic
    pInfo->mutabilityOption = option;
    pInfo->allowNewTypes = allowNewTypes;
    
    result = parseXMLPropertyList(pInfo);
    if (result && format) *format = kCFPropertyListXMLFormat_v1_0;
    if (!result) {
        CFErrorRef xmlParserErr = pInfo->error;
        // Reset pInfo so we can try again
        pInfo->curr = pInfo->begin;
        pInfo->error = NULL;
        
        // Try pList
        result = parseOldStylePropertyListOrStringsFile(pInfo);
        if (result && format) {
            *format = kCFPropertyListOpenStepFormat;
        }
        
        if (!result && xmlParserErr && error) {
            // Add the new error from the old-style property list parser to the user info of the original error
            CFDictionaryRef xmlUserInfo = CFErrorCopyUserInfo(xmlParserErr);
            CFMutableDictionaryRef userInfo = CFDictionaryCreateMutableCopy(kCFAllocatorSystemDefault, CFDictionaryGetCount(xmlUserInfo) + 1, xmlUserInfo);
            CFDictionaryAddValue(userInfo, CFPropertyListOldStyleParserErrorKey, pInfo->error);
            
            // Re-create the xml parser error with this new user info dictionary
            CFErrorRef newError = CFErrorCreate(kCFAllocatorSystemDefault, CFErrorGetDomain(xmlParserErr), CFErrorGetCode(xmlParserErr), userInfo);
            
            CFRelease(xmlUserInfo);
            CFRelease(userInfo);
                        
            // It is the responsibility of the caller to release this newly created error
            *error = newError;
        }
        
        if (xmlParserErr) {
            CFRelease(xmlParserErr);
        }
    }
    
    if (createdBuffer) {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, (void *)pInfo->begin);
    }
    if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(pInfo->stringSet);
    if (pInfo->error) CFRelease(pInfo->error);
    CFRelease(xmlString);
    return result;
}

CFTypeRef _CFPropertyListCreateFromXMLString(CFAllocatorRef allocator, CFStringRef xmlString, CFOptionFlags option, CFStringRef *errorString, Boolean allowNewTypes, CFPropertyListFormat *format) {
    initStatics();
    if (errorString) *errorString = NULL;
    CFErrorRef error = NULL;
    CFTypeRef result = _CFPropertyListCreateFromXMLStringError(allocator, xmlString, option, &error, allowNewTypes, format);

    if (errorString && error) {
        // The caller is interested in receiving an error message. Pull the debug string out of the CFError returned above
        CFDictionaryRef userInfo = CFErrorCopyUserInfo(error);
        CFStringRef debugString = NULL;
        
        // Handle a special-case for compatibility - if the XML parse failed and the old-style plist parse failed, construct a special string
        CFErrorRef underlyingError = NULL;

        Boolean oldStyleFailed = CFDictionaryGetValueIfPresent(userInfo, CFPropertyListOldStyleParserErrorKey, (const void **)&underlyingError);
        
        if (oldStyleFailed) {
            CFStringRef xmlParserErrorString, oldStyleParserErrorString;
            xmlParserErrorString = (CFStringRef)CFDictionaryGetValue(userInfo, kCFErrorDebugDescriptionKey);
            
            CFDictionaryRef oldStyleParserUserInfo = CFErrorCopyUserInfo(underlyingError);
            oldStyleParserErrorString = (CFStringRef)CFDictionaryGetValue(userInfo, kCFErrorDebugDescriptionKey);
            
            // Combine the two strings into a single one that matches the previous implementation
            debugString = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("XML parser error:\n\t%@\nOld-style plist parser error:\n\t%@\n"), xmlParserErrorString, oldStyleParserErrorString);
            
            CFRelease(oldStyleParserUserInfo);
        } else {
            debugString = (CFStringRef)CFDictionaryGetValue(userInfo, kCFErrorDebugDescriptionKey); 
            CFRetain(debugString);
        }
        
        // Give the debugString to the caller, who is responsible for releasing it
        CFRelease(userInfo);
        *errorString = debugString;
    }
    
    if (error) {
        CFRelease(error);
    }
    
    return result;
}

/* Get a single value for a given key in a top-level dictionary in a property list.
 @param allocator The allocator to use.
 @param data The property list data.
 @param option Currently unused, set to 0.
 @param keyPath The keyPath to search for in the property list. Keys are colon-separated. Indexes into arrays are specified with an integer (zero-based).
 @param value If the key is found and the parameter is non-NULL, this will be set to a reference to the value. It is the caller's responsibility to release the object. If no object is found, will be set to NULL. If this parameter is NULL, this function can be used to check for the existence of a key without creating it by just checking the return value.
 @param error If an error occurs, will be set to a valid CFErrorRef. It is the caller's responsibility to release this value.
 @return True if the key is found, false otherwise.
 */
bool _CFPropertyListCreateSingleValue(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags option, CFStringRef keyPath, CFPropertyListRef *value, CFErrorRef *error) {
    
    initStatics();
    
    if (!keyPath || CFStringGetLength(keyPath) == 0) {
        return false;
    }
    
    uint8_t marker;    
    CFBinaryPlistTrailer trailer;
    uint64_t offset;
    const uint8_t *databytes = CFDataGetBytePtr(data);
    uint64_t datalen = CFDataGetLength(data);
    bool result = false;
    
    // First check to see if it is a binary property list
    if (8 <= datalen && __CFBinaryPlistGetTopLevelInfo(databytes, datalen, &marker, &offset, &trailer)) {
        // Split up the key path
        CFArrayRef keyPathArray = CFStringCreateArrayBySeparatingStrings(kCFAllocatorSystemDefaultGCRefZero, keyPath, CFSTR(":"));
        uint64_t keyOffset, valueOffset = offset;
        
        // Create a dictionary to cache objects in
        CFMutableDictionaryRef objects = CFDictionaryCreateMutable(kCFAllocatorSystemDefaultGCRefZero, 0, NULL, &kCFTypeDictionaryValueCallBacks);
        CFIndex keyPathCount = CFArrayGetCount(keyPathArray);
        _CFDictionarySetCapacity(objects, keyPathCount + 1);

        for (CFIndex i = 0; i < keyPathCount; i++) {
            CFStringRef oneKey = (CFStringRef)CFArrayGetValueAtIndex(keyPathArray, i);
            SInt32 intValue = CFStringGetIntValue(oneKey);
            if ((intValue == 0 && CFStringCompare(CFSTR("0"), oneKey, 0) != kCFCompareEqualTo) || intValue == INT_MAX || intValue == INT_MIN) {
                // Treat as a string key into a dictionary
                result = __CFBinaryPlistGetOffsetForValueFromDictionary3(databytes, datalen, valueOffset, &trailer, (CFTypeRef)oneKey, &keyOffset, &valueOffset, false, objects);
            } else {
                // Treat as integer index into an array
                result = __CFBinaryPlistGetOffsetForValueFromArray2(databytes, datalen, valueOffset, &trailer, intValue, &valueOffset, objects);
            }
            
            if (!result) {
                break;
            }
        }
        
        // value could be null if the caller wanted to check for the existence of a key but not bother creating it
        if (result && value) {
            CFPropertyListRef pl;
	    result = __CFBinaryPlistCreateObject2(databytes, datalen, valueOffset, &trailer, allocator, option, objects, NULL, 0, &pl);
	    if (result) {
		// caller's responsibility to release the created object
		*value = pl;
	    }
	}
        
        if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(keyPathArray);
        if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(objects);
    } else {
	// Try an XML property list
	// Note: This is currently not any more efficient than grabbing the whole thing. This could be improved in the future.
	CFPropertyListRef plist = CFPropertyListCreateWithData(allocator, data, option, NULL, error);
        CFPropertyListRef nextObject = plist;
        result = true;
        
	if (!(*error) && plist) {
            CFArrayRef keyPathArray = CFStringCreateArrayBySeparatingStrings(kCFAllocatorSystemDefaultGCRefZero, keyPath, CFSTR(":"));
            for (CFIndex i = 0;  i < CFArrayGetCount(keyPathArray); i++) {
                CFStringRef oneKey = (CFStringRef)CFArrayGetValueAtIndex(keyPathArray, i);
                SInt32 intValue = CFStringGetIntValue(oneKey);
                if (((intValue == 0 && CFStringCompare(CFSTR("0"), oneKey, 0) != kCFCompareEqualTo) || intValue == INT_MAX || intValue == INT_MIN) && CFGetTypeID((CFTypeRef)nextObject) == dicttype) {
                    // Treat as a string key into a dictionary
                    nextObject = (CFPropertyListRef)CFDictionaryGetValue((CFDictionaryRef)nextObject, oneKey);
                } else if (CFGetTypeID((CFTypeRef)nextObject) == arraytype) {
                    // Treat as integer index into an array
                    nextObject = (CFPropertyListRef)CFArrayGetValueAtIndex((CFArrayRef)nextObject, intValue);
                } else {
                    result = false;
                    break;
                }
            }
            
            if (result && nextObject && value) {
                *value = nextObject;
                // caller's responsibility to release the created object
                CFRetain(*value);
            }
            
            if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(keyPathArray);
	}
        if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(plist);
    }
    
    return result;
}

CFTypeRef _CFPropertyListCreateWithData(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags option, CFErrorRef *error, Boolean allowNewTypes, CFPropertyListFormat *format) {
    initStatics();
    CFStringEncoding encoding;
    CFPropertyListRef plist;
    
    if (!data || CFDataGetLength(data) == 0) {
        if (error) {
            *error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Cannot parse a NULL or zero-length data"));
        }
        return NULL;
    }
    
#if SAVE_PLISTS
    __savePlistData(data, option);
#endif
    
    // Ignore the error from CFTryParseBinaryPlist -- if it doesn't work, we're going to try again anyway using the XML parser
    if (__CFTryParseBinaryPlist(allocator, data, option, &plist, NULL)) {
	if (format) *format = kCFPropertyListBinaryFormat_v1_0;
	return plist;
    }
    
    // Use our own error variable here so we can check it against NULL later
    CFErrorRef subError = NULL;
    encoding = encodingForXMLData(data, &subError); // 0 is an error return, NOT MacRoman.
    
    if (encoding == 0) {
        // Couldn't find an encoding
        // Note that encodingForXMLData() will give us the right values for a standard plist, too.
        if (error && subError == NULL) {
	    // encodingForXMLData didn't set an error, so we create a new one here
            *error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Could not determine the encoding of the XML data"));
        } else if (error && subError) {
	    // give the caller the subError, they will release
	    *error = subError;
	} else if (!error && subError) {
	    // Release the error
	    CFRelease(subError);
	}
        return NULL;
    }
    
    CFStringRef xmlString = CFStringCreateWithBytes(allocator, CFDataGetBytePtr(data), CFDataGetLength(data), encoding, true);
    if (NULL == xmlString && (!_CFExecutableLinkedOnOrAfter(CFSystemVersionLeopard) || _CFPropertyListAllowNonUTF8)) {	// conversion failed, probably because not in proper encoding
        // Call __CFStringCreateImmutableFunnel3() the same way CFStringCreateWithBytes() does, except with the addt'l flag
        if (encoding == kCFStringEncodingUTF8) xmlString = __CFStringCreateImmutableFunnel3(allocator, CFDataGetBytePtr(data), CFDataGetLength(data), kCFStringEncodingUTF8, true, true, false, false, false, (CFAllocatorRef)-1  /* ALLOCATORSFREEFUNC */, kCFStringEncodingLenientUTF8Conversion);
    }
    
    // Haven't done anything XML-specific to this point.  However, the encoding we used to translate the bytes should be kept in mind; we used Unicode if the byte-order mark was present; UTF-8 otherwise.  If the system encoding is not UTF-8 or some variant of 7-bit ASCII, we'll be in trouble.....
    plist = _CFPropertyListCreateFromXMLStringError(allocator, xmlString, option, error, allowNewTypes, format);
    
    if (xmlString) {
        if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(xmlString);
        xmlString = NULL;
    }
    
    return plist;
}


CFTypeRef _CFPropertyListCreateFromXMLData(CFAllocatorRef allocator, CFDataRef xmlData, CFOptionFlags option, CFStringRef *errorString, Boolean allowNewTypes, CFPropertyListFormat *format) {
    initStatics();
    CFTypeRef result;
    if (errorString) *errorString = NULL;
    CFErrorRef error = NULL;
    result = _CFPropertyListCreateWithData(allocator, xmlData, option, &error, allowNewTypes, format);
    if (error && errorString) {
        *errorString = __CFPropertyListCopyErrorDebugDescription(error);
    }
    if (error) CFRelease(error);
    return result;
}

CFPropertyListRef CFPropertyListCreateWithData(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags options, CFPropertyListFormat *format, CFErrorRef *error) {
    initStatics();
    CFAssert1(data != NULL, __kCFLogAssertion, "%s(): NULL data not allowed", __PRETTY_FUNCTION__);
    CFAssert2(options == kCFPropertyListImmutable || options == kCFPropertyListMutableContainers || options == kCFPropertyListMutableContainersAndLeaves, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, options);
    return _CFPropertyListCreateWithData(allocator, data, options, error, true, format);
}

CFPropertyListRef CFPropertyListCreateFromXMLData(CFAllocatorRef allocator, CFDataRef xmlData, CFOptionFlags option, CFStringRef *errorString) {
    initStatics();
    if (errorString) *errorString = NULL;
    CFErrorRef error = NULL;
    CFPropertyListRef result = CFPropertyListCreateWithData(allocator, xmlData, option, NULL, &error);
    if (error && errorString) {
        *errorString = __CFPropertyListCopyErrorDebugDescription(error);
    }
    if (error) CFRelease(error);
    return result;
}

CFDataRef CFPropertyListCreateData(CFAllocatorRef allocator, CFPropertyListRef propertyList, CFPropertyListFormat format, CFOptionFlags options, CFErrorRef *error) {
    initStatics();
    CFAssert1(format != kCFPropertyListOpenStepFormat, __kCFLogAssertion, "%s(): kCFPropertyListOpenStepFormat not supported for writing", __PRETTY_FUNCTION__);
    CFAssert2(format == kCFPropertyListXMLFormat_v1_0 || format == kCFPropertyListBinaryFormat_v1_0, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, format);
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL property list", __PRETTY_FUNCTION__);
    __CFAssertIsPList(propertyList);
    
    CFDataRef data = NULL;
    
    
    CFStringRef validErr = NULL;
    if (!_CFPropertyListIsValidWithErrorString(propertyList, format, &validErr)) {
        CFLog(kCFLogLevelError, CFSTR("Property list invalid for format: %d (%@)"), format, validErr);
	if (validErr) CFRelease(validErr);
        return NULL;
    }
    
    if (format == kCFPropertyListOpenStepFormat) {
        CFLog(kCFLogLevelError, CFSTR("Property list format kCFPropertyListOpenStepFormat not supported for writing"));
        return NULL;
    } else if (format == kCFPropertyListXMLFormat_v1_0) {
        data = _CFPropertyListCreateXMLData(allocator, propertyList, true);
    } else if (format == kCFPropertyListBinaryFormat_v1_0) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS
        // TODO: Is it more efficient to create a stream here or just use a mutable data?
        CFWriteStreamRef stream = CFWriteStreamCreateWithAllocatedBuffers(kCFAllocatorSystemDefault, allocator);
        CFWriteStreamOpen(stream);
        CFIndex len = CFPropertyListWrite(propertyList, stream, format, options, error);
        if (0 < len) {
            data = (CFDataRef)CFWriteStreamCopyProperty(stream, kCFStreamPropertyDataWritten);
        }
        CFWriteStreamClose(stream);
	CFRelease(stream);
#else
        CFMutableDataRef dataForPlist = CFDataCreateMutable(allocator, 0);
        __CFBinaryPlistWrite(propertyList, dataForPlist, 0, options, error);
        return dataForPlist;
#endif
    } else {
	CFLog(kCFLogLevelError, CFSTR("Unknown format option"));
    }
    
    return data;
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS

CFIndex CFPropertyListWrite(CFPropertyListRef propertyList, CFWriteStreamRef stream, CFPropertyListFormat format, CFOptionFlags options, CFErrorRef *error) {
    initStatics();
    CFAssert1(stream != NULL, __kCFLogAssertion, "%s(): NULL stream not allowed", __PRETTY_FUNCTION__);
    CFAssert1(format != kCFPropertyListOpenStepFormat, __kCFLogAssertion, "%s(): kCFPropertyListOpenStepFormat not supported for writing", __PRETTY_FUNCTION__);
    CFAssert2(format == kCFPropertyListXMLFormat_v1_0 || format == kCFPropertyListBinaryFormat_v1_0, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, format);
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL property list", __PRETTY_FUNCTION__);
    __CFAssertIsPList(propertyList);
    CFAssert1(CFWriteStreamGetTypeID() == CFGetTypeID(stream), __kCFLogAssertion, "%s(): stream argument is not a write stream", __PRETTY_FUNCTION__);
    CFAssert1(kCFStreamStatusOpen == CFWriteStreamGetStatus(stream) || kCFStreamStatusWriting == CFWriteStreamGetStatus(stream), __kCFLogAssertion, "%s():  stream is not open", __PRETTY_FUNCTION__);
    
    CFStringRef validErr = NULL;
    if (!_CFPropertyListIsValidWithErrorString(propertyList, format, &validErr)) {
        CFLog(kCFLogLevelError, CFSTR("Property list invalid for format: %d (%@)"), format, validErr);
	if (validErr) CFRelease(validErr);
        return 0;
    }
    if (format == kCFPropertyListOpenStepFormat) {
        CFLog(kCFLogLevelError, CFSTR("Property list format kCFPropertyListOpenStepFormat not supported for writing"));
        return 0;
    }
    if (format == kCFPropertyListXMLFormat_v1_0) {
        CFDataRef data = _CFPropertyListCreateXMLData(kCFAllocatorSystemDefault, propertyList, true);
        CFIndex len = CFDataGetLength(data);
	const uint8_t *ptr = CFDataGetBytePtr(data);
	while (0 < len) {
	    CFIndex ret = CFWriteStreamWrite(stream, ptr, len);
	    if (ret == 0) {
                if (error) *error = __CFPropertyListCreateError(kCFPropertyListWriteStreamError, CFSTR("Property list writing could not be completed because stream is full."));
                CFRelease(data);
	        return 0;
	    }
	    if (ret < 0) {
		CFErrorRef underlyingError = CFWriteStreamCopyError(stream);
                if (underlyingError) {
                    if (error) {
                        // Wrap the error from CFWriteStreamCopy in a new error
                        CFMutableDictionaryRef userInfo = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); 
                        CFDictionarySetValue(userInfo, kCFErrorDebugDescriptionKey, CFSTR("Property list writing could not be completed because the stream had an unknown error."));
                        CFDictionarySetValue(userInfo, kCFErrorUnderlyingErrorKey, underlyingError);
                        *error = CFErrorCreate(kCFAllocatorSystemDefault, kCFErrorDomainCocoa, kCFPropertyListWriteStreamError, userInfo);
                        CFRelease(userInfo);
                    }
                    CFRelease(underlyingError);
                }
		CFRelease(data);
		return 0;
	    }
	    ptr += ret;
	    len -= ret;
	}
	len = CFDataGetLength(data);
	CFRelease(data);
        return len;
    }
    if (format == kCFPropertyListBinaryFormat_v1_0) {
        CFIndex len = __CFBinaryPlistWrite(propertyList, stream, 0, options, error);
        return len;
    }
    CFLog(kCFLogLevelError, CFSTR("Unknown format option"));
    return 0;
}

CFIndex CFPropertyListWriteToStream(CFPropertyListRef propertyList, CFWriteStreamRef stream, CFPropertyListFormat format, CFStringRef *errorString) {
    initStatics();
    if (errorString) *errorString = NULL;
    CFErrorRef error = NULL;
    
    // For backwards compatibility, we check the format parameter up front since these do not have CFError counterparts in the newer API
    CFStringRef validErr = NULL;
    if (!_CFPropertyListIsValidWithErrorString(propertyList, format, &validErr)) {
	if (errorString) *errorString = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("Property list invalid for format (%@)"), validErr);
	if (validErr) CFRelease(validErr);
        return 0;
    }
    if (format == kCFPropertyListOpenStepFormat) {
        if (errorString) *errorString = (CFStringRef)CFRetain(CFSTR("Property list format kCFPropertyListOpenStepFormat not supported for writing"));
        return 0;
    }
    if (format != kCFPropertyListBinaryFormat_v1_0 && format != kCFPropertyListXMLFormat_v1_0) {
        if (errorString) *errorString = (CFStringRef)CFRetain(CFSTR("Unknown format option"));
        return 0;
    }
    
    CFIndex result = CFPropertyListWrite(propertyList, stream, format, 0, &error);
    if (error && errorString) {
        *errorString = __CFPropertyListCopyErrorDebugDescription(error);
    }
    if (error) CFRelease(error);
    return result;    
}

static void __CFConvertReadStreamToBytes(CFReadStreamRef stream, CFIndex max, uint8_t **buffer, CFIndex *length, CFErrorRef *error) {
    int32_t buflen = 0, bufsize = 0, retlen;
    uint8_t *buf = NULL, sbuf[8192];
    for (;;) {
	retlen = CFReadStreamRead(stream, sbuf, __CFMin(8192, max));
        if (retlen <= 0) {
            *buffer = buf;
            *length = buflen;
            
            if (retlen < 0 && error) {
                // Copy the error out
                *error = CFReadStreamCopyError(stream);
            }
            
	    return;
	}
        if (bufsize < buflen + retlen) {
	    if (bufsize < 256 * 1024) {
		bufsize *= 4;
	    } else if (bufsize < 16 * 1024 * 1024) {
		bufsize *= 2;
	    } else {
		// once in this stage, this will be really slow
		// and really potentially fragment memory
		bufsize += 256 * 1024;
	    }
	    if (bufsize < buflen + retlen) bufsize = buflen + retlen;
	    buf = (uint8_t *)CFAllocatorReallocate(kCFAllocatorSystemDefault, buf, bufsize, 0);
	    if (!buf) HALT;
	}
	memmove(buf + buflen, sbuf, retlen);
	buflen += retlen;
        max -= retlen;
	if (max <= 0) {
	    *buffer = buf;
	    *length = buflen;
	    return;
	}
    }
}

CFPropertyListRef CFPropertyListCreateWithStream(CFAllocatorRef allocator, CFReadStreamRef stream, CFIndex streamLength, CFOptionFlags mutabilityOption, CFPropertyListFormat *format, CFErrorRef *error) {
    initStatics();
    CFPropertyListRef pl;
    CFDataRef data;
    CFIndex buflen = 0;
    uint8_t *buffer = NULL;
    CFAssert1(stream != NULL, __kCFLogAssertion, "%s(): NULL stream not allowed", __PRETTY_FUNCTION__);
    CFAssert1(CFReadStreamGetTypeID() == CFGetTypeID(stream), __kCFLogAssertion, "%s(): stream argument is not a read stream", __PRETTY_FUNCTION__);
    CFAssert1(kCFStreamStatusOpen == CFReadStreamGetStatus(stream) || kCFStreamStatusReading == CFReadStreamGetStatus(stream), __kCFLogAssertion, "%s():  stream is not open", __PRETTY_FUNCTION__);
    CFAssert2(mutabilityOption == kCFPropertyListImmutable || mutabilityOption == kCFPropertyListMutableContainers || mutabilityOption == kCFPropertyListMutableContainersAndLeaves, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, mutabilityOption);
    
    if (0 == streamLength) streamLength = LONG_MAX;
    CFErrorRef underlyingError = NULL;
    __CFConvertReadStreamToBytes(stream, streamLength, &buffer, &buflen, &underlyingError);
    if (underlyingError) {
        if (error) {
            // Wrap the error from CFReadStream in a new error in the cocoa domain
            CFMutableDictionaryRef userInfo = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); 
            CFDictionarySetValue(userInfo, kCFErrorDebugDescriptionKey, CFSTR("Property list reading could not be completed because the stream had an unknown error."));
            CFDictionarySetValue(userInfo, kCFErrorUnderlyingErrorKey, underlyingError);
            *error = CFErrorCreate(kCFAllocatorSystemDefault, kCFErrorDomainCocoa, kCFPropertyListReadStreamError, userInfo);
            CFRelease(userInfo);
        }
        CFRelease(underlyingError);
        return NULL;
    }
    
    if (!buffer || buflen < 6) {
        if (buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, buffer);
        if (error) *error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("stream had too few bytes"));
        return NULL;
    }
    data = CFDataCreateWithBytesNoCopy(kCFAllocatorSystemDefault, buffer, buflen, kCFAllocatorSystemDefault);
    pl = _CFPropertyListCreateWithData(allocator, data, mutabilityOption, error, true, format);
    CFRelease(data);
    return pl;
}

CFPropertyListRef CFPropertyListCreateFromStream(CFAllocatorRef allocator, CFReadStreamRef stream, CFIndex length, CFOptionFlags mutabilityOption, CFPropertyListFormat *format, CFStringRef *errorString) {
    initStatics();
    if (errorString) *errorString = NULL;
    CFErrorRef error = NULL;
    CFPropertyListRef result = CFPropertyListCreateWithStream(allocator, stream, length, mutabilityOption, format, &error);
    if (error && errorString) {
        *errorString = __CFPropertyListCopyErrorDebugDescription(error);
    }
    if (error) CFRelease(error);
    return result;
}

#endif //DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS

// ========================================================================

//
// Old NeXT-style property lists
//

static CFTypeRef parsePlistObject(_CFXMLPlistParseInfo *pInfo, bool requireObject);

#define isValidUnquotedStringCharacter(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z') || ((x) >= '0' && (x) <= '9') || (x) == '_' || (x) == '$' || (x) == '/' || (x) == ':' || (x) == '.' || (x) == '-')

// Returns true if the advance found something before the end of the buffer, false otherwise
static Boolean advanceToNonSpace(_CFXMLPlistParseInfo *pInfo) {
    UniChar ch2;
    while (pInfo->curr < pInfo->end) {
	ch2 = *(pInfo->curr);
        pInfo->curr ++;
        if (ch2 >= 9 && ch2 <= 0x0d) continue;	// tab, newline, vt, form feed, carriage return
        if (ch2 == ' ' || ch2 == 0x2028 || ch2 == 0x2029) continue;	// space and Unicode line sep, para sep
	if (ch2 == '/') {
            if (pInfo->curr >= pInfo->end) {
                // whoops; back up and return
                pInfo->curr --;
                return true;
            } else if (*(pInfo->curr) == '/') {
                pInfo->curr ++;
                while (pInfo->curr < pInfo->end) {	// go to end of comment line
                    UniChar ch3 = *(pInfo->curr);
                    if (ch3 == '\n' || ch3 == '\r' || ch3 == 0x2028 || ch3 == 0x2029) break;
                    pInfo->curr ++;
		}
	    } else if (*(pInfo->curr) == '*') {		// handle /* ... */
                pInfo->curr ++;
		while (pInfo->curr < pInfo->end) {
		    ch2 = *(pInfo->curr);
                    pInfo->curr ++;
		    if (ch2 == '*' && pInfo->curr < pInfo->end && *(pInfo->curr) == '/') {
                        pInfo->curr ++; // advance past the '/'
                        break;
                    }
                }
            } else {
                pInfo->curr --;
                return true;
	    }
        } else {
            pInfo->curr --;
            return true;
        }
    }
    return false;
}

static UniChar getSlashedChar(_CFXMLPlistParseInfo *pInfo) {
    UniChar ch = *(pInfo->curr);
    pInfo->curr ++;
    switch (ch) {
	case '0':
	case '1':	
	case '2':	
	case '3':	
	case '4':	
	case '5':	
	case '6':	
	case '7':  {
            uint8_t num = ch - '0';
            UniChar result;
            CFIndex usedCharLen;
	    /* three digits maximum to avoid reading \000 followed by 5 as \5 ! */
	    if ((ch = *(pInfo->curr)) >= '0' && ch <= '7') { // we use in this test the fact that the buffer is zero-terminated
                pInfo->curr ++;
		num = (num << 3) + ch - '0';
		if ((pInfo->curr < pInfo->end) && (ch = *(pInfo->curr)) >= '0' && ch <= '7') {
                    pInfo->curr ++;
		    num = (num << 3) + ch - '0';
		}
	    }
            CFStringEncodingBytesToUnicode(kCFStringEncodingNextStepLatin, 0, &num, sizeof(uint8_t), NULL,  &result, 1, &usedCharLen);
            return (usedCharLen == 1) ? result : 0;
	}
	case 'U': {
	    unsigned num = 0, numDigits = 4;	/* Parse four digits */
	    while (pInfo->curr < pInfo->end && numDigits--) {
                if (((ch = *(pInfo->curr)) < 128) && isxdigit(ch)) { 
                    pInfo->curr ++;
		    num = (num << 4) + ((ch <= '9') ? (ch - '0') : ((ch <= 'F') ? (ch - 'A' + 10) : (ch - 'a' + 10)));
		}
	    }
	    return num;
	}
	case 'a':	return '\a';	// Note: the meaning of '\a' varies with -traditional to gcc
	case 'b':	return '\b';
	case 'f':	return '\f';
	case 'n':	return '\n';
	case 'r':	return '\r';
	case 't':	return '\t';
	case 'v':	return '\v';
	case '"':	return '\"';
	case '\n':	return '\n';
    }
    return ch;
}

static CFStringRef parseQuotedPlistString(_CFXMLPlistParseInfo *pInfo, UniChar quote) {
    CFMutableStringRef str = NULL;
    const UniChar *startMark = pInfo->curr;
    const UniChar *mark = pInfo->curr;
    while (pInfo->curr < pInfo->end) {
	UniChar ch = *(pInfo->curr);
        if (ch == quote) break;
        if (ch == '\\') {
            _catFromMarkToBuf(mark, pInfo->curr, &str, pInfo);
            pInfo->curr ++;
            ch = getSlashedChar(pInfo);
            CFStringAppendCharacters(str, &ch, 1);
            mark = pInfo->curr;
	} else {
            // Note that the original NSParser code was much more complex at this point, but it had to deal with 8-bit characters in a non-UniChar stream.  We always have UniChar (we translated the data by the system encoding at the very beginning, hopefully), so this is safe.
            pInfo->curr ++;
        }
    }
    if (pInfo->end <= pInfo->curr) {
        __CFPListRelease(str, pInfo);
        pInfo->curr = startMark;
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unterminated quoted string starting on line %d"), lineNumber(pInfo));
        return NULL;
    }
    if (!str) {
        if (pInfo->mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
            _catFromMarkToBuf(mark, pInfo->curr, &str, pInfo);
        } else {
            str = (CFMutableStringRef)_uniqueStringForCharacters(pInfo, mark, pInfo->curr-mark);
        }
    } else {
        if (mark != pInfo->curr) {
            _catFromMarkToBuf(mark, pInfo->curr, &str, pInfo);
        }
        if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
            CFStringRef uniqueString = _uniqueStringForString(pInfo, str);
            __CFPListRelease(str, pInfo);
            str = (CFMutableStringRef)uniqueString;
        }
    }
    pInfo->curr ++;  // Advance past the quote character before returning.
    if (pInfo->error) {
        CFRelease(pInfo->error);
        pInfo->error = NULL;
    }
    return str;
}

static CFStringRef parseUnquotedPlistString(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *mark = pInfo->curr;
    while (pInfo->curr < pInfo->end) {
        UniChar ch = *pInfo->curr;
        if (isValidUnquotedStringCharacter(ch))
            pInfo->curr ++;
        else break;
    }
    if (pInfo->curr != mark) {
        if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
            CFStringRef str = _uniqueStringForCharacters(pInfo, mark, pInfo->curr-mark);
            return str;
        } else {
            CFMutableStringRef str = CFStringCreateMutable(pInfo->allocator, 0);
            CFStringAppendCharacters(str, mark, pInfo->curr - mark);
            return str;
        }
    }
    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unexpected EOF"));
    return NULL;
}

static CFStringRef parsePlistString(_CFXMLPlistParseInfo *pInfo, bool requireObject) {
    UniChar ch;
    Boolean foundChar = advanceToNonSpace(pInfo);
    if (!foundChar) {
        if (requireObject) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unexpected EOF while parsing string"));
        }
        return NULL;
    }
    ch = *(pInfo->curr);
    if (ch == '\'' || ch == '\"') {
        pInfo->curr ++;
        return parseQuotedPlistString(pInfo, ch);
    } else if (isValidUnquotedStringCharacter(ch)) {
        return parseUnquotedPlistString(pInfo);
    } else {
        if (requireObject) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Invalid string character at line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
}

static CFTypeRef parsePlistArray(_CFXMLPlistParseInfo *pInfo) {
    CFMutableArrayRef array = CFArrayCreateMutable(pInfo->allocator, 0, &kCFTypeArrayCallBacks);
    CFTypeRef tmp = parsePlistObject(pInfo, false);
    Boolean foundChar;
    while (tmp) {
        CFArrayAppendValue(array, tmp);
        __CFPListRelease(tmp, pInfo);
        foundChar = advanceToNonSpace(pInfo);
	if (!foundChar) {
	    __CFPListRelease(array, pInfo);
	    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Expected ',' for array at line %d"), lineNumber(pInfo));
	    return NULL;
	}
        if (*pInfo->curr != ',') {
            tmp = NULL;
        } else {
            pInfo->curr ++;
            tmp = parsePlistObject(pInfo, false);
        }
    }
    foundChar = advanceToNonSpace(pInfo);
    if (!foundChar || *pInfo->curr != ')') {
        __CFPListRelease(array, pInfo);
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Expected terminating ')' for array at line %d"), lineNumber(pInfo));
        return NULL;
    }
    if (pInfo->error) {
        CFRelease(pInfo->error);
        pInfo->error = NULL;
    }
    pInfo->curr ++;
    return array;
}

static CFDictionaryRef parsePlistDictContent(_CFXMLPlistParseInfo *pInfo) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef key = NULL;
    Boolean failedParse = false;
    key = parsePlistString(pInfo, false);
    while (key) {
        CFTypeRef value;
        Boolean foundChar = advanceToNonSpace(pInfo);
        if (!foundChar) {
            CFLog(kCFLogLevelWarning, CFSTR("CFPropertyListCreateFromXMLData(): Unexpected end of file. Missing semicolon or value in dictionary."));
            failedParse = true;
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Missing ';' on line %d"), lineNumber(pInfo));
            break;
        }
	
	if (*pInfo->curr == ';') {
	    /* This is a strings file using the shortcut format */
	    /* although this check here really applies to all plists. */
	    value = CFRetain(key);
	} else if (*pInfo->curr == '=') {
	    pInfo->curr ++;
	    value = parsePlistObject(pInfo, true);
	    if (!value) {
		failedParse = true;
		break;
	    }
	} else {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unexpected ';' or '=' after key at line %d"), lineNumber(pInfo));
	    failedParse = true;
	    break;
	}
	CFDictionarySetValue(dict, key, value);
	__CFPListRelease(key, pInfo);
	key = NULL;
	__CFPListRelease(value, pInfo);
	value = NULL;
	foundChar = advanceToNonSpace(pInfo);
	if (foundChar && *pInfo->curr == ';') {
	    pInfo->curr ++;
	    key = parsePlistString(pInfo, false);
	} else if (true || !foundChar) {
	    CFLog(kCFLogLevelWarning, CFSTR("CFPropertyListCreateFromXMLData(): Old-style plist parser: missing semicolon in dictionary."));
	    failedParse = true;
	    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Missing ';' on line %d"), lineNumber(pInfo));
	}
    }

    if (failedParse) {
        __CFPListRelease(key, pInfo);
        __CFPListRelease(dict, pInfo);
        return NULL;
    }
    if (pInfo->error) {
        CFRelease(pInfo->error);
        pInfo->error = NULL;
    }
    return dict;
}

static CFTypeRef parsePlistDict(_CFXMLPlistParseInfo *pInfo) {
    CFDictionaryRef dict = parsePlistDictContent(pInfo);
    if (!dict) return NULL;
    Boolean foundChar = advanceToNonSpace(pInfo);
    if (!foundChar || *pInfo->curr != '}') {
        __CFPListRelease(dict, pInfo);
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Expected terminating '}' for dictionary at line %d"), lineNumber(pInfo));
        return NULL;
    }
    pInfo->curr ++;
    return dict;
}

CF_INLINE unsigned char fromHexDigit(unsigned char ch) {
    if (isdigit(ch)) return ch - '0';
    if ((ch >= 'a') && (ch <= 'f')) return ch - 'a' + 10;
    if ((ch >= 'A') && (ch <= 'F')) return ch - 'A' + 10;
    return 0xff; // Just choose a large number for the error code
}

/* Gets up to bytesSize bytes from a plist data. Returns number of bytes actually read. Leaves cursor at first non-space, non-hex character.
   -1 is returned for unexpected char, -2 for uneven number of hex digits
*/
static int getDataBytes(_CFXMLPlistParseInfo *pInfo, unsigned char *bytes, int bytesSize) {
    int numBytesRead = 0;
    while ((pInfo->curr < pInfo->end) && (numBytesRead < bytesSize)) {
	int first, second;
	UniChar ch1 = *pInfo->curr;
	if (ch1 == '>') return numBytesRead;  // Meaning we're done
	first = fromHexDigit((unsigned char)ch1);
	if (first != 0xff) {	// If the first char is a hex, then try to read a second hex
	    pInfo->curr++;
	    if (pInfo->curr >= pInfo->end) return -2;   // Error: uneven number of hex digits
	    UniChar ch2 = *pInfo->curr;
	    second = fromHexDigit((unsigned char)ch2);
	    if (second == 0xff) return -2;  // Error: uneven number of hex digits
	    bytes[numBytesRead++] = (first << 4) + second;
	    pInfo->curr++;
	} else if (ch1 == ' ' || ch1 == '\n' || ch1 == '\t' || ch1 == '\r' || ch1 == 0x2028 || ch1 == 0x2029) {
	    pInfo->curr++;
	} else {
	    return -1;  // Error: unexpected character
	}
    }
    return numBytesRead;    // This does likely mean we didn't encounter a '>', but we'll let the caller deal with that
}

#define numBytes 400
static CFTypeRef parsePlistData(_CFXMLPlistParseInfo *pInfo) {
    CFMutableDataRef result = CFDataCreateMutable(pInfo->allocator, 0);

    // Read hex bytes and append them to result
    while (1) {
	unsigned char bytes[numBytes];
	int numBytesRead = getDataBytes(pInfo, bytes, numBytes);
	if (numBytesRead < 0) {
	    __CFPListRelease(result, pInfo);
            switch (numBytesRead) {
                case -2: 
                    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Malformed data byte group at line %d; uneven length"), lineNumber(pInfo));
                    break;
                default: 
                    pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Malformed data byte group at line %d; invalid hex"), lineNumber(pInfo));
                    break;
            }
	    return NULL;
	}
	if (numBytesRead == 0) break;
	CFDataAppendBytes(result, bytes, numBytesRead);
    }

    if (pInfo->error) {
        CFRelease(pInfo->error);
        pInfo->error = NULL;
    }

    if (*(pInfo->curr) == '>') {
        pInfo->curr ++; // Move past '>'
        return result;
    } else {
        __CFPListRelease(result, pInfo);
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Expected terminating '>' for data at line %d"), lineNumber(pInfo));
        return NULL;
    }
}
#undef numBytes

// Returned object is retained; caller must free.
static CFTypeRef parsePlistObject(_CFXMLPlistParseInfo *pInfo, bool requireObject) {
    UniChar ch;
    Boolean foundChar = advanceToNonSpace(pInfo);
    if (!foundChar) {
        if (requireObject) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unexpected EOF while parsing plist"));
        }
        return NULL;
    }
    ch = *(pInfo->curr);
    pInfo->curr ++;
    if (ch == '{') {
        return parsePlistDict(pInfo);
    } else if (ch == '(') {
        return parsePlistArray(pInfo);
    } else if (ch == '<') {
        return parsePlistData(pInfo);
    } else if (ch == '\'' || ch == '\"') {
        return parseQuotedPlistString(pInfo, ch);
    } else if (isValidUnquotedStringCharacter(ch)) {
        pInfo->curr --;
        return parseUnquotedPlistString(pInfo);
    } else {
        pInfo->curr --;  // Must back off the charcter we just read
        if (requireObject) {
            pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Unexpected character '0x%x' at line %d"), ch, lineNumber(pInfo));
        }
        return NULL;
    }
}

static CFTypeRef parseOldStylePropertyListOrStringsFile(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *begin = pInfo->curr;
    CFTypeRef result;
    Boolean foundChar = advanceToNonSpace(pInfo);
    // A file consisting only of whitespace (or empty) is now defined to be an empty dictionary
    if (!foundChar) return CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    result = parsePlistObject(pInfo, true);
    foundChar = advanceToNonSpace(pInfo);
    if (!foundChar) return result;
    if (!result) return NULL;
    if (CFGetTypeID(result) != stringtype) {
        __CFPListRelease(result, pInfo);
        pInfo->error = __CFPropertyListCreateError(kCFPropertyListReadCorruptError, CFSTR("Junk after plist at line %d"), lineNumber(pInfo));
        return NULL;
    }
    __CFPListRelease(result, pInfo);
    // Check for a strings file (looks like a dictionary without the opening/closing curly braces)
    pInfo->curr = begin;
    return parsePlistDictContent(pInfo);
}

#undef isValidUnquotedStringCharacter

static CFArrayRef _arrayDeepImmutableCopy(CFAllocatorRef allocator, CFArrayRef array, CFOptionFlags mutabilityOption) {
    CFArrayRef result = NULL;
    CFIndex i, c = CFArrayGetCount(array);
    if (c == 0) {
        result = CFArrayCreate(allocator, NULL, 0, &kCFTypeArrayCallBacks);
    } else {
        new_cftype_array(values, c);
        CFArrayGetValues(array, CFRangeMake(0, c), values);
        for (i = 0; i < c; i ++) {
            CFTypeRef newValue = CFPropertyListCreateDeepCopy(allocator, values[i], mutabilityOption);
            if (newValue == NULL) {
                break;
            }
            __CFAssignWithWriteBarrier((void **)values + i, (void *)newValue);
        }
        result = (i == c) ? CFArrayCreate(allocator, values, c, &kCFTypeArrayCallBacks) : NULL;
        c = i;
        if (!_CFAllocatorIsGCRefZero(allocator)) {
            for (i = 0; i < c; i ++) CFRelease(values[i]);
        }
        free_cftype_array(values);
    }
    return result;
}

static CFMutableArrayRef _arrayDeepMutableCopy(CFAllocatorRef allocator, CFArrayRef array, CFOptionFlags mutabilityOption) {
    CFIndex i, c = CFArrayGetCount(array);
    CFMutableArrayRef result = CFArrayCreateMutable(allocator, 0, &kCFTypeArrayCallBacks);
    if (result) {
        for (i = 0; i < c; i ++) {
            CFTypeRef newValue = CFPropertyListCreateDeepCopy(allocator, CFArrayGetValueAtIndex(array, i), mutabilityOption);
            if (!newValue) break;
            CFArrayAppendValue(result, newValue);
            if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(newValue);
        }
        if (i != c) {
            if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(result);
            result = NULL;
        }
    }
    return result;
}

CFPropertyListRef CFPropertyListCreateDeepCopy(CFAllocatorRef allocator, CFPropertyListRef propertyList, CFOptionFlags mutabilityOption) {
    initStatics();
    CFPropertyListRef result = NULL;
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): cannot copy a NULL property list", __PRETTY_FUNCTION__);
    __CFAssertIsPList(propertyList);
    CFAssert2(mutabilityOption == kCFPropertyListImmutable || mutabilityOption == kCFPropertyListMutableContainers || mutabilityOption == kCFPropertyListMutableContainersAndLeaves, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, mutabilityOption);
	if (!CFPropertyListIsValid(propertyList, kCFPropertyListBinaryFormat_v1_0)) return NULL;
    
    CFTypeID typeID = CFGetTypeID(propertyList);
    if (typeID == dicttype) {
        CFDictionaryRef dict = (CFDictionaryRef)propertyList;
        Boolean isMutable = (mutabilityOption != kCFPropertyListImmutable);
        CFIndex count = CFDictionaryGetCount(dict);
        if (count == 0) {
            result = isMutable ? CFDictionaryCreateMutable(allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks): CFDictionaryCreate(allocator, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        } else {
            new_cftype_array(keys, 2 * count);
            CFTypeRef *values;
            CFIndex i;
            values = keys+count;
            CFDictionaryGetKeysAndValues(dict, keys, values);
            for (i = 0; i < count; i ++) {
                CFTypeRef newKey = CFStringCreateCopy(allocator, (CFStringRef)keys[i]);
                if (newKey == NULL) {
                    break;
                }
                __CFAssignWithWriteBarrier((void **)keys + i, (void *)newKey);
                CFTypeRef newValue = CFPropertyListCreateDeepCopy(allocator, values[i], mutabilityOption);
                if (newValue == NULL) {
                    if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(keys[i]);
                    break;
                }
                __CFAssignWithWriteBarrier((void **)values + i, (void *)newValue);
            }
            if (i == count) {
                result = isMutable ? CFDictionaryCreateMutable(allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : CFDictionaryCreate(allocator, keys, values, count, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                for (i = 0; i < count; i ++) {
                    if (isMutable) {
                        CFDictionarySetValue((CFMutableDictionaryRef)result, keys[i], values[i]);
                    }
                    if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(keys[i]);
                    if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(values[i]);
                }
            } else {
                result = NULL;
                count = i;
                for (i = 0; i < count; i ++) {
                    if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(keys[i]);
                    if (!_CFAllocatorIsGCRefZero(allocator)) CFRelease(values[i]);
                }
            }
            free_cftype_array(keys);
        }
    } else if (typeID == arraytype) {
        if (mutabilityOption == kCFPropertyListImmutable) {
            result = _arrayDeepImmutableCopy(allocator, (CFArrayRef)propertyList, mutabilityOption);
        } else {
            result = _arrayDeepMutableCopy(allocator, (CFArrayRef)propertyList, mutabilityOption);
        }
    } else if (typeID == datatype) {
        if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
            result = CFDataCreateMutableCopy(allocator, 0, (CFDataRef)propertyList);
        } else {
            result = CFDataCreateCopy(allocator, (CFDataRef)propertyList);
        }
    } else if (typeID == numbertype) {
        // Warning - this will break if byteSize is ever greater than 32
        uint8_t bytes[32];
        CFNumberType numType = CFNumberGetType((CFNumberRef)propertyList);
        CFNumberGetValue((CFNumberRef)propertyList, numType, (void *)bytes);
        result = CFNumberCreate(allocator, numType, (void *)bytes);
    } else if (typeID == booltype) {
        // Booleans are immutable & shared instances
        CFRetain(propertyList);
        result = propertyList;
    } else if (typeID == datetype) {
        // Dates are immutable
        result = CFDateCreate(allocator, CFDateGetAbsoluteTime((CFDateRef)propertyList));
    } else if (typeID == stringtype) {
        if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
            result = CFStringCreateMutableCopy(allocator, 0, (CFStringRef)propertyList);
        } else {
            result = CFStringCreateCopy(allocator, (CFStringRef)propertyList);
        }
    } else {
        CFAssert2(false, __kCFLogAssertion, "%s(): %p is not a property list type", __PRETTY_FUNCTION__, propertyList);
        result = NULL;
    }
    return result;
}

