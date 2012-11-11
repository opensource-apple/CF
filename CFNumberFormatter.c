/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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

/*	CFNumberFormatter.c
	Copyright (c) 2002-2009, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFNumberFormatter.h>
#include "CFInternal.h"
#include "CFLocaleInternal.h"
#include <unicode/unum.h>
#include <unicode/ucurr.h>
#include <math.h>
#include <float.h>

static void __CFNumberFormatterCustomize(CFNumberFormatterRef formatter);
static CFStringRef __CFNumberFormatterCreateCompressedString(CFStringRef inString, Boolean isFormat, CFRange *rangep);
static UErrorCode __CFNumberFormatterApplyPattern(CFNumberFormatterRef formatter, CFStringRef pattern);

#define BUFFER_SIZE 768

struct __CFNumberFormatter {
    CFRuntimeBase _base;
    UNumberFormat *_nf;
    CFLocaleRef _locale;
    CFNumberFormatterStyle _style;
    CFStringRef _format;	// NULL for RBNFs
    CFStringRef _defformat;
    CFStringRef _compformat;
    CFNumberRef _multiplier;
    CFStringRef _zeroSym;
    Boolean _isLenient;
    Boolean _userSetMultiplier;
};

static CFStringRef __CFNumberFormatterCopyDescription(CFTypeRef cf) {
    CFNumberFormatterRef formatter = (CFNumberFormatterRef)cf;
    return CFStringCreateWithFormat(CFGetAllocator(formatter), NULL, CFSTR("<CFNumberFormatter %p [%p]>"), cf, CFGetAllocator(formatter));
}

static void __CFNumberFormatterDeallocate(CFTypeRef cf) {
    CFNumberFormatterRef formatter = (CFNumberFormatterRef)cf;
    if (formatter->_nf) unum_close(formatter->_nf);
    if (formatter->_locale) CFRelease(formatter->_locale);
    if (formatter->_format) CFRelease(formatter->_format);
    if (formatter->_defformat) CFRelease(formatter->_defformat);
    if (formatter->_compformat) CFRelease(formatter->_compformat);
    if (formatter->_multiplier) CFRelease(formatter->_multiplier);
    if (formatter->_zeroSym) CFRelease(formatter->_zeroSym);
}

static CFTypeID __kCFNumberFormatterTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFNumberFormatterClass = {
    0,
    "CFNumberFormatter",
    NULL,	// init
    NULL,	// copy
    __CFNumberFormatterDeallocate,
    NULL,
    NULL,
    NULL,	// 
    __CFNumberFormatterCopyDescription
};

static void __CFNumberFormatterInitialize(void) {
    __kCFNumberFormatterTypeID = _CFRuntimeRegisterClass(&__CFNumberFormatterClass);
}

CFTypeID CFNumberFormatterGetTypeID(void) {
    if (_kCFRuntimeNotATypeID == __kCFNumberFormatterTypeID) __CFNumberFormatterInitialize();
    return __kCFNumberFormatterTypeID;
}

CFNumberFormatterRef CFNumberFormatterCreate(CFAllocatorRef allocator, CFLocaleRef locale, CFNumberFormatterStyle style) {
    struct __CFNumberFormatter *memory;
    uint32_t size = sizeof(struct __CFNumberFormatter) - sizeof(CFRuntimeBase);
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(locale, CFLocaleGetTypeID());
    memory = (struct __CFNumberFormatter *)_CFRuntimeCreateInstance(allocator, CFNumberFormatterGetTypeID(), size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    memory->_nf = NULL;
    memory->_locale = NULL;
    memory->_format = NULL;
    memory->_defformat = NULL;
    memory->_compformat = NULL;
    memory->_multiplier = NULL;
    memory->_zeroSym = NULL;
    memory->_isLenient = false;
    memory->_userSetMultiplier = false;
    if (NULL == locale) locale = CFLocaleGetSystem();
    memory->_style = style;
    uint32_t ustyle;
    switch (style) {
    case kCFNumberFormatterNoStyle: ustyle = UNUM_IGNORE; break;
    case kCFNumberFormatterDecimalStyle: ustyle = UNUM_DECIMAL; break;
    case kCFNumberFormatterCurrencyStyle: ustyle = UNUM_CURRENCY; break;
    case kCFNumberFormatterPercentStyle: ustyle = UNUM_PERCENT; break;
    case kCFNumberFormatterScientificStyle: ustyle = UNUM_SCIENTIFIC; break;
    case kCFNumberFormatterSpellOutStyle: ustyle = UNUM_SPELLOUT; break;
    default:
	CFAssert2(0, __kCFLogAssertion, "%s(): unknown style %d", __PRETTY_FUNCTION__, style);
	ustyle = UNUM_DECIMAL;
	memory->_style = kCFNumberFormatterDecimalStyle;
	break;
    }
    CFStringRef localeName = locale ? CFLocaleGetIdentifier(locale) : CFSTR("");
    char buffer[BUFFER_SIZE];
    const char *cstr = CFStringGetCStringPtr(localeName, kCFStringEncodingASCII);
    if (NULL == cstr) {
	if (CFStringGetCString(localeName, buffer, BUFFER_SIZE, kCFStringEncodingASCII)) cstr = buffer;
    }
    if (NULL == cstr) {
	CFRelease(memory);
	return NULL;
    }
    UErrorCode status = U_ZERO_ERROR;
    memory->_nf = unum_open((UNumberFormatStyle)ustyle, NULL, 0, cstr, NULL, &status);
    CFAssert2(memory->_nf, __kCFLogAssertion, "%s(): error (%d) creating number formatter", __PRETTY_FUNCTION__, status);
    if (NULL == memory->_nf) {
	CFRelease(memory);
	return NULL;
    }
    UChar ubuff[4];
    if (kCFNumberFormatterNoStyle == style) {
        status = U_ZERO_ERROR;
	ubuff[0] = '#'; ubuff[1] = ';'; ubuff[2] = '#';
        unum_applyPattern(memory->_nf, false, ubuff, 3, NULL, &status);
	unum_setAttribute(memory->_nf, UNUM_MAX_INTEGER_DIGITS, 42);
	unum_setAttribute(memory->_nf, UNUM_MAX_FRACTION_DIGITS, 0);
    }
    memory->_locale = locale ? CFLocaleCreateCopy(allocator, locale) : CFLocaleGetSystem();
    __CFNumberFormatterCustomize(memory);
    if (kCFNumberFormatterSpellOutStyle != memory->_style) {
	UChar ubuffer[BUFFER_SIZE];
	status = U_ZERO_ERROR;
	int32_t ret = unum_toPattern(memory->_nf, false, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && ret <= BUFFER_SIZE) {
	    memory->_format = CFStringCreateWithCharacters(allocator, (const UniChar *)ubuffer, ret);
	}
    }
    memory->_defformat = memory->_format ? (CFStringRef)CFRetain(memory->_format) : NULL;
    memory->_compformat = memory->_format ? __CFNumberFormatterCreateCompressedString(memory->_format, true, NULL) : NULL;
    if (kCFNumberFormatterSpellOutStyle != memory->_style) {
	int32_t n = unum_getAttribute(memory->_nf, UNUM_MULTIPLIER);
	if (1 != n) {
	    memory->_multiplier = CFNumberCreate(allocator, kCFNumberSInt32Type, &n);
	    unum_setAttribute(memory->_nf, UNUM_MULTIPLIER, 1);
	}
    }
    unum_setAttribute(memory->_nf, UNUM_LENIENT_PARSE, 0);
    return (CFNumberFormatterRef)memory;
}

extern CFDictionaryRef __CFLocaleGetPrefs(CFLocaleRef locale);

static void __substituteFormatStringFromPrefsNF(CFNumberFormatterRef formatter) {
    CFIndex formatStyle = formatter->_style;
    if (kCFNumberFormatterSpellOutStyle == formatStyle) return;
    CFStringRef prefName = CFSTR("AppleICUNumberFormatStrings");
    if (kCFNumberFormatterNoStyle != formatStyle) {
	CFStringRef pref = NULL;
	CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);
	CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, prefName) : NULL;
	if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
	    CFStringRef key;
	    switch (formatStyle) {
	    case kCFNumberFormatterDecimalStyle: key = CFSTR("1"); break;
	    case kCFNumberFormatterCurrencyStyle: key = CFSTR("2"); break;
	    case kCFNumberFormatterPercentStyle: key = CFSTR("3"); break;
	    case kCFNumberFormatterScientificStyle: key = CFSTR("4"); break;
	    case kCFNumberFormatterSpellOutStyle: key = CFSTR("5"); break;
	    default: key = CFSTR("0"); break;
	    }
	    pref = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)metapref, key);
	}
	if (NULL != pref && CFGetTypeID(pref) == CFStringGetTypeID()) {
	    int32_t icustyle = UNUM_IGNORE;
	    switch (formatStyle) {
	    case kCFNumberFormatterDecimalStyle: icustyle = UNUM_DECIMAL; break;
	    case kCFNumberFormatterCurrencyStyle: icustyle = UNUM_CURRENCY; break;
	    case kCFNumberFormatterPercentStyle: icustyle = UNUM_PERCENT; break;
	    case kCFNumberFormatterScientificStyle: icustyle = UNUM_SCIENTIFIC; break;
	    case kCFNumberFormatterSpellOutStyle: icustyle = UNUM_SPELLOUT; break;
	    }
	    CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
	    char buffer[BUFFER_SIZE];
	    const char *cstr = CFStringGetCStringPtr(localeName, kCFStringEncodingASCII);
	    if (NULL == cstr) {
		if (CFStringGetCString(localeName, buffer, BUFFER_SIZE, kCFStringEncodingASCII)) cstr = buffer;
	    }
	    UErrorCode status = U_ZERO_ERROR;
	    UNumberFormat *nf = unum_open((UNumberFormatStyle)icustyle, NULL, 0, cstr, NULL, &status);
	    if (NULL != nf) {
		UChar ubuffer[BUFFER_SIZE];
		status = U_ZERO_ERROR;
		int32_t number_len = unum_toPattern(nf, false, ubuffer, BUFFER_SIZE, &status);
		if (U_SUCCESS(status) && number_len <= BUFFER_SIZE) {
		    CFStringRef numberString = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar *)ubuffer, number_len);
		    status = U_ZERO_ERROR;
		    int32_t formatter_len = unum_toPattern(formatter->_nf, false, ubuffer, BUFFER_SIZE, &status);
		    if (U_SUCCESS(status) && formatter_len <= BUFFER_SIZE) {
			CFMutableStringRef formatString = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
			CFStringAppendCharacters(formatString, (const UniChar *)ubuffer, formatter_len);
			// find numberString inside formatString, substitute the pref in that range
			CFRange result;
			if (CFStringFindWithOptions(formatString, numberString, CFRangeMake(0, formatter_len), 0, &result)) {
			    CFStringReplace(formatString, result, pref);
			    __CFNumberFormatterApplyPattern(formatter, formatString);
			}
			CFRelease(formatString);
		    }
		    CFRelease(numberString);
		}
		unum_close(nf);
	    }
	}
    }
}

static UniChar __CFNumberFormatterNormalizeCharacter(UniChar c) {
    if (CFCharacterSetIsCharacterMember(CFCharacterSetGetPredefined(kCFCharacterSetWhitespace), c)) {
	return ' ';
    } else {
	return c;
    }
}

/* Attempt to match the unimplemented lenient parsing behavior described at http://www.unicode.org/reports/tr35/#Lenient_Parsing -- specifically any whitespace is ignored that does not exist between two letters or two numbers, and no-break spaces map to spaces. */
static CFStringRef __CFNumberFormatterCreateCompressedString(CFStringRef inString, Boolean isFormat, CFRange *rangep) {
    if (!inString) return NULL;
    CFRange range = { 0, 0 };
    if (rangep) {
	range = *rangep;
    } else {
	range.length = CFStringGetLength(inString);
    }
    CFMutableStringRef outString = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFCharacterSetRef letters = CFCharacterSetGetPredefined(kCFCharacterSetLetter);
    CFCharacterSetRef numbers = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
    UniChar prevCh = 0, nextCh = 0;
    Boolean inQuote = false;
    for (CFIndex in_idx = range.location; in_idx < range.location + range.length; in_idx++) {
        UniChar ch = __CFNumberFormatterNormalizeCharacter(CFStringGetCharacterAtIndex(inString, in_idx));
	nextCh = (in_idx+1 < range.length) ? CFStringGetCharacterAtIndex(inString, in_idx+1) : 0;
	if (isFormat && ch == '\'') inQuote = !inQuote;
	if (inQuote || ch != ' ' || (CFCharacterSetIsCharacterMember(letters, prevCh) && CFCharacterSetIsCharacterMember(letters, nextCh)) || (CFCharacterSetIsCharacterMember(numbers, prevCh) && CFCharacterSetIsCharacterMember(numbers, nextCh))) {
	    CFStringAppendCharacters(outString, &ch, 1);
	    prevCh = ch;
	}
    }
    return outString;
}

