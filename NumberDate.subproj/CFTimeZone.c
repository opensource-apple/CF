/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
/*	CFTimeZone.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFPropertyList.h>
#include "CFUtilitiesPriv.h"
#include "CFInternal.h"
#include <math.h>
#include <limits.h>
#include <sys/stat.h>
#if !defined(__WIN32__)
#include <dirent.h>
#else
#include <windows.h>
#include <winreg.h>
#include <tchar.h>
#include <time.h>
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#if defined(__WIN32__)
#include <io.h>
#endif

// For Windows(TM) time zone information, see registry key:
// HKEY_LOCAL_MACHINE/SOFTWARE/Microsoft/Windows NT/CurrentVersion/Time Zones

#if defined(__WIN32__)
#define TZZONEINFO "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"
#else
#define TZZONELINK	"/etc/localtime"
#define TZZONEINFO	"/usr/share/zoneinfo/"
#endif

static CFTimeZoneRef __CFTimeZoneSystem = NULL;
static CFTimeZoneRef __CFTimeZoneDefault = NULL;
static CFDictionaryRef __CFTimeZoneAbbreviationDict = NULL;
static CFSpinLock_t __CFTimeZoneAbbreviationLock = 0;
static CFMutableDictionaryRef __CFTimeZoneCompatibilityMappingDict = NULL;
static CFMutableDictionaryRef __CFTimeZoneCompatibilityMappingDict2 = NULL;
static CFSpinLock_t __CFTimeZoneCompatibilityMappingLock = 0;
static CFArrayRef __CFKnownTimeZoneList = NULL;
static CFMutableDictionaryRef __CFTimeZoneCache = NULL;
static CFSpinLock_t __CFTimeZoneGlobalLock = 0;

CF_INLINE void __CFTimeZoneLockGlobal(void) {
    __CFSpinLock(&__CFTimeZoneGlobalLock);
}

CF_INLINE void __CFTimeZoneUnlockGlobal(void) {
    __CFSpinUnlock(&__CFTimeZoneGlobalLock);
}

CF_INLINE void __CFTimeZoneLockAbbreviations(void) {
    __CFSpinLock(&__CFTimeZoneAbbreviationLock);
}

CF_INLINE void __CFTimeZoneUnlockAbbreviations(void) {
    __CFSpinUnlock(&__CFTimeZoneAbbreviationLock);
}

CF_INLINE void __CFTimeZoneLockCompatibilityMapping(void) {
    __CFSpinLock(&__CFTimeZoneCompatibilityMappingLock);
}

CF_INLINE void __CFTimeZoneUnlockCompatibilityMapping(void) {
    __CFSpinUnlock(&__CFTimeZoneCompatibilityMappingLock);
}

/* This function should be used for WIN32 instead of
 * __CFCopyRecursiveDirectoryList function.
 * It takes TimeZone names from the registry
 * (Aleksey Dukhnyakov)
 */
#if defined(__WIN32__)
static CFMutableArrayRef __CFCopyWindowsTimeZoneList() {
    CFMutableArrayRef result = NULL;
    HKEY hkResult;
    TCHAR lpName[MAX_PATH+1];
    DWORD dwIndex, retCode;

    if (RegOpenKey(HKEY_LOCAL_MACHINE,_T(TZZONEINFO),&hkResult) !=
        ERROR_SUCCESS )
        return NULL;

    result = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

    for (dwIndex=0; (retCode = RegEnumKey(hkResult,dwIndex,lpName,MAX_PATH)) != ERROR_NO_MORE_ITEMS ; dwIndex++) {

        if (retCode != ERROR_SUCCESS) {
            RegCloseKey(hkResult);
            CFRelease(result);
            return NULL;
        }
        else {
#if defined(UNICODE)
		    CFStringRef string = CFStringCreateWithBytes(kCFAllocatorDefault, lpName, _tcslen(lpName), kCFStringEncodingUnicode, false);
#else
		    CFStringRef string = CFStringCreateWithBytes(kCFAllocatorDefault, lpName, _tcslen(lpName), CFStringGetSystemEncoding(), false);
#endif
		    CFArrayAppendValue(result, string);
		    CFRelease(string);
        }
    }

    RegCloseKey(hkResult);
    return result;
}
#endif

#if !defined(__WIN32__)
static CFMutableArrayRef __CFCopyRecursiveDirectoryList(const char *topDir) {
    CFMutableArrayRef result = NULL, temp;
    long fd, numread, plen, basep = 0;
    CFIndex idx, cnt, usedLen;
    char *dirge, path[CFMaxPathSize];

// No d_namlen in dirent struct on Linux
#if defined(__LINUX__)
	#define dentDNameLen strlen(dent->d_name)
#else
	#define dentDNameLen dent->d_namlen
#endif
    fd = open(topDir, O_RDONLY, 0);
    if (fd < 0) {
	return NULL;
    }
    dirge = malloc(8192);
    result = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    numread = getdirentries(fd, dirge, 8192, &basep);
    while (0 < numread) {
	struct dirent *dent = (struct dirent *)dirge;
	for (; dent < (struct dirent *)(dirge + numread); dent = (struct dirent *)((char *)dent + dent->d_reclen)) {
            if (0 == dent->d_fileno) continue;
	    if (1 == dentDNameLen && '.' == dent->d_name[0]) continue;
	    if (2 == dentDNameLen && '.' == dent->d_name[0] && '.' == dent->d_name[1]) continue;
            if (dent->d_type == DT_UNKNOWN) {
		struct stat statbuf;
		strcpy(path, topDir);
		strcat(path, "/");
		plen = strlen(path);
		memmove(path + plen, dent->d_name, dentDNameLen);
		path[plen + dentDNameLen] = '\0';
		if (0 <= stat(path, &statbuf) && (statbuf.st_mode & S_IFMT) == S_IFDIR) {
		    dent->d_type = DT_DIR;
		}
	    }
            if (DT_DIR == dent->d_type) {
		strcpy(path, topDir);
		strcat(path, "/");
		plen = strlen(path);
		memmove(path + plen, dent->d_name, dentDNameLen);
		path[plen + dentDNameLen] = '\0';
		temp = __CFCopyRecursiveDirectoryList(path);
		for (idx = 0, cnt = CFArrayGetCount(temp); idx < cnt; idx++) {
		    CFStringRef string, item = CFArrayGetValueAtIndex(temp, idx);
		    memmove(path, dent->d_name, dentDNameLen);
		    path[dentDNameLen] = '/';
		    CFStringGetBytes(item, CFRangeMake(0, CFStringGetLength(item)), kCFStringEncodingUTF8, 0, false, path + dentDNameLen + 1, CFMaxPathLength - dentDNameLen - 2, &usedLen);
		    string = CFStringCreateWithBytes(kCFAllocatorDefault, path, dentDNameLen + 1 + usedLen, kCFStringEncodingUTF8, false);
		    CFArrayAppendValue(result, string);
		    CFRelease(string);
		}
		CFRelease(temp);
            } else {
		CFStringRef string = CFStringCreateWithBytes(kCFAllocatorDefault, dent->d_name, dentDNameLen, kCFStringEncodingUTF8, false);
		CFArrayAppendValue(result, string);
		CFRelease(string);
            }
	}
	numread = getdirentries(fd, dirge, 8192, &basep);
    }
    close(fd);
    free(dirge);
    if  (-1 == numread) {
        CFRelease(result);
        return NULL;
    }
    return result;
}
#endif

typedef struct _CFTZPeriod {
    int32_t startSec;
    CFStringRef abbrev;
    uint32_t info;
} CFTZPeriod;

struct __CFTimeZone {
    CFRuntimeBase _base;
    CFStringRef _name;		/* immutable */
    CFDataRef _data;		/* immutable */
    CFTZPeriod *_periods;	/* immutable */
    int32_t _periodCnt;		/* immutable */
};

/* startSec is the whole integer seconds from a CFAbsoluteTime, giving dates
 * between 1933 and 2069; info outside these years is discarded on read-in */
/* Bits 31-18 of the info are unused */
/* Bit 17 of the info is used for the is-DST state */
/* Bit 16 of the info is used for the sign of the offset (1 == negative) */
/* Bits 15-0 of the info are used for abs(offset) in seconds from GMT */

