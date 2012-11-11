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
/*	CFPropertyList.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFSet.h>
#include "CFUtilities.h"
#include "CFStringEncodingConverter.h"
#include "CFInternal.h"
#include <limits.h>
#include <float.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#if defined(__MACH__) || defined(__WIN32__)
#include <ctype.h>
#elif !defined(__WIN32__)
#define isspace(x) ((x)==' ' || (x)=='\n' || (x)=='\f' || (x)=='\r' || (x)=='\t' || (x)=='\v')
#define isdigit(x) ((x) <= '9' && (x) >= '0')
#define isxdigit(x) (((x) <= '9' && (x) >= '0') || ((x) >= 'a' && (x) <= 'f') || ((x) >= 'A' && (x) <= 'F'))
#endif

__private_extern__ bool allowMissingSemi = false;

// Should move this somewhere else
intptr_t _CFDoOperation(intptr_t code, intptr_t subcode1, intptr_t subcode2) {
    switch (code) {
    case 15317: allowMissingSemi = subcode1 ? true : false; break;
    }
    return code;
}

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

// don't allow _CFKeyedArchiverUID here
#define __CFAssertIsPList(cf) CFAssert2(CFGetTypeID(cf) == CFStringGetTypeID() || CFGetTypeID(cf) == CFArrayGetTypeID() || CFGetTypeID(cf) == CFBooleanGetTypeID() || CFGetTypeID(cf) == CFNumberGetTypeID() || CFGetTypeID(cf) == CFDictionaryGetTypeID() || CFGetTypeID(cf) == CFDateGetTypeID() || CFGetTypeID(cf) == CFDataGetTypeID(), __kCFLogAssertion, "%s(): 0x%x not of a property list type", __PRETTY_FUNCTION__, (UInt32)cf);

static bool __CFPropertyListIsValidAux(CFPropertyListRef plist, bool recursive, CFMutableSetRef set, CFPropertyListFormat format);

struct context {
    bool answer;
    CFMutableSetRef set;
    CFPropertyListFormat format;
};

static void __CFPropertyListIsArrayPlistAux(const void *value, void *context) {
    struct context *ctx = (struct context *)context;
    if (!ctx->answer) return;
#if defined(DEBUG)
    if (!value) CFLog(0, CFSTR("CFPropertyListIsValid(): property list arrays cannot contain NULL"));
#endif
    ctx->answer = value && __CFPropertyListIsValidAux(value, true, ctx->set, ctx->format);
}

static void __CFPropertyListIsDictPlistAux(const void *key, const void *value, void *context) {
    struct context *ctx = (struct context *)context;
    if (!ctx->answer) return;
#if defined(DEBUG)
    if (!key) CFLog(0, CFSTR("CFPropertyListIsValid(): property list dictionaries cannot contain NULL keys"));
    if (!value) CFLog(0, CFSTR("CFPropertyListIsValid(): property list dictionaries cannot contain NULL values"));
    if (CFStringGetTypeID() != CFGetTypeID(key)) {
	CFStringRef desc = CFCopyTypeIDDescription(CFGetTypeID(key));
	CFLog(0, CFSTR("CFPropertyListIsValid(): property list dictionaries may only have keys which are CFStrings, not '%@'"), desc);
	CFRelease(desc);
    }
#endif
    ctx->answer = key && value && (CFStringGetTypeID() == CFGetTypeID(key)) && __CFPropertyListIsValidAux(value, true, ctx->set, ctx->format);
}

static bool __CFPropertyListIsValidAux(CFPropertyListRef plist, bool recursive, CFMutableSetRef set, CFPropertyListFormat format) {
    CFTypeID type;
#if defined(DEBUG)
    if (!plist) CFLog(0, CFSTR("CFPropertyListIsValid(): property lists cannot contain NULL"));
#endif
    if (!plist) return false;
    type = CFGetTypeID(plist);
    if (CFStringGetTypeID() == type) return true;
    if (CFDataGetTypeID() == type) return true;
    if (kCFPropertyListOpenStepFormat != format) {
	if (CFBooleanGetTypeID() == type) return true;
	if (CFNumberGetTypeID() == type) return true;
	if (CFDateGetTypeID() == type) return true;
	if (_CFKeyedArchiverUIDGetTypeID() == type) return true;
    }
    if (!recursive && CFArrayGetTypeID() == type) return true;
    if (!recursive && CFDictionaryGetTypeID() == type) return true;
    // at any one invocation of this function, set should contain the objects in the "path" down to this object
#if defined(DEBUG)
    if (CFSetContainsValue(set, plist)) CFLog(0, CFSTR("CFPropertyListIsValid(): property lists cannot contain recursive container references"));
#endif
    if (CFSetContainsValue(set, plist)) return false;
    if (CFArrayGetTypeID() == type) {
	struct context ctx = {true, set, format}; 
	CFSetAddValue(set, plist);
	CFArrayApplyFunction(plist, CFRangeMake(0, CFArrayGetCount(plist)), __CFPropertyListIsArrayPlistAux, &ctx);
	CFSetRemoveValue(set, plist);
	return ctx.answer;
    }
    if (CFDictionaryGetTypeID() == type) {
	struct context ctx = {true, set, format}; 
	CFSetAddValue(set, plist);
	CFDictionaryApplyFunction(plist, __CFPropertyListIsDictPlistAux, &ctx);
	CFSetRemoveValue(set, plist);
	return ctx.answer;
    }
#if defined(DEBUG)
    {
	CFStringRef desc = CFCopyTypeIDDescription(type);
	CFLog(0, CFSTR("CFPropertyListIsValid(): property lists cannot contain objects of type '%@'"), desc);
	CFRelease(desc);
    }
#endif
    return false;
}

Boolean CFPropertyListIsValid(CFPropertyListRef plist, CFPropertyListFormat format) {
    CFMutableSetRef set;
    bool result;
    CFAssert1(plist != NULL, __kCFLogAssertion, "%s(): NULL is not a property list", __PRETTY_FUNCTION__);
    set = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    result = __CFPropertyListIsValidAux(plist, true, set, format);
    CFRelease(set);
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
    CFStringRef errorString;
    CFAllocatorRef allocator;
    UInt32 mutabilityOption;
    CFMutableSetRef stringSet;  // set of all strings involved in this parse; allows us to share non-mutable strings in the returned plist
    CFMutableStringRef tmpString; // Mutable string with external characters that functions can feel free to use as temporary storage as the parse progresses
    Boolean allowNewTypes; // Whether to allow the new types supported by XML property lists, but not by the old, OPENSTEP ASCII property lists (CFNumber, CFBoolean, CFDate)
    char _padding[3];
} _CFXMLPlistParseInfo;

static CFTypeRef parseOldStylePropertyListOrStringsFile(_CFXMLPlistParseInfo *pInfo);



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
        if ((str = CFStringCreateWithCharactersNoCopy(NULL, chars + curLoc, length - curLoc, kCFAllocatorNull))) {
            if ((data = CFStringCreateExternalRepresentation(NULL, str, kCFStringEncodingUTF8, 0))) {
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
    } else if ((data = CFStringCreateExternalRepresentation(NULL, str, kCFStringEncodingUTF8, 0))) {
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
    fStr = CFStringCreateWithFormatAndArguments(NULL, NULL, format, argList);
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
    if (typeID == CFStringGetTypeID()) {
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[STRING_IX], STRING_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">");
	_appendEscapedString(object, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[STRING_IX], STRING_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == _CFKeyedArchiverUIDGetTypeID()) {
	uint64_t v = _CFKeyedArchiverUIDGetValue(object);
	CFNumberRef num = CFNumberCreate(kCFAllocatorSystemDefault, kCFNumberSInt64Type, &v);
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
            _appendIndents(indentation+1, xmlString);
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[KEY_IX], KEY_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">");
	    _appendEscapedString(CFSTR("CF$UID"), xmlString);
            _plistAppendUTF8CString(xmlString, "</");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[KEY_IX], KEY_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">\n");
            _CFAppendXML0(num, indentation+1, xmlString);
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == CFArrayGetTypeID()) {
        UInt32 i, count = CFArrayGetCount(object);
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
            _CFAppendXML0(CFArrayGetValueAtIndex(object, i), indentation+1, xmlString);
        }
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == CFDictionaryGetTypeID()) {
        UInt32 i, count = CFDictionaryGetCount(object);
	CFAllocatorRef allocator = CFGetAllocator(xmlString);
        CFMutableArrayRef keyArray;
        CFTypeRef *keys;
        if (count == 0) {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, "/>\n");
            return;
        }
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
        keys = (CFTypeRef *)CFAllocatorAllocate(allocator, count * sizeof(CFTypeRef), 0);
        CFDictionaryGetKeysAndValues(object, keys, NULL);
        keyArray = CFArrayCreateMutable(allocator, count, &kCFTypeArrayCallBacks);
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
	    _appendEscapedString(key, xmlString);
            _plistAppendUTF8CString(xmlString, "</");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[KEY_IX], KEY_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">\n");
            _CFAppendXML0(CFDictionaryGetValue(object, key), indentation+1, xmlString);
        }
        CFAllocatorDeallocate(allocator, keys);
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DICT_IX], DICT_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == CFDataGetTypeID()) {
        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
        _XMLPlistAppendDataUsingBase64(xmlString, object, indentation);       
        _appendIndents(indentation, xmlString);
        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == CFDateGetTypeID()) {
        // YYYY '-' MM '-' DD 'T' hh ':' mm ':' ss 'Z'
        CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(CFDateGetAbsoluteTime(object), NULL);

        _plistAppendUTF8CString(xmlString, "<");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">");
        
        _plistAppendFormat(xmlString, CFSTR("%04d-%02d-%02dT%02d:%02d:%02dZ"), date.year, date.month, date.day, date.hour, date.minute, (int)date.second);

        _plistAppendUTF8CString(xmlString, "</");
        _plistAppendCharacters(xmlString, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH);
        _plistAppendUTF8CString(xmlString, ">\n");
    } else if (typeID == CFNumberGetTypeID()) {
        if (CFNumberIsFloatType(object)) {
            _plistAppendUTF8CString(xmlString, "<");
            _plistAppendCharacters(xmlString, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH);
            _plistAppendUTF8CString(xmlString, ">");

            if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
                CFStringRef s = __CFNumberCopyFormattingDescriptionAsFloat64(object);
                _plistAppendString(xmlString, s);
                CFRelease(s);
            } else if (CFNumberGetType(object) == kCFNumberFloat64Type || CFNumberGetType(object) == kCFNumberDoubleType) {
                double  doubleVal;
                static CFStringRef doubleFormatString = NULL;
                CFNumberGetValue(object, kCFNumberDoubleType, &doubleVal);
                if (!doubleFormatString) {
                    doubleFormatString = CFStringCreateWithFormat(NULL, NULL, CFSTR("%%.%de"), DBL_DIG);
                }
                _plistAppendFormat(xmlString, doubleFormatString, doubleVal);
            } else {
                float floatVal;
                static CFStringRef floatFormatString = NULL; 
                CFNumberGetValue(object, kCFNumberFloatType, &floatVal);
                if (!floatFormatString) {
                    floatFormatString = CFStringCreateWithFormat(NULL, NULL, CFSTR("%%.%de"), FLT_DIG);
                }
                _plistAppendFormat(xmlString, floatFormatString, floatVal);
            }

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
    } else if (typeID == CFBooleanGetTypeID()) {
        if (CFBooleanGetValue(object)) {
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
    _plistAppendUTF8CString(xml, " PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<");
    _plistAppendCharacters(xml, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH);
    _plistAppendUTF8CString(xml, " version=\"1.0\">\n");

    _CFAppendXML0(propertyList, 0, xml);

    _plistAppendUTF8CString(xml, "</");
    _plistAppendCharacters(xml, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH);
    _plistAppendUTF8CString(xml, ">\n");
}

CFDataRef CFPropertyListCreateXMLData(CFAllocatorRef allocator, CFPropertyListRef propertyList) {
    CFMutableDataRef xml;
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL property list", __PRETTY_FUNCTION__);
    __CFAssertIsPList(propertyList);
    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	if (!CFPropertyListIsValid(propertyList, kCFPropertyListXMLFormat_v1_0)) return NULL;
    }
    xml = CFDataCreateMutable(allocator, 0);
    _CFGenerateXMLPropertyListToData(xml, propertyList);
    return xml;
}

CFDataRef _CFPropertyListCreateXMLDataWithExtras(CFAllocatorRef allocator, CFPropertyListRef propertyList) {
    CFMutableDataRef xml;
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL property list", __PRETTY_FUNCTION__);
    xml = CFDataCreateMutable(allocator, 0);
    _CFGenerateXMLPropertyListToData(xml, propertyList);
    return xml;
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
    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unterminated comment started on line %d"), lineNumber(pInfo));
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
    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected EOF while parsing the processing instruction begun on line %d"), lineNumber(pInfo));
}

// first character should be immediately after the "<!"
static void skipDTD(_CFXMLPlistParseInfo *pInfo) {
    // First pass "DOCTYPE"
    if (pInfo->end - pInfo->curr < DOCTYPE_TAG_LENGTH || !matchString(pInfo->curr, CFXMLPlistTags[DOCTYPE_IX], DOCTYPE_TAG_LENGTH)) {
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Malformed DTD on line %d"), lineNumber(pInfo));
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
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF while parsing DTD", CFStringGetSystemEncoding());
        return;
    }

    // *Sigh* Must parse in-line DTD
    skipInlineDTD(pInfo);
    if (pInfo->errorString)  return;
    skipWhitespace(pInfo);
    if (pInfo->errorString) return;
    if (pInfo->curr < pInfo->end) {
        if (*(pInfo->curr) == '>') {
            pInfo->curr ++;
        } else {
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c on line %d while parsing DTD"), *(pInfo->curr), lineNumber(pInfo));
        }
    } else {
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF while parsing DTD", CFStringGetSystemEncoding());
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
    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected EOF while parsing percent-escape sequence begun on line %d"), lineNumber(pInfo));
}

// First character should be just past '['
static void skipInlineDTD(_CFXMLPlistParseInfo *pInfo) {
    while (!pInfo->errorString && pInfo->curr < pInfo->end) {
        UniChar ch;
        skipWhitespace(pInfo);
        ch = *pInfo->curr;
        if (ch == '%') {
            pInfo->curr ++;
            skipPERef(pInfo);
        } else if (ch == '<') {
            pInfo->curr ++;
            if (pInfo->curr >= pInfo->end) {
                pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF while parsing inline DTD", CFStringGetSystemEncoding());
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
                        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF while parsing inline DTD", CFStringGetSystemEncoding());
                        return;
                 
   }
                    pInfo->curr ++;
                }
            } else {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c on line %d while parsing inline DTD"), ch, lineNumber(pInfo));
                return;
            }
        } else if (ch == ']') {
            pInfo->curr ++;
            return;
        } else {
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c on line %d while parsing inline DTD"), ch, lineNumber(pInfo));
            return;
        }
    }
    if (!pInfo->errorString)
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF while parsing inline DTD", CFStringGetSystemEncoding());
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

static CFDataRef __CFPLDataDecode(_CFXMLPlistParseInfo *pInfo, Boolean mutable) {
    int tmpbufpos = 0;
    int tmpbuflen = 64;
    uint8_t *tmpbuf;
    int numeq = 0;
    int acc = 0;
    int cntr = 0;

    tmpbuf = CFAllocatorAllocate(pInfo->allocator, tmpbuflen, 0);
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
                tmpbuflen <<= 2;
                tmpbuf = CFAllocatorReallocate(pInfo->allocator, tmpbuf, tmpbuflen, 0);
            }
            tmpbuf[tmpbufpos++] = (acc >> 16) & 0xff;
            if (numeq < 2)
                tmpbuf[tmpbufpos++] = (acc >> 8) & 0xff;
            if (numeq < 1)
                tmpbuf[tmpbufpos++] = acc & 0xff;
        }
    }
    if (mutable) {
        CFMutableDataRef result = CFDataCreateMutable(pInfo->allocator, 0);
        CFDataAppendBytes(result, tmpbuf, tmpbufpos);
	CFAllocatorDeallocate(pInfo->allocator, tmpbuf);
        return result;
    } else {
        return CFDataCreateWithBytesNoCopy(pInfo->allocator, (char const *) tmpbuf, tmpbufpos, pInfo->allocator);
    }
}

// content ::== (element | CharData | Reference | CDSect | PI | Comment)*
// In the context of a plist, CharData, Reference and CDSect are not legal (they all resolve to strings).  Skipping whitespace, then, the next character should be '<'.  From there, we figure out which of the three remaining cases we have (element, PI, or Comment).
static CFTypeRef getContentObject(_CFXMLPlistParseInfo *pInfo, Boolean *isKey) {
    if (isKey) *isKey = false;
    while (!pInfo->errorString && pInfo->curr < pInfo->end) {
        skipWhitespace(pInfo);
        if (pInfo->curr >= pInfo->end) {
            pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
            return NULL;
        }
        if (*(pInfo->curr) != '<') {
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c on line %d"), *(pInfo->curr), lineNumber(pInfo));
            return NULL;
        }
        pInfo->curr ++;
        if (pInfo->curr >= pInfo->end) {
            pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
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
                    pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
                    return NULL;
                }
                if (*(pInfo->curr+1) == '-' && *(pInfo->curr+2) == '-') {
                    pInfo->curr += 2;
                    skipXMLComment(pInfo);
                } else {
                    pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
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

static void _catFromMarkToBuf(const UniChar *mark, const UniChar *buf, CFMutableStringRef *string, CFAllocatorRef allocator ) {
    if (!(*string)) {
        *string = CFStringCreateMutable(allocator, 0);
    }
    CFStringAppendCharacters(*string, mark, buf-mark);
}

static void parseCDSect_pl(_CFXMLPlistParseInfo *pInfo, CFMutableStringRef string) {
    const UniChar *end, *begin;
    if (pInfo->end - pInfo->curr < CDSECT_TAG_LENGTH) {
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
        return;
    }
    if (!matchString(pInfo->curr, CFXMLPlistTags[CDSECT_IX], CDSECT_TAG_LENGTH)) {
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered improper CDATA opening at line %d"), lineNumber(pInfo));
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
    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Could not find end of CDATA started on line %d"), lineNumber(pInfo));
}

// Only legal references are {lt, gt, amp, apos, quote, #ddd, #xAAA}
static void parseEntityReference_pl(_CFXMLPlistParseInfo *pInfo, CFMutableStringRef string) {
    int len;
    UniChar ch;
    pInfo->curr ++; // move past the '&';
    len = pInfo->end - pInfo->curr; // how many characters we can safely scan
    if (len < 1) {
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
        return;
    }
    switch (*(pInfo->curr)) {
        case 'l':  // "lt"
            if (len >= 3 && *(pInfo->curr+1) == 't' && *(pInfo->curr+2) == ';') {
                ch = '<';
                pInfo->curr += 3;
                break;
            }
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case 'g': // "gt"
            if (len >= 3 && *(pInfo->curr+1) == 't' && *(pInfo->curr+2) == ';') {
                ch = '>';
                pInfo->curr += 3;
                break;
            }
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case 'a': // "apos" or "amp"
            if (len < 4) {   // Not enough characters for either conversion
                pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
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
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case 'q':  // "quote"
            if (len >= 5 && *(pInfo->curr+1) == 'u' && *(pInfo->curr+2) == 'o' && *(pInfo->curr+3) == 't' && *(pInfo->curr+4) == ';') {
                ch = '\"';
                pInfo->curr += 5;
                break;
            }
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
        case '#':
        {
            uint16_t num = 0;
            Boolean isHex = false;
            if ( len < 4) {  // Not enough characters to make it all fit!  Need at least "&#d;"
                pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
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
                    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c at line %d"), ch, lineNumber(pInfo));
                    return;
                } else if (ch >= 'a' && ch <= 'f') {
                    num += 10 + (ch - 'a');
                } else if (ch >= 'A' && ch <= 'F') {
                    num += 10 + (ch - 'A');
                } else {
                    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c at line %d"), ch, lineNumber(pInfo));
                    return;                    
                }
            }
            pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
            return;
        }
        default:
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unknown ampersand-escape sequence at line %d"), lineNumber(pInfo));
            return;
    }
    CFStringAppendCharacters(string, &ch, 1);
}

extern const void *__CFSetAddValueAndReturn(CFMutableSetRef set, const void *value);

static CFStringRef _uniqueStringForString(_CFXMLPlistParseInfo *pInfo, CFStringRef stringToUnique) {
    if (!pInfo->stringSet) {
        pInfo->stringSet = CFSetCreateMutable(pInfo->allocator, 0, &kCFCopyStringSetCallBacks);
	_CFSetSetCapacity(pInfo->stringSet, 160);	// set capacity high to avoid lots of rehashes, though waste some memory
    }
    return __CFSetAddValueAndReturn(pInfo->stringSet, stringToUnique);
}

extern void _CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex len);

static CFStringRef _uniqueStringForCharacters(_CFXMLPlistParseInfo *pInfo, const UniChar *base, CFIndex length) {
    CFIndex idx;
    uint8_t *ascii, buffer[1024];
    bool isASCII;
    if (!pInfo->stringSet) {
        pInfo->stringSet = CFSetCreateMutable(pInfo->allocator, 0, &kCFCopyStringSetCallBacks);
	_CFSetSetCapacity(pInfo->stringSet, 160);	// set capacity high to avoid lots of rehashes, though waste some memory
    }
    if (pInfo->tmpString) {
	CFStringDelete(pInfo->tmpString, CFRangeMake(0, CFStringGetLength(pInfo->tmpString)));
    } else {
        pInfo->tmpString = CFStringCreateMutable(pInfo->allocator, 0);
	_CFStrSetDesiredCapacity(pInfo->tmpString, 512);
    }
    // This is to avoid having to promote the buffers of all the strings compared against
    // during the set probe; if a Unicode string is passed in, that's what happens.
    isASCII = true;
    for (idx = 0; isASCII && idx < length; idx++) isASCII = isASCII && (base[idx] < 0x80);
    if (isASCII) {
	ascii = (length < (CFIndex)sizeof(buffer)) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, length + 1, 0);
	for (idx = 0; idx < length; idx++) ascii[idx] = (uint8_t)base[idx];
	ascii[length] = '\0';
	CFStringAppendCString(pInfo->tmpString, ascii, kCFStringEncodingASCII);
	if (ascii != buffer) CFAllocatorDeallocate(kCFAllocatorSystemDefault, ascii);
    } else {
	CFStringAppendCharacters(pInfo->tmpString, base, length);
    }
    return __CFSetAddValueAndReturn(pInfo->stringSet, pInfo->tmpString);
}


// String could be comprised of characters, CDSects, or references to one of the "well-known" entities ('<', '>', '&', ''', '"')
// returns a retained object in *string.
static CFStringRef getString(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *mark = pInfo->curr; // At any time in the while loop below, the characters between mark and p have not yet been added to *string
    CFMutableStringRef string = NULL;
    while (!pInfo->errorString && pInfo->curr < pInfo->end) {
        UniChar ch = *(pInfo->curr);
        if (ch == '<') {
            // Could be a CDSect; could be the end of the string
            if (*(pInfo->curr+1) != '!') break; // End of the string
            _catFromMarkToBuf(mark, pInfo->curr, &string, pInfo->allocator);
            parseCDSect_pl(pInfo, string);
            mark = pInfo->curr;
        } else if (ch == '&') {
            _catFromMarkToBuf(mark, pInfo->curr, &string, pInfo->allocator);
            parseEntityReference_pl(pInfo, string);
            mark = pInfo->curr;
        } else {
            pInfo->curr ++;
        }
    }

    if (pInfo->errorString) {
        if (string) CFRelease(string);
        return NULL;
    }
    if (!string) {
        if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
            CFStringRef uniqueString = _uniqueStringForCharacters(pInfo, mark, pInfo->curr-mark);
            CFRetain(uniqueString);
            return uniqueString;
        } else {
            string = CFStringCreateMutable(pInfo->allocator, 0);
            CFStringAppendCharacters(string, mark, pInfo->curr - mark);
            return string;
        }
    }
    _catFromMarkToBuf(mark, pInfo->curr, &string, pInfo->allocator);
    if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
        CFStringRef uniqueString = _uniqueStringForString(pInfo, string);
        CFRetain(uniqueString);
        CFRelease(string);
        return uniqueString;
    }
    return string;
}

static Boolean checkForCloseTag(_CFXMLPlistParseInfo *pInfo, const UniChar *tag, CFIndex tagLen) {
    if (pInfo->end - pInfo->curr < tagLen + 3) {
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
        return false;
    }
    if (*(pInfo->curr) != '<' || *(++pInfo->curr) != '/') {
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c on line %d"), *(pInfo->curr), lineNumber(pInfo));
        return false;
    }
    pInfo->curr ++;
    if (!matchString(pInfo->curr, tag, tagLen)) {
        CFStringRef str = CFStringCreateWithCharactersNoCopy(pInfo->allocator, tag, tagLen, kCFAllocatorNull);
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Close tag on line %d does not match open tag %@"), lineNumber(pInfo), str);
        CFRelease(str);
        return false;
    }
    pInfo->curr += tagLen;
    skipWhitespace(pInfo);
    if (pInfo->curr == pInfo->end) {
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
        return false;
    }
    if (*(pInfo->curr) != '>') {
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected character %c on line %d"), *(pInfo->curr), lineNumber(pInfo));
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
        if (!pInfo->errorString) pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered empty plist tag", CFStringGetSystemEncoding());
        return NULL;
    }
    save = pInfo->curr; // Save this in case the next step fails
    tmp = getContentObject(pInfo, NULL);
    if (tmp) {
        // Got an extra object
        CFRelease(tmp);
        CFRelease(result);
        pInfo->curr = save;
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unexpected element at line %d (plist can only include one object)"), lineNumber(pInfo));
        return NULL;
    }
    if (pInfo->errorString) {
        // Parse failed catastrophically
        CFRelease(result);
        return NULL;
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[PLIST_IX], PLIST_TAG_LENGTH)) {
        return result;
    }
    CFRelease(result);
    return NULL;
}

static int allowImmutableCollections = -1;

static void checkImmutableCollections(void) {
    allowImmutableCollections = (NULL == getenv("CFPropertyListAllowImmutableCollections")) ? 0 : 1;
}

static CFTypeRef parseArrayTag(_CFXMLPlistParseInfo *pInfo) {
    CFMutableArrayRef array = CFArrayCreateMutable(pInfo->allocator, 0, &kCFTypeArrayCallBacks);
    CFTypeRef tmp = getContentObject(pInfo, NULL);
    while (tmp) {
        CFArrayAppendValue(array, tmp);
        CFRelease(tmp);
        tmp = getContentObject(pInfo, NULL);
    }
    if (pInfo->errorString) { // getContentObject encountered a parse error
        CFRelease(array);
        return NULL;
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[ARRAY_IX], ARRAY_TAG_LENGTH)) {
	if (-1 == allowImmutableCollections) checkImmutableCollections();
	if (1 == allowImmutableCollections) {
	    if (pInfo->mutabilityOption == kCFPropertyListImmutable) {
		CFArrayRef newArray = CFArrayCreateCopy(pInfo->allocator, array);
		CFRelease(array);
		array = (CFMutableArrayRef)newArray;
	    }
	}
	return array;
    }
    CFRelease(array);
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
            if (key) CFRelease(key);
            if (dict) CFRelease(dict);
            pInfo->curr = base;
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Found non-key inside <dict> at line %d"), lineNumber(pInfo));
            return NULL;
        }
        value = getContentObject(pInfo, NULL);
        if (!value) {
            if (key) CFRelease(key);
            if (dict) CFRelease(dict);
            if (!pInfo->errorString)
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Value missing for key inside <dict> at line %d"), lineNumber(pInfo));
            return NULL;
        }
	if (NULL == dict) {
	    dict = CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	    _CFDictionarySetCapacity(dict, 10);
	}
        CFDictionarySetValue(dict, key, value);
        CFRelease(key);
        key = NULL;
        CFRelease(value);
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
		if (val && CFGetTypeID(val) == CFNumberGetTypeID()) {
		    CFTypeRef uid;
		    uint32_t v;
		    CFNumberGetValue(val, kCFNumberSInt32Type, &v);
		    uid = (CFTypeRef)_CFKeyedArchiverUIDCreate(pInfo->allocator, v);
		    CFRelease(dict);
		    return uid;
		}
	    }
	    if (-1 == allowImmutableCollections) checkImmutableCollections();
	    if (1 == allowImmutableCollections) {
		if (pInfo->mutabilityOption == kCFPropertyListImmutable) {
		    CFDictionaryRef newDict = CFDictionaryCreateCopy(pInfo->allocator, dict);
		    CFRelease(dict);
		    dict = (CFMutableDictionaryRef)newDict;
		}
	    }
	}
        return dict;
    }
    if (dict) CFRelease(dict);
    return NULL;
}

static CFTypeRef parseDataTag(_CFXMLPlistParseInfo *pInfo) {
    CFDataRef result;
    const UniChar *base = pInfo->curr;
    result = __CFPLDataDecode(pInfo, pInfo->mutabilityOption == kCFPropertyListMutableContainersAndLeaves);
    if (!result) {
        pInfo->curr = base;
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Could not interpret <data> at line %d (should be base64-encoded)"), lineNumber(pInfo));
        return NULL;
    }
    if (checkForCloseTag(pInfo, CFXMLPlistTags[DATA_IX], DATA_TAG_LENGTH)) return result;
    CFRelease(result);
    return NULL;
}

CF_INLINE Boolean read2DigitNumber(_CFXMLPlistParseInfo *pInfo, int8_t *result) {
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
    CFGregorianDate date;
    int8_t num;
    Boolean badForm = false;

    date.year = 0;
    while (pInfo->curr < pInfo->end && isdigit(*pInfo->curr)) {
        date.year = 10*date.year + (*pInfo->curr) - '0';
        pInfo->curr ++;
    }
    if (pInfo->curr >= pInfo->end || *pInfo->curr != '-') {
        badForm = true;
    } else {
        pInfo->curr ++;
    }

    if (!badForm && read2DigitNumber(pInfo, &date.month) && pInfo->curr < pInfo->end && *pInfo->curr == '-') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &date.day) && pInfo->curr < pInfo->end && *pInfo->curr == 'T') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &date.hour) && pInfo->curr < pInfo->end && *pInfo->curr == ':') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &date.minute) && pInfo->curr < pInfo->end && *pInfo->curr == ':') {
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (!badForm && read2DigitNumber(pInfo, &num) && pInfo->curr < pInfo->end && *pInfo->curr == 'Z') {
        date.second = num;
        pInfo->curr ++;
    } else {
        badForm = true;
    }

    if (badForm) {
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Could not interpret <date> at line %d"), lineNumber(pInfo));
        return NULL;
    }
    if (!checkForCloseTag(pInfo, CFXMLPlistTags[DATE_IX], DATE_TAG_LENGTH)) return NULL;
    return CFDateCreate(pInfo->allocator, CFGregorianDateGetAbsoluteTime(date, NULL));
}

static CFTypeRef parseRealTag(_CFXMLPlistParseInfo *pInfo) {
    CFStringRef str = getString(pInfo);
    SInt32 idx, len;
    double val;
    CFNumberRef result;
    CFStringInlineBuffer buf;
    if (!str) {
        if (!pInfo->errorString)
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered empty <real> on line %d"), lineNumber(pInfo));
        return NULL;
    }
    
    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("nan"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberNaN) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("+infinity"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("-infinity"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberNegativeInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("infinity"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("-inf"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberNegativeInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("inf"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
	if (kCFCompareEqualTo == CFStringCompare(str, CFSTR("+inf"), kCFCompareCaseInsensitive)) {
	    CFRelease(str);
	    return (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) ? CFRetain(kCFNumberPositiveInfinity) : NULL;
	}
    }

    len = CFStringGetLength(str);
    CFStringInitInlineBuffer(str, &buf, CFRangeMake(0, len));
    idx = 0;
    if (!__CFStringScanDouble(&buf, NULL, &idx, &val) || idx != len) {
        CFRelease(str);
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered misformatted real on line %d"), lineNumber(pInfo));
        return NULL;
    }
    CFRelease(str);
    result = CFNumberCreate(pInfo->allocator, kCFNumberDoubleType, &val);
    if (checkForCloseTag(pInfo, CFXMLPlistTags[REAL_IX], REAL_TAG_LENGTH)) return result;
    CFRelease(result);
    return NULL;
}

#define GET_CH	if (pInfo->curr == pInfo->end) {	\
			pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Premature end of file after <integer> on line %d"), lineNumber(pInfo)); \
			return NULL;			\
		}					\
		ch = *(pInfo->curr)

static CFTypeRef parseIntegerTag(_CFXMLPlistParseInfo *pInfo) {
    bool isHex = false, isNeg = false, hadLeadingZero = false;
    int64_t value = (int64_t)0;
    UniChar ch = 0;

	// decimal_constant	S*(-|+)?S*[0-9]+		(S == space)
	// hex_constant		S*(-|+)?S*0[xX][0-9a-fA-F]+	(S == space)

    while (pInfo->curr < pInfo->end && __CFIsWhitespace(*(pInfo->curr))) pInfo->curr++;
    GET_CH;
    if ('<' == ch) {
	pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered empty <integer> on line %d"), lineNumber(pInfo));
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
	pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Incomplete <integer> on line %d"), lineNumber(pInfo));
	return NULL;
    }
    while ('<' != ch) {
	int64_t old_value = value;
	switch (ch) {
	case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
	    value = (isHex ? 16 : 10) * value + (ch - '0');
	    break;
	case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
	    if (!isHex) {
		pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Hex digit in non-hex <integer> on line %d"), lineNumber(pInfo));
		return NULL;
	    }
	    value = 16 * value + (ch - 'a' + 10);
	    break;
	case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
	    if (!isHex) {
		pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Hex digit in non-hex <integer> on line %d"), lineNumber(pInfo));
		return NULL;
	    }
	    value = 16 * value + (ch - 'A' + 10);
	    break;
	default:	// other character
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unknown character '%c' (0x%x) in <integer> on line %d"), ch, ch, lineNumber(pInfo));
	    return NULL;
	}
	if (isNeg && LLONG_MIN == value) {
	    // overflow by one when isNeg gives the proper value, if we're done with the number
	    if (pInfo->curr + 1 < pInfo->end && '<' == *(pInfo->curr + 1)) {
		pInfo->curr++;
		isNeg = false;
		break;
	    }
	}
	if (value < old_value) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered <integer> too large to represent on line %d"), lineNumber(pInfo));
	    return NULL;
	}
	pInfo->curr++;
	GET_CH;
    }
    if (!checkForCloseTag(pInfo, CFXMLPlistTags[INTEGER_IX], INTEGER_TAG_LENGTH)) {
	// checkForCloseTag() sets error string
	return NULL;
    }
    if (isNeg) value = -value;
    return CFNumberCreate(pInfo->allocator, kCFNumberSInt64Type, &value);
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
        pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Malformed tag on line %d"), lineNumber(pInfo));
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
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered new tag when expecting only old-style property list objects", CFStringGetSystemEncoding());
        return NULL;
    }

    switch (markerIx) {
        case PLIST_IX:
            if (isEmpty) {
                pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered empty plist tag", CFStringGetSystemEncoding());
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
                CFRelease(str);
                return NULL;
            }
            return str;
        }
        case DATA_IX:
            if (isEmpty) {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered empty <data> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseDataTag(pInfo);
            }
        case DATE_IX:
            if (isEmpty) {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered empty <date> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseDateTag(pInfo);
            }
        case TRUE_IX:
            if (!isEmpty) {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered non-empty <true> tag on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return CFRetain(kCFBooleanTrue);
            }
        case FALSE_IX:
            if (!isEmpty) {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered non-empty <false> tag on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return CFRetain(kCFBooleanFalse);
            }
        case REAL_IX:
            if (isEmpty) {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered empty <real> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseRealTag(pInfo);
            }
        case INTEGER_IX:
            if (isEmpty) {
                pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered empty <integer> on line %d"), lineNumber(pInfo));
                return NULL;
            } else {
                return parseIntegerTag(pInfo);
            }
        default:  {
            CFStringRef markerStr = CFStringCreateWithCharacters(pInfo->allocator, marker, markerLength);
            pInfo->curr = marker;
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Encountered unknown tag %@ on line %d"), markerStr, lineNumber(pInfo));
            CFRelease(markerStr);
            return NULL;
        }
    }
}

static CFTypeRef parseXMLPropertyList(_CFXMLPlistParseInfo *pInfo) {
    while (!pInfo->errorString && pInfo->curr < pInfo->end) {
        UniChar ch;
        skipWhitespace(pInfo);
        if (pInfo->curr+1 >= pInfo->end) {
            pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "No XML content found", CFStringGetSystemEncoding());
            return NULL;
        }
        if (*(pInfo->curr) != '<') {
            pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unexpected character %c at line %d"), *(pInfo->curr), lineNumber(pInfo));
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
    if (!(pInfo->errorString))
        pInfo->errorString = CFStringCreateWithCString(pInfo->allocator, "Encountered unexpected EOF", CFStringGetSystemEncoding());
    return NULL;
}

static CFStringEncoding encodingForXMLData(CFDataRef data, CFStringRef *error) {
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
        encodingName = CFStringCreateWithBytes(NULL, base, len, kCFStringEncodingISOLatin1, false);
        enc = CFStringConvertIANACharSetNameToEncoding(encodingName);
        if (enc != kCFStringEncodingInvalidId) {
            CFRelease(encodingName);
            return enc;
        }

        if (error) {
            *error = CFStringCreateWithFormat(NULL, NULL, CFSTR("Encountered unknown encoding (%@)"), encodingName);
            CFRelease(encodingName);
        }
        return 0;
    }
}

CFTypeRef __CFNastyFile__ = NULL;
static CFSpinLock_t __CFNastyFileLock__ = 0;

void __CFSetNastyFile(CFTypeRef cf) {
    __CFSpinLock(&__CFNastyFileLock__);
    if (__CFNastyFile__) CFRelease(__CFNastyFile__);
    __CFNastyFile__ = cf ? CFRetain(cf) : cf;
    __CFSpinUnlock(&__CFNastyFileLock__);
}

extern bool __CFTryParseBinaryPlist(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags option, CFPropertyListRef *plist, CFStringRef *errorString);
int _CFPropertyListAllowNonUTF8 = 1;

static CFTypeRef _CFPropertyListCreateFromXMLData(CFAllocatorRef allocator, CFDataRef xmlData, CFOptionFlags option, CFStringRef *errorString, Boolean allowNewTypes, CFPropertyListFormat *format) {
    CFStringEncoding encoding;
    CFStringRef xmlString;
    UInt32 length;
    CFPropertyListRef plist;

    if (!xmlData || CFDataGetLength(xmlData) == 0) {
        if (errorString) {
            *errorString = CFSTR("Cannot parse a NULL or zero-length data");
            CFRetain(*errorString); // Caller expects to release
        }
        return NULL;
    }

    if (__CFTryParseBinaryPlist(allocator, xmlData, option, &plist, errorString)) {
	if (format) *format = kCFPropertyListBinaryFormat_v1_0;
	return plist;
    }
    
    allocator = allocator ? allocator : __CFGetDefaultAllocator();
    CFRetain(allocator);
    
    if (errorString) *errorString = NULL;
    encoding = encodingForXMLData(xmlData, errorString); // 0 is an error return, NOT MacRoman.

    if (encoding == 0) {
        // Couldn't find an encoding; encodingForXMLData already set *errorString if necessary
        // Note that encodingForXMLData() will give us the right values for a standard plist, too.
        if (errorString) *errorString = CFStringCreateWithFormat(allocator, NULL, CFSTR("Could not determine the encoding of the XML data"));
        return NULL;
    }

    xmlString = CFStringCreateWithBytes(allocator, CFDataGetBytePtr(xmlData), CFDataGetLength(xmlData), encoding, true);
    if (NULL == xmlString && !_CFExecutableLinkedOnOrAfter(CFSystemVersionMerlot) && _CFPropertyListAllowNonUTF8) {	// conversion failed, probably because not in proper encoding
	static int yanmode = -1;
	if (-1 == yanmode) yanmode = (getenv("YanMode") != NULL);
	if (1 != yanmode && _CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    CFTypeRef f = (__CFNastyFile__) ? (__CFNastyFile__) : CFSTR("(UNKNOWN)");
            if (encoding == kCFStringEncodingUTF8) {
                CFLog(0, CFSTR("\n\tCFPropertyListCreateFromXMLData(): plist parse failed; the data is not proper UTF-8. The file name for this data could be:\n\t%@\n\tThe parser will retry as in 10.2, but the problem should be corrected in the plist."), f);
#if defined(DEBUG)
            } else {
                CFLog(0, CFSTR("\n\tCFPropertyListCreateFromXMLData(): conversion of data failed.\n\tThe file is not in the encoding specified in XML header if XML.\n\tThe file name for this data could be:\n\t\t%@\n."), f);
#endif
            }
	}
        // Call __CFStringCreateImmutableFunnel3() the same way CFStringCreateWithBytes() does, except with the addt'l flag
        if (encoding == kCFStringEncodingUTF8) xmlString = __CFStringCreateImmutableFunnel3(allocator, CFDataGetBytePtr(xmlData), CFDataGetLength(xmlData), kCFStringEncodingUTF8, true, true, false, false, false, (void *)-1  /* ALLOCATORSFREEFUNC */, kCFStringEncodingLenientUTF8Conversion);
    }
    length = xmlString ? CFStringGetLength(xmlString) : 0;

    if (length) {
        _CFXMLPlistParseInfo pInfoBuf;
        _CFXMLPlistParseInfo *pInfo = &pInfoBuf;
        CFTypeRef result;
        UniChar *buf = (UniChar *)CFStringGetCharactersPtr(xmlString);

        if (errorString) *errorString = NULL;
        if (!buf) {
            buf = (UniChar *)CFAllocatorAllocate(allocator, length * sizeof(UniChar), 0);
            CFStringGetCharacters(xmlString, CFRangeMake(0, length), buf);
            CFRelease(xmlString);
            xmlString = NULL;
        }
        pInfo->begin = buf;
        pInfo->end = buf+length;
        pInfo->curr = buf;
        pInfo->allocator = allocator;
        pInfo->errorString = NULL;
        pInfo->stringSet = NULL;
        pInfo->tmpString = NULL;
        pInfo->mutabilityOption = option;
        pInfo->allowNewTypes = allowNewTypes;
        
        // Haven't done anything XML-specific to this point.  However, the encoding we used to translate the bytes should be kept in mind; we used Unicode if the byte-order mark was present; UTF-8 otherwise.  If the system encoding is not UTF-8 or some variant of 7-bit ASCII, we'll be in trouble.....
        result = parseXMLPropertyList(pInfo);
        if (result && format) *format = kCFPropertyListXMLFormat_v1_0;
        if (!result) {
	    CFStringRef err = pInfo->errorString;
            // Reset pInfo so we can try again
            pInfo->curr = pInfo->begin;
            pInfo->errorString = NULL;
            // Try pList
            result = parseOldStylePropertyListOrStringsFile(pInfo);
	    if (result && format) *format = kCFPropertyListOpenStepFormat;
            if (!result) {
		if (errorString) *errorString = CFStringCreateWithFormat(NULL, NULL, CFSTR("XML parser error:\n\t%@\nOld-style plist parser error:\n\t%@\n"), err, pInfo->errorString);
            }
	    if (err) CFRelease(err);
	    if (pInfo->errorString) CFRelease(pInfo->errorString);
        }
        if (xmlString) {
            CFRelease(xmlString);
        } else {
            CFAllocatorDeallocate(allocator, (void *)pInfo->begin);
        }
        if (pInfo->stringSet) CFRelease(pInfo->stringSet);
        if (pInfo->tmpString) CFRelease(pInfo->tmpString);
        CFRelease(allocator);
        return result;
    } else {
        if (errorString)
            *errorString = CFRetain(CFSTR("Conversion of data failed. The file is not UTF-8, or in the encoding specified in XML header if XML."));
        return NULL;
    }
}