static void __CFNumberFormatterApplySymbolPrefs(const void *key, const void *value, void *context) {
    if (CFGetTypeID(key) == CFStringGetTypeID() && CFGetTypeID(value) == CFStringGetTypeID()) {
	CFNumberFormatterRef formatter = (CFNumberFormatterRef)context;
	UNumberFormatSymbol sym = (UNumberFormatSymbol)CFStringGetIntValue((CFStringRef)key);
	CFStringRef item = (CFStringRef)value;
	CFIndex item_cnt = CFStringGetLength(item);
	STACK_BUFFER_DECL(UChar, item_buffer, item_cnt);
	UChar *item_ustr = (UChar *)CFStringGetCharactersPtr(item);
	if (NULL == item_ustr) {
	    CFStringGetCharacters(item, CFRangeMake(0, __CFMin(BUFFER_SIZE, item_cnt)), (UniChar *)item_buffer);
	    item_ustr = item_buffer;
	}
	UErrorCode status = U_ZERO_ERROR;
	unum_setSymbol(formatter->_nf, sym, item_ustr, item_cnt, &status);
   }
}

static UErrorCode __CFNumberFormatterApplyPattern(CFNumberFormatterRef formatter, CFStringRef pattern) {
    CFIndex cnt = CFStringGetLength(pattern);
    STACK_BUFFER_DECL(UChar, ubuffer, cnt);
    const UChar *ustr = (const UChar *)CFStringGetCharactersPtr(pattern);
    if (NULL == ustr) {
	CFStringGetCharacters(pattern, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	ustr = ubuffer;
    }
    UErrorCode status = U_ZERO_ERROR;
    unum_applyPattern(formatter->_nf, false, ustr, cnt, NULL, &status);

    // unum_applyPattern() may have magically changed other attributes based on
    // the contents of the format string; we simply expose that ICU behavior, except
    // for UNUM_MULTIPLIER, which we re-read and reset, like we did at initialization
    // time though any user-set multiplier state takes precedence.
    if (formatter->_userSetMultiplier) {
	unum_setAttribute(formatter->_nf, UNUM_MULTIPLIER, 1);
    } else {
	if (formatter->_multiplier) CFRelease(formatter->_multiplier);
        formatter->_multiplier = NULL;
        int32_t n = unum_getAttribute(formatter->_nf, UNUM_MULTIPLIER);
        if (1 != n) {
	    formatter->_multiplier = CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	    unum_setAttribute(formatter->_nf, UNUM_MULTIPLIER, 1);
        }
    }
    return status;
}

static void __CFNumberFormatterCustomize(CFNumberFormatterRef formatter) {
    __substituteFormatStringFromPrefsNF(formatter);
    CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);
    CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUNumberSymbols")) : NULL;
    if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
	CFDictionaryApplyFunction((CFDictionaryRef)metapref, __CFNumberFormatterApplySymbolPrefs, formatter);
    }
}