CF_INLINE void __CFTZPeriodInit(CFTZPeriod *period, int32_t startTime, CFStringRef abbrev, int32_t offset, Boolean isDST) {
    period->startSec = startTime;
    period->abbrev = abbrev ? CFRetain(abbrev) : NULL;
    __CFBitfieldSetValue(period->info, 15, 0, abs(offset));
    __CFBitfieldSetValue(period->info, 16, 16, (offset < 0 ? 1 : 0));
    __CFBitfieldSetValue(period->info, 17, 17, (isDST ? 1 : 0));
}

CF_INLINE int32_t __CFTZPeriodStartSeconds(const CFTZPeriod *period) {
    return period->startSec;
}

CF_INLINE CFStringRef __CFTZPeriodAbbreviation(const CFTZPeriod *period) {
    return period->abbrev;
}

CF_INLINE int32_t __CFTZPeriodGMTOffset(const CFTZPeriod *period) {
    int32_t v = __CFBitfieldGetValue(period->info, 15, 0);
    if (__CFBitfieldGetValue(period->info, 16, 16)) v = -v;
    return v;
}

CF_INLINE Boolean __CFTZPeriodIsDST(const CFTZPeriod *period) {
    return (Boolean)__CFBitfieldGetValue(period->info, 17, 17);
}

static CFComparisonResult __CFCompareTZPeriods(const void *val1, const void *val2, void *context) {
    CFTZPeriod *tzp1 = (CFTZPeriod *)val1;
    CFTZPeriod *tzp2 = (CFTZPeriod *)val2;
    // we treat equal as less than, as the code which uses the
    // result of the bsearch doesn't expect exact matches
    // (they're pretty rare, so no point in over-coding for them)
    if (__CFTZPeriodStartSeconds(tzp1) <= __CFTZPeriodStartSeconds(tzp2)) return kCFCompareLessThan;
    return kCFCompareGreaterThan;
}

static CFIndex __CFBSearchTZPeriods(CFTimeZoneRef tz, CFAbsoluteTime at) {
    CFTZPeriod elem;
    CFIndex idx;
    __CFTZPeriodInit(&elem, (int32_t)(float)floor(at), NULL, 0, false);
    idx = CFBSearch(&elem, sizeof(CFTZPeriod), tz->_periods, tz->_periodCnt, __CFCompareTZPeriods, NULL);
    if (tz->_periodCnt <= idx) {
	idx = tz->_periodCnt;
    } else if (0 == idx) {
	// We want anything before the time zone records start to be not in DST;
	// we assume that if period[0] is DST, then period[1] is not; could do a search instead.
	idx = __CFTZPeriodIsDST(&(tz->_periods[0])) ? 2 : 1;
    }
    return idx - 1;
}

/*
** Each time zone data file begins with. . .
*/

struct tzhead {
	char	tzh_reserved[20];	/* reserved for future use */
	char	tzh_ttisgmtcnt[4];	/* coded number of trans. time flags */
	char	tzh_ttisstdcnt[4];	/* coded number of trans. time flags */
	char	tzh_leapcnt[4];		/* coded number of leap seconds */
	char	tzh_timecnt[4];		/* coded number of transition times */
	char	tzh_typecnt[4];		/* coded number of local time types */
	char	tzh_charcnt[4];		/* coded number of abbr. chars */
};

/*
** . . .followed by. . .
**
**	tzh_timecnt (char [4])s		coded transition times a la time(2)
**	tzh_timecnt (UInt8)s	types of local time starting at above
**	tzh_typecnt repetitions of
**		one (char [4])		coded GMT offset in seconds
**		one (UInt8)	used to set tm_isdst
**		one (UInt8)	that's an abbreviation list index
**	tzh_charcnt (char)s		'\0'-terminated zone abbreviations
**	tzh_leapcnt repetitions of
**		one (char [4])		coded leap second transition times
**		one (char [4])		total correction after above
**	tzh_ttisstdcnt (char)s		indexed by type; if 1, transition
**					time is standard time, if 0,
**					transition time is wall clock time
**					if absent, transition times are
**					assumed to be wall clock time
**	tzh_ttisgmtcnt (char)s		indexed by type; if 1, transition
**					time is GMT, if 0,
**					transition time is local time
**					if absent, transition times are
**					assumed to be local time
*/

CF_INLINE int32_t __CFDetzcode(const unsigned char *bufp) {
    int32_t result = (bufp[0] & 0x80) ? ~0L : 0L;
    result = (result << 8) | (bufp[0] & 0xff);
    result = (result << 8) | (bufp[1] & 0xff);
    result = (result << 8) | (bufp[2] & 0xff);
    result = (result << 8) | (bufp[3] & 0xff);
    return result;
}

CF_INLINE void __CFEntzcode(int32_t value, unsigned char *bufp) {
    bufp[0] = (value >> 24) & 0xff;
    bufp[1] = (value >> 16) & 0xff;
    bufp[2] = (value >> 8) & 0xff;
    bufp[3] = (value >> 0) & 0xff;
}