CFTypeRef CFPropertyListCreateFromXMLData(CFAllocatorRef allocator, CFDataRef xmlData, CFOptionFlags option, CFStringRef *errorString) {
    CFAssert1(xmlData != NULL, __kCFLogAssertion, "%s(): NULL data not allowed", __PRETTY_FUNCTION__);
    CFAssert2(option == kCFPropertyListImmutable || option == kCFPropertyListMutableContainers || option == kCFPropertyListMutableContainersAndLeaves, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, option);
    return _CFPropertyListCreateFromXMLData(allocator, xmlData, option, errorString, true, NULL);
}


// ========================================================================

//
// Old NeXT-style property lists
//

static CFTypeRef parsePlistObject(_CFXMLPlistParseInfo *pInfo, bool requireObject);

#define isValidUnquotedStringCharacter(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z') || ((x) >= '0' && (x) <= '9') || (x) == '_' || (x) == '$' || (x) == '/' || (x) == ':' || (x) == '.' || (x) == '-')

static void advanceToNonSpace(_CFXMLPlistParseInfo *pInfo) {
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
                return;
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
                return;
	    }
        } else {
            pInfo->curr --;
            return;
        }
    }
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
            UInt32 usedCharLen;
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
            _catFromMarkToBuf(mark, pInfo->curr, &str, pInfo->allocator);
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
        if (str) CFRelease(str);
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->curr = startMark;
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unterminated quoted string starting on line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
    if (!str) {
        if (pInfo->mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
            _catFromMarkToBuf(mark, pInfo->curr, &str, pInfo->allocator);
        } else {
            str = (CFMutableStringRef)_uniqueStringForCharacters(pInfo, mark, pInfo->curr-mark);
            CFRetain(str);
        }
    } else {
        if (mark != pInfo->curr) {
            _catFromMarkToBuf(mark, pInfo->curr, &str, pInfo->allocator);
        }
        if (pInfo->mutabilityOption != kCFPropertyListMutableContainersAndLeaves) {
            CFStringRef uniqueString = _uniqueStringForString(pInfo, str);
            CFRelease(str);
            CFRetain(uniqueString);
            str = (CFMutableStringRef)uniqueString;
        }
    }
    pInfo->curr ++;  // Advance past the quote character before returning.
    if (pInfo->errorString) {
	CFRelease(pInfo->errorString);
	pInfo->errorString = NULL;
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
            CFRetain(str);
            return str;
        } else {
            CFMutableStringRef str = CFStringCreateMutable(pInfo->allocator, 0);
            CFStringAppendCharacters(str, mark, pInfo->curr - mark);
            return str;
        }
    }
    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unexpected EOF"));
    }
    return NULL;
}