CFLocaleRef CFNumberFormatterGetLocale(CFNumberFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    return formatter->_locale;
}

CFNumberFormatterStyle CFNumberFormatterGetStyle(CFNumberFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    return formatter->_style;
}

CFStringRef CFNumberFormatterGetFormat(CFNumberFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    if (kCFNumberFormatterSpellOutStyle == formatter->_style) return NULL;
    UChar ubuffer[BUFFER_SIZE];
    CFStringRef newString = NULL;
    UErrorCode status = U_ZERO_ERROR;
    int32_t ret = unum_toPattern(formatter->_nf, false, ubuffer, BUFFER_SIZE, &status);
    if (U_SUCCESS(status) && ret <= BUFFER_SIZE) {
	newString = CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, ret);
    }
    if (newString && !formatter->_format) {
	formatter->_format = newString;
	if (formatter->_compformat) CFRelease(formatter->_compformat);
	formatter->_compformat = __CFNumberFormatterCreateCompressedString(formatter->_format, true, NULL);
    } else if (newString && !CFEqual(newString, formatter->_format)) {
	CFRelease(formatter->_format);
	formatter->_format = newString;
	if (formatter->_compformat) CFRelease(formatter->_compformat);
	formatter->_compformat = __CFNumberFormatterCreateCompressedString(formatter->_format, true, NULL);
    } else if (newString) {
	CFRelease(newString);
    }
    return formatter->_format;
}