static Boolean __CFParseTimeZoneData(CFAllocatorRef allocator, CFDataRef data, CFTZPeriod **tzpp, CFIndex *cntp) {
#if !defined(__WIN32__)
    int32_t len, timecnt, typecnt, charcnt, idx, cnt;
    const char *p, *timep, *typep, *ttisp, *charp;
    CFStringRef *abbrs;
    Boolean result = true;

    p = CFDataGetBytePtr(data);
    len = CFDataGetLength(data);
    if (len < (int32_t)sizeof(struct tzhead)) {
	return false;
    }
    p += 20 + 4 + 4 + 4;	/* skip reserved, ttisgmtcnt, ttisstdcnt, leapcnt */
    timecnt = __CFDetzcode(p);
    p += 4;
    typecnt = __CFDetzcode(p);
    p += 4;
    charcnt = __CFDetzcode(p);
    p += 4;
    if (typecnt <= 0 || timecnt < 0 || charcnt < 0) {
	return false;
    }
    if (len - (int32_t)sizeof(struct tzhead) < (4 + 1) * timecnt + (4 + 1 + 1) * typecnt + charcnt) {
	return false;
    }
    timep = p;
    typep = timep + 4 * timecnt;
    ttisp = typep + timecnt;
    charp = ttisp + (4 + 1 + 1) * typecnt;
    cnt = (0 < timecnt) ? timecnt : 1;
    *tzpp = CFAllocatorAllocate(allocator, cnt * sizeof(CFTZPeriod), 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(*tzpp, "CFTimeZone (store)");
    memset(*tzpp, 0, cnt * sizeof(CFTZPeriod));
    abbrs = CFAllocatorAllocate(allocator, (charcnt + 1) * sizeof(CFStringRef), 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(abbrs, "CFTimeZone (temp)");
    for (idx = 0; idx < charcnt + 1; idx++) {
	abbrs[idx] = NULL;
    }
    for (idx = 0; idx < cnt; idx++) {
	CFAbsoluteTime at;
	int32_t itime, offset;
	uint8_t type, dst, abbridx;

	at = (CFAbsoluteTime)(__CFDetzcode(timep) + 0.0) - kCFAbsoluteTimeIntervalSince1970;
	if (0 == timecnt) itime = INT_MIN;
	else if (at < (CFAbsoluteTime)INT_MIN) itime = INT_MIN;
	else if ((CFAbsoluteTime)INT_MAX < at) itime = INT_MAX;
	else itime = (int32_t)at;
	timep += 4;	/* harmless if 0 == timecnt */
	type = (0 < timecnt) ? (uint8_t)*typep++ : 0;
	if (typecnt <= type) {
	    result = false;
	    break;
	}
	offset = __CFDetzcode(ttisp + 6 * type);
	dst = (uint8_t)*(ttisp + 6 * type + 4);
	if (0 != dst && 1 != dst) {
	    result = false;
	    break;
	}
	abbridx = (uint8_t)*(ttisp + 6 * type + 5);
	if (charcnt < abbridx) {
	    result = false;
	    break;
	}
	if (NULL == abbrs[abbridx]) {
	    abbrs[abbridx] = CFStringCreateWithCString(allocator, &charp[abbridx], kCFStringEncodingASCII);
	}
	__CFTZPeriodInit(*tzpp + idx, itime, abbrs[abbridx], offset, (dst ? true : false));
    }
    for (idx = 0; idx < charcnt + 1; idx++) {
	if (NULL != abbrs[idx]) {
	    CFRelease(abbrs[idx]);
	}
    }
    CFAllocatorDeallocate(allocator, abbrs);
    if (result) {
	// dump all but the last INT_MIN and the first INT_MAX
	for (idx = 0; idx < cnt; idx++) {
	    if (((*tzpp + idx)->startSec == INT_MIN) && (idx + 1 < cnt) && (((*tzpp + idx + 1)->startSec == INT_MIN))) {
		if (NULL != (*tzpp + idx)->abbrev) CFRelease((*tzpp + idx)->abbrev);
		cnt--;
		memmove((*tzpp + idx), (*tzpp + idx + 1), sizeof(CFTZPeriod) * (cnt - idx));
		idx--;
	    }
	}
	// Don't combine these loops!  Watch the idx decrementing...
	for (idx = 0; idx < cnt; idx++) {
	    if (((*tzpp + idx)->startSec == INT_MAX) && (0 < idx) && (((*tzpp + idx - 1)->startSec == INT_MAX))) {
		if (NULL != (*tzpp + idx)->abbrev) CFRelease((*tzpp + idx)->abbrev);
		cnt--;
		memmove((*tzpp + idx), (*tzpp + idx + 1), sizeof(CFTZPeriod) * (cnt - idx));
		idx--;
	    }
	}
	CFQSortArray(*tzpp, cnt, sizeof(CFTZPeriod), __CFCompareTZPeriods, NULL);
	*cntp = cnt;
    } else {
	CFAllocatorDeallocate(allocator, *tzpp);
	*tzpp = NULL;
    }
    return result;
#else
/* We use Win32 function to find TimeZone
 * (Aleksey Dukhnyakov)
 */
    *tzpp = CFAllocatorAllocate(allocator, sizeof(CFTZPeriod), 0);
    __CFTZPeriodInit(*tzpp, 0, NULL, 0, false);
    *cntp = 1;
    return TRUE;
#endif
}

static Boolean __CFTimeZoneEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFTimeZoneRef tz1 = (CFTimeZoneRef)cf1;
    CFTimeZoneRef tz2 = (CFTimeZoneRef)cf2;
    if (!CFEqual(CFTimeZoneGetName(tz1), CFTimeZoneGetName(tz2))) return false;
    if (!CFEqual(CFTimeZoneGetData(tz1), CFTimeZoneGetData(tz2))) return false;
    return true;
}

static CFHashCode __CFTimeZoneHash(CFTypeRef cf) {
    CFTimeZoneRef tz = (CFTimeZoneRef)cf;
    return CFHash(CFTimeZoneGetName(tz));
}

static CFStringRef __CFTimeZoneCopyDescription(CFTypeRef cf) {
    CFTimeZoneRef tz = (CFTimeZoneRef)cf;
    CFStringRef result, abbrev;
    CFAbsoluteTime at;
    at = CFAbsoluteTimeGetCurrent();
    abbrev = CFTimeZoneCopyAbbreviation(tz, at);
    result = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<CFTimeZone %p [%p]>{name = %@; abbreviation = %@; GMT offset = %g; is DST = %s}"), cf, CFGetAllocator(tz), tz->_name, abbrev, CFTimeZoneGetSecondsFromGMT(tz, at), CFTimeZoneIsDaylightSavingTime(tz, at) ? "true" : "false");
    CFRelease(abbrev);
    return result;
}

static void __CFTimeZoneDeallocate(CFTypeRef cf) {
    CFTimeZoneRef tz = (CFTimeZoneRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(tz);
    CFIndex idx;
    if (tz->_name) CFRelease(tz->_name);
    if (tz->_data) CFRelease(tz->_data);
    for (idx = 0; idx < tz->_periodCnt; idx++) {
	if (NULL != tz->_periods[idx].abbrev) CFRelease(tz->_periods[idx].abbrev);
    }
    if (NULL != tz->_periods) CFAllocatorDeallocate(allocator, tz->_periods);
}

static CFTypeID __kCFTimeZoneTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFTimeZoneClass = {
    0,
    "CFTimeZone",
    NULL,	// init
    NULL,	// copy
    __CFTimeZoneDeallocate,
    __CFTimeZoneEqual,
    __CFTimeZoneHash,
    NULL,	//
    __CFTimeZoneCopyDescription
};

__private_extern__ void __CFTimeZoneInitialize(void) {
    __kCFTimeZoneTypeID = _CFRuntimeRegisterClass(&__CFTimeZoneClass);
}

CFTypeID CFTimeZoneGetTypeID(void) {
    return __kCFTimeZoneTypeID;
}

static CFTimeZoneRef __CFTimeZoneCreateSystem(void) {
    CFTimeZoneRef result = NULL;
#if defined(__WIN32__)
/* The GetTimeZoneInformation function retrieves the current
 * time-zone parameters for Win32
 * (Aleksey Dukhnyakov)
 */
    CFDataRef data;
    TIME_ZONE_INFORMATION tz;
    DWORD dw_result;
    dw_result=GetTimeZoneInformation(&tz);

    if ( dw_result == TIME_ZONE_ID_STANDARD ||
            dw_result == TIME_ZONE_ID_DAYLIGHT ) {
        CFStringRef name = CFStringCreateWithCharacters(kCFAllocatorDefault, tz.StandardName, wcslen(tz.StandardName));
        data = CFDataCreate(kCFAllocatorDefault, (UInt8*)&tz, sizeof(tz));
        result = CFTimeZoneCreate(kCFAllocatorSystemDefault, name, data);
        CFRelease(name);
        CFRelease(data);
        if (result) return result;
    }
#else
    char *tzenv;
    int ret;
    char linkbuf[CFMaxPathSize];

    tzenv = getenv("TZFILE");
    if (NULL != tzenv) {
	CFStringRef name = CFStringCreateWithBytes(kCFAllocatorDefault, tzenv, strlen(tzenv), kCFStringEncodingUTF8, false);
	result = CFTimeZoneCreateWithName(kCFAllocatorSystemDefault, name, false);
	CFRelease(name);
	if (result) return result;
    }
    tzenv = getenv("TZ");
    if (NULL != tzenv) {
	CFStringRef name = CFStringCreateWithBytes(kCFAllocatorDefault, tzenv, strlen(tzenv), kCFStringEncodingUTF8, false);
	result = CFTimeZoneCreateWithName(kCFAllocatorSystemDefault, name, true);
	CFRelease(name);
	if (result) return result;
    }
    ret = readlink(TZZONELINK, linkbuf, sizeof(linkbuf));
    if (0 < ret) {
	CFStringRef name;
	linkbuf[ret] = '\0';
	if (strncmp(linkbuf, TZZONEINFO, sizeof(TZZONEINFO) - 1) == 0) {
	    name = CFStringCreateWithBytes(kCFAllocatorDefault, linkbuf + sizeof(TZZONEINFO) - 1, strlen(linkbuf) - sizeof(TZZONEINFO) + 1, kCFStringEncodingUTF8, false);
	} else {
	    name = CFStringCreateWithBytes(kCFAllocatorDefault, linkbuf, strlen(linkbuf), kCFStringEncodingUTF8, false);
	}
	result = CFTimeZoneCreateWithName(kCFAllocatorSystemDefault, name, false);
	CFRelease(name);
	if (result) return result;
    }
#endif
    return CFTimeZoneCreateWithTimeIntervalFromGMT(kCFAllocatorSystemDefault, 0.0);
}

CFTimeZoneRef CFTimeZoneCopySystem(void) {
    CFTimeZoneRef tz;
    __CFTimeZoneLockGlobal();
    if (NULL == __CFTimeZoneSystem) {
	__CFTimeZoneUnlockGlobal();
	tz = __CFTimeZoneCreateSystem();
	__CFTimeZoneLockGlobal();
	if (NULL == __CFTimeZoneSystem) {
	    __CFTimeZoneSystem = tz;
	} else {
	    if (tz) CFRelease(tz);
	}
    }
    tz = __CFTimeZoneSystem ? CFRetain(__CFTimeZoneSystem) : NULL;
    __CFTimeZoneUnlockGlobal();
    return tz;
}

void CFTimeZoneResetSystem(void) {
    __CFTimeZoneLockGlobal();
    if (__CFTimeZoneDefault == __CFTimeZoneSystem) {
	if (__CFTimeZoneDefault) CFRelease(__CFTimeZoneDefault);
	__CFTimeZoneDefault = NULL;
    }
    if (__CFTimeZoneSystem) CFRelease(__CFTimeZoneSystem);
    __CFTimeZoneSystem = NULL;
    __CFTimeZoneUnlockGlobal();
}

CFTimeZoneRef CFTimeZoneCopyDefault(void) {
    CFTimeZoneRef tz;
    __CFTimeZoneLockGlobal();
    if (NULL == __CFTimeZoneDefault) {
	__CFTimeZoneUnlockGlobal();
	tz = CFTimeZoneCopySystem();
	__CFTimeZoneLockGlobal();
	if (NULL == __CFTimeZoneDefault) {
	    __CFTimeZoneDefault = tz;
	} else {
	    if (tz) CFRelease(tz);
	}
    }
    tz = __CFTimeZoneDefault ? CFRetain(__CFTimeZoneDefault) : NULL;
    __CFTimeZoneUnlockGlobal();
    return tz;
}

void CFTimeZoneSetDefault(CFTimeZoneRef tz) {
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
    __CFTimeZoneLockGlobal();
    if (tz != __CFTimeZoneDefault) {
	if (tz) CFRetain(tz);
	if (__CFTimeZoneDefault) CFRelease(__CFTimeZoneDefault);
	__CFTimeZoneDefault = tz;
    }
    __CFTimeZoneUnlockGlobal();
}

static CFDictionaryRef __CFTimeZoneCopyCompatibilityDictionary(void);

CFArrayRef CFTimeZoneCopyKnownNames(void) {
    CFArrayRef tzs;
    __CFTimeZoneLockGlobal();
    if (NULL == __CFKnownTimeZoneList) {
	CFMutableArrayRef list;
/* TimeZone information locate in the registry for Win32
 * (Aleksey Dukhnyakov)
 */
#if !defined(__WIN32__)
        list = __CFCopyRecursiveDirectoryList(TZZONEINFO);
#else
        list = __CFCopyWindowsTimeZoneList();
#endif
	// Remove undesirable ancient cruft
	CFDictionaryRef dict = __CFTimeZoneCopyCompatibilityDictionary();
	CFIndex idx;
	for (idx = CFArrayGetCount(list); idx--; ) {
	    CFStringRef item = CFArrayGetValueAtIndex(list, idx);
	    if (CFDictionaryContainsKey(dict, item)) {
		CFArrayRemoveValueAtIndex(list, idx);
	    }
	}
	__CFKnownTimeZoneList = CFArrayCreateCopy(kCFAllocatorSystemDefault, list);
	CFRelease(list);
    }
    tzs = __CFKnownTimeZoneList ? CFRetain(__CFKnownTimeZoneList) : NULL;
    __CFTimeZoneUnlockGlobal();
    return tzs;
}

static const unsigned char *__CFTimeZoneAbbreviationDefaults =
#if defined(__WIN32__)
/*
 * TimeZone abbreviations for Win32
 * (Andrew Dzubandovsky)
 *
 */
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
" <!DOCTYPE plist SYSTEM \"file://localhost/System/Library/DTDs/PropertyList.dtd\">"
" <plist version=\"1.0\">"
" <dict>"
"        <key>AFG</key> <string>Afghanistan Standard Time</string>"
"        <key>ALS</key> <string>Alaskan Standard Time</string>"
"        <key>ARA</key> <string>Arab Standard Time</string>"
"        <key>ARB</key> <string>Arabian Standard Time</string>"
"        <key>ARC</key> <string>Arabic Standard Time</string>"
"        <key>ATL</key> <string>Atlantic Standard Time</string>"
"        <key>ASC</key> <string>AUS Central Standard Time</string>"
"        <key>ASE</key> <string>AUS Eastern Standard Time</string>"
"        <key>AZS</key> <string>Azores Standard Time</string>"
"        <key>CND</key> <string>Canada Central Standard Time</string>"
"        <key>CPV</key> <string>Cape Verde Standard Time</string>"
"        <key>CCS</key> <string>Caucasus Standard Time</string>"
"        <key>CNAS</key> <string>Cen. Australia Standard Time</string>"
"        <key>CAMR</key> <string>Central America Standard Time</string>"
"        <key>CAS</key> <string>Central Asia Standard Time</string>"
"        <key>CER</key> <string>Central Europe Standard Time</string>"
"        <key>CEPN</key> <string>Central European Standard Time</string>"
"        <key>CPC</key> <string>Central Pacific Standard Time</string>"
"        <key>CSTD</key> <string>Central Standard Time</string>"
"        <key>CHN</key> <string>China Standard Time</string>"
"        <key>DTLN</key> <string>Dateline Standard Time</string>"
"        <key>EAFR</key> <string>E. Africa Standard Time</string>"
"        <key>EAS</key> <string>E. Australia Standard Time</string>"
"        <key>ERP</key> <string>E. Europe Standard Time</string>"
"        <key>ESTH</key> <string>E. South America Standard Time</string>"
"        <key>ESTM</key> <string>Eastern Standard Time</string>"
"        <key>EGP</key> <string>Egypt Standard Time</string>"
"        <key>EKT</key> <string>Ekaterinburg Standard Time</string>"
"        <key>FST</key> <string>Fiji Standard Time</string>"
"        <key>FLE</key> <string>FLE Standard Time</string>"
"        <key>GMT</key> <string>GMT Standard Time</string>"
"        <key>GRLD</key> <string>Greenland Standard Time</string>"
"        <key>GRW</key> <string>Greenwich Standard Time</string>"
"        <key>GTB</key> <string>GTB Standard Time</string>"
"        <key>HWT</key> <string>Hawaiian Standard Time</string>"
"        <key>INT</key> <string>India Standard Time</string>"
"        <key>IRT</key> <string>Iran Standard Time</string>"
"        <key>ISL</key> <string>Israel Standard Time</string>"
"        <key>KRT</key> <string>Korea Standard Time</string>"
"        <key>MXST</key> <string>Mexico Standard Time</string>"
"        <key>MTL</key> <string>Mid-Atlantic Standard Time</string>"
"        <key>MNT</key> <string>Mountain Standard Time</string>"
"        <key>MNM</key> <string>Myanmar Standard Time</string>"
"        <key>NCNA</key> <string>N. Central Asia Standard Time</string>"
"        <key>MPL</key> <string>Nepal Standard Time</string>"
"        <key>NWZ</key> <string>New Zealand Standard Time</string>"
"        <key>NWF</key> <string>Newfoundland Standard Time</string>"
"        <key>NTAE</key> <string>North Asia East Standard Time</string>"
"        <key>NTAS</key> <string>North Asia Standard Time</string>"
"        <key>HSAT</key> <string>Pacific SA Standard Time</string>"
"        <key>PST</key> <string>Pacific Standard Time</string>"
"        <key>RMC</key> <string>Romance Standard Time</string>"
"        <key>MSK</key> <string>Russian Standard Time</string>"
"        <key>SSS</key> <string>SA Eastern Standard Time</string>"
"        <key>SPS</key> <string>SA Pacific Standard Time</string>"
"        <key>SWS</key> <string>SA Western Standard Time</string>"
"        <key>SMS</key> <string>Samoa Standard Time</string>"
"        <key>SAS</key> <string>SE Asia Standard Time</string>"
"        <key>SNG</key> <string>Singapore Standard Time</string>"
"        <key>STAF</key> <string>South Africa Standard Time</string>"
"        <key>SRLK</key> <string>Sri Lanka Standard Time</string>"
"        <key>TPS</key> <string>Taipei Standard Time</string>"
"        <key>TSM</key> <string>Tasmania Standard Time</string>"
"        <key>JPN</key> <string>Tokyo Standard Time</string>"
"        <key>TNG</key> <string>Tonga Standard Time</string>"
"        <key>AEST</key> <string>US Eastern Standard Time</string>"
"        <key>AMST</key> <string>US Mountain Standard Time</string>"
"        <key>VLD</key> <string>Vladivostok Standard Time</string>"
"        <key>AUSW</key> <string>W. Australia Standard Time</string>"
"        <key>AFCW</key> <string>W. Central Africa Standard Time</string>"
"        <key>EWS</key> <string>W. Europe Standard Time</string>"
"        <key>ASW</key> <string>West Asia Standard Time</string>"
"        <key>PWS</key> <string>West Pacific Standard Time</string>"
"        <key>RKS</key> <string>Yakutsk Standard Time</string>"
" </dict>"
" </plist>";
#else
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
" <!DOCTYPE plist SYSTEM \"file://localhost/System/Library/DTDs/PropertyList.dtd\">"
" <plist version=\"1.0\">"
" <dict>"
"        <key>ADT</key> <string>America/Halifax</string>"
"        <key>AFT</key> <string>Asia/Kabul</string>"
"        <key>AKDT</key> <string>America/Juneau</string>"
"        <key>AKST</key> <string>America/Juneau</string>"
"        <key>AST</key> <string>America/Halifax</string>"
"        <key>CDT</key> <string>America/Chicago</string>"
"        <key>CEST</key> <string>Europe/Rome</string>"
"        <key>CET</key> <string>Europe/Rome</string>"
"        <key>CST</key> <string>America/Chicago</string>"
"        <key>EDT</key> <string>America/New_York</string>"
"        <key>EEST</key> <string>Europe/Warsaw</string>"
"        <key>EET</key> <string>Europe/Warsaw</string>"
"        <key>EST</key> <string>America/New_York</string>"
"        <key>GMT</key> <string>GMT</string>"
"        <key>HKST</key> <string>Asia/Hong_Kong</string>"
"        <key>HST</key> <string>Pacific/Honolulu</string>"
"        <key>JST</key> <string>Asia/Tokyo</string>"
"        <key>MDT</key> <string>America/Denver</string>"
"        <key>MSD</key> <string>Europe/Moscow</string>"
"        <key>MSK</key> <string>Europe/Moscow</string>"
"        <key>MST</key> <string>America/Denver</string>"
"        <key>NZDT</key> <string>Pacific/Auckland</string>"
"        <key>NZST</key> <string>Pacific/Auckland</string>"
"        <key>PDT</key> <string>America/Los_Angeles</string>"
"        <key>PST</key> <string>America/Los_Angeles</string>"
"        <key>UTC</key> <string>UTC</string>"
"        <key>WEST</key> <string>Europe/Paris</string>"
"        <key>WET</key> <string>Europe/Paris</string>"
"        <key>YDT</key> <string>America/Yakutat</string>"
"        <key>YST</key> <string>America/Yakutat</string>"
" </dict>"
" </plist>";
#endif

CFDictionaryRef CFTimeZoneCopyAbbreviationDictionary(void) {
    CFDictionaryRef dict;
    __CFTimeZoneLockAbbreviations();
    if (NULL == __CFTimeZoneAbbreviationDict) {
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, __CFTimeZoneAbbreviationDefaults, strlen(__CFTimeZoneAbbreviationDefaults));
	__CFTimeZoneAbbreviationDict = CFPropertyListCreateFromXMLData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL);
	CFRelease(data);
    }
    if (NULL == __CFTimeZoneAbbreviationDict) {
	__CFTimeZoneAbbreviationDict = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, NULL, NULL);
    }
    dict = __CFTimeZoneAbbreviationDict ? CFRetain(__CFTimeZoneAbbreviationDict) : NULL;
    __CFTimeZoneUnlockAbbreviations();
    return dict;
}

