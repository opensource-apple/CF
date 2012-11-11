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

/*        CFDateFormatter.c
        Copyright (c) 2002-2009, Apple Inc. All rights reserved.
        Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFDateFormatter.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFCalendar.h>
#include <CoreFoundation/CFNumber.h>
#include "CFInternal.h"
#include "CFLocaleInternal.h"
#include <unicode/udat.h>
#include <unicode/udatpg.h>
#include <math.h>
#include <float.h>

extern UCalendar *__CFCalendarCreateUCalendar(CFStringRef calendarID, CFStringRef localeID, CFTimeZoneRef tz);
static void __CFDateFormatterCustomize(CFDateFormatterRef formatter);

CF_EXPORT const CFStringRef kCFDateFormatterCalendarIdentifierKey;

#undef CFReleaseIfNotNull
#define CFReleaseIfNotNull(X) if (X) CFRelease(X)

#define BUFFER_SIZE 768

CFStringRef CFDateFormatterCreateDateFormatFromTemplate(CFAllocatorRef allocator, CFStringRef tmplate, CFOptionFlags options, CFLocaleRef locale) {
    if (allocator) __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    if (locale) __CFGenericValidateType(locale, CFLocaleGetTypeID());
    __CFGenericValidateType(tmplate, CFStringGetTypeID());

    CFStringRef localeName = locale ? CFLocaleGetIdentifier(locale) : CFSTR("");
    char buffer[BUFFER_SIZE];
    const char *cstr = CFStringGetCStringPtr(localeName, kCFStringEncodingASCII);
    if (NULL == cstr) {
        if (CFStringGetCString(localeName, buffer, BUFFER_SIZE, kCFStringEncodingASCII)) cstr = buffer;
    }
    if (NULL == cstr) {
        return NULL;
    }

    UErrorCode status = U_ZERO_ERROR;
    UDateTimePatternGenerator *ptg = udatpg_open(cstr, &status);
    if (NULL == ptg || U_FAILURE(status)) {
	return NULL;
    }

    CFIndex jCount = 0; // the only interesting cases are 0, 1, and 2 (adjacent)
    CFRange r = CFStringFind(tmplate, CFSTR("j"), 0);
    if (kCFNotFound != r.location) {
        jCount++;
        if ((r.location + 1 < CFStringGetLength(tmplate)) && ('j' == CFStringGetCharacterAtIndex(tmplate, r.location + 1))) {
            jCount++;
        }
    }

    UChar pattern[BUFFER_SIZE], skel[BUFFER_SIZE], bpat[BUFFER_SIZE];
    CFIndex cnt = CFStringGetLength(tmplate);
    if (BUFFER_SIZE < cnt) cnt = BUFFER_SIZE;
    CFStringGetCharacters(tmplate, CFRangeMake(0, cnt), (UniChar *)pattern);
    int32_t patlen = cnt;

    status = U_ZERO_ERROR;
    int32_t skellen = udatpg_getSkeleton(ptg, pattern, patlen, skel, sizeof(skel) / sizeof(skel[0]), &status);
    if (U_FAILURE(status)) {
	return NULL;
    }

    if ((0 < jCount) && (skellen + jCount < (sizeof(skel) / sizeof(skel[0])))) {
        skel[skellen++] = 'j';
        if (1 < jCount) skel[skellen++] = 'j';
    }

    status = U_ZERO_ERROR;
    int32_t bpatlen = udatpg_getBestPattern(ptg, skel, skellen, bpat, sizeof(bpat) / sizeof(bpat[0]), &status);
    if (U_FAILURE(status)) {
	return NULL;
    }
    udatpg_close(ptg);

    return CFStringCreateWithCharacters(allocator, (const UniChar *)bpat, bpatlen);
}

struct __CFDateFormatter {
    CFRuntimeBase _base;
    UDateFormat *_df;
    CFLocaleRef _locale;
    CFDateFormatterStyle _timeStyle;
    CFDateFormatterStyle _dateStyle;
    CFStringRef _format;
    CFStringRef _defformat;
    struct {
        CFBooleanRef _IsLenient;
	CFBooleanRef _DoesRelativeDateFormatting;
	CFBooleanRef _HasCustomFormat;
        CFTimeZoneRef _TimeZone; 
        CFCalendarRef _Calendar;
        CFStringRef _CalendarName;
        CFDateRef _TwoDigitStartDate;
        CFDateRef _DefaultDate;
        CFDateRef _GregorianStartDate;
        CFArrayRef _EraSymbols;
        CFArrayRef _LongEraSymbols;
        CFArrayRef _MonthSymbols;
        CFArrayRef _ShortMonthSymbols;
        CFArrayRef _VeryShortMonthSymbols;
        CFArrayRef _StandaloneMonthSymbols;
        CFArrayRef _ShortStandaloneMonthSymbols;
        CFArrayRef _VeryShortStandaloneMonthSymbols;
        CFArrayRef _WeekdaySymbols;
        CFArrayRef _ShortWeekdaySymbols;
        CFArrayRef _VeryShortWeekdaySymbols;
        CFArrayRef _StandaloneWeekdaySymbols;
        CFArrayRef _ShortStandaloneWeekdaySymbols;
        CFArrayRef _VeryShortStandaloneWeekdaySymbols;
        CFArrayRef _QuarterSymbols;
        CFArrayRef _ShortQuarterSymbols;
        CFArrayRef _StandaloneQuarterSymbols;
        CFArrayRef _ShortStandaloneQuarterSymbols;
        CFStringRef _AMSymbol;
        CFStringRef _PMSymbol;
    } _property;
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
    CFReleaseIfNotNull(formatter->_property._IsLenient);
    CFReleaseIfNotNull(formatter->_property._DoesRelativeDateFormatting);
    CFReleaseIfNotNull(formatter->_property._TimeZone);
    CFReleaseIfNotNull(formatter->_property._Calendar);
    CFReleaseIfNotNull(formatter->_property._CalendarName);
    CFReleaseIfNotNull(formatter->_property._TwoDigitStartDate);
    CFReleaseIfNotNull(formatter->_property._DefaultDate);
    CFReleaseIfNotNull(formatter->_property._GregorianStartDate);
    CFReleaseIfNotNull(formatter->_property._EraSymbols);
    CFReleaseIfNotNull(formatter->_property._LongEraSymbols);
    CFReleaseIfNotNull(formatter->_property._MonthSymbols);
    CFReleaseIfNotNull(formatter->_property._ShortMonthSymbols);
    CFReleaseIfNotNull(formatter->_property._VeryShortMonthSymbols);
    CFReleaseIfNotNull(formatter->_property._StandaloneMonthSymbols);
    CFReleaseIfNotNull(formatter->_property._ShortStandaloneMonthSymbols);
    CFReleaseIfNotNull(formatter->_property._VeryShortStandaloneMonthSymbols);
    CFReleaseIfNotNull(formatter->_property._WeekdaySymbols);
    CFReleaseIfNotNull(formatter->_property._ShortWeekdaySymbols);
    CFReleaseIfNotNull(formatter->_property._VeryShortWeekdaySymbols);
    CFReleaseIfNotNull(formatter->_property._StandaloneWeekdaySymbols);
    CFReleaseIfNotNull(formatter->_property._ShortStandaloneWeekdaySymbols);
    CFReleaseIfNotNull(formatter->_property._VeryShortStandaloneWeekdaySymbols);
    CFReleaseIfNotNull(formatter->_property._QuarterSymbols);
    CFReleaseIfNotNull(formatter->_property._ShortQuarterSymbols);
    CFReleaseIfNotNull(formatter->_property._StandaloneQuarterSymbols);
    CFReleaseIfNotNull(formatter->_property._ShortStandaloneQuarterSymbols);
    CFReleaseIfNotNull(formatter->_property._AMSymbol);
    CFReleaseIfNotNull(formatter->_property._PMSymbol);
}

static CFStringRef __CFDateFormatterCreateForcedString(CFDateFormatterRef formatter, CFStringRef inString);

static void __CFDateFormatterSetProperty(CFDateFormatterRef formatter, CFStringRef key, CFTypeRef value, Boolean directToICU);

#define RESET_PROPERTY(C, K) \
    if (df->_property. C) __CFDateFormatterSetProperty(df, K, df->_property. C, true);

static void __ResetUDateFormat(CFDateFormatterRef df, Boolean goingToHaveCustomFormat) {
    if (df->_df) udat_close(df->_df);
    df->_df = NULL;

    // uses _timeStyle, _dateStyle, _locale, _property._TimeZone; sets _df, _format, _defformat
    char loc_buffer[BUFFER_SIZE];
    loc_buffer[0] = 0;
    CFStringRef tmpLocName = df->_locale ? CFLocaleGetIdentifier(df->_locale) : CFSTR("");
    CFStringGetCString(tmpLocName, loc_buffer, BUFFER_SIZE, kCFStringEncodingASCII);

    UChar tz_buffer[BUFFER_SIZE];
    tz_buffer[0] = 0;
    CFStringRef tmpTZName = df->_property._TimeZone ? CFTimeZoneGetName(df->_property._TimeZone) : CFSTR("GMT");
    CFStringGetCharacters(tmpTZName, CFRangeMake(0, CFStringGetLength(tmpTZName)), (UniChar *)tz_buffer);

    df->_property._HasCustomFormat = kCFBooleanFalse;

    int32_t udstyle = 0, utstyle = 0;
    switch (df->_dateStyle) {
    case kCFDateFormatterNoStyle: udstyle = UDAT_NONE; break;
    case kCFDateFormatterShortStyle: udstyle = UDAT_SHORT; break;
    case kCFDateFormatterMediumStyle: udstyle = UDAT_MEDIUM; break;
    case kCFDateFormatterLongStyle: udstyle = UDAT_LONG; break;
    case kCFDateFormatterFullStyle: udstyle = UDAT_FULL; break;
    }
    switch (df->_timeStyle) {
    case kCFDateFormatterNoStyle: utstyle = UDAT_NONE; break;
    case kCFDateFormatterShortStyle: utstyle = UDAT_SHORT; break;
    case kCFDateFormatterMediumStyle: utstyle = UDAT_MEDIUM; break;
    case kCFDateFormatterLongStyle: utstyle = UDAT_LONG; break;
    case kCFDateFormatterFullStyle: utstyle = UDAT_FULL; break;
    }
    Boolean wantRelative = (NULL != df->_property._DoesRelativeDateFormatting && df->_property._DoesRelativeDateFormatting == kCFBooleanTrue);
    Boolean hasFormat = (NULL != df->_property._HasCustomFormat && df->_property._HasCustomFormat == kCFBooleanTrue) || goingToHaveCustomFormat;
    if (wantRelative && !hasFormat) {
	udstyle |= UDAT_RELATIVE;
    }

    UErrorCode status = U_ZERO_ERROR;
    UDateFormat *icudf = udat_open((UDateFormatStyle)utstyle, (UDateFormatStyle)udstyle, loc_buffer, tz_buffer, CFStringGetLength(tmpTZName), NULL, 0, &status);
    if (NULL == icudf || U_FAILURE(status)) {
        return;
    }
    udat_setLenient(icudf, 0);
    if (kCFDateFormatterNoStyle == df->_dateStyle && kCFDateFormatterNoStyle == df->_timeStyle) {
        udat_applyPattern(icudf, false, NULL, 0);
    }
    CFStringRef calident = (CFStringRef)CFLocaleGetValue(df->_locale, kCFLocaleCalendarIdentifierKey);
    if (calident && CFEqual(calident, kCFCalendarIdentifierGregorian)) {
        status = U_ZERO_ERROR;
        udat_set2DigitYearStart(icudf, -631152000000.0, &status); // 1950-01-01 00:00:00 GMT
    }
    df->_df = icudf;

    __CFDateFormatterCustomize(df);

    UChar ubuffer[BUFFER_SIZE];
    status = U_ZERO_ERROR;
    int32_t ret = udat_toPattern(icudf, false, ubuffer, BUFFER_SIZE, &status);
    if (U_SUCCESS(status) && ret <= BUFFER_SIZE) {
        CFStringRef newFormat = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar *)ubuffer, ret);
        CFStringRef formatString = __CFDateFormatterCreateForcedString(df, newFormat);
        CFIndex cnt = CFStringGetLength(formatString);
        CFAssert1(cnt <= 1024, __kCFLogAssertion, "%s(): format string too long", __PRETTY_FUNCTION__);
        if (df->_format != formatString && cnt <= 1024) {
            STACK_BUFFER_DECL(UChar, ubuffer, cnt);
            const UChar *ustr = (UChar *)CFStringGetCharactersPtr((CFStringRef)formatString);
            if (NULL == ustr) {
                CFStringGetCharacters(formatString, CFRangeMake(0, cnt), (UniChar *)ubuffer);
                ustr = ubuffer;
            }
            UErrorCode status = U_ZERO_ERROR;
//            udat_applyPattern(df->_df, false, ustr, cnt, &status);
            udat_applyPattern(df->_df, false, ustr, cnt);
            if (U_SUCCESS(status)) {
                if (df->_format) CFRelease(df->_format);
                df->_format = (CFStringRef)CFStringCreateCopy(CFGetAllocator(df), formatString);
            }
        }
        CFRelease(formatString);
        CFRelease(newFormat);
    }
    if (df->_defformat) CFRelease(df->_defformat);
    df->_defformat = df->_format ? (CFStringRef)CFRetain(df->_format) : NULL;

    RESET_PROPERTY(_IsLenient, kCFDateFormatterIsLenientKey);
    RESET_PROPERTY(_DoesRelativeDateFormatting, kCFDateFormatterDoesRelativeDateFormattingKey);
    RESET_PROPERTY(_Calendar, kCFDateFormatterCalendarKey);
    RESET_PROPERTY(_CalendarName, kCFDateFormatterCalendarIdentifierKey);
    RESET_PROPERTY(_TimeZone, kCFDateFormatterTimeZoneKey);
    RESET_PROPERTY(_TwoDigitStartDate, kCFDateFormatterTwoDigitStartDateKey);
    RESET_PROPERTY(_DefaultDate, kCFDateFormatterDefaultDateKey);
    RESET_PROPERTY(_GregorianStartDate, kCFDateFormatterGregorianStartDateKey);
    RESET_PROPERTY(_EraSymbols, kCFDateFormatterEraSymbolsKey);
    RESET_PROPERTY(_LongEraSymbols, kCFDateFormatterLongEraSymbolsKey);
    RESET_PROPERTY(_MonthSymbols, kCFDateFormatterMonthSymbolsKey);
    RESET_PROPERTY(_ShortMonthSymbols, kCFDateFormatterShortMonthSymbolsKey);
    RESET_PROPERTY(_VeryShortMonthSymbols, kCFDateFormatterVeryShortMonthSymbolsKey);
    RESET_PROPERTY(_StandaloneMonthSymbols, kCFDateFormatterStandaloneMonthSymbolsKey);
    RESET_PROPERTY(_ShortStandaloneMonthSymbols, kCFDateFormatterShortStandaloneMonthSymbolsKey);
    RESET_PROPERTY(_VeryShortStandaloneMonthSymbols, kCFDateFormatterVeryShortStandaloneMonthSymbolsKey);
    RESET_PROPERTY(_WeekdaySymbols, kCFDateFormatterWeekdaySymbolsKey);
    RESET_PROPERTY(_ShortWeekdaySymbols, kCFDateFormatterShortWeekdaySymbolsKey);
    RESET_PROPERTY(_VeryShortWeekdaySymbols, kCFDateFormatterVeryShortWeekdaySymbolsKey);
    RESET_PROPERTY(_StandaloneWeekdaySymbols, kCFDateFormatterStandaloneWeekdaySymbolsKey);
    RESET_PROPERTY(_ShortStandaloneWeekdaySymbols, kCFDateFormatterShortStandaloneWeekdaySymbolsKey);
    RESET_PROPERTY(_VeryShortStandaloneWeekdaySymbols, kCFDateFormatterVeryShortStandaloneWeekdaySymbolsKey);
    RESET_PROPERTY(_QuarterSymbols, kCFDateFormatterQuarterSymbolsKey);
    RESET_PROPERTY(_ShortQuarterSymbols, kCFDateFormatterShortQuarterSymbolsKey);
    RESET_PROPERTY(_StandaloneQuarterSymbols, kCFDateFormatterStandaloneQuarterSymbolsKey);
    RESET_PROPERTY(_ShortStandaloneQuarterSymbols, kCFDateFormatterShortStandaloneQuarterSymbolsKey);
    RESET_PROPERTY(_AMSymbol, kCFDateFormatterAMSymbolKey);
    RESET_PROPERTY(_PMSymbol, kCFDateFormatterPMSymbolKey);
}

static CFTypeID __kCFDateFormatterTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFDateFormatterClass = {
    0,
    "CFDateFormatter",
    NULL,        // init
    NULL,        // copy
    __CFDateFormatterDeallocate,
    NULL,
    NULL,
    NULL,        // 
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
    memory->_dateStyle = dateStyle;
    memory->_timeStyle = timeStyle;
    memory->_property._IsLenient = NULL;
    memory->_property._DoesRelativeDateFormatting = NULL;
    memory->_property._HasCustomFormat = NULL;
    memory->_property._TimeZone = NULL;
    memory->_property._Calendar = NULL;
    memory->_property._CalendarName = NULL;
    memory->_property._TwoDigitStartDate = NULL;
    memory->_property._DefaultDate = NULL;
    memory->_property._GregorianStartDate = NULL;
    memory->_property._EraSymbols = NULL;
    memory->_property._LongEraSymbols = NULL;
    memory->_property._MonthSymbols = NULL;
    memory->_property._ShortMonthSymbols = NULL;
    memory->_property._VeryShortMonthSymbols = NULL;
    memory->_property._StandaloneMonthSymbols = NULL;
    memory->_property._ShortStandaloneMonthSymbols = NULL;
    memory->_property._VeryShortStandaloneMonthSymbols = NULL;
    memory->_property._WeekdaySymbols = NULL;
    memory->_property._ShortWeekdaySymbols = NULL;
    memory->_property._VeryShortWeekdaySymbols = NULL;
    memory->_property._StandaloneWeekdaySymbols = NULL;
    memory->_property._ShortStandaloneWeekdaySymbols = NULL;
    memory->_property._VeryShortStandaloneWeekdaySymbols = NULL;
    memory->_property._QuarterSymbols = NULL;
    memory->_property._ShortQuarterSymbols = NULL;
    memory->_property._StandaloneQuarterSymbols = NULL;
    memory->_property._ShortStandaloneQuarterSymbols = NULL;
    memory->_property._AMSymbol = NULL;
    memory->_property._PMSymbol = NULL;

    switch (dateStyle) {
    case kCFDateFormatterNoStyle:
    case kCFDateFormatterShortStyle:
    case kCFDateFormatterMediumStyle:
    case kCFDateFormatterLongStyle:
    case kCFDateFormatterFullStyle: break;
    default:
        CFAssert2(0, __kCFLogAssertion, "%s(): unknown date style %d", __PRETTY_FUNCTION__, dateStyle);
        memory->_dateStyle = kCFDateFormatterMediumStyle;
        break;
    }
    switch (timeStyle) {
    case kCFDateFormatterNoStyle:
    case kCFDateFormatterShortStyle:
    case kCFDateFormatterMediumStyle:
    case kCFDateFormatterLongStyle:
    case kCFDateFormatterFullStyle: break;
    default:
        CFAssert2(0, __kCFLogAssertion, "%s(): unknown time style %d", __PRETTY_FUNCTION__, timeStyle);
        memory->_timeStyle = kCFDateFormatterMediumStyle;
        break;
    }

    memory->_locale = locale ? CFLocaleCreateCopy(allocator, locale) : (CFLocaleRef)CFRetain(CFLocaleGetSystem());
    memory->_property._TimeZone = CFTimeZoneCopyDefault();
    __ResetUDateFormat(memory, false);
    if (!memory->_df) {
        CFRelease(memory);
	return NULL;
    }
    return (CFDateFormatterRef)memory;
}

extern CFDictionaryRef __CFLocaleGetPrefs(CFLocaleRef locale);

static void __substituteFormatStringFromPrefsDFRelative(CFDateFormatterRef formatter) {
    CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);

    CFIndex dateLen = -1;
    UChar dateBuffer[BUFFER_SIZE];
    if (kCFDateFormatterNoStyle != formatter->_dateStyle) {
        CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUDateFormatStrings")) : NULL;
        if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
            CFStringRef key;
            switch (formatter->_dateStyle) {
            case kCFDateFormatterShortStyle: key = CFSTR("1"); break;
            case kCFDateFormatterMediumStyle: key = CFSTR("2"); break;
            case kCFDateFormatterLongStyle: key = CFSTR("3"); break;
            case kCFDateFormatterFullStyle: key = CFSTR("4"); break;
            default: key = CFSTR("0"); break;
            }
            CFStringRef pref = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)metapref, key);
            if (NULL != pref && CFGetTypeID(pref) == CFStringGetTypeID()) {
                dateLen = __CFMin(CFStringGetLength(pref), BUFFER_SIZE);
                CFStringGetCharacters(pref, CFRangeMake(0, dateLen), (UniChar *)dateBuffer);
            }
        }
    }
    if (-1 == dateLen) {
        UErrorCode status = U_ZERO_ERROR;
        int32_t ret = udat_toPatternRelativeDate(formatter->_df, dateBuffer, BUFFER_SIZE, &status);
        if (!U_FAILURE(status)) {
            dateLen = ret;
        }
    }

    CFIndex timeLen = -1;
    UChar timeBuffer[BUFFER_SIZE];
    if (kCFDateFormatterNoStyle != formatter->_timeStyle) {
        CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUTimeFormatStrings")) : NULL;
        if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
            CFStringRef key;
            switch (formatter->_timeStyle) {
            case kCFDateFormatterShortStyle: key = CFSTR("1"); break;
            case kCFDateFormatterMediumStyle: key = CFSTR("2"); break;
            case kCFDateFormatterLongStyle: key = CFSTR("3"); break;
            case kCFDateFormatterFullStyle: key = CFSTR("4"); break;
            default: key = CFSTR("0"); break;
            }
            CFStringRef pref = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)metapref, key);
            if (NULL != pref && CFGetTypeID(pref) == CFStringGetTypeID()) {
                timeLen = __CFMin(CFStringGetLength(pref), BUFFER_SIZE);
                CFStringGetCharacters(pref, CFRangeMake(0, timeLen), (UniChar *)timeBuffer);
            }
        }
    }
    if (-1 == timeLen) {
        UErrorCode status = U_ZERO_ERROR;
        int32_t ret = udat_toPatternRelativeTime(formatter->_df, timeBuffer, BUFFER_SIZE, &status);
        if (!U_FAILURE(status)) {
            timeLen = ret;
        }
    }

    UErrorCode status = U_ZERO_ERROR;
    udat_applyPatternRelative(formatter->_df, (0 <= dateLen) ? dateBuffer : NULL, (0 <= dateLen) ? dateLen : 0, (0 <= timeLen) ? timeBuffer : NULL, (0 <= timeLen) ? timeLen : 0, &status);
}

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
//                            udat_applyPattern(formatter->_df, false, new_ustr, new_len, &status);
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

static CFStringRef __CFDateFormatterCreateForcedString(CFDateFormatterRef formatter, CFStringRef inString) {
    if (!inString) return NULL;
    Boolean doForce24 = false, doForce12 = false;
    CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);
    CFPropertyListRef pref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUForce24HourTime")) : NULL;
    if (NULL != pref && CFGetTypeID(pref) == CFBooleanGetTypeID()) {
        doForce24 = CFBooleanGetValue((CFBooleanRef)pref);
    }
    pref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUForce12HourTime")) : NULL;
    if (NULL != pref && CFGetTypeID(pref) == CFBooleanGetTypeID()) {
        doForce12 = CFBooleanGetValue((CFBooleanRef)pref);
    }
    if (!doForce24 && !doForce12) return (CFStringRef)CFRetain(inString);
    if (doForce24) doForce12 = false; // if both are set, Force24 wins, period
    CFMutableStringRef outString = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFIndex cnt = CFStringGetLength(inString);
    CFIndex lastSecond = -1, lastMinute = -1, firstHour = -1;
    Boolean isInQuote = false, hasA = false, had12Hour = false, had24Hour = false;
    for (CFIndex idx = 0; idx < cnt; idx++) {
        Boolean emit = true;
        UniChar ch = CFStringGetCharacterAtIndex(inString, idx);
        switch (ch) {
        case '\'': isInQuote = !isInQuote; break;
        case 'h': if (!isInQuote) {if (-1 == firstHour) firstHour = CFStringGetLength(outString); had12Hour = true; if (doForce24) ch = 'H';} break; // switch 12-hour to 24-hour
        case 'K': if (!isInQuote) {if (-1 == firstHour) firstHour = CFStringGetLength(outString); had12Hour = true; if (doForce24) ch = 'k';} break; // switch 12-hour to 24-hour
        case 'H': if (!isInQuote) {if (-1 == firstHour) firstHour = CFStringGetLength(outString); had24Hour = true; if (doForce12) ch = 'h';} break; // switch 24-hour to 12-hour
        case 'k': if (!isInQuote) {if (-1 == firstHour) firstHour = CFStringGetLength(outString); had24Hour = true; if (doForce12) ch = 'K';} break; // switch 24-hour to 12-hour
        case 'm': if (!isInQuote) lastMinute = CFStringGetLength(outString); break;
        case 's': if (!isInQuote) lastSecond = CFStringGetLength(outString); break;
        case 'a': if (!isInQuote) hasA = true;
            if (!isInQuote && doForce24) {
                // skip 'a' and one optional trailing space
                emit = false;
                if (idx + 1 < cnt && ' ' == CFStringGetCharacterAtIndex(inString, idx + 1)) idx++;
            }
            break;
        case ' ':
            if (!isInQuote && doForce24) {
                // if next character is 'a' AND we have seen the hour designator, skip space and 'a'
                if (idx + 1 < cnt && 'a' == CFStringGetCharacterAtIndex(inString, idx + 1) && -1 != firstHour) {
                    emit = false;
                    idx++;
                }
            }
            break;
        }
        if (emit) CFStringAppendCharacters(outString, &ch, 1);
    }
    if (doForce12 && !hasA && had24Hour) {
        CFStringRef locName = CFLocaleGetIdentifier(formatter->_locale);
        if (-1 != firstHour && (CFStringHasPrefix(locName, CFSTR("ko")) || CFEqual(locName, CFSTR("zh_SG")))) {
            CFStringInsert(outString, firstHour, CFSTR("a "));
        } else if (-1 != firstHour && (CFStringHasPrefix(locName, CFSTR("zh")) || CFStringHasPrefix(locName, CFSTR("ja")))) {
            CFStringInsert(outString, firstHour, CFSTR("a"));
        } else {
            CFIndex lastPos = (-1 != lastSecond) ? lastSecond : ((-1 != lastMinute) ? lastMinute : -1);
            if (-1 != lastPos) {
                cnt = CFStringGetLength(outString);
                lastPos++;
                UniChar ch = (lastPos < cnt) ? CFStringGetCharacterAtIndex(outString, lastPos) : 0;
                switch (ch) {
                case '\"': lastPos++; break;
                case '\'':;
		    again:;
                    do {
                        lastPos++;
                        ch = (lastPos < cnt) ? CFStringGetCharacterAtIndex(outString, lastPos) : 0;
                    } while ('\'' != ch && '\0' != ch);
                    if ('\'' == ch) lastPos++;
		    ch = (lastPos < cnt) ? CFStringGetCharacterAtIndex(outString, lastPos) : 0;
                    if ('\'' == ch) goto again;
                    break;
                }
                CFStringInsert(outString, lastPos, CFSTR(" a"));
            }
        }
    }
    return outString;
}

static void __CFDateFormatterCustomize(CFDateFormatterRef formatter) {
    Boolean wantRelative = (NULL != formatter->_property._DoesRelativeDateFormatting && formatter->_property._DoesRelativeDateFormatting == kCFBooleanTrue);
    Boolean hasFormat = (NULL != formatter->_property._HasCustomFormat && formatter->_property._HasCustomFormat == kCFBooleanTrue);
    if (wantRelative && !hasFormat) {
        __substituteFormatStringFromPrefsDFRelative(formatter);
    } else {
        __substituteFormatStringFromPrefsDF(formatter, false);
        __substituteFormatStringFromPrefsDF(formatter, true);
    }
    CFDictionaryRef prefs = __CFLocaleGetPrefs(formatter->_locale);
    CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUDateTimeSymbols")) : NULL;
    if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
        CFDictionaryApplyFunction((CFDictionaryRef)metapref, __CFDateFormatterApplySymbolPrefs, formatter);
    }
    metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleFirstWeekday")) : NULL;
    CFStringRef calID = (CFStringRef)CFLocaleGetValue(formatter->_locale, kCFLocaleCalendarIdentifierKey);
    if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
        metapref = (CFNumberRef)CFDictionaryGetValue((CFDictionaryRef)metapref, calID);
    }
    if (NULL != metapref && CFGetTypeID(metapref) == CFNumberGetTypeID()) {
        CFIndex wkdy;
        if (CFNumberGetValue((CFNumberRef)metapref, kCFNumberCFIndexType, &wkdy)) {
            UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
            if (cal) ucal_setAttribute(cal, UCAL_FIRST_DAY_OF_WEEK, wkdy);
        }
    }
    metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleMinDaysInFirstWeek")) : NULL;
    if (NULL != metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
        metapref = (CFNumberRef)CFDictionaryGetValue((CFDictionaryRef)metapref, calID);
    }
    if (NULL != metapref && CFGetTypeID(metapref) == CFNumberGetTypeID()) {
        CFIndex mwd;
        if (CFNumberGetValue((CFNumberRef)metapref, kCFNumberCFIndexType, &mwd)) {
            UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
            if (cal) ucal_setAttribute(cal, UCAL_MINIMAL_DAYS_IN_FIRST_WEEK, mwd);
        }
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
    formatString = __CFDateFormatterCreateForcedString(formatter, formatString);
    CFIndex cnt = CFStringGetLength(formatString);
    CFAssert1(cnt <= 1024, __kCFLogAssertion, "%s(): format string too long", __PRETTY_FUNCTION__);
    if (formatter->_format != formatString && cnt <= 1024) {
        __ResetUDateFormat(formatter, true);
        STACK_BUFFER_DECL(UChar, ubuffer, cnt);
        const UChar *ustr = (UChar *)CFStringGetCharactersPtr((CFStringRef)formatString);
        if (NULL == ustr) {
            CFStringGetCharacters(formatString, CFRangeMake(0, cnt), (UniChar *)ubuffer);
            ustr = ubuffer;
        }
        UErrorCode status = U_ZERO_ERROR;
//        udat_applyPattern(formatter->_df, false, ustr, cnt, &status);
        udat_applyPattern(formatter->_df, false, ustr, cnt);
        if (U_SUCCESS(status)) {
            if (formatter->_format) CFRelease(formatter->_format);
            formatter->_format = (CFStringRef)CFStringCreateCopy(CFGetAllocator(formatter), formatString);
        }
    }
    if (formatString) CFRelease(formatString);
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
    if (formatter->_property._DefaultDate) {
        CFAbsoluteTime at = CFDateGetAbsoluteTime(formatter->_property._DefaultDate);
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

static void __CFDateFormatterSetSymbolsArray(UDateFormat *icudf, int32_t icucode, int index_base, CFTypeRef value) {
    UErrorCode status = U_ZERO_ERROR;
    __CFGenericValidateType(value, CFArrayGetTypeID());
    CFArrayRef array = (CFArrayRef)value;
    CFIndex idx, cnt = CFArrayGetCount(array);
    for (idx = 0; idx < cnt; idx++) {
	CFStringRef item = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
	__CFGenericValidateType(item, CFStringGetTypeID());
	CFIndex item_cnt = CFStringGetLength(item);
	STACK_BUFFER_DECL(UChar, item_buffer, __CFMin(BUFFER_SIZE, item_cnt));
	UChar *item_ustr = (UChar *)CFStringGetCharactersPtr(item);
	if (NULL == item_ustr) {
	    item_cnt = __CFMin(BUFFER_SIZE, item_cnt);
	    CFStringGetCharacters(item, CFRangeMake(0, item_cnt), (UniChar *)item_buffer);
	    item_ustr = item_buffer;
	}
	status = U_ZERO_ERROR;
	udat_setSymbols(icudf, (UDateFormatSymbolType)icucode, idx + index_base, item_ustr, item_cnt, &status);
    }
}

static CFArrayRef __CFDateFormatterGetSymbolsArray(UDateFormat *icudf, int32_t icucode, int index_base) {
    UErrorCode status = U_ZERO_ERROR;
    CFIndex idx, cnt = udat_countSymbols(icudf, (UDateFormatSymbolType)icucode) - index_base;
    STACK_BUFFER_DECL(CFStringRef, strings, cnt);
    for (idx = 0; idx < cnt; idx++) {
        UChar ubuffer[BUFFER_SIZE];
	CFStringRef str = NULL;
	status = U_ZERO_ERROR;
	CFIndex ucnt = udat_getSymbols(icudf, (UDateFormatSymbolType)icucode, idx + index_base, ubuffer, BUFFER_SIZE, &status);
	if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
	    str = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar *)ubuffer, ucnt);
	}
	strings[idx] = !str ? (CFStringRef)CFRetain(CFSTR("<error>")) : str;
    }
    CFArrayRef array = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)strings, cnt, &kCFTypeArrayCallBacks);
    while (cnt--) {
	CFRelease(strings[cnt]);
    }
    return array;
}

#define SET_SYMBOLS_ARRAY(A, B, C) \
	if (!directToICU) { \
	    oldProperty = formatter->_property. C; \
	    formatter->_property. C = NULL; \
	} \
        __CFDateFormatterSetSymbolsArray(formatter->_df, A, B, value); \
	if (!directToICU) { \
	    formatter->_property. C = __CFDateFormatterGetSymbolsArray(formatter->_df, A, B); \
	}

static void __CFDateFormatterSetProperty(CFDateFormatterRef formatter, CFStringRef key, CFTypeRef value, Boolean directToICU) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(key, CFStringGetTypeID());
    CFTypeRef oldProperty = NULL;
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[BUFFER_SIZE];

    if (kCFDateFormatterIsLenientKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _IsLenient;
            formatter->_property. _IsLenient = NULL;
	}
        __CFGenericValidateType(value, CFBooleanGetTypeID());
        udat_setLenient(formatter->_df, (kCFBooleanTrue == value));
        UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
        if (cal) ucal_setAttribute(cal, UCAL_LENIENT, (kCFBooleanTrue == value));
	if (!directToICU) {
            formatter->_property. _IsLenient = (CFBooleanRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterIsLenientKey);
	}
    } else if (kCFDateFormatterDoesRelativeDateFormattingKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _DoesRelativeDateFormatting;
            formatter->_property. _DoesRelativeDateFormatting = NULL;
	}
        __CFGenericValidateType(value, CFBooleanGetTypeID());
	if (!directToICU) {
	    if (kCFBooleanTrue != value) value = kCFBooleanFalse;
            formatter->_property. _DoesRelativeDateFormatting = value ? (CFBooleanRef)CFRetain(value) : NULL;
	    __ResetUDateFormat(formatter, false);
	}
    } else if (kCFDateFormatterCalendarKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _Calendar;
            formatter->_property. _Calendar = NULL;
	}
        __CFGenericValidateType(value, CFCalendarGetTypeID());
        CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
        CFDictionaryRef components = CFLocaleCreateComponentsFromLocaleIdentifier(kCFAllocatorSystemDefault, localeName);
        CFMutableDictionaryRef mcomponents = CFDictionaryCreateMutableCopy(kCFAllocatorSystemDefault, 0, components);
        CFDictionarySetValue(mcomponents, kCFLocaleCalendarIdentifierKey, CFCalendarGetIdentifier((CFCalendarRef)value));
        localeName = CFLocaleCreateLocaleIdentifierFromComponents(kCFAllocatorSystemDefault, mcomponents);
        CFRelease(mcomponents);
        CFRelease(components);
        CFLocaleRef newLocale = CFLocaleCreate(CFGetAllocator(formatter->_locale), localeName);
        CFRelease(localeName);
        CFRelease(formatter->_locale);
        formatter->_locale = newLocale;
        UCalendar *cal = __CFCalendarCreateUCalendar(NULL, CFLocaleGetIdentifier(formatter->_locale), formatter->_property._TimeZone);
        if (cal) ucal_setAttribute(cal, UCAL_FIRST_DAY_OF_WEEK, CFCalendarGetFirstWeekday((CFCalendarRef)value));
        if (cal) ucal_setAttribute(cal, UCAL_MINIMAL_DAYS_IN_FIRST_WEEK, CFCalendarGetMinimumDaysInFirstWeek((CFCalendarRef)value));
        if (cal) udat_setCalendar(formatter->_df, cal);
        if (cal) ucal_close(cal);
	if (!directToICU) {
            formatter->_property. _Calendar = (CFCalendarRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterCalendarKey);
	}
    } else if (kCFDateFormatterCalendarIdentifierKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _CalendarName;
            formatter->_property. _CalendarName = NULL;
	}
        __CFGenericValidateType(value, CFStringGetTypeID());
        CFStringRef localeName = CFLocaleGetIdentifier(formatter->_locale);
        CFDictionaryRef components = CFLocaleCreateComponentsFromLocaleIdentifier(kCFAllocatorSystemDefault, localeName);
        CFMutableDictionaryRef mcomponents = CFDictionaryCreateMutableCopy(kCFAllocatorSystemDefault, 0, components);
        CFDictionarySetValue(mcomponents, kCFLocaleCalendarIdentifierKey, value);
        localeName = CFLocaleCreateLocaleIdentifierFromComponents(kCFAllocatorSystemDefault, mcomponents);
        CFRelease(mcomponents);
        CFRelease(components);
        CFLocaleRef newLocale = CFLocaleCreate(CFGetAllocator(formatter->_locale), localeName);
        CFRelease(localeName);
        CFRelease(formatter->_locale);
        formatter->_locale = newLocale;
        UCalendar *cal = __CFCalendarCreateUCalendar(NULL, CFLocaleGetIdentifier(formatter->_locale), formatter->_property._TimeZone);
        if (cal) udat_setCalendar(formatter->_df, cal);
        if (cal) ucal_close(cal);
	if (!directToICU) {
            formatter->_property. _CalendarName = (CFStringRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterCalendarIdentifierKey);
	}
    } else if (kCFDateFormatterTimeZoneKey == key) {
	if (formatter->_property. _TimeZone != value) {
	    if (!directToICU) {
		oldProperty = formatter->_property. _TimeZone;
		formatter->_property. _TimeZone = NULL;
	    }
	    __CFGenericValidateType(value, CFTimeZoneGetTypeID());
	    CFTimeZoneRef old = formatter->_property._TimeZone;
	    formatter->_property._TimeZone = value ? (CFTimeZoneRef)CFRetain(value) : CFTimeZoneCopyDefault();
	    if (old) CFRelease(old);
	    CFStringRef tznam = CFTimeZoneGetName(formatter->_property._TimeZone);
	    UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
	    CFIndex ucnt = CFStringGetLength(tznam);
	    if (BUFFER_SIZE < ucnt) ucnt = BUFFER_SIZE;
	    CFStringGetCharacters(tznam, CFRangeMake(0, ucnt), (UniChar *)ubuffer);
	    ucal_setTimeZone(cal, ubuffer, ucnt, &status);
	    if (!directToICU) {
		old = formatter->_property._TimeZone;
		formatter->_property. _TimeZone = (CFTimeZoneRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterTimeZoneKey);
		if (old) CFRelease(old);
	    }
	}
    } else if (kCFDateFormatterDefaultFormatKey == key) {
        // read-only attribute
    } else if (kCFDateFormatterTwoDigitStartDateKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _TwoDigitStartDate;
            formatter->_property. _TwoDigitStartDate = NULL;
	}
        __CFGenericValidateType(value, CFDateGetTypeID());
        CFAbsoluteTime at = CFDateGetAbsoluteTime((CFDateRef)value);
        UDate udate = (at + kCFAbsoluteTimeIntervalSince1970) * 1000.0;
        udat_set2DigitYearStart(formatter->_df, udate, &status);
	if (!directToICU) {
            formatter->_property. _TwoDigitStartDate = (CFDateRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterTwoDigitStartDateKey);
	}
    } else if (kCFDateFormatterDefaultDateKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _DefaultDate;
            formatter->_property. _DefaultDate = NULL;
	}
        __CFGenericValidateType(value, CFDateGetTypeID());
	if (!directToICU) {
            formatter->_property._DefaultDate = value ? (CFDateRef)CFRetain(value) : NULL;
	}
    } else if (kCFDateFormatterGregorianStartDateKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _GregorianStartDate;
            formatter->_property. _GregorianStartDate = NULL;
	}
        __CFGenericValidateType(value, CFDateGetTypeID());
        CFAbsoluteTime at = CFDateGetAbsoluteTime((CFDateRef)value);
        UDate udate = (at + kCFAbsoluteTimeIntervalSince1970) * 1000.0;
        UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
        ucal_setGregorianChange(cal, udate, &status);
	if (!directToICU) {
            formatter->_property. _GregorianStartDate = (CFDateRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterGregorianStartDateKey);
	}
    } else if (kCFDateFormatterEraSymbolsKey == key) {
       SET_SYMBOLS_ARRAY(UDAT_ERAS, 0, _EraSymbols)
    } else if (kCFDateFormatterLongEraSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_ERA_NAMES, 0, _LongEraSymbols)
    } else if (kCFDateFormatterMonthSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_MONTHS, 0, _MonthSymbols)
    } else if (kCFDateFormatterShortMonthSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_SHORT_MONTHS, 0, _ShortMonthSymbols)
    } else if (kCFDateFormatterVeryShortMonthSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_NARROW_MONTHS, 0, _VeryShortMonthSymbols)
    } else if (kCFDateFormatterStandaloneMonthSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_MONTHS, 0, _StandaloneMonthSymbols)
    } else if (kCFDateFormatterShortStandaloneMonthSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_MONTHS, 0, _ShortStandaloneMonthSymbols)
    } else if (kCFDateFormatterVeryShortStandaloneMonthSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_NARROW_MONTHS, 0, _VeryShortStandaloneMonthSymbols)
    } else if (kCFDateFormatterWeekdaySymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_WEEKDAYS, 1, _WeekdaySymbols)
    } else if (kCFDateFormatterShortWeekdaySymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_SHORT_WEEKDAYS, 1, _ShortWeekdaySymbols)
    } else if (kCFDateFormatterVeryShortWeekdaySymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_NARROW_WEEKDAYS, 1, _VeryShortWeekdaySymbols)
    } else if (kCFDateFormatterStandaloneWeekdaySymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_WEEKDAYS, 1, _StandaloneWeekdaySymbols)
    } else if (kCFDateFormatterShortStandaloneWeekdaySymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_WEEKDAYS, 1, _ShortStandaloneWeekdaySymbols)
    } else if (kCFDateFormatterVeryShortStandaloneWeekdaySymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_NARROW_WEEKDAYS, 1, _VeryShortStandaloneWeekdaySymbols)
    } else if (kCFDateFormatterQuarterSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_QUARTERS, 0, _QuarterSymbols)
    } else if (kCFDateFormatterShortQuarterSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_SHORT_QUARTERS, 0, _ShortQuarterSymbols)
    } else if (kCFDateFormatterStandaloneQuarterSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_QUARTERS, 0, _StandaloneQuarterSymbols)
    } else if (kCFDateFormatterShortStandaloneQuarterSymbolsKey == key) {
        SET_SYMBOLS_ARRAY(UDAT_STANDALONE_SHORT_QUARTERS, 0, _ShortStandaloneQuarterSymbols)
    } else if (kCFDateFormatterAMSymbolKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _AMSymbol;
            formatter->_property. _AMSymbol = NULL;
	}
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
	if (!directToICU) {
            formatter->_property. _AMSymbol = (CFStringRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterAMSymbolKey);
	}
    } else if (kCFDateFormatterPMSymbolKey == key) {
	if (!directToICU) {
	    oldProperty = formatter->_property. _PMSymbol;
            formatter->_property. _PMSymbol = NULL;
	}
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
	if (!directToICU) {
            formatter->_property. _PMSymbol = (CFStringRef)CFDateFormatterCopyProperty(formatter, kCFDateFormatterPMSymbolKey);
	}
    } else {
        CFAssert3(0, __kCFLogAssertion, "%s(): unknown key %p (%@)", __PRETTY_FUNCTION__, key, key);
    }
    if (oldProperty) CFRelease(oldProperty);
}

void CFDateFormatterSetProperty(CFDateFormatterRef formatter, CFStringRef key, CFTypeRef value) {
    __CFDateFormatterSetProperty(formatter, key, value, false);
}

CFTypeRef CFDateFormatterCopyProperty(CFDateFormatterRef formatter, CFStringRef key) {
    __CFGenericValidateType(formatter, CFDateFormatterGetTypeID());
    __CFGenericValidateType(key, CFStringGetTypeID());
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[BUFFER_SIZE];

    if (kCFDateFormatterIsLenientKey == key) {
	if (formatter->_property._IsLenient) return CFRetain(formatter->_property._IsLenient);
        return CFRetain(udat_isLenient(formatter->_df) ? kCFBooleanTrue : kCFBooleanFalse);
    } else if (kCFDateFormatterDoesRelativeDateFormattingKey == key) {
	if (formatter->_property._DoesRelativeDateFormatting) return CFRetain(formatter->_property._DoesRelativeDateFormatting);
        return CFRetain(kCFBooleanFalse);
    } else if (kCFDateFormatterCalendarKey == key) {
	if (formatter->_property._Calendar) return CFRetain(formatter->_property._Calendar);
        CFCalendarRef calendar = (CFCalendarRef)CFLocaleGetValue(formatter->_locale, kCFLocaleCalendarKey);
        return calendar ? CFRetain(calendar) : NULL;
    } else if (kCFDateFormatterCalendarIdentifierKey == key) {
	if (formatter->_property._CalendarName) return CFRetain(formatter->_property._CalendarName);
        CFStringRef ident = (CFStringRef)CFLocaleGetValue(formatter->_locale, kCFLocaleCalendarIdentifierKey);
        return ident ? CFRetain(ident) : NULL;
    } else if (kCFDateFormatterTimeZoneKey == key) {
	if (formatter->_property._TwoDigitStartDate) return CFRetain(formatter->_property._TwoDigitStartDate);
        return CFRetain(formatter->_property._TimeZone);
    } else if (kCFDateFormatterDefaultFormatKey == key) {
        return formatter->_defformat ? CFRetain(formatter->_defformat) : NULL;
    } else if (kCFDateFormatterTwoDigitStartDateKey == key) {
	if (formatter->_property._TwoDigitStartDate) return CFRetain(formatter->_property._TwoDigitStartDate);
        UDate udate = udat_get2DigitYearStart(formatter->_df, &status);
        if (U_SUCCESS(status)) {
            CFAbsoluteTime at = (double)udate / 1000.0 - kCFAbsoluteTimeIntervalSince1970;
            return CFDateCreate(CFGetAllocator(formatter), at);
        }
    } else if (kCFDateFormatterDefaultDateKey == key) {
        return formatter->_property._DefaultDate ? CFRetain(formatter->_property._DefaultDate) : NULL;
    } else if (kCFDateFormatterGregorianStartDateKey == key) {
	if (formatter->_property._GregorianStartDate) return CFRetain(formatter->_property._GregorianStartDate);
        UCalendar *cal = (UCalendar *)udat_getCalendar(formatter->_df);
        UDate udate = ucal_getGregorianChange(cal, &status);
        if (U_SUCCESS(status)) {
            CFAbsoluteTime at = (double)udate / 1000.0 - kCFAbsoluteTimeIntervalSince1970;
            return CFDateCreate(CFGetAllocator(formatter), at);
        }
    } else if (kCFDateFormatterEraSymbolsKey == key) {
	if (formatter->_property._EraSymbols) return CFRetain(formatter->_property._EraSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_ERAS, 0);
    } else if (kCFDateFormatterLongEraSymbolsKey == key) {
	if (formatter->_property._LongEraSymbols) return CFRetain(formatter->_property._LongEraSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_ERA_NAMES, 0);
    } else if (kCFDateFormatterMonthSymbolsKey == key) {
	if (formatter->_property._MonthSymbols) return CFRetain(formatter->_property._MonthSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_MONTHS, 0);
    } else if (kCFDateFormatterShortMonthSymbolsKey == key) {
	if (formatter->_property._ShortMonthSymbols) return CFRetain(formatter->_property._ShortMonthSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_SHORT_MONTHS, 0);
    } else if (kCFDateFormatterVeryShortMonthSymbolsKey == key) {
	if (formatter->_property._VeryShortMonthSymbols) return CFRetain(formatter->_property._VeryShortMonthSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_NARROW_MONTHS, 0);
    } else if (kCFDateFormatterStandaloneMonthSymbolsKey == key) {
	if (formatter->_property._StandaloneMonthSymbols) return CFRetain(formatter->_property._StandaloneMonthSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_MONTHS, 0);
    } else if (kCFDateFormatterShortStandaloneMonthSymbolsKey == key) {
	if (formatter->_property._ShortStandaloneMonthSymbols) return CFRetain(formatter->_property._ShortStandaloneMonthSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_SHORT_MONTHS, 0);
    } else if (kCFDateFormatterVeryShortStandaloneMonthSymbolsKey == key) {
	if (formatter->_property._VeryShortStandaloneMonthSymbols) return CFRetain(formatter->_property._VeryShortStandaloneMonthSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_NARROW_MONTHS, 0);
    } else if (kCFDateFormatterWeekdaySymbolsKey == key) {
	if (formatter->_property._WeekdaySymbols) return CFRetain(formatter->_property._WeekdaySymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_WEEKDAYS, 1);
    } else if (kCFDateFormatterShortWeekdaySymbolsKey == key) {
	if (formatter->_property._ShortWeekdaySymbols) return CFRetain(formatter->_property._ShortWeekdaySymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_SHORT_WEEKDAYS, 1);
    } else if (kCFDateFormatterVeryShortWeekdaySymbolsKey == key) {
	if (formatter->_property._VeryShortWeekdaySymbols) return CFRetain(formatter->_property._VeryShortWeekdaySymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_NARROW_WEEKDAYS, 1);
    } else if (kCFDateFormatterStandaloneWeekdaySymbolsKey == key) {
	if (formatter->_property._StandaloneWeekdaySymbols) return CFRetain(formatter->_property._StandaloneWeekdaySymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_WEEKDAYS, 1);
    } else if (kCFDateFormatterShortStandaloneWeekdaySymbolsKey == key) {
	if (formatter->_property._ShortStandaloneWeekdaySymbols) return CFRetain(formatter->_property._ShortStandaloneWeekdaySymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_SHORT_WEEKDAYS, 1);
    } else if (kCFDateFormatterVeryShortStandaloneWeekdaySymbolsKey == key) {
	if (formatter->_property._VeryShortStandaloneWeekdaySymbols) return CFRetain(formatter->_property._VeryShortStandaloneWeekdaySymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_NARROW_WEEKDAYS, 1);
    } else if (kCFDateFormatterQuarterSymbolsKey == key) {
	if (formatter->_property._QuarterSymbols) return CFRetain(formatter->_property._QuarterSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_QUARTERS, 0);
    } else if (kCFDateFormatterShortQuarterSymbolsKey == key) {
	if (formatter->_property._ShortQuarterSymbols) return CFRetain(formatter->_property._ShortQuarterSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_SHORT_QUARTERS, 0);
    } else if (kCFDateFormatterStandaloneQuarterSymbolsKey == key) {
	if (formatter->_property._StandaloneQuarterSymbols) return CFRetain(formatter->_property._StandaloneQuarterSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_QUARTERS, 0);
    } else if (kCFDateFormatterShortStandaloneQuarterSymbolsKey == key) {
	if (formatter->_property._ShortStandaloneQuarterSymbols) return CFRetain(formatter->_property._ShortStandaloneQuarterSymbols);
        return __CFDateFormatterGetSymbolsArray(formatter->_df, UDAT_STANDALONE_SHORT_QUARTERS, 0);
    } else if (kCFDateFormatterAMSymbolKey == key) {
	if (formatter->_property._AMSymbol) return CFRetain(formatter->_property._AMSymbol);
        CFIndex cnt = udat_countSymbols(formatter->_df, UDAT_AM_PMS);
        if (2 <= cnt) {
            CFIndex ucnt = udat_getSymbols(formatter->_df, UDAT_AM_PMS, 0, ubuffer, BUFFER_SIZE, &status);
            if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
                return CFStringCreateWithCharacters(CFGetAllocator(formatter), (UniChar *)ubuffer, ucnt);
            }
        }        
    } else if (kCFDateFormatterPMSymbolKey == key) {
	if (formatter->_property._PMSymbol) return CFRetain(formatter->_property._PMSymbol);
        CFIndex cnt = udat_countSymbols(formatter->_df, UDAT_AM_PMS);
        if (2 <= cnt) {
            CFIndex ucnt = udat_getSymbols(formatter->_df, UDAT_AM_PMS, 1, ubuffer, BUFFER_SIZE, &status);
            if (U_SUCCESS(status) && cnt <= BUFFER_SIZE) {
                return CFStringCreateWithCharacters(CFGetAllocator(formatter), (UniChar *)ubuffer, ucnt);
            }
        }        
    } else {
        CFAssert3(0, __kCFLogAssertion, "%s(): unknown key %p (%@)", __PRETTY_FUNCTION__, key, key);
    }
    return NULL;
}