void CFNumberFormatterSetFormat(CFNumberFormatterRef formatter, CFStringRef formatString) {
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    __CFGenericValidateType(formatString, CFStringGetTypeID());
    if (kCFNumberFormatterSpellOutStyle == formatter->_style) return;
    CFIndex cnt = CFStringGetLength(formatString);
    CFAssert1(cnt <= 1024, __kCFLogAssertion, "%s(): format string too long", __PRETTY_FUNCTION__);
    if ((!formatter->_format || !CFEqual(formatter->_format, formatString)) && cnt <= 1024) {
	UErrorCode status = __CFNumberFormatterApplyPattern(formatter, formatString);
	if (U_SUCCESS(status)) {
	    UChar ubuffer2[BUFFER_SIZE];
	    status = U_ZERO_ERROR;
	    int32_t ret = unum_toPattern(formatter->_nf, false, ubuffer2, BUFFER_SIZE, &status);
	    if (U_SUCCESS(status) && ret <= BUFFER_SIZE) {
	        if (formatter->_format) CFRelease(formatter->_format);
		formatter->_format = CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer2, ret);
		if (formatter->_compformat) CFRelease(formatter->_compformat);
		formatter->_compformat = __CFNumberFormatterCreateCompressedString(formatter->_format, true, NULL);
	    }
	}
    }
}

CFStringRef CFNumberFormatterCreateStringWithNumber(CFAllocatorRef allocator, CFNumberFormatterRef formatter, CFNumberRef number) {
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    __CFGenericValidateType(number, CFNumberGetTypeID());
    CFNumberType type = CFNumberGetType(number);
    char buffer[64];
    CFNumberGetValue(number, type, buffer);
    return CFNumberFormatterCreateStringWithValue(allocator, formatter, type, buffer);
}

#define FORMAT(T, FUNC)							\
	T value = *(T *)valuePtr;					\
	if (0 == value && formatter->_zeroSym) { return (CFStringRef)CFRetain(formatter->_zeroSym); }	\
	if (1.0 != multiplier) {					\
		double dummy;						\
		if (modf(multiplier, &dummy) < FLT_EPSILON) { /* float epsilon specifically chosen cuz it is a bit bigger */	\
			value = value * (T)floor(multiplier);		\
		} else {						\
			value = (T)(value * multiplier);		\
		}							\
	}								\
	status = U_ZERO_ERROR;						\
	used = FUNC(formatter->_nf, value, ubuffer, cnt, NULL, &status); \
	if (status == U_BUFFER_OVERFLOW_ERROR || cnt < used) {		\
	    cnt = used + 1;						\
	    ustr = (UChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UChar) * cnt, 0); \
	    status = U_ZERO_ERROR;					\
	    used = FUNC(formatter->_nf, value, ustr, cnt, NULL, &status); \
	}