void CFTimeZoneSetAbbreviationDictionary(CFDictionaryRef dict) {
    __CFGenericValidateType(dict, CFDictionaryGetTypeID());
    __CFTimeZoneLockGlobal();
    if (dict != __CFTimeZoneAbbreviationDict) {
	if (dict) CFRetain(dict);
	if (__CFTimeZoneAbbreviationDict) CFRelease(__CFTimeZoneAbbreviationDict);
	__CFTimeZoneAbbreviationDict = dict;
    }
    __CFTimeZoneUnlockGlobal();
}

CFTimeZoneRef CFTimeZoneCreate(CFAllocatorRef allocator, CFStringRef name, CFDataRef data) {
// assert:    (NULL != name && NULL != data);
    CFTimeZoneRef memory;
    uint32_t size;
    CFTZPeriod *tzp;
    CFIndex idx, cnt;

    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(name, CFStringGetTypeID());
    __CFGenericValidateType(data, CFDataGetTypeID());
    __CFTimeZoneLockGlobal();
    if (NULL != __CFTimeZoneCache && CFDictionaryGetValueIfPresent(__CFTimeZoneCache, name, (const void **)&memory)) {
	__CFTimeZoneUnlockGlobal();
	return (CFTimeZoneRef)CFRetain(memory);
    }
    if (!__CFParseTimeZoneData(allocator, data, &tzp, &cnt)) {
	__CFTimeZoneUnlockGlobal();
	return NULL;
    }
    size = sizeof(struct __CFTimeZone) - sizeof(CFRuntimeBase);
    memory = _CFRuntimeCreateInstance(allocator, __kCFTimeZoneTypeID, size, NULL);
    if (NULL == memory) {
	__CFTimeZoneUnlockGlobal();
	for (idx = 0; idx < cnt; idx++) {
	    if (NULL != tzp[idx].abbrev) CFRelease(tzp[idx].abbrev);
	}
	if (NULL != tzp) CFAllocatorDeallocate(allocator, tzp);
        return NULL;
    }
    ((struct __CFTimeZone *)memory)->_name = CFStringCreateCopy(allocator, name);
    ((struct __CFTimeZone *)memory)->_data = CFDataCreateCopy(allocator, data);
    ((struct __CFTimeZone *)memory)->_periods = tzp;
    ((struct __CFTimeZone *)memory)->_periodCnt = cnt;
    if (NULL == __CFTimeZoneCache) {
	CFDictionaryKeyCallBacks kcb = kCFTypeDictionaryKeyCallBacks;
	kcb.retain = kcb.release = NULL;
	__CFTimeZoneCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kcb, &kCFTypeDictionaryValueCallBacks);
    }
    CFDictionaryAddValue(__CFTimeZoneCache, ((struct __CFTimeZone *)memory)->_name, memory);
    __CFTimeZoneUnlockGlobal();
    return memory;
}

