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
/*	CFCalendar.h
	Copyright (c) 2004-2007, Apple Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFCALENDAR__)
#define __COREFOUNDATION_CFCALENDAR__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFLocale.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFTimeZone.h>

#if MAC_OS_X_VERSION_10_4 <= MAC_OS_X_VERSION_MAX_ALLOWED

CF_EXTERN_C_BEGIN

typedef struct __CFCalendar * CFCalendarRef;

CF_EXPORT
CFTypeID CFCalendarGetTypeID(void) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFCalendarRef CFCalendarCopyCurrent(void) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFCalendarRef CFCalendarCreateWithIdentifier(CFAllocatorRef allocator, CFStringRef identifier) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;
	// Create a calendar.  The identifiers are the kCF*Calendar
	// constants in CFLocale.h.

CF_EXPORT
CFStringRef CFCalendarGetIdentifier(CFCalendarRef calendar) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;
	// Returns the calendar's identifier.

CF_EXPORT
CFLocaleRef CFCalendarCopyLocale(CFCalendarRef calendar) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
void CFCalendarSetLocale(CFCalendarRef calendar, CFLocaleRef locale) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFTimeZoneRef CFCalendarCopyTimeZone(CFCalendarRef calendar) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
void CFCalendarSetTimeZone(CFCalendarRef calendar, CFTimeZoneRef tz) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFIndex CFCalendarGetFirstWeekday(CFCalendarRef calendar) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
void CFCalendarSetFirstWeekday(CFCalendarRef calendar, CFIndex wkdy) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFIndex CFCalendarGetMinimumDaysInFirstWeek(CFCalendarRef calendar) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
void CFCalendarSetMinimumDaysInFirstWeek(CFCalendarRef calendar, CFIndex mwd) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;


enum {
	kCFCalendarUnitEra = (1 << 1),
	kCFCalendarUnitYear = (1 << 2),
	kCFCalendarUnitMonth = (1 << 3),
	kCFCalendarUnitDay = (1 << 4),
	kCFCalendarUnitHour = (1 << 5),
	kCFCalendarUnitMinute = (1 << 6),
	kCFCalendarUnitSecond = (1 << 7),
	kCFCalendarUnitWeek = (1 << 8),
	kCFCalendarUnitWeekday = (1 << 9),
	kCFCalendarUnitWeekdayOrdinal = (1 << 10)
};
typedef CFOptionFlags CFCalendarUnit;

CF_EXPORT
CFRange CFCalendarGetMinimumRangeOfUnit(CFCalendarRef calendar, CFCalendarUnit unit) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFRange CFCalendarGetMaximumRangeOfUnit(CFCalendarRef calendar, CFCalendarUnit unit) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFRange CFCalendarGetRangeOfUnit(CFCalendarRef calendar, CFCalendarUnit smallerUnit, CFCalendarUnit biggerUnit, CFAbsoluteTime at) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
CFIndex CFCalendarGetOrdinalityOfUnit(CFCalendarRef calendar, CFCalendarUnit smallerUnit, CFCalendarUnit biggerUnit, CFAbsoluteTime at) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
Boolean CFCalendarGetTimeRangeOfUnit(CFCalendarRef calendar, CFCalendarUnit unit, CFAbsoluteTime at, CFAbsoluteTime *startp, CFTimeInterval *tip) AVAILABLE_MAC_OS_X_VERSION_10_5_AND_LATER;

CF_EXPORT
Boolean CFCalendarComposeAbsoluteTime(CFCalendarRef calendar, /* out */ CFAbsoluteTime *at, const char *componentDesc, ...) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
Boolean CFCalendarDecomposeAbsoluteTime(CFCalendarRef calendar, CFAbsoluteTime at, const char *componentDesc, ...) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;


enum {
    kCFCalendarComponentsWrap = (1 << 0)  // option for adding
};

CF_EXPORT
Boolean CFCalendarAddComponents(CFCalendarRef calendar, /* inout */ CFAbsoluteTime *at, CFOptionFlags options, const char *componentDesc, ...) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;

CF_EXPORT
Boolean CFCalendarGetComponentDifference(CFCalendarRef calendar, CFAbsoluteTime startingAT, CFAbsoluteTime resultAT, CFOptionFlags options, const char *componentDesc, ...) AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER;


CF_EXTERN_C_END

#endif

#endif /* ! __COREFOUNDATION_CFCALENDAR__ */