CFStringRef CFNumberFormatterCreateStringWithValue(CFAllocatorRef allocator, CFNumberFormatterRef formatter, CFNumberType numberType, const void *valuePtr) {
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    double multiplier = 1.0;
    if (formatter->_multiplier) {
	if (!CFNumberGetValue(formatter->_multiplier, kCFNumberFloat64Type, &multiplier)) {
	    multiplier = 1.0;
	}
    }
    UChar *ustr = NULL, ubuffer[BUFFER_SIZE];
    UErrorCode status = U_ZERO_ERROR;
    CFIndex used, cnt = BUFFER_SIZE;
    if (numberType == kCFNumberFloat64Type || numberType == kCFNumberDoubleType) {
	FORMAT(double, unum_formatDouble)
    } else if (numberType == kCFNumberFloat32Type || numberType == kCFNumberFloatType) {
	FORMAT(float, unum_formatDouble)
    } else if (numberType == kCFNumberSInt64Type || numberType == kCFNumberLongLongType) {
	FORMAT(int64_t, unum_formatInt64)
    } else if (numberType == kCFNumberLongType || numberType == kCFNumberCFIndexType) {
#if __LP64__
	FORMAT(int64_t, unum_formatInt64)
#else
	FORMAT(int32_t, unum_formatInt64)
#endif
    } else if (numberType == kCFNumberSInt32Type || numberType == kCFNumberIntType) {
	FORMAT(int32_t, unum_formatInt64)
    } else if (numberType == kCFNumberSInt16Type || numberType == kCFNumberShortType) {
	FORMAT(int16_t, unum_formatInt64)
    } else if (numberType == kCFNumberSInt8Type || numberType == kCFNumberCharType) {
	FORMAT(int8_t, unum_formatInt64)
    } else {
	CFAssert2(0, __kCFLogAssertion, "%s(): unknown CFNumberType (%d)", __PRETTY_FUNCTION__, numberType);
	return NULL;
    }
    CFStringRef string = NULL;
    if (U_SUCCESS(status)) {
	string = CFStringCreateWithCharacters(allocator, ustr ? (const UniChar *)ustr : (const UniChar *)ubuffer, used);
    }
    if (ustr) CFAllocatorDeallocate(kCFAllocatorSystemDefault, ustr);
    return string;
}

#undef FORMAT

CFNumberRef CFNumberFormatterCreateNumberFromString(CFAllocatorRef allocator, CFNumberFormatterRef formatter, CFStringRef string, CFRange *rangep, CFOptionFlags options) {
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    __CFGenericValidateType(string, CFStringGetTypeID());
    CFNumberType type = (options & kCFNumberFormatterParseIntegersOnly) ? kCFNumberSInt64Type : kCFNumberFloat64Type;
    char buffer[16];
    if (CFNumberFormatterGetValueFromString(formatter, string, rangep, type, buffer)) {
	return CFNumberCreate(allocator, type, buffer);
    }
    return NULL;
}