#if !defined(__WIN32__)
static CFTimeZoneRef __CFTimeZoneCreateFixed(CFAllocatorRef allocator, int32_t seconds, CFStringRef name, int isDST) {
    CFTimeZoneRef result;
    CFDataRef data;
    int32_t nameLen = CFStringGetLength(name);
#if defined(__WIN32__)
    unsigned char *dataBytes = CFAllocatorAllocate(allocator, 52 + nameLen + 1, 0);
    if (!dataBytes) return NULL;
    if (__CFOASafe) __CFSetLastAllocationEventName(dataBytes, "CFTimeZone (temp)");
#else
    unsigned char dataBytes[52 + nameLen + 1];
#endif
    memset(dataBytes, 0, sizeof(dataBytes));
    __CFEntzcode(1, dataBytes + 20);
    __CFEntzcode(1, dataBytes + 24);
    __CFEntzcode(1, dataBytes + 36);
    __CFEntzcode(nameLen + 1, dataBytes + 40);
    __CFEntzcode(seconds, dataBytes + 44);
    dataBytes[48] = isDST ? 1 : 0;
    CFStringGetCString(name, dataBytes + 50, nameLen + 1, kCFStringEncodingASCII);
    data = CFDataCreate(allocator, dataBytes, 52 + nameLen + 1);
    result = CFTimeZoneCreate(allocator, name, data);
    CFRelease(data);
#if defined(__WIN32__)
    CFAllocatorDeallocate(allocator, dataBytes);
#endif
    return result;
}
#endif

// rounds offset to nearest minute
CFTimeZoneRef CFTimeZoneCreateWithTimeIntervalFromGMT(CFAllocatorRef allocator, CFTimeInterval ti) {
    CFTimeZoneRef result;
    CFStringRef name;
    int32_t seconds, minute, hour;
    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    if (ti < -18.0 * 3600 || 18.0 * 3600 < ti) return NULL;
    ti = (ti < 0.0) ? ceil((ti / 60.0) - 0.5) * 60.0 : floor((ti / 60.0) + 0.5) * 60.0;
    seconds = (int32_t)ti;
    hour = (ti < 0) ? (-seconds / 3600) : (seconds / 3600);
    seconds -= ((ti < 0) ? -hour : hour) * 3600;
    minute = (ti < 0) ? (-seconds / 60) : (seconds / 60);
    if (fabs(ti) < 1.0) {
	name = CFRetain(CFSTR("GMT"));
    } else {
	name = CFStringCreateWithFormat(allocator, NULL, CFSTR("GMT%c%02d%02d"), (ti < 0.0 ? '-' : '+'), hour, minute);
    }
#if !defined(__WIN32__)
    result = __CFTimeZoneCreateFixed(allocator, (int32_t)ti, name, 0);
#else
/* CFTimeZoneRef->_data will contain TIME_ZONE_INFORMATION structure
 * to find current timezone
 * (Aleksey Dukhnyakov)
 */
    {
        TIME_ZONE_INFORMATION tzi;
        CFDataRef data;
        CFIndex length = CFStringGetLength(name);

        memset(&tzi,0,sizeof(tzi));
        tzi.Bias=(long)(-ti/60);
        CFStringGetCharacters(name, CFRangeMake(0, length < 31 ? length : 31 ), tzi.StandardName);
        data = CFDataCreate(allocator,(UInt8*)&tzi, sizeof(tzi));
        result = CFTimeZoneCreate(allocator, name, data);
        CFRelease(data);
    }
#endif
    CFRelease(name);
    return result;
}