static CFStringRef parsePlistString(_CFXMLPlistParseInfo *pInfo, bool requireObject) {
    UniChar ch;
    advanceToNonSpace(pInfo);
    if (pInfo->curr >= pInfo->end) {
	if (requireObject && _CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unexpected EOF while parsing string"));
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
	if (requireObject && _CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Invalid string character at line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
}

static CFTypeRef parsePlistArray(_CFXMLPlistParseInfo *pInfo) {
    CFMutableArrayRef array = CFArrayCreateMutable(pInfo->allocator, 0, &kCFTypeArrayCallBacks);
    CFTypeRef tmp = parsePlistObject(pInfo, false);
    while (tmp) {
        CFArrayAppendValue(array, tmp);
        CFRelease(tmp);
        advanceToNonSpace(pInfo);
        if (*pInfo->curr != ',') {
            tmp = NULL;
        } else {
            pInfo->curr ++;
            tmp = parsePlistObject(pInfo, false);
        }
    }
    advanceToNonSpace(pInfo);
    if (*pInfo->curr != ')') {
        CFRelease(array);
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Expected terminating ')' for array at line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
    if (pInfo->errorString) {
	CFRelease(pInfo->errorString);
	pInfo->errorString = NULL;
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
        advanceToNonSpace(pInfo);
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
	    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
		pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unexpected ';' or '=' after key at line %d"), lineNumber(pInfo));
	    }
	    failedParse = true;
	    break;
	}
	CFDictionarySetValue(dict, key, value);
	CFRelease(key);
	key = NULL;
	CFRelease(value);
	value = NULL;
	advanceToNonSpace(pInfo);
	if (*pInfo->curr == ';') {
	    pInfo->curr ++;
	    key = parsePlistString(pInfo, false);
	} else if (!allowMissingSemi && _CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    static int yanmode = -1;
	    if (-1 == yanmode) yanmode = (getenv("YanMode") != NULL);
	    if (1 != yanmode) {
		CFLog(0, CFSTR("CFPropertyListCreateFromXMLData(): Old-style plist parser: missing semicolon in dictionary."));
		if (__CFNastyFile__) {
		    CFLog(0, CFSTR("CFPropertyListCreateFromXMLData(): The file name for this data might be (or it might not): %@"), __CFNastyFile__);
		}
	    }
	    failedParse = true;
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Missing ';' on line %d"), lineNumber(pInfo));
	} else {
	    // on pre-Jaguar systems, do nothing except silently ignore the rest
	    // of the dictionary, which is what happened on those systems.
	}
    }

    if (failedParse) {
        if (key) CFRelease(key);
        CFRelease(dict);
        return NULL;
    }
    if (pInfo->errorString) {
	CFRelease(pInfo->errorString);
	pInfo->errorString = NULL;
    }
    return dict;
}

static CFTypeRef parsePlistDict(_CFXMLPlistParseInfo *pInfo) {
    CFDictionaryRef dict = parsePlistDictContent(pInfo);
    if (!dict) return NULL;
    advanceToNonSpace(pInfo);
    if (*pInfo->curr != '}') {
        CFRelease(dict);
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Expected terminating '}' for dictionary at line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
    pInfo->curr ++;
    return dict;
}

static unsigned char fromHexDigit(unsigned char ch) {
    if (isdigit(ch)) return ch - '0';
    if ((ch >= 'a') && (ch <= 'f')) return ch - 'a' + 10;
    if ((ch >= 'A') && (ch <= 'F')) return ch - 'A' + 10;
    return 0xff; // Just choose a large number for the error code
}

static CFTypeRef parsePlistData(_CFXMLPlistParseInfo *pInfo) {
    CFStringRef token;
    unsigned length = 0;
    CFMutableDataRef result = CFDataCreateMutable(pInfo->allocator, 0);

    advanceToNonSpace(pInfo);
    while ( (token = parseUnquotedPlistString(pInfo)) ) {
        unsigned tlength = CFStringGetLength(token);
        unsigned char *bytes;
        unsigned idx;
        if (tlength & 1) { // Token must have an even number of characters
            CFRelease(token);
            CFRelease(result);
	    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
		pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Malformed data byte group at line %d; uneven length"), lineNumber(pInfo));
	    }
            return NULL;
        }
        CFDataSetLength(result, length + tlength/2);
        bytes = (unsigned char *) CFDataGetMutableBytePtr(result) + length;
        length += tlength / 2;
        for (idx = 0; idx < tlength; idx += 2) {
            unsigned char hi = fromHexDigit(CFStringGetCharacterAtIndex(token, idx)), lo = fromHexDigit(CFStringGetCharacterAtIndex(token, idx+1));
            if (hi == 0xff || lo == 0xff) {
                CFRelease(token);
                CFRelease(result);
		if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
		    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Malformed data byte group at line %d; invalid hex"), lineNumber(pInfo));
		}
                return NULL;
            }
            *bytes = (hi << 4) + lo;
            bytes++;
        }
        CFRelease(token);
        token = NULL;
        advanceToNonSpace(pInfo);
    }
    if (pInfo->errorString) {
	CFRelease(pInfo->errorString);
	pInfo->errorString = NULL;
    }

    if (*(pInfo->curr) == '>') {
        pInfo->curr ++; // Move past '>'
        return result;
    } else {
        CFRelease(result);
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Expected terminating '>' for data at line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
}

// Returned object is retained; caller must free.
static CFTypeRef parsePlistObject(_CFXMLPlistParseInfo *pInfo, bool requireObject) {
    UniChar ch;
    advanceToNonSpace(pInfo);
    if (pInfo->curr + 1 >= pInfo->end) {
	if (requireObject && _CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unexpected EOF while parsing plist"));
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
	if (requireObject && _CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Unexpected character '0x%x' at line %d"), ch, lineNumber(pInfo));
	}
        return NULL;
    }
}

static CFTypeRef parseOldStylePropertyListOrStringsFile(_CFXMLPlistParseInfo *pInfo) {
    const UniChar *begin = pInfo->curr;
    CFTypeRef result;
    advanceToNonSpace(pInfo);
    // A file consisting only of whitespace (or empty) is now defined to be an empty dictionary
    if (pInfo->curr >= pInfo->end) return CFDictionaryCreateMutable(pInfo->allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    result = parsePlistObject(pInfo, true);
    advanceToNonSpace(pInfo);
    if (pInfo->curr >= pInfo->end) return result;
    if (!result) return NULL;
    if (CFGetTypeID(result) != CFStringGetTypeID()) {
        CFRelease(result);
	if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	    pInfo->errorString = CFStringCreateWithFormat(pInfo->allocator, NULL, CFSTR("Junk after plist at line %d"), lineNumber(pInfo));
	}
        return NULL;
    }
    CFRelease(result);
    // Check for a strings file (looks like a dictionary without the opening/closing curly braces)
    pInfo->curr = begin;
    return parsePlistDictContent(pInfo);
}

#undef isValidUnquotedStringCharacter

static CFArrayRef _arrayDeepImmutableCopy(CFAllocatorRef allocator, CFArrayRef array, CFOptionFlags mutabilityOption) {
    CFArrayRef result = NULL;
    CFIndex i, c = CFArrayGetCount(array);
    CFTypeRef *values;
    if (c == 0) {
        result = CFArrayCreate(allocator, NULL, 0, &kCFTypeArrayCallBacks);
    } else if ((values = CFAllocatorAllocate(allocator, c*sizeof(CFTypeRef), 0)) != NULL) {
        CFArrayGetValues(array, CFRangeMake(0, c), values);
        for (i = 0; i < c; i ++) {
            values[i] = CFPropertyListCreateDeepCopy(allocator, values[i], mutabilityOption);
            if (values[i] == NULL) {
                break;
            }
        }
        result = (i == c) ? CFArrayCreate(allocator, values, c, &kCFTypeArrayCallBacks) : NULL;
        c = i;
        for (i = 0; i < c; i ++) {
            CFRelease(values[i]);
        }
        CFAllocatorDeallocate(allocator, values);
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
            CFRelease(newValue);
        }
        if (i != c) {
            CFRelease(result);
            result = NULL;
        }
    }
    return result;
}

CFPropertyListRef CFPropertyListCreateDeepCopy(CFAllocatorRef allocator, CFPropertyListRef propertyList, CFOptionFlags mutabilityOption) {
    CFTypeID typeID;
    CFPropertyListRef result = NULL;
    CFAssert1(propertyList != NULL, __kCFLogAssertion, "%s(): cannot copy a NULL property list", __PRETTY_FUNCTION__);
    __CFAssertIsPList(propertyList);
    CFAssert2(mutabilityOption == kCFPropertyListImmutable || mutabilityOption == kCFPropertyListMutableContainers || mutabilityOption == kCFPropertyListMutableContainersAndLeaves, __kCFLogAssertion, "%s(): Unrecognized option %d", __PRETTY_FUNCTION__, mutabilityOption);
    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar)) {
	if (!CFPropertyListIsValid(propertyList, kCFPropertyListBinaryFormat_v1_0)) return NULL;
    }
    
    if (allocator == NULL) {
        allocator = CFRetain(__CFGetDefaultAllocator());
    } else {
        CFRetain(allocator);
    }
    
    typeID = CFGetTypeID(propertyList);
    if (typeID == CFDictionaryGetTypeID()) {
        CFDictionaryRef dict = (CFDictionaryRef)propertyList;
        Boolean mutable = (mutabilityOption != kCFPropertyListImmutable);
        CFIndex count = CFDictionaryGetCount(dict);
        CFTypeRef *keys, *values;
        if (count == 0) {
            result = mutable ? CFDictionaryCreateMutable(allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks): CFDictionaryCreate(allocator, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        } else if ((keys = CFAllocatorAllocate(allocator, 2 * count * sizeof(CFTypeRef), 0)) != NULL)   {
            CFIndex i;
            values = keys+count;
            CFDictionaryGetKeysAndValues(dict, keys, values);
            for (i = 0; i < count; i ++) {
                keys[i] = CFStringCreateCopy(allocator, keys[i]);
                if (keys[i] == NULL) {
                    break;
                }
                values[i] = CFPropertyListCreateDeepCopy(allocator, values[i], mutabilityOption);
                if (values[i] == NULL) {
                    CFRelease(keys[i]);
                    break;
                }
            }
            if (i == count) {
                result = mutable ? CFDictionaryCreateMutable(allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : CFDictionaryCreate(allocator, keys, values, count, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                for (i = 0; i < count; i ++) {
                    if (mutable) {
                        CFDictionarySetValue((CFMutableDictionaryRef)result, keys[i], values[i]);
                    }
                    CFRelease(keys[i]);
                    CFRelease(values[i]);
                }
            } else {
                result = NULL;
                count = i;
                for (i = 0; i < count; i ++) {
                    CFRelease(keys[i]);
                    CFRelease(values[i]);
                }
            }
            CFAllocatorDeallocate(allocator, keys);
        } else {
            result = NULL;
        }
    } else if (typeID == CFArrayGetTypeID()) {
        if (mutabilityOption == kCFPropertyListImmutable) {
            result = _arrayDeepImmutableCopy(allocator, (CFArrayRef)propertyList, mutabilityOption);
        } else {
            result = _arrayDeepMutableCopy(allocator, (CFArrayRef)propertyList, mutabilityOption);
        }
    } else if (typeID == CFDataGetTypeID()) {
        if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
            result = CFDataCreateMutableCopy(allocator, 0, (CFDataRef)propertyList);
        } else {
            result = CFDataCreateCopy(allocator, (CFDataRef)propertyList);
        }
    } else if (typeID == CFNumberGetTypeID()) {
        // Warning - this will break if byteSize is ever greater than 16
        uint8_t bytes[16];
        CFNumberType numType = CFNumberGetType((CFNumberRef)propertyList);
        CFNumberGetValue((CFNumberRef)propertyList, numType, (void *)bytes);
        result = CFNumberCreate(allocator, numType, (void *)bytes);
    } else if (typeID == CFBooleanGetTypeID()) {
        // Booleans are immutable & shared instances
        CFRetain(propertyList);
        result = propertyList;
    } else if (typeID == CFDateGetTypeID()) {
        // Dates are immutable
        result = CFDateCreate(allocator, CFDateGetAbsoluteTime((CFDateRef)propertyList));
    } else if (typeID == CFStringGetTypeID()) {
        if (mutabilityOption == kCFPropertyListMutableContainersAndLeaves) {
            result = CFStringCreateMutableCopy(allocator, 0, (CFStringRef)propertyList);
        } else {
            result = CFStringCreateCopy(allocator, (CFStringRef)propertyList);
        }
    } else {
        CFAssert2(false, __kCFLogAssertion, "%s(): 0x%x is not a property list type", __PRETTY_FUNCTION__, propertyList);
        result = NULL;
    }
    CFRelease(allocator);
    return result;
}