Boolean CFNumberFormatterGetValueFromString(CFNumberFormatterRef formatter, CFStringRef string, CFRange *rangep, CFNumberType numberType, void *valuePtr) {
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    __CFGenericValidateType(string, CFStringGetTypeID());
    CFStringRef stringToParse = formatter->_isLenient ? __CFNumberFormatterCreateCompressedString(string, false, rangep) : (CFStringRef)CFRetain(string);
    CFRange range = {0, 0};
    if(formatter->_isLenient) {
        range.length = CFStringGetLength(stringToParse);
    } else {
        if (rangep) {
            range = *rangep;
        } else {
            range.length = CFStringGetLength(stringToParse);
        }
        // unum_parse chokes on leading whitespace
        CFCharacterSetRef whitespace = CFCharacterSetGetPredefined(kCFCharacterSetWhitespace);
        while(range.length > 0 && CFCharacterSetIsCharacterMember(whitespace, CFStringGetCharacterAtIndex(stringToParse, range.location))) {
            range.location++;
            range.length--;
        }
    }
    Boolean isZero = false;
    if (formatter->_zeroSym) {
	CFStringRef zeroSym = formatter->_isLenient ? __CFNumberFormatterCreateCompressedString(formatter->_zeroSym, false, NULL) : (CFStringRef)CFRetain(formatter->_zeroSym);
	if (kCFCompareEqualTo == CFStringCompare(stringToParse, zeroSym, 0)) {
	    isZero = true;
	}
	CFRelease(zeroSym);
    }
    if (1024 < range.length) range.length = 1024;
    const UChar *ustr = (const UChar *)CFStringGetCharactersPtr(stringToParse);
    STACK_BUFFER_DECL(UChar, ubuffer, (NULL == ustr) ? range.length : 1);
    if (NULL == ustr) {
	CFStringGetCharacters(stringToParse, range, (UniChar *)ubuffer);
	ustr = ubuffer;
    } else if (!formatter->_isLenient) {
	ustr += range.location;
    }
    if (formatter->_isLenient) __CFNumberFormatterApplyPattern(formatter, formatter->_compformat);
    Boolean integerOnly = 1;
    switch (numberType) {
    case kCFNumberSInt8Type: case kCFNumberCharType:
    case kCFNumberSInt16Type: case kCFNumberShortType:
    case kCFNumberSInt32Type: case kCFNumberIntType:
    case kCFNumberLongType: case kCFNumberCFIndexType:
    case kCFNumberSInt64Type: case kCFNumberLongLongType:
	unum_setAttribute(formatter->_nf, UNUM_PARSE_INT_ONLY, 1);
	break;
    default:
	unum_setAttribute(formatter->_nf, UNUM_PARSE_INT_ONLY, 0);
	integerOnly = 0;
	break;
    }
    int32_t dpos = 0;
    UErrorCode status = U_ZERO_ERROR;
    int64_t dreti = 0;
    double dretd = 0.0;
    if (isZero) {
	dpos = rangep ? rangep->length : 0;
    } else {
	if (integerOnly) {
	    dreti = unum_parseInt64(formatter->_nf, ustr, range.length, &dpos, &status);
	} else {
	    dretd = unum_parseDouble(formatter->_nf, ustr, range.length, &dpos, &status);
	}
    }
    if (formatter->_isLenient) {
	if (rangep) {
	    CFIndex uncompEnd = rangep->location + rangep->length;
	    CFIndex uncompIdx = rangep->location;
	    for (CFIndex compIdx = 0; compIdx < dpos && uncompIdx < uncompEnd; compIdx++, uncompIdx++) {
		while (uncompIdx < uncompEnd && ustr[compIdx] != __CFNumberFormatterNormalizeCharacter(CFStringGetCharacterAtIndex(string, uncompIdx))) uncompIdx++;
	    }
	    rangep->length = uncompIdx - rangep->location;
	}
	__CFNumberFormatterApplyPattern(formatter, formatter->_format);
    } else if (rangep) {
        rangep->length = dpos + (range.location - rangep->location);
    }
    CFRelease(stringToParse);
    if (U_FAILURE(status)) {
	return false;
    }
    if (formatter->_multiplier) {
        double multiplier = 1.0;
        if (!CFNumberGetValue(formatter->_multiplier, kCFNumberFloat64Type, &multiplier)) {
            multiplier = 1.0;
        }
        dreti = (int64_t)((double)dreti / multiplier); // integer truncation
        dretd = dretd / multiplier;
    }
    switch (numberType) {
    case kCFNumberSInt8Type: case kCFNumberCharType:
	if (INT8_MIN <= dreti && dreti <= INT8_MAX) {
	    *(int8_t *)valuePtr = (int8_t)dreti;
	    return true;
	}
	break;
    case kCFNumberSInt16Type: case kCFNumberShortType:
	if (INT16_MIN <= dreti && dreti <= INT16_MAX) {
	    *(int16_t *)valuePtr = (int16_t)dreti;
	    return true;
	}
	break;
    case kCFNumberSInt32Type: case kCFNumberIntType:
#if !__LP64__
    case kCFNumberLongType: case kCFNumberCFIndexType:
#endif
	if (INT32_MIN <= dreti && dreti <= INT32_MAX) {
	    *(int32_t *)valuePtr = (int32_t)dreti;
	    return true;
	}
	break;
    case kCFNumberSInt64Type: case kCFNumberLongLongType:
#if __LP64__
    case kCFNumberLongType: case kCFNumberCFIndexType:
#endif
	if (INT64_MIN <= dreti && dreti <= INT64_MAX) {
	    *(int64_t *)valuePtr = (int64_t)dreti;
	    return true;
	}
	break;
    case kCFNumberFloat32Type: case kCFNumberFloatType:
	if (-FLT_MAX <= dretd && dretd <= FLT_MAX) {
	    *(float *)valuePtr = (float)dretd;
	    return true;
	}
	break;
    case kCFNumberFloat64Type: case kCFNumberDoubleType:
	if (-DBL_MAX <= dretd && dretd <= DBL_MAX) {
	    *(double *)valuePtr = (double)dretd;
	    return true;
	}
	break;
    }
    return false;
}