CFTimeZoneRef CFTimeZoneCreateWithName(CFAllocatorRef allocator, CFStringRef name, Boolean tryAbbrev) {
    CFTimeZoneRef result = NULL;
    CFStringRef tzName = NULL;
    CFDataRef data = NULL;

    if (allocator == NULL) allocator = __CFGetDefaultAllocator();
    __CFGenericValidateType(allocator, CFAllocatorGetTypeID());
    __CFGenericValidateType(name, CFStringGetTypeID());
    if (CFEqual(CFSTR(""), name)) {
	// empty string is not a time zone name, just abort now,
	// following stuff will fail anyway
	return NULL;
    }
    __CFTimeZoneLockGlobal();
    if (NULL != __CFTimeZoneCache && CFDictionaryGetValueIfPresent(__CFTimeZoneCache, name, (const void **)&result)) {
	__CFTimeZoneUnlockGlobal();
	return (CFTimeZoneRef)CFRetain(result);
    }
    __CFTimeZoneUnlockGlobal();
#if !defined(__WIN32__)
    CFURLRef baseURL, tempURL;
    void *bytes;
    CFIndex length;

    baseURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR(TZZONEINFO), kCFURLPOSIXPathStyle, true);
    if (tryAbbrev) {
	CFDictionaryRef abbrevs = CFTimeZoneCopyAbbreviationDictionary();
	tzName = CFDictionaryGetValue(abbrevs, name);
	if (NULL != tzName) {
	    tempURL = CFURLCreateCopyAppendingPathComponent(kCFAllocatorDefault, baseURL, tzName, false);
	    if (NULL != tempURL) {
		if (_CFReadBytesFromFile(kCFAllocatorDefault, tempURL, &bytes, &length, 0)) {
		    data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes, length, kCFAllocatorDefault);
		}
		CFRelease(tempURL);
	    }
	}
	CFRelease(abbrevs);
    }
    if (NULL == data) {
	CFDictionaryRef dict = __CFTimeZoneCopyCompatibilityDictionary();
	CFStringRef mapping = CFDictionaryGetValue(dict, name);
	if (mapping) {
	    name = mapping;
	} else if (CFStringHasPrefix(name, CFSTR(TZZONEINFO))) {
	    CFMutableStringRef unprefixed = CFStringCreateMutableCopy(kCFAllocatorDefault, CFStringGetLength(name), name);
	    CFStringDelete(unprefixed, CFRangeMake(0, sizeof(TZZONEINFO)));
	    mapping = CFDictionaryGetValue(dict, unprefixed);
	    if (mapping) {
		name = mapping;
	    }
	    CFRelease(unprefixed);
	}
	CFRelease(dict);
	if (CFEqual(CFSTR(""), name)) {
	    return NULL;
	}
    }
    if (NULL == data) {
	tzName = name;
	tempURL = CFURLCreateCopyAppendingPathComponent(kCFAllocatorDefault, baseURL, tzName, false);
	if (NULL != tempURL) {
	    if (_CFReadBytesFromFile(kCFAllocatorDefault, tempURL, &bytes, &length, 0)) {
		data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes, length, kCFAllocatorDefault);
	    }
	    CFRelease(tempURL);
	}
    }
    CFRelease(baseURL);
    if (NULL == data) {
	tzName = name;
	tempURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, tzName, kCFURLPOSIXPathStyle, false);
	if (NULL != tempURL) {
	    if (_CFReadBytesFromFile(kCFAllocatorDefault, tempURL, &bytes, &length, 0)) {
		data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes, length, kCFAllocatorDefault);
	    }
	    CFRelease(tempURL);
	}
    }
    if (NULL != data) {
	result = CFTimeZoneCreate(allocator, tzName, data);
	CFRelease(data);
    }
#else
/* Reading GMT offset and daylight flag from the registry
 * for TimeZone name
 * (Aleksey Dukhnyakov)
 */
    {
        CFStringRef safeName = name;
        struct {
            LONG Bias;
            LONG StandardBias;
            LONG DaylightBias;
            SYSTEMTIME StandardDate;
            SYSTEMTIME DaylightDate;
        } tzi;
        TIME_ZONE_INFORMATION tzi_system;

        HKEY hkResult;
        DWORD dwType, dwSize=sizeof(tzi),
        dwSize_name1=sizeof(tzi_system.StandardName),
        dwSize_name2=sizeof(tzi_system.DaylightName);

        if (tryAbbrev) {
            CFDictionaryRef abbrevs = CFTimeZoneCopyAbbreviationDictionary();
            tzName = CFDictionaryGetValue(abbrevs, name);
            if (NULL == tzName) {
                return NULL;
            }
            name = tzName;
        	CFRelease(abbrevs);
        }

/* Open regestry and move down to the TimeZone information
 */
        if (RegOpenKey(HKEY_LOCAL_MACHINE,_T(TZZONEINFO),&hkResult) !=
            ERROR_SUCCESS ) {
            return NULL;
        }
/* Move down to specific TimeZone name
 */
#if defined(UNICODE)
        if (RegOpenKey(hkResult,CFStringGetCharactersPtr(name) ,&hkResult) !=
                ERROR_SUCCESS ) {
#else
        if (RegOpenKey(hkResult,CFStringGetCStringPtr(name, CFStringGetSystemEncoding()),&hkResult) != ERROR_SUCCESS ) {
#endif
            return NULL;
        }
/* TimeZone information(offsets, daylight flag, ...) assign to tzi structure
 */
        if ( RegQueryValueEx(hkResult,_T("TZI"),NULL,&dwType,(LPBYTE)&tzi,&dwSize) != ERROR_SUCCESS &&
            RegQueryValueEx(hkResult,_T("Std"),NULL,&dwType,(LPBYTE)&tzi_system.StandardName,&dwSize_name1) != ERROR_SUCCESS &&
            RegQueryValueEx(hkResult,_T("Dlt"),NULL,&dwType,(LPBYTE)&tzi_system.DaylightName,&dwSize_name2) != ERROR_SUCCESS )
        {
            return NULL;
        }

        tzi_system.Bias=tzi.Bias;
        tzi_system.StandardBias=tzi.StandardBias;
        tzi_system.DaylightBias=tzi.DaylightBias;
        tzi_system.StandardDate=tzi.StandardDate;
        tzi_system.DaylightDate=tzi.DaylightDate;

/* CFTimeZoneRef->_data will contain TIME_ZONE_INFORMATION structure
 * to find current timezone
 * (Aleksey Dukhnyakov)
 */
        data = CFDataCreate(allocator,(UInt8*)&tzi_system, sizeof(tzi_system));

        RegCloseKey(hkResult);
        result = CFTimeZoneCreate(allocator, name, data);
        if (result) {
            if (tryAbbrev)
                result->_periods->abbrev = CFStringCreateCopy(allocator,safeName);
            else {
           }
        }
        CFRelease(data);
    }
#endif
    return result;
}

CFStringRef CFTimeZoneGetName(CFTimeZoneRef tz) {
    CF_OBJC_FUNCDISPATCH0(__kCFTimeZoneTypeID, CFStringRef, tz, "name");
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
    return tz->_name;
}

CFDataRef CFTimeZoneGetData(CFTimeZoneRef tz) {
    CF_OBJC_FUNCDISPATCH0(__kCFTimeZoneTypeID, CFDataRef, tz, "data");
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
    return tz->_data;
}

/* This function converts CFAbsoluteTime to (Win32) SYSTEMTIME
 * (Aleksey Dukhnyakov)
 */
