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
/*	CFDateFormatter.c
	Copyright 2002-2003, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFDateFormatter.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFCalendar.h>
#include <CoreFoundation/CFNumber.h>
#include "CFInternal.h"
#include <unicode/udat.h>
#include <math.h>
#include <float.h>

extern UCalendar *__CFCalendarCreateUCalendar(CFStringRef calendarID, CFStringRef localeID, CFTimeZoneRef tz);
static void __CFDateFormatterCustomize(CFDateFormatterRef formatter);

extern const CFStringRef kCFDateFormatterCalendarIdentifier;

#define BUFFER_SIZE 768

struct __CFDateFormatter {
    CFRuntimeBase _base;
    UDateFormat *_df;
    CFLocaleRef _locale;
    CFDateFormatterStyle _timeStyle;
    CFDateFormatterStyle _dateStyle;
    CFStringRef _format;
    CFStringRef _defformat;
    CFStringRef _calendarName;
    CFTimeZoneRef _tz;
    CFDateRef _defaultDate;
};

static CFStringRef __CFDateFormatterCopyDescription(CFTypeRef cf) {
    CFDateFormatterRef formatter = (CFDateFormatterRef)cf;
    return CFStringCreateWithFormat(CFGetAllocator(formatter), NULL, CFSTR("<CFDateFormatter %p [%p]>"), cf, CFGetAllocator(formatter));
}

static void __CFDateFormatterDeallocate(CFTypeRef cf) {
    CFDateFormatterRef formatter = (CFDateFormatterRef)cf;
    if (formatter->_df) udat_close(formatter->_df);
    if (formatter->_locale) CFRelease(formatter->_locale);
    if (formatter->_format) CFRelease(formatter->_format);
    if (formatter->_defformat) CFRelease(formatter->_defformat);
    if (formatter->_calendarName) CFRelease(formatter->_calendarName);
    if (formatter->_tz) CFRelease(formatter->_tz);
    if (formatter->_defaultDate) CFRelease(formatter->_defaultDate);
}

static CFTypeID __kCFDateFormatterTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFDateFormatterClass = {
    0,
    "CFDateFormatter",
    NULL,	// init
    NULL,	// copy
    __CFDateFormatterDeallocate,
    NULL,
    NULL,
    NULL,	// 
    __CFDateFormatterCopyDescription
};

static void __CFDateFormatterInitialize(void) {
    __kCFDateFormatterTypeID = _CFRuntimeRegisterClass(&__CFDateFormatterClass);
}

CFTypeID CFDateFormatterGetTypeID(void) {
    if (_kCFRuntimeNotATypeID == __kCFDateFormatterTypeID) __CFDateFormatterInitialize();
    return __kCFDateFormatterTypeID;
}

CFDateFormatterRef CFDateFormatterCreate(CFAllocatorRef allocator, CFLocaleRef locale, CFDateFormatterStyle dateStyle, CFDateFormatterStyle timeStyle) {
    struct __CFDateFormatter *memory;
    uint32_t size = sizeof(struct __CFDateFormatter) - sizeof(CFRuntimeBase);
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    if (locale) __CFGenericValidateType(locale, CFLocaleGetTypeID());
    memory = (struct __CFDateFormatter *)_CFRuntimeCreateInstance(allocator, CFDateFormatterGetTypeID(), size, NULL);
    if (NULL == memory) {
	return NULL;
    }
    memory->_df = NULL;
    memory->_locale = NULL;
    memory->_format = NULL;
    memory->_defformat = NULL;
    memory->_calendarName = NULL;
    memory->_tz = NULL;
    memory->_defaultDate = NULL;
    if (NULL == locale) locale = CFLocaleGetSystem();
    memory->_dateStyle = dateStyle;
    memory->_timeStyle = timeStyle;
    int32_t udstyle, utstyle;
    switch (dateStyle) {
    case kCFDateFormatterNoStyle: udstyle = UDAT_NONE; break;
    case kCFDateFormatterShortStyle: udstyle = UDAT_SHORT; break;
    case kCFDateFormatterMediumStyle: udstyle = UDAT_MEDIUM; break;
    case kCFDateFormatterLongStyle: udstyle = UDAT_LONG; break;
    case kCFDateFormatterFullStyle: udstyle = UDAT_FULL; break;
    default:
	CFAssert2(0, __kCFLogAssertion, "%s(): unknown date style %d", __PRETTY_FUNCTION__, dateStyle);
	udstyle = UDAT_MEDIUM;
	memory->_dateStyle = kCFDateFormatterMediumStyle;
	break;
    }
    switch (timeStyle) {
    case kCFDateFormatterNoStyle: utstyle = UDAT_NONE; break;
    case kCFDateFormatterShortStyle: utstyle = UDAT_SHORT; break;
    case kCFDateFormatterMediumStyle: utstyle = UDAT_MEDIUM; break;
    case kCFDateFormatterLongStyle: utstyle = UDAT_LONG; break;
    case kCFDateFormatterFullStyle: utstyle = UDAT_FULL; break;
    default:
	CFAssert2(0, __kCFLogAssertion, "%s(): unknown time style %d", __PRETTY_FUNCTION__, timeStyle);
	utstyle = UDAT_MEDIUM;
	memory->_timeStyle = kCFDateFormatterMediumStyle;
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
    UChar ubuffer[BUFFER_SIZE];
    memory->_tz = CFTimeZoneCopyDefault();
    CFStringRef tznam = CFTimeZoneGetName(memory->_tz);
    CFIndex cnt = CFStringGetLength(tznam);
    if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
    CFStringGetCharacters(tznam, CFRangeMake(0, cnt), (UniChar *)ubuffer);
    UErrorCode status = U_ZERO_ERROR;
    memory->_df = udat_open((UDateFormatStyle)utstyle, (UDateFormatStyle)udstyle, cstr, ubuffer, cnt, NULL, 0, &status);
    CFAssert2(memory->_df, __kCFLogAssertion, "%s(): error (%d) creating date formatter", __PRETTY_FUNCTION__, status);
    if (NULL == memory->_df) {
	CFRelease(memory->_tz);
	CFRelease(memory);
	return NULL;
    }
    udat_setLenient(memory->_df, 0);
    if (kCFDateFormatterNoStyle == dateStyle && kCFDateFormatterNoStyle == timeStyle) {
	udat_applyPattern(memory->_df, false, NULL, 0);
    }
    CFTypeRef calident = CFLocaleGetValue(locale, kCFLocaleCalendarIdentifier);
    if (calident && CFEqual(calident, kCFGregorianCalendar)) {
	status = U_ZERO_ERROR;
	udat_set2DigitYearStart(memory->_df, -631152000000.0, &status); // 1950-01-01 00:00:00 GMT
    }
    memory->_locale = locale ? CFLocaleCreateCopy(allocator, locale) : CFLocaleGetSystem();
    __CFDateFormatterCustomize(memory);
    status = U_ZERO_ERROR;
    int32_t ret = udat_toPattern(memory->_df, false, ubuffer, BUFFER_SIZE, &status);
    if (U_SUCCESS(status) && ret <= BUFFER_SIZE) {
	memory->_format = CFStringCreateWithCharacters(allocator, (const UniChar *)ubuffer, ret);
    }
    memory->_defformat = memory->_format ? (CFStringRef)CFRetain(memory->_format) : NULL;
    return (CFDateFormatterRef)memory;
}

extern CFDictionaryRef __CFLocaleGetPrefs(CFLocaleRef locale);

static void __substituteFormatStringFromPrefsDF(CFDateFormatterRef formatter, bool doTime) {
    CFIndex formatStyle = doTime ? formatter->_timeStyle : formatter->_dateStyle;
    CFStringRef prefName = doTime ? CFSTR("AppleICUTimeFormatStrings") : CFSTR("AppleICUDateFormatStrings");
    if (kCFDateFormatterNoStyle != formatStyle) {
	CFStringRef pref = NULL;
	CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);
	CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, prefName) : NULL;
	if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
	    CFStringRef key;
	    switch (formatStyle) {
	    case kCFDateFormatterShortStyle: key = CFSTR("1"); break;
	    case kCFDateFormatterMediumStyle: key = CFSTR("2"); break;
	    case kCFDateFormatterLongStyle: key = CFSTR("3"); break;
	    case kCFDateFormatterFullStyle: key = CFSTR("4"); break;
	    default: key = CFSTR("0"); break;
	    }
	    pref = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)metapref, key);
	}
	if (NULL != pref && CFGetTypeID(pref) == CFStringGetTypeID()) {
	    int32_t icustyle = UDAT_NONE;
	    switch (formatStyle) {
	    case kCFDateFormatterShortStyle: icustyle = UDAT_SHORT; break;
	    case kCFDateFormatterMediumStyle: icustyle = UDAT_MEDIUM; break;
	    case kCFDateFormatterLongStyle: icustyle = UDAT_LONG; break;
	    case kCFDateFormatterFullStyle: icustyle = UDAT_FULL; break;
	    }
	    CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
	    char buffer[BUFFER_SIZE];
	    const char *cstr = CFStringGetCStringPtr(localeName, kCFStringEncodingASCII);
	    if (NULL == cstr) {
		if (CFStringGetCString(localeName, buffer, BUFFER_SIZE, kCFStringEncodingASCII)) cstr = buffer;
	    }
	    UErrorCode status = U_ZERO_ERROR;
	    UDateFormat *df = udat_open((UDateFormatStyle)(doTime ? icustyle : UDAT_NONE), (UDateFormatStyle)(doTime ? UDAT_NONE : icustyle), cstr, NULL, 0, NULL, 0, &status);
	    if (NULL != df) {
		UChar ubuffer[BUFFER_SIZE];
		status = U_ZERO_ERROR;
		int32_t date_len = udat_toPattern(df, false, ubuffer, BUFFER_SIZE, &status);
		if (U_SUCCESS(status) && date_len <= BUFFER_SIZE) {
		    CFStringRef dateString = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar *)ubuffer, date_len);
		    status = U_ZERO_ERROR;
		    int32_t formatter_len = udat_toPattern(formatter->_df, false, ubuffer, BUFFER_SIZE, &status);
		    if (U_SUCCESS(status) && formatter_len <= BUFFER_SIZE) {
			CFMutableStringRef formatString = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
			CFStringAppendCharacters(formatString, (UniChar *)ubuffer, formatter_len);
			// find dateString inside formatString, substitute the pref in that range
			CFRange result;
			if (CFStringFindWithOptions(formatString, dateString, CFRangeMake(0, formatter_len), 0, &result)) {
			    CFStringReplace(formatString, result, pref);
			    int32_t new_len = CFStringGetLength(formatString);
			    STACK_BUFFER_DECL(UChar, new_buffer, new_len);
			    const UChar *new_ustr = (UChar *)CFStringGetCharactersPtr(formatString);
			    if (NULL == new_ustr) {
				CFStringGetCharacters(formatString, CFRangeMake(0, new_len), (UniChar *)new_buffer);
				new_ustr = new_buffer;
				}
			    status = U_ZERO_ERROR;
//			    udat_applyPattern(formatter->_df, false, new_ustr, new_len, &status);
			    udat_applyPattern(formatter->_df, false, new_ustr, new_len);
			}
			CFRelease(formatString);
		    }
		    CFRelease(dateString);
		}
		udat_close(df);
	    }
	}
    }
}

static void __CFDateFormatterApplySymbolPrefs(const void *key, const void *value, void *context) {
    if (CFGetTypeID(key) == CFStringGetTypeID() && CFGetTypeID(value) == CFArrayGetTypeID()) {
	CFDateFormatterRef formatter = (CFDateFormatterRef)context;
	UDateFormatSymbolType sym = (UDateFormatSymbolType)CFStringGetIntValue((CFStringRef)key);
	CFArrayRef array = (CFArrayRef)value;
	CFIndex idx, cnt = CFArrayGetCount(array);
	for (idx = 0; idx < cnt; idx++) {
	    CFStringRef item = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
	    if (CFGetTypeID(item) != CFStringGetTypeID()) continue;
	    CFIndex item_cnt = CFStringGetLength(item);
	    STACK_BUFFER_DECL(UChar, item_buffer, __CFMin(BUFFER_SIZE, item_cnt));
	    UChar *item_ustr = (UChar *)CFStringGetCharactersPtr(item);
	    if (NULL == item_ustr) {
		item_cnt = __CFMin(BUFFER_SIZE, item_cnt);
	        CFStringGetCharacters(item, CFRangeMake(0, item_cnt), (UniChar *)item_buffer);
		item_ustr = item_buffer;
	    }
	    UErrorCode status = U_ZERO_ERROR;
	    udat_setSymbols(formatter->_df, sym, idx, item_ustr, item_cnt, &status);
	}
    }
}

static void __CFDateFormatterCustomize(CFDateFormatterRef formatter) {
    __substituteFormatStringFromPrefsDF(formatter, false);
    __substituteFormatStringFromPrefsDF(formatter, true);
    CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);
    CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUDateTimeSymbols")) : NULL;
    if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
	CFDictionaryApplyFunction((CFDictionaryRef)metapref, __CFDateFormatterApplySymbolPrefs, formatter);
    }
}

CFLocaleRef CFDateFormatterGetLocale(CFDateFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    return formatter->_locale;
}

CFDateFormatterStyle CFDateFormatterGetDateStyle(CFDateFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    return formatter->_dateStyle;
}

CFDateFormatterStyle CFDateFormatterGetTimeStyle(CFDateFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    return formatter->_timeStyle;
}

CFStringRef CFDateFormatterGetFormat(CFDateFormatterRef formatter) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    return formatter->_format;
}

void CFDateFormatterSetFormat(CFDateFormatterRef formatter, CFStringRef formatString) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(formatString, CFStringGetTypeID());
    CFIndex cnt = CFStringGetLength(formatString);
    CFAssert1(cnt <= 1024, __kCFLogAssertion, "%s(): format string too long", __PRETTY_FUNCTION__);
    if (formatter->_format != formatString && cnt <= 1024) {
	STACK_BUFFER_DECL(UChar, ubuffer, cnt);
	const UChar *ustr = (UChar *)CFStringGetCharactersPtr((CFStringRef)formatString);
	if (NULL == ustr) {
	    CFStringGetCharacters(formatString, CFRangeMake(0, cnt), (UniChar *)ubuffer);
	    ustr = ubuffer;
	}
	UErrorCode status = U_ZERO_ERROR;
//	udat_applyPattern(formatter->_df, false, ustr, cnt, &status);
	udat_applyPattern(formatter->_df, false, ustr, cnt);
	if (U_SUCCESS(status)) {
	    if (formatter->_format) CFRelease(formatter->_format);
	    formatter->_format = (CFStringRef)CFStringCreateCopy(CFGetAllocator(formatter), formatString);
	}
    }
}

CFStringRef CFDateFormatterCreateStringWithDate(CFAllocatorRef allocator, CFDateFormatterRef formatter, CFDateRef date) {
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(date, CFDateGetTypeID());
    return CFDateFormatterCreateStringWithAbsoluteTime(allocator, formatter, CFDateGetAbsoluteTime(date));
}

CFStringRef CFDateFormatterCreateStringWithAbsoluteTime(CFAllocatorRef allocator, CFDateFormatterRef formatter, CFAbsoluteTime at) {
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    UChar *ustr = NULL, ubuffer[BUFFER_SIZE];
    UErrorCode status = U_ZERO_ERROR;
    CFIndex used, cnt = BUFFER_SIZE;
    UDate ud = (at + kCFAbsoluteTimeIntervalSince1970) * 1000.0 + 0.5;
    used = udat_format(formatter->_df, ud, ubuffer, cnt, NULL, &status);
    if (status == U_BUFFER_OVERFLOW_ERROR || cnt < used) {
	cnt = used + 1;
	ustr = (UChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UChar) * cnt, 0);
	status = U_ZERO_ERROR;
	used = udat_format(formatter->_df, ud, ustr, cnt, NULL, &status);
    }
    CFStringRef string = NULL;
    if (U_SUCCESS(status)) {
	string = CFStringCreateWithCharacters(allocator, (const UniChar *)(ustr ? ustr : ubuffer), used);
    }
    if (ustr) CFAllocatorDeallocate(kCFAllocatorSystemDefault, ustr);
    return string;
}

CFDateRef CFDateFormatterCreateDateFromString(CFAllocatorRef allocator, CFDateFormatterRef formatter, CFStringRef string, CFRange *rangep) {
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(string, CFStringGetTypeID());
    CFAbsoluteTime at;
    if (CFDateFormatterGetAbsoluteTimeFromString(formatter, string, rangep, &at)) {
	return CFDateCreate(allocator, at);
    }
    return NULL;
}

Boolean CFDateFormatterGetAbsoluteTimeFromString(CFDateFormatterRef formatter, CFStringRef string, CFRange *rangep, CFAbsoluteTime *atp) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(string, CFStringGetTypeID());
    CFRange range = {0, 0};
    if (rangep) {
       range = *rangep;
    } else {
        range.length = CFStringGetLength(string);
    }
    if (1024 < range.length) range.length = 1024;
    const UChar *ustr = (UChar *)CFStringGetCharactersPtr(string);
    STACK_BUFFER_DECL(UChar, ubuffer, (NULL == ustr) ? range.length : 1);
    if (NULL == ustr) {
	CFStringGetCharacters(string, range, (UniChar *)ubuffer);
	ustr = ubuffer;
    } else {
        ustr += range.location;
    }
    UDate udate;
    int32_t dpos = 0;
    UErrorCode status = U_ZERO_ERROR;
    if (formatter->_defaultDate) {
	CFAbsoluteTime at = CFDateGetAbsoluteTime(formatter->_defaultDate);
	udate = (at + kCFAbsoluteTimeIntervalSince1970) * 1000.0;
	UDateFormat *df2 = udat_clone(formatter->_df, &status);
	UCalendar *cal2 = (UCalendar *)udat_getCalendar(df2);
	ucal_setMillis(cal2, udate, &status);
        udat_parseCalendar(formatter->_df, cal2, ustr, range.length, &dpos, &status);
	udate = ucal_getMillis(cal2, &status);
	udat_close(df2);
    } else {
        udate = udat_parse(formatter->_df, ustr, range.length, &dpos, &status);
    }
    if (rangep) rangep->length = dpos;
    if (U_FAILURE(status)) {
	return false;
    }
    if (atp) {
	*atp = (double)udate / 1000.0 - kCFAbsoluteTimeIntervalSince1970;
    }
    return true;
}

#define SET_SYMBOLS_ARRAY(ICU_CODE, INDEX_BASE) \
        __CFGenericValidateType(value, CFArrayGetTypeID()); \
	CFArrayRef array = (CFArrayRef)value; \
	CFIndex idx, cnt = CFArrayGetCount(array); \
	for (idx = 0; idx < cnt; idx++) { \
	    CFStringRef item = (CFStringRef)CFArrayGetValueAtIndex(array, idx); \
	    __CFGenericValidateType(item, CFStringGetTypeID()); \
	    CFIndex item_cnt = CFStringGetLength(item); \
	    STACK_BUFFER_DECL(UChar, item_buffer, __CFMin(BUFFER_SIZE, item_cnt)); \
	    UChar *item_ustr = (UChar *)CFStringGetCharactersPtr(item); \
	    if (NULL == item_ustr) { \
		item_cnt = __CFMin(BUFFER_SIZE, item_cnt); \
	        CFStringGetCharacters(item, CFRangeMake(0, item_cnt), (UniChar *)item_buffer); \
		item_ustr = item_buffer; \
	    } \
	    status = U_ZERO_ERROR; \
	    udat_setSymbols(formatter->_df, ICU_CODE, idx + INDEX_BASE, item_ustr, item_cnt, &status); \
	}

void CFDateFormatterSetProperty(CFDateFormatterRef formatter, CFStringRef key, CFTypeRef value) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(key, CFStringGetTypeID());
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[BUFFER_SIZE];

    if (kCFDateFormatterIsLenient == key) {
	__CFGenericValidateType(value, CFBooleanGetTypeID());
	udat_setLenient(formatter->_df, (kCFBooleanTrue == value));
    } else if (kCFDateFormatterCalendar == key) {
	__CFGenericValidateType(value, CFCalendarGetTypeID());
	CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
	CFDictionaryRef components = CFLocaleCreateComponentsFromLocaleIdentifier(kCFAllocatorSystemDefault, localeName);
	CFMutableDictionaryRef mcomponents = CFDictionaryCreateMutableCopy(kCFAllocatorSystemDefault, 0, components);
	CFDictionarySetValue(mcomponents, kCFLocaleCalendarIdentifier, CFCalendarGetIdentifier((CFCalendarRef)value));
	localeName = CFLocaleCreateLocaleIdentifierFromComponents(kCFAllocatorSystemDefault, mcomponents);
	CFRelease(mcomponents);
	CFRelease(components);
	CFLocaleRef newLocale = CFLocaleCreate(CFGetAllocator(formatter->_locale), localeName);
	CFRelease(localeName);
	CFRelease(formatter->_locale);
	formatter->_locale = newLocale;
	UCalendar *cal = __CFCalendarCreateUCalendar(NULL, CFLocaleGetIdentifier(formatter->_locale), formatter->_tz);
	if (cal) udat_setCalendar(formatter->_df, cal);
	if (cal) ucal_close(cal);
    } else if (kCFDateFormatterCalendarIdentifier == key || kCFDateFormatterCalendarName == key) {
	__CFGenericValidateType(value, CFStringGetTypeID());
	CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
	CFDictionaryRef components = CFLocaleCreateComponentsFromLocaleIdentifier(kCFAllocatorSystemDefault, localeName);
	CFMutableDictionaryRef mcomponents = CFDictionaryCreateMutableCopy(kCFAllocatorSystemDefault, 0, components);
	CFDictionarySetValue(mcomponents, kCFLocaleCalendarIdentifier, value);
	localeName = CFLocaleCreateLocaleIdentifierFromComponents(kCFAllocatorSystemDefault, mcomponents);
	CFRelease(mcomponents);
	CFRelease(components);
	CFLocaleRef newLocale = CFLocaleCreate(CFGetAllocator(formatter->_locale), localeName);
	CFRelease(localeName);
	CFRelease(formatter->_locale);
	formatter->_locale = newLocale;
	UCalendar *cal = __CFCalendarCreateUCalendar(NULL, CFLocaleGetIdentifier(formatter->_locale), formatter->_tz);
	if (cal) udat_setCalendar(formatter->_df, cal);
	if (cal) ucal_close(cal);
    } else if (kCFDateFormatterTimeZone == key) {
	__CFGenericValidateType(value, CFTimeZoneGetTypeID());
	CFTimeZoneRef old = formatter->_tz;
	formatter->_tz = value ? (CFTimeZoneRef)CFRetain(value) : CFTimeZoneCopyDefault();
	if (old) CFRelease(old);
	CFStringRef tznam = CFTimeZoneGetName(formatter->_tz);
	UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
	CFIndex ucnt = CFStringGetLength(tznam);
	if (BUFFER_SIZE < ucnt) ucnt = BUFFER_SIZE;
	CFStringGetCharacters(tznam, CFRangeMake(0, ucnt), (UniChar *)ubuffer);
	ucal_setTimeZone(cal, ubuffer, ucnt, &status);
    } else if (kCFDateFormatterDefaultFormat == key) {
	// read-only attribute
    } else if (kCFDateFormatterTwoDigitStartDate == key) {
        __CFGenericValidateType(value, CFDateGetTypeID());
	CFAbsoluteTime at = CFDateGetAbsoluteTime((CFDateRef)value);
	UDate udate = (at + kCFAbsoluteTimeIntervalSince1970) * 1000.0;
	udat_set2DigitYearStart(formatter->_df, udate, &status);
    } else if (kCFDateFormatterDefaultDate == key) {
        __CFGenericValidateType(value, CFDateGetTypeID());
	CFDateRef old = formatter->_defaultDate;
	formatter->_defaultDate = value ? (CFDateRef)CFRetain(value) : NULL;
	if (old) CFRelease(old);
    } else if (kCFDateFormatterEraSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_ERAS, 0)
    } else if (kCFDateFormatterMonthSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_MONTHS, 0)
    } else if (kCFDateFormatterShortMonthSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_SHORT_MONTHS, 0)
    } else if (kCFDateFormatterWeekdaySymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_WEEKDAYS, 1)
    } else if (kCFDateFormatterShortWeekdaySymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_SHORT_WEEKDAYS, 1)
    } else if (kCFDateFormatterAMSymbol == key) {
        __CFGenericValidateType(value, CFStringGetTypeID());
	CFIndex item_cnt = CFStringGetLength((CFStringRef)value);
	STACK_BUFFER_DECL(UChar, item_buffer, __CFMin(BUFFER_SIZE, item_cnt));
	UChar *item_ustr = (UChar *)CFStringGetCharactersPtr((CFStringRef)value);
	if (NULL == item_ustr) {
	    item_cnt = __CFMin(BUFFER_SIZE, item_cnt);
	    CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, item_cnt), (UniChar *)item_buffer);
	    item_ustr = item_buffer;
	}
	udat_setSymbols(formatter->_df, UDAT_AM_PMS, 0, item_ustr, item_cnt, &status);
    } else if (kCFDateFormatterPMSymbol == key) {
        __CFGenericValidateType(value, CFStringGetTypeID());
	CFIndex item_cnt = CFStringGetLength((CFStringRef)value);
	STACK_BUFFER_DECL(UChar, item_buffer, __CFMin(BUFFER_SIZE, item_cnt));
	UChar *item_ustr = (UChar *)CFStringGetCharactersPtr((CFStringRef)value);
	if (NULL == item_ustr) {
	    item_cnt = __CFMin(BUFFER_SIZE, item_cnt);
	    CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, item_cnt), (UniChar *)item_buffer);
	    item_ustr = item_buffer;
	}
	udat_setSymbols(formatter->_df, UDAT_AM_PMS, 1, item_ustr, item_cnt, &status);
    } else if (kCFDateFormatterGregorianStartDate == key) {
	__CFGenericValidateType(value, CFDateGetTypeID());
	CFAbsoluteTime at = CFDateGetAbsoluteTime((CFDateRef)value);
	UDate udate = (at + kCFAbsoluteTimeIntervalSince1970) * 1000.0;
	UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
	ucal_setGregorianChange(cal, udate, &status);
    } else if (kCFDateFormatterLongEraSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_ERA_NAMES, 0)
    } else if (kCFDateFormatterVeryShortMonthSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_NARROW_MONTHS, 0)
    } else if (kCFDateFormatterStandaloneMonthSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_MONTHS, 0)
    } else if (kCFDateFormatterShortStandaloneMonthSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_MONTHS, 0)
    } else if (kCFDateFormatterVeryShortStandaloneMonthSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_NARROW_MONTHS, 0)
    } else if (kCFDateFormatterVeryShortWeekdaySymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_NARROW_WEEKDAYS, 1)
    } else if (kCFDateFormatterStandaloneWeekdaySymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_WEEKDAYS, 1)
    } else if (kCFDateFormatterShortStandaloneWeekdaySymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_WEEKDAYS, 1)
    } else if (kCFDateFormatterVeryShortStandaloneWeekdaySymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_NARROW_WEEKDAYS, 1)
    } else if (kCFDateFormatterQuarterSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_QUARTERS, 1)
    } else if (kCFDateFormatterShortQuarterSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_SHORT_QUARTERS, 1)
    } else if (kCFDateFormatterStandaloneQuarterSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_QUARTERS, 1)
    } else if (kCFDateFormatterShortStandaloneQuarterSymbols == key) {
	SET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_QUARTERS, 1)
    } else {
	CFAssert3(0, __kCFLogAssertion, "%s(): unknown key %p (%@)", __PRETTY_FUNCTION__, key, key);
    }
}

#define GET_SYMBOLS_ARRAY(ICU_CODE, INDEX_BASE) \
	CFIndex idx, cnt = udat_countSymbols(formatter->_df, ICU_CODE) - INDEX_BASE; \
	STACK_BUFFER_DECL(CFStringRef, strings, cnt); \
	for (idx = 0; idx < cnt; idx++) { \
	    CFStringRef str = NULL; \
	    status = U_ZERO_ERROR; \
	    CFIndex ucnt = udat_getSymbols(formatter->_df, ICU_CODE, idx + INDEX_BASE, ubuffer, BUFFER_SIZE, &status); \
	    if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) { \
		str = CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar *)ubuffer, ucnt); \
	    } \
	    strings[idx] = !str ? (CFStringRef)CFRetain(CFSTR("<error>")) : str; \
	} \
	CFArrayRef array = CFArrayCreate(CFGetAllocator(formatter), (const void **)strings, cnt, &kCFTypeArrayCallBacks); \
	while (cnt--) { \
	    CFRelease(strings[cnt]); \
	} \
	return array;

CFTypeRef CFDateFormatterCopyProperty(CFDateFormatterRef formatter, CFStringRef key) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(key, CFStringGetTypeID());
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[BUFFER_SIZE];

    if (kCFDateFormatterIsLenient == key) {
	return CFRetain(udat_isLenient(formatter->_df) ? kCFBooleanTrue : kCFBooleanFalse);
    } else if (kCFDateFormatterCalendar == key) {
	CFCalendarRef calendar = (CFCalendarRef)CFLocaleGetValue(formatter->_locale, kCFLocaleCalendar);
	return calendar ? CFRetain(calendar) : NULL;
    } else if (kCFDateFormatterCalendarIdentifier == key || kCFDateFormatterCalendarName == key) {
	CFStringRef ident = (CFStringRef)CFLocaleGetValue(formatter->_locale, kCFLocaleCalendarIdentifier);
	return ident ? CFRetain(ident) : NULL;
    } else if (kCFDateFormatterTimeZone == key) {
	return CFRetain(formatter->_tz);
    } else if (kCFDateFormatterDefaultFormat == key) {
	return formatter->_defformat ? CFRetain(formatter->_defformat) : NULL;
    } else if (kCFDateFormatterTwoDigitStartDate == key) {
	UDate udate = udat_get2DigitYearStart(formatter->_df, &status);
        if (U_SUCCESS(status)) {
	    CFAbsoluteTime at = (double)udate / 1000.0 - kCFAbsoluteTimeIntervalSince1970;
            return CFDateCreate(CFGetAllocator(formatter), at);
        }
    } else if (kCFDateFormatterDefaultDate == key) {
	return formatter->_defaultDate ? CFRetain(formatter->_defaultDate) : NULL;
    } else if (kCFDateFormatterEraSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_ERAS, 0)
    } else if (kCFDateFormatterMonthSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_MONTHS, 0)
    } else if (kCFDateFormatterShortMonthSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_SHORT_MONTHS, 0)
    } else if (kCFDateFormatterWeekdaySymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_WEEKDAYS, 1)
    } else if (kCFDateFormatterShortWeekdaySymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_SHORT_WEEKDAYS, 1)
    } else if (kCFDateFormatterAMSymbol == key) {
	CFIndex cnt = udat_countSymbols(formatter->_df, UDAT_AM_PMS);
	if (2 <= cnt) {
	    CFIndex ucnt = udat_getSymbols(formatter->_df, UDAT_AM_PMS, 0, ubuffer, BUFFER_SIZE, &status);
	    if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
		return CFStringCreateWithCharacters(CFGetAllocator(formatter), (UniChar *)ubuffer, ucnt);
	    }
	}	
    } else if (kCFDateFormatterPMSymbol == key) {
	CFIndex cnt = udat_countSymbols(formatter->_df, UDAT_AM_PMS);
	if (2 <= cnt) {
	    CFIndex ucnt = udat_getSymbols(formatter->_df, UDAT_AM_PMS, 1, ubuffer, BUFFER_SIZE, &status);
	    if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
		return CFStringCreateWithCharacters(CFGetAllocator(formatter), (UniChar *)ubuffer, ucnt);
	    }
	}	
    } else if (kCFDateFormatterGregorianStartDate == key) {
	UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
	UDate udate = ucal_getGregorianChange(cal, &status);
        if (U_SUCCESS(status)) {
	    CFAbsoluteTime at = (double)udate / 1000.0 - kCFAbsoluteTimeIntervalSince1970;
            return CFDateCreate(CFGetAllocator(formatter), at);
        }
    } else if (kCFDateFormatterLongEraSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_ERA_NAMES, 0)
    } else if (kCFDateFormatterVeryShortMonthSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_NARROW_MONTHS, 0)
    } else if (kCFDateFormatterStandaloneMonthSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_MONTHS, 0)
    } else if (kCFDateFormatterShortStandaloneMonthSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_MONTHS, 0)
    } else if (kCFDateFormatterVeryShortStandaloneMonthSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_NARROW_MONTHS, 0)
    } else if (kCFDateFormatterVeryShortWeekdaySymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_NARROW_WEEKDAYS, 1)
    } else if (kCFDateFormatterStandaloneWeekdaySymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_WEEKDAYS, 1)
    } else if (kCFDateFormatterShortStandaloneWeekdaySymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_WEEKDAYS, 1)
    } else if (kCFDateFormatterVeryShortStandaloneWeekdaySymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_NARROW_WEEKDAYS, 1)
    } else if (kCFDateFormatterQuarterSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_QUARTERS, 1)
    } else if (kCFDateFormatterShortQuarterSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_SHORT_QUARTERS, 1)
    } else if (kCFDateFormatterStandaloneQuarterSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_QUARTERS, 1)
    } else if (kCFDateFormatterShortStandaloneQuarterSymbols == key) {
	GET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_QUARTERS, 1)
    } else {
	CFAssert3(0, __kCFLogAssertion, "%s(): unknown key %p (%@)", __PRETTY_FUNCTION__, key, key);
    }
    return NULL;
}

CONST_STRING_DECL(kCFDateFormatterIsLenient, "kCFDateFormatterIsLenient")
CONST_STRING_DECL(kCFDateFormatterTimeZone, "kCFDateFormatterTimeZone")
CONST_STRING_DECL(kCFDateFormatterCalendarName, "kCFDateFormatterCalendarName")
CONST_STRING_DECL(kCFDateFormatterCalendarIdentifier, "kCFDateFormatterCalendarIdentifier")
CONST_STRING_DECL(kCFDateFormatterCalendar, "kCFDateFormatterCalendar")
CONST_STRING_DECL(kCFDateFormatterDefaultFormat, "kCFDateFormatterDefaultFormat")

CONST_STRING_DECL(kCFDateFormatterTwoDigitStartDate, "kCFDateFormatterTwoDigitStartDate")
CONST_STRING_DECL(kCFDateFormatterDefaultDate, "kCFDateFormatterDefaultDate")
CONST_STRING_DECL(kCFDateFormatterEraSymbols, "kCFDateFormatterEraSymbols")
CONST_STRING_DECL(kCFDateFormatterMonthSymbols, "kCFDateFormatterMonthSymbols")
CONST_STRING_DECL(kCFDateFormatterShortMonthSymbols, "kCFDateFormatterShortMonthSymbols")
CONST_STRING_DECL(kCFDateFormatterWeekdaySymbols, "kCFDateFormatterWeekdaySymbols")
CONST_STRING_DECL(kCFDateFormatterShortWeekdaySymbols, "kCFDateFormatterShortWeekdaySymbols")
CONST_STRING_DECL(kCFDateFormatterAMSymbol, "kCFDateFormatterAMSymbol")
CONST_STRING_DECL(kCFDateFormatterPMSymbol, "kCFDateFormatterPMSymbol")

CONST_STRING_DECL(kCFDateFormatterLongEraSymbols, "kCFDateFormatterLongEraSymbols")
CONST_STRING_DECL(kCFDateFormatterVeryShortMonthSymbols, "kCFDateFormatterVeryShortMonthSymbols")
CONST_STRING_DECL(kCFDateFormatterStandaloneMonthSymbols, "kCFDateFormatterStandaloneMonthSymbols")
CONST_STRING_DECL(kCFDateFormatterShortStandaloneMonthSymbols, "kCFDateFormatterShortStandaloneMonthSymbols")
CONST_STRING_DECL(kCFDateFormatterVeryShortStandaloneMonthSymbols, "kCFDateFormatterVeryShortStandaloneMonthSymbols")
CONST_STRING_DECL(kCFDateFormatterVeryShortWeekdaySymbols, "kCFDateFormatterVeryShortWeekdaySymbols")
CONST_STRING_DECL(kCFDateFormatterStandaloneWeekdaySymbols, "kCFDateFormatterStandaloneWeekdaySymbols")
CONST_STRING_DECL(kCFDateFormatterShortStandaloneWeekdaySymbols, "kCFDateFormatterShortStandaloneWeekdaySymbols")
CONST_STRING_DECL(kCFDateFormatterVeryShortStandaloneWeekdaySymbols, "kCFDateFormatterVeryShortStandaloneWeekdaySymbols")
CONST_STRING_DECL(kCFDateFormatterQuarterSymbols, "kCFDateFormatterQuarterSymbols")
CONST_STRING_DECL(kCFDateFormatterShortQuarterSymbols, "kCFDateFormatterShortQuarterSymbols")
CONST_STRING_DECL(kCFDateFormatterStandaloneQuarterSymbols, "kCFDateFormatterStandaloneQuarterSymbols")
CONST_STRING_DECL(kCFDateFormatterShortStandaloneQuarterSymbols, "kCFDateFormatterShortStandaloneQuarterSymbols")
CONST_STRING_DECL(kCFDateFormatterGregorianStartDate, "kCFDateFormatterGregorianStartDate")

#undef BUFFER_SIZE