void CFNumberFormatterSetProperty(CFNumberFormatterRef formatter, CFStringRef key, CFTypeRef value) {
    int32_t n;
    double d;
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[BUFFER_SIZE];
    CFIndex cnt;
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    __CFGenericValidateType(key, CFStringGetTypeID());
    if (kCFNumberFormatterCurrencyCodeKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setTextAttribute(formatter->_nf, UNUM_CURRENCY_CODE, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterDecimalSeparatorKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_DECIMAL_SEPARATOR_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterCurrencyDecimalSeparatorKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_MONETARY_SEPARATOR_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterAlwaysShowDecimalSeparatorKey == key) {
	__CFGenericValidateType(value, CFBooleanGetTypeID());
	unum_setAttribute(formatter->_nf, UNUM_DECIMAL_ALWAYS_SHOWN, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterGroupingSeparatorKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_GROUPING_SEPARATOR_SYMBOL, (const UChar *)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterUseGroupingSeparatorKey == key) {
	__CFGenericValidateType(value, CFBooleanGetTypeID());
	unum_setAttribute(formatter->_nf, UNUM_GROUPING_USED, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterPercentSymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_PERCENT_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterZeroSymbolKey == key) {
        __CFGenericValidateType(value, CFStringGetTypeID());
        CFStringRef old = formatter->_zeroSym;
        formatter->_zeroSym = value ? (CFStringRef)CFRetain(value) : NULL;
        if (old) CFRelease(old);
    } else if (kCFNumberFormatterNaNSymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_NAN_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterInfinitySymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_INFINITY_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterMinusSignKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_MINUS_SIGN_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterPlusSignKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_PLUS_SIGN_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterCurrencySymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_CURRENCY_SYMBOL, (const UChar *)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterExponentSymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setSymbol(formatter->_nf, UNUM_EXPONENTIAL_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterMinIntegerDigitsKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_MIN_INTEGER_DIGITS, n);
    } else if (kCFNumberFormatterMaxIntegerDigitsKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_MAX_INTEGER_DIGITS, n);
    } else if (kCFNumberFormatterMinFractionDigitsKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_MIN_FRACTION_DIGITS, n);
    } else if (kCFNumberFormatterMaxFractionDigitsKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_MAX_FRACTION_DIGITS, n);
    } else if (kCFNumberFormatterGroupingSizeKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_GROUPING_SIZE, n);
    } else if (kCFNumberFormatterSecondaryGroupingSizeKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_SECONDARY_GROUPING_SIZE, n);
    } else if (kCFNumberFormatterRoundingModeKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_ROUNDING_MODE, n);
    } else if (kCFNumberFormatterRoundingIncrementKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &d);
	unum_setDoubleAttribute(formatter->_nf, UNUM_ROUNDING_INCREMENT, d);
    } else if (kCFNumberFormatterFormatWidthKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_FORMAT_WIDTH, n);
    } else if (kCFNumberFormatterPaddingPositionKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_PADDING_POSITION, n);
    } else if (kCFNumberFormatterPaddingCharacterKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setTextAttribute(formatter->_nf, UNUM_PADDING_CHARACTER, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterDefaultFormatKey == key) {
	// read-only attribute
    } else if (kCFNumberFormatterMultiplierKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
        CFNumberRef old = formatter->_multiplier;
        formatter->_multiplier = value ? (CFNumberRef)CFRetain(value) : NULL;
	formatter->_userSetMultiplier = value ? true : false;
        if (old) CFRelease(old);
    } else if (kCFNumberFormatterPositivePrefixKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setTextAttribute(formatter->_nf, UNUM_POSITIVE_PREFIX, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterPositiveSuffixKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setTextAttribute(formatter->_nf, UNUM_POSITIVE_SUFFIX, (const UChar *)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterNegativePrefixKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setTextAttribute(formatter->_nf, UNUM_NEGATIVE_PREFIX, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterNegativeSuffixKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	cnt = CFStringGetLength((CFStringRef)value);
	if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
	CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	unum_setTextAttribute(formatter->_nf, UNUM_NEGATIVE_SUFFIX, (const UChar *)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterPerMillSymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_PERMILL_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterInternationalCurrencySymbolKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_INTL_CURRENCY_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterCurrencyGroupingSeparatorKey == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar *)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_MONETARY_GROUPING_SEPARATOR_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterIsLenientKey == key) {
	__CFGenericValidateType(value, CFBooleanGetTypeID());
	formatter->_isLenient = (kCFBooleanTrue == value);
	unum_setAttribute(formatter->_nf, UNUM_LENIENT_PARSE, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterUseSignificantDigitsKey == key) {
	__CFGenericValidateType(value, CFBooleanGetTypeID());
	unum_setAttribute(formatter->_nf, UNUM_SIGNIFICANT_DIGITS_USED, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterMinSignificantDigitsKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_MIN_SIGNIFICANT_DIGITS, n);
    } else if (kCFNumberFormatterMaxSignificantDigitsKey == key) {
	__CFGenericValidateType(value, CFNumberGetTypeID());
	CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
	unum_setAttribute(formatter->_nf, UNUM_MAX_SIGNIFICANT_DIGITS, n);
    } else {
	CFAssert3(0, __kCFLogAssertion, "%s(): unknown key %p (%@)", __PRETTY_FUNCTION__, key, key);
    }
    if (_CFExecutableLinkedOnOrAfter(CFSystemVersionSnowLeopard)) {
        // do a dummy call to CFNumberFormatterGetFormat() after changing an attribute because
        // ICU sometimes changes the pattern due to a property change, and we need to poke
        // unum_toPattern() and also update our own variables
        CFNumberFormatterGetFormat(formatter);
    }
}

CFTypeRef CFNumberFormatterCopyProperty(CFNumberFormatterRef formatter, CFStringRef key) {
    int32_t n;
    double d;
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[BUFFER_SIZE];
    CFIndex cnt;
    __CFGenericValidateType(formatter, CFNumberFormatterGetTypeID());
    __CFGenericValidateType(key, CFStringGetTypeID());
    if (kCFNumberFormatterCurrencyCodeKey == key) {
	cnt = unum_getTextAttribute(formatter->_nf, UNUM_CURRENCY_CODE, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt == 0) {
	    CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
	    char buffer[BUFFER_SIZE];
	    const char *cstr = CFStringGetCStringPtr(localeName, kCFStringEncodingASCII);
	    if (NULL == cstr) {
		if (CFStringGetCString(localeName, buffer, BUFFER_SIZE, kCFStringEncodingASCII)) cstr = buffer;
	    }
	    if (NULL == cstr) {
	        return NULL;
	    }
	    UErrorCode status = U_ZERO_ERROR;
	    UNumberFormat *nf = unum_open(UNUM_CURRENCY, NULL, 0, cstr, NULL, &status);
	    if (NULL != nf) {
		cnt = unum_getTextAttribute(nf, UNUM_CURRENCY_CODE, ubuffer, BUFFER_SIZE, &status);
		unum_close(nf);
	    }
	}
	if (U_SUCCESS(status) && 0 < cnt && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterDecimalSeparatorKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_DECIMAL_SEPARATOR_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterCurrencyDecimalSeparatorKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_MONETARY_SEPARATOR_SYMBOL, (UChar *)ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterAlwaysShowDecimalSeparatorKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_DECIMAL_ALWAYS_SHOWN);
	if (1) {
	    return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
	}
    } else if (kCFNumberFormatterGroupingSeparatorKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_GROUPING_SEPARATOR_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterUseGroupingSeparatorKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_GROUPING_USED);
	if (1) {
	    return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
	}
   } else if (kCFNumberFormatterPercentSymbolKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_PERCENT_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterZeroSymbolKey == key) {
        return formatter->_zeroSym ? CFRetain(formatter->_zeroSym) : NULL;
    } else if (kCFNumberFormatterNaNSymbolKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_NAN_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterInfinitySymbolKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_INFINITY_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterMinusSignKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_MINUS_SIGN_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterPlusSignKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_PLUS_SIGN_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterCurrencySymbolKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_CURRENCY_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterExponentSymbolKey == key) {
	cnt = unum_getSymbol(formatter->_nf, UNUM_EXPONENTIAL_SYMBOL, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterMinIntegerDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_MIN_INTEGER_DIGITS);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterMaxIntegerDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_MAX_INTEGER_DIGITS);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterMinFractionDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_MIN_FRACTION_DIGITS);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterMaxFractionDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_MAX_FRACTION_DIGITS);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterGroupingSizeKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_GROUPING_SIZE);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterSecondaryGroupingSizeKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_SECONDARY_GROUPING_SIZE);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterRoundingModeKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_ROUNDING_MODE);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterRoundingIncrementKey == key) {
	d = unum_getDoubleAttribute(formatter->_nf, UNUM_ROUNDING_INCREMENT);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberDoubleType, &d);
	}
    } else if (kCFNumberFormatterFormatWidthKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_FORMAT_WIDTH);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterPaddingPositionKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_PADDING_POSITION);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterPaddingCharacterKey == key) {
	cnt = unum_getTextAttribute(formatter->_nf, UNUM_PADDING_CHARACTER, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
	}
    } else if (kCFNumberFormatterDefaultFormatKey == key) {
	return formatter->_defformat ? CFRetain(formatter->_defformat) : NULL;
    } else if (kCFNumberFormatterMultiplierKey == key) {
        return formatter->_multiplier ? CFRetain(formatter->_multiplier) : NULL;
    } else if (kCFNumberFormatterPositivePrefixKey == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_POSITIVE_PREFIX, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterPositiveSuffixKey == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_POSITIVE_SUFFIX, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterNegativePrefixKey == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_NEGATIVE_PREFIX, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterNegativeSuffixKey == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_NEGATIVE_SUFFIX, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterPerMillSymbolKey == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_PERMILL_SYMBOL, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterInternationalCurrencySymbolKey == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_INTL_CURRENCY_SYMBOL, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterCurrencyGroupingSeparatorKey == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_MONETARY_GROUPING_SEPARATOR_SYMBOL, ubuffer, BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterIsLenientKey == key) {
	// unum_getAttribute(, UNUM_LENIENT_PARSE) is undefined.
	return CFRetain(formatter->_isLenient ? kCFBooleanTrue : kCFBooleanFalse);
    } else if (kCFNumberFormatterUseSignificantDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_SIGNIFICANT_DIGITS_USED);
	if (1) {
	    return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
	}
    } else if (kCFNumberFormatterMinSignificantDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_MIN_SIGNIFICANT_DIGITS);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else if (kCFNumberFormatterMaxSignificantDigitsKey == key) {
	n = unum_getAttribute(formatter->_nf, UNUM_MAX_SIGNIFICANT_DIGITS);
	if (1) {
	    return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
	}
    } else {
	CFAssert3(0, __kCFLogAssertion, "%s(): unknown key %p (%@)", __PRETTY_FUNCTION__, key, key);
    }
    return NULL;
}


Boolean CFNumberFormatterGetDecimalInfoForCurrencyCode(CFStringRef currencyCode, int32_t *defaultFractionDigits, double *roundingIncrement) {
    UChar ubuffer[4];
    __CFGenericValidateType(currencyCode, CFStringGetTypeID());
    CFAssert1(3 == CFStringGetLength(currencyCode), __kCFLogAssertion, "%s(): currencyCode is not 3 characters", __PRETTY_FUNCTION__);
    CFStringGetCharacters(currencyCode, CFRangeMake(0, 3), (UniChar *)ubuffer);
    ubuffer[3] = 0;
    UErrorCode icuStatus = U_ZERO_ERROR;
    if (defaultFractionDigits) *defaultFractionDigits = ucurr_getDefaultFractionDigits(ubuffer, &icuStatus);
    if (roundingIncrement) *roundingIncrement = ucurr_getRoundingIncrement(ubuffer, &icuStatus);
    if (U_FAILURE(icuStatus))
        return false;
    return (!defaultFractionDigits || 0 <= *defaultFractionDigits) && (!roundingIncrement || 0.0 <= *roundingIncrement);
}

#undef BUFFER_SIZE