#if defined(__WIN32__)
BOOL __CFTimeZoneGetWin32SystemTime(SYSTEMTIME * sys_time, CFAbsoluteTime time)
{
    LONGLONG l;
    FILETIME * ftime=(FILETIME*)&l;

    /*  seconds between 1601 and 1970 : 11644473600,
     *  seconds between 1970 and 2001 : 978307200,
     *  FILETIME - number of 100-nanosecond intervals since January 1, 1601
     */
    l=(time+11644473600LL+978307200)*10000000;
    if (FileTimeToSystemTime(ftime,sys_time))
        return TRUE;
    else
        return FALSE;
}
#endif

CFTimeInterval _CFTimeZoneGetDSTOffset(CFTimeZoneRef tz, CFAbsoluteTime at) {
#if !defined(__WIN32__)
// #warning this does not work for non-CFTimeZoneRefs
    CFIndex idx;
    idx = __CFBSearchTZPeriods(tz, at);
    // idx 0 is never returned if it is in DST
    if (__CFTZPeriodIsDST(&(tz->_periods[idx]))) {
	return __CFTZPeriodGMTOffset(&(tz->_periods[idx])) - __CFTZPeriodGMTOffset(&(tz->_periods[idx - 1]));
    }
#endif
    return 0.0;
}

// returns 0.0 if there is no data for the next switch after 'at'
CFAbsoluteTime _CFTimeZoneGetNextDSTSwitch(CFTimeZoneRef tz, CFAbsoluteTime at) {
#if !defined(__WIN32__)
// #warning this does not work for non-CFTimeZoneRefs
    CFIndex idx;
    idx = __CFBSearchTZPeriods(tz, at);
    if (tz->_periodCnt <= idx + 1) {
	return 0.0;
    }
    return (CFAbsoluteTime)__CFTZPeriodStartSeconds(&(tz->_periods[idx + 1]));
#endif
    return 0.0;
}

CFTimeInterval CFTimeZoneGetSecondsFromGMT(CFTimeZoneRef tz, CFAbsoluteTime at) {
#if !defined(__WIN32__)
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH1(__kCFTimeZoneTypeID, CFTimeInterval, tz, "_secondsFromGMTForAbsoluteTime:", at);
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
    idx = __CFBSearchTZPeriods(tz, at);
    return __CFTZPeriodGMTOffset(&(tz->_periods[idx]));
#else
/* To calculate seconds from GMT, calculate current timezone time and
 * subtract GMT timnezone time
 * (Aleksey Dukhnyakov)
 */
 	TIME_ZONE_INFORMATION tzi;
	FILETIME ftime1,ftime2;
	SYSTEMTIME stime0,stime1,stime2;
	LONGLONG * l1= (LONGLONG*)&ftime1;
    LONGLONG * l2= (LONGLONG*)&ftime2;
    CFRange range={0,sizeof(TIME_ZONE_INFORMATION)};
    double result;

    CF_OBJC_FUNCDISPATCH1(__kCFTimeZoneTypeID, CFTimeInterval, tz, "_secondsFromGMTForAbsoluteTime:", at);

    CFDataGetBytes(tz->_data,range,(UInt8*)&tzi);

    if (!__CFTimeZoneGetWin32SystemTime(&stime0,at) ||
            !SystemTimeToTzSpecificLocalTime(&tzi,&stime0,&stime1) ||
	        !SystemTimeToFileTime(&stime1,&ftime1) )
	{
        CFAssert(0, __kCFLogAssertion, "Win32 system time/timezone failed !\n");
		return 0;
	}

    tzi.DaylightDate.wMonth=0;
	tzi.StandardDate.wMonth=0;
    tzi.StandardBias=0;
    tzi.DaylightBias=0;
    tzi.Bias=0;

	if ( !SystemTimeToTzSpecificLocalTime(&tzi,&stime0,&stime2) ||
            !SystemTimeToFileTime(&stime2,&ftime2))
    {
        CFAssert(0, __kCFLogAssertion, "Win32 system time/timezone failed !\n");
		return 0;
    }
    result=(double)((*l1-*l2)/10000000);
	return result;
#endif
}

#if defined(__WIN32__)
/*
 * Get abbreviation for name for WIN32 platform
 * (Aleksey Dukhnyakov)
 */

typedef struct {
    CFStringRef tzName;
    CFStringRef tzAbbr;
} _CFAbbrFind;

static void _CFFindKeyForValue(const void *key, const void *value, void *context) {
    if ( ((_CFAbbrFind *)context)->tzAbbr != NULL ) {
        if ( ((_CFAbbrFind *)context)->tzName == (CFStringRef) value ) {
            ((_CFAbbrFind *)context)->tzAbbr = key ;
        }
    }
}

CFIndex __CFTimeZoneInitAbbrev(CFTimeZoneRef tz) {

    if ( tz->_periods->abbrev == NULL ) {
        _CFAbbrFind abbr = { NULL, NULL };
        CFDictionaryRef abbrevs = CFTimeZoneCopyAbbreviationDictionary();

        CFDictionaryApplyFunction(abbrevs, _CFFindKeyForValue, &abbr);

        if ( abbr.tzAbbr != NULL)
            tz->_periods->abbrev = CFStringCreateCopy(kCFAllocatorDefault, abbr.tzAbbr);
        else
            tz->_periods->abbrev = CFStringCreateCopy(kCFAllocatorDefault, tz->_name);
/* We should return name of TimeZone if couldn't find abbrevation.
 * (Ala on MACOSX)
 *
 * old line : tz->_periods->abbrev =
 * CFStringCreateWithCString(kCFAllocatorDefault,"UNKNOWN",
 * CFStringGetSystemEncoding());
 *
 * (Aleksey Dukhnyakov)
*/
        CFRelease( abbrevs );
    }

    return 0;
}
#endif

CFStringRef CFTimeZoneCopyAbbreviation(CFTimeZoneRef tz, CFAbsoluteTime at) {
    CFStringRef result;
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH1(__kCFTimeZoneTypeID, CFStringRef, tz, "_abbreviationForAbsoluteTime:", at);
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
#if !defined(__WIN32__)
    idx = __CFBSearchTZPeriods(tz, at);
#else
/*
 * Initialize abbreviation for this TimeZone
 * (Aleksey Dukhnyakov)
 */
    idx = __CFTimeZoneInitAbbrev(tz);
#endif
    result = __CFTZPeriodAbbreviation(&(tz->_periods[idx]));
    return result ? CFRetain(result) : NULL;
}

Boolean CFTimeZoneIsDaylightSavingTime(CFTimeZoneRef tz, CFAbsoluteTime at) {
#if !defined(__WIN32__)
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH1(__kCFTimeZoneTypeID, Boolean, tz, "_isDaylightSavingTimeForAbsoluteTime:", at);
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
    idx = __CFBSearchTZPeriods(tz, at);
    return __CFTZPeriodIsDST(&(tz->_periods[idx]));
#else
/* Compare current timezone time and current timezone time without
 * transition to day light saving time
 * (Aleskey Dukhnyakov)
 */
	TIME_ZONE_INFORMATION tzi;
	SYSTEMTIME stime0,stime1,stime2;
    CFRange range={0,sizeof(TIME_ZONE_INFORMATION)};

    CF_OBJC_FUNCDISPATCH1(__kCFTimeZoneTypeID, Boolean, tz, "_isDaylightSavingTimeForAbsoluteTime:", at);

    CFDataGetBytes(tz->_data,range,(UInt8*)&tzi);

	if ( !__CFTimeZoneGetWin32SystemTime(&stime0,at) ||
            !SystemTimeToTzSpecificLocalTime(&tzi,&stime0,&stime1)) {
        CFAssert(0, __kCFLogAssertion, "Win32 system time/timezone failed !\n");
		return FALSE;
    }

    tzi.DaylightDate.wMonth=0;
	tzi.StandardDate.wMonth=0;

	if ( !SystemTimeToTzSpecificLocalTime(&tzi,&stime0,&stime2)) {
        CFAssert(0, __kCFLogAssertion, "Win32 system time/timezone failed !\n");
		return FALSE;
    }

    if ( !memcmp(&stime1,&stime2,sizeof(stime1)) )
        return FALSE;

    return TRUE;
#endif
}

CFTimeInterval _CFTimeZoneGetDSTDelta(CFTimeZoneRef tz, CFAbsoluteTime at) {
    CFIndex idx;
    __CFGenericValidateType(tz, __kCFTimeZoneTypeID);
    idx = __CFBSearchTZPeriods(tz, at);
    CFTimeInterval delta = __CFTZPeriodGMTOffset(&(tz->_periods[idx]));
    if (idx + 1 < tz->_periodCnt) {
	return fabs(delta - __CFTZPeriodGMTOffset(&(tz->_periods[idx + 1])));
    } else if (0 < idx) {
	return fabs(delta - __CFTZPeriodGMTOffset(&(tz->_periods[idx - 1])));
    }
    return 0.0;
}

static CFDictionaryRef __CFTimeZoneCopyCompatibilityDictionary(void) {
    CFDictionaryRef dict;
    __CFTimeZoneLockCompatibilityMapping();
    if (NULL == __CFTimeZoneCompatibilityMappingDict) {
	__CFTimeZoneCompatibilityMappingDict = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 112, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	
	// Empty string means delete/ignore these 
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Factory"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Pacific-New"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Mideast/Riyadh87"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Mideast/Riyadh88"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Mideast/Riyadh89"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/AST4"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/AST4ADT"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/CST6"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/CST6CDT"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/EST5"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/EST5EDT"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/HST10"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/MST7"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/MST7MDT"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/PST8"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/PST8PDT"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/YST9"), CFSTR(""));
	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("SystemV/YST9YDT"), CFSTR(""));

	CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Atka"), CFSTR("America/Adak"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Ensenada"), CFSTR("America/Tijuana"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Fort_Wayne"), CFSTR("America/Indianapolis"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Indiana/Indianapolis"), CFSTR("America/Indianapolis"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Kentucky/Louisville"), CFSTR("America/Louisville"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Knox_IN"), CFSTR("America/Indiana/Knox"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Porto_Acre"), CFSTR("America/Rio_Branco"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Rosario"), CFSTR("America/Cordoba"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Shiprock"), CFSTR("America/Denver"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("America/Virgin"), CFSTR("America/St_Thomas"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Antarctica/South_Pole"), CFSTR("Antarctica/McMurdo"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Ashkhabad"), CFSTR("Asia/Ashgabat"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Chungking"), CFSTR("Asia/Chongqing"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Macao"), CFSTR("Asia/Macau"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Tel_Aviv"), CFSTR("Asia/Jerusalem"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Thimbu"), CFSTR("Asia/Thimphu"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Ujung_Pandang"), CFSTR("Asia/Makassar"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Asia/Ulan_Bator"), CFSTR("Asia/Ulaanbaatar"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/ACT"), CFSTR("Australia/Sydney"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/LHI"), CFSTR("Australia/Lord_Howe"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/NSW"), CFSTR("Australia/Sydney"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/North"), CFSTR("Australia/Darwin"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/Queensland"), CFSTR("Australia/Brisbane"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/South"), CFSTR("Australia/Adelaide"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/Tasmania"), CFSTR("Australia/Hobart"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/Victoria"), CFSTR("Australia/Melbourne"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/West"), CFSTR("Australia/Perth"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Australia/Yancowinna"), CFSTR("Australia/Broken_Hill"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Brazil/Acre"), CFSTR("America/Porto_Acre"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Brazil/DeNoronha"), CFSTR("America/Noronha"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Brazil/West"), CFSTR("America/Manaus"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("CST6CDT"), CFSTR("America/Chicago"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Canada/Central"), CFSTR("America/Winnipeg"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Canada/East-Saskatchewan"), CFSTR("America/Regina"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Canada/Pacific"), CFSTR("America/Vancouver"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Canada/Yukon"), CFSTR("America/Whitehorse"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Chile/Continental"), CFSTR("America/Santiago"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Chile/EasterIsland"), CFSTR("Pacific/Easter"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Cuba"), CFSTR("America/Havana"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("EST5EDT"), CFSTR("America/New_York"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Egypt"), CFSTR("Africa/Cairo"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Eire"), CFSTR("Europe/Dublin"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Etc/GMT+0"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Etc/GMT-0"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Etc/GMT0"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Etc/Greenwich"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Etc/Universal"), CFSTR("UTC"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Etc/Zulu"), CFSTR("UTC"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Europe/Nicosia"), CFSTR("Asia/Nicosia"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Europe/Tiraspol"), CFSTR("Europe/Chisinau"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("GB-Eire"), CFSTR("Europe/London"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("GB"), CFSTR("Europe/London"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("GMT+0"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("GMT-0"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("GMT0"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Greenwich"), CFSTR("GMT"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Hongkong"), CFSTR("Asia/Hong_Kong"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Iceland"), CFSTR("Atlantic/Reykjavik"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Iran"), CFSTR("Asia/Tehran"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Israel"), CFSTR("Asia/Jerusalem"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Jamaica"), CFSTR("America/Jamaica"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Kwajalein"), CFSTR("Pacific/Kwajalein"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Libya"), CFSTR("Africa/Tripoli"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("MST7MDT"), CFSTR("America/Denver"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Mexico/BajaNorte"), CFSTR("America/Tijuana"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Mexico/BajaSur"), CFSTR("America/Mazatlan"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Mexico/General"), CFSTR("America/Mexico_City"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("NZ-CHAT"), CFSTR("Pacific/Chatham"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("NZ"), CFSTR("Pacific/Auckland"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Navajo"), CFSTR("America/Denver"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("PRC"), CFSTR("Asia/Shanghai"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("PST8PDT"), CFSTR("America/Los_Angeles"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Pacific/Samoa"), CFSTR("Pacific/Pago_Pago"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Poland"), CFSTR("Europe/Warsaw"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Portugal"), CFSTR("Europe/Lisbon"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("ROC"), CFSTR("Asia/Taipei"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("ROK"), CFSTR("Asia/Seoul"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Singapore"), CFSTR("Asia/Singapore"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Turkey"), CFSTR("Europe/Istanbul"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("UCT"), CFSTR("UTC"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Alaska"), CFSTR("America/Anchorage"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Aleutian"), CFSTR("America/Adak"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Arizona"), CFSTR("America/Phoenix"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/East-Indiana"), CFSTR("America/Indianapolis"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Hawaii"), CFSTR("Pacific/Honolulu"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Indiana-Starke"), CFSTR("America/Indiana/Knox"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Michigan"), CFSTR("America/Detroit"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("US/Samoa"), CFSTR("Pacific/Pago_Pago"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Universal"), CFSTR("UTC"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("W-SU"), CFSTR("Europe/Moscow"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict, CFSTR("Zulu"), CFSTR("UTC"));
    }
    dict = __CFTimeZoneCompatibilityMappingDict ? CFRetain(__CFTimeZoneCompatibilityMappingDict) : NULL;
    __CFTimeZoneUnlockCompatibilityMapping();
    return dict;
}

__private_extern__ CFDictionaryRef __CFTimeZoneCopyCompatibilityDictionary2(void) {
    CFDictionaryRef dict;
    __CFTimeZoneLockCompatibilityMapping();
    if (NULL == __CFTimeZoneCompatibilityMappingDict2) {
	__CFTimeZoneCompatibilityMappingDict2 = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 16, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Asia/Dacca"), CFSTR("Asia/Dhaka"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Asia/Istanbul"), CFSTR("Europe/Istanbul"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Australia/Canberra"), CFSTR("Australia/Sydney"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Brazil/East"), CFSTR("America/Sao_Paulo"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Canada/Atlantic"), CFSTR("America/Halifax"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Canada/Eastern"), CFSTR("America/Montreal"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Canada/Mountain"), CFSTR("America/Edmonton"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Canada/Newfoundland"), CFSTR("America/St_Johns"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Canada/Saskatchewan"), CFSTR("America/Regina"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("Japan"), CFSTR("Asia/Tokyo"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("US/Central"), CFSTR("America/Chicago"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("US/Eastern"), CFSTR("America/New_York"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("US/Mountain"), CFSTR("America/Denver"));
        CFDictionaryAddValue(__CFTimeZoneCompatibilityMappingDict2, CFSTR("US/Pacific"), CFSTR("America/Los_Angeles"));
   }
    dict = __CFTimeZoneCompatibilityMappingDict2 ? CFRetain(__CFTimeZoneCompatibilityMappingDict2) : NULL;
    __CFTimeZoneUnlockCompatibilityMapping();
    return dict;
}


