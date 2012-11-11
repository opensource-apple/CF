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
/*	CFUtilities.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFUtilities.h"
#include "CFInternal.h"
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFTimeZone.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#if defined(__MACH__)
    #include <mach/mach.h>
    #include <pthread.h>
    #include <mach-o/dyld.h>
    #include <mach-o/loader.h>
    #include <crt_externs.h>
    #include <Security/AuthSession.h>
#endif
#if defined(__WIN32__)
    #include <windows.h>
    #include <process.h>
#endif
#if defined(__LINUX__) || defined(__FREEBSD__)
    #include <string.h>
    #include <pthread.h>
    #include <stdlib.h>
#endif

extern char **_CFGetProgname(void);

#define LESS16(A, W)	do { if (A < ((uint64_t)1 << (W))) LESS8(A, (W) - 8); LESS8(A, (W) + 8); } while (0)
#define LESS8(A, W)	do { if (A < ((uint64_t)1 << (W))) LESS4(A, (W) - 4); LESS4(A, (W) + 4); } while (0)
#define LESS4(A, W)	do { if (A < ((uint64_t)1 << (W))) LESS2(A, (W) - 2); LESS2(A, (W) + 2); } while (0)
#define LESS2(A, W)	do { if (A < ((uint64_t)1 << (W))) LESS1(A, (W) - 1); LESS1(A, (W) + 1); } while (0)
#define LESS1(A, W)	do { if (A < ((uint64_t)1 << (W))) return (W) - 1; return (W); } while (0)

uint32_t CFLog2(uint64_t x) {
    if (x < ((uint64_t)1 << 32))
	LESS16(x, 16);
    LESS16(x, 48);
    return 0;
}

#if 0
// faster version for PPC
int Lg2d(unsigned x) {
// use PPC-specific instruction to count leading zeros
    int ret;
    if (0 == x) return 0;
    __asm__ volatile("cntlzw %0,%1" : "=r" (ret) : "r" (x));
    return 31 - ret;
}
#endif

#undef LESS1
#undef LESS2
#undef LESS4
#undef LESS8
#undef LESS16

/* Comparator is passed the address of the values. */
/* Binary searches a sorted-increasing array of some type.
   Return value is either 1) the index of the element desired,
   if the target value exists in the list, 2) greater than or
   equal to count, if the element is greater than all the values
   in the list, or 3) the index of the element greater than the
   target value.

   For example, a search in the list of integers:
	2 3 5 7 11 13 17

   For...		Will Return...
	2		    0
   	5		    2
	23		    7
	1		    0
	9		    4

   For instance, if you just care about found/not found:
   index = CFBSearch(list, count, elem);
   if (count <= index || list[index] != elem) {
   	* Not found *
   } else {
   	* Found *
   }
   
   This isn't optimal yet.
*/
__private_extern__ CFIndex CFBSearch(const void *element, CFIndex elementSize, const void *list, CFIndex count, CFComparatorFunction comparator, void *context) {
    SInt32 idx, lg;
    const char *ptr = (const char *)list;
    if (count < 4) {
	switch (count) {
	case 3: if (comparator(ptr + elementSize * 2, element, context) < 0) return 3;
	case 2: if (comparator(ptr + elementSize * 1, element, context) < 0) return 2;
	case 1: if (comparator(ptr + elementSize * 0, element, context) < 0) return 1;
	}
	return 0;
    }
    if (comparator(ptr + elementSize * (count - 1), element, context) < 0) return count;
    if (comparator(element, ptr + elementSize * 0, context) < 0) return 0;
    lg = CFLog2(count); /* This takes about 1/3rd of the time, discounting calls to comparator */
    idx = (comparator(ptr + elementSize * (-1 + (1 << lg)), element, context) < 0) ? count - (1 << lg) : -1;
    switch (--lg) {
    case 30: if (comparator(ptr + elementSize * (idx + (1 << 30)), element, context) < 0) idx += (1 << 30);
    case 29: if (comparator(ptr + elementSize * (idx + (1 << 29)), element, context) < 0) idx += (1 << 29);
    case 28: if (comparator(ptr + elementSize * (idx + (1 << 28)), element, context) < 0) idx += (1 << 28);
    case 27: if (comparator(ptr + elementSize * (idx + (1 << 27)), element, context) < 0) idx += (1 << 27);
    case 26: if (comparator(ptr + elementSize * (idx + (1 << 26)), element, context) < 0) idx += (1 << 26);
    case 25: if (comparator(ptr + elementSize * (idx + (1 << 25)), element, context) < 0) idx += (1 << 25);
    case 24: if (comparator(ptr + elementSize * (idx + (1 << 24)), element, context) < 0) idx += (1 << 24);
    case 23: if (comparator(ptr + elementSize * (idx + (1 << 23)), element, context) < 0) idx += (1 << 23);
    case 22: if (comparator(ptr + elementSize * (idx + (1 << 22)), element, context) < 0) idx += (1 << 22);
    case 21: if (comparator(ptr + elementSize * (idx + (1 << 21)), element, context) < 0) idx += (1 << 21);
    case 20: if (comparator(ptr + elementSize * (idx + (1 << 20)), element, context) < 0) idx += (1 << 20);
    case 19: if (comparator(ptr + elementSize * (idx + (1 << 19)), element, context) < 0) idx += (1 << 19);
    case 18: if (comparator(ptr + elementSize * (idx + (1 << 18)), element, context) < 0) idx += (1 << 18);
    case 17: if (comparator(ptr + elementSize * (idx + (1 << 17)), element, context) < 0) idx += (1 << 17);
    case 16: if (comparator(ptr + elementSize * (idx + (1 << 16)), element, context) < 0) idx += (1 << 16);
    case 15: if (comparator(ptr + elementSize * (idx + (1 << 15)), element, context) < 0) idx += (1 << 15);
    case 14: if (comparator(ptr + elementSize * (idx + (1 << 14)), element, context) < 0) idx += (1 << 14);
    case 13: if (comparator(ptr + elementSize * (idx + (1 << 13)), element, context) < 0) idx += (1 << 13);
    case 12: if (comparator(ptr + elementSize * (idx + (1 << 12)), element, context) < 0) idx += (1 << 12);
    case 11: if (comparator(ptr + elementSize * (idx + (1 << 11)), element, context) < 0) idx += (1 << 11);
    case 10: if (comparator(ptr + elementSize * (idx + (1 << 10)), element, context) < 0) idx += (1 << 10);
    case 9:  if (comparator(ptr + elementSize * (idx + (1 << 9)), element, context) < 0) idx += (1 << 9);
    case 8:  if (comparator(ptr + elementSize * (idx + (1 << 8)), element, context) < 0) idx += (1 << 8);
    case 7:  if (comparator(ptr + elementSize * (idx + (1 << 7)), element, context) < 0) idx += (1 << 7);
    case 6:  if (comparator(ptr + elementSize * (idx + (1 << 6)), element, context) < 0) idx += (1 << 6);
    case 5:  if (comparator(ptr + elementSize * (idx + (1 << 5)), element, context) < 0) idx += (1 << 5);
    case 4:  if (comparator(ptr + elementSize * (idx + (1 << 4)), element, context) < 0) idx += (1 << 4);
    case 3:  if (comparator(ptr + elementSize * (idx + (1 << 3)), element, context) < 0) idx += (1 << 3);
    case 2:  if (comparator(ptr + elementSize * (idx + (1 << 2)), element, context) < 0) idx += (1 << 2);
    case 1:  if (comparator(ptr + elementSize * (idx + (1 << 1)), element, context) < 0) idx += (1 << 1);
    case 0:  if (comparator(ptr + elementSize * (idx + (1 << 0)), element, context) < 0) idx += (1 << 0);
    }
    return ++idx;
}

	
#define ELF_STEP(B) T1 = (H << 4) + B; T2 = T1 & 0xF0000000; if (T2) T1 ^= (T2 >> 24); T1 &= (~T2); H = T1;

CFHashCode CFHashBytes(uint8_t *bytes, CFIndex length) {
    /* The ELF hash algorithm, used in the ELF object file format */
    UInt32 H = 0, T1, T2;
    SInt32 rem = length;
    while (3 < rem) {
	ELF_STEP(bytes[length - rem]);
	ELF_STEP(bytes[length - rem + 1]);
	ELF_STEP(bytes[length - rem + 2]);
	ELF_STEP(bytes[length - rem + 3]);
	rem -= 4;
    }
    switch (rem) {
    case 3:  ELF_STEP(bytes[length - 3]);
    case 2:  ELF_STEP(bytes[length - 2]);
    case 1:  ELF_STEP(bytes[length - 1]);
    case 0:  ;
    }
    return H;
}

#undef ELF_STEP

#if defined(__WIN32__)
struct _args {
    void *func;
    void *arg;
    HANDLE handle;
};
static __stdcall unsigned __CFWinThreadFunc(void *arg) {
    struct _args *args = arg; 
    ((void (*)(void *))args->func)(args->arg);
    CloseHandle(args->handle);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, arg);
    _endthreadex(0);
    return 0; 
}
#endif

__private_extern__ void *__CFStartSimpleThread(void *func, void *arg) {
#if defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
    pthread_attr_t attr;
    pthread_t tid;
    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 60 * 1024);	// 60K stack for our internal threads is sufficient
    pthread_create(&tid, &attr, func, arg);
    pthread_attr_destroy(&attr);
//warning CF: we dont actually know that a pthread_t is the same size as void *
    return (void *)tid;
#else
    unsigned tid;
    struct _args *args = CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(struct _args), 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(args, "CFUtilities (thread-args)");
    HANDLE handle;
    args->func = func;
    args->arg = arg;
    /* The thread is created suspended, because otherwise there would be a race between the assignment below of the handle field, and it's possible use in the thread func above. */
    args->handle = (HANDLE)_beginthreadex(NULL, 0, (void *)__CFWinThreadFunc, args, CREATE_SUSPENDED, &tid);
    handle = args->handle;
    ResumeThread(handle);
    return handle;
#endif
}

__private_extern__ CFStringRef _CFCreateLimitedUniqueString() {
    /* this unique string is only unique to the current host during the current boot */
    uint64_t tsr = __CFReadTSR();
    UInt32 tsrh = (tsr >> 32), tsrl = (tsr & (int64_t)0xFFFFFFFF);
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("CFUniqueString-%lu%lu$"), tsrh, tsrl);
}


// Looks for localized version of "nonLocalized" in the SystemVersion bundle
// If not found, and returnNonLocalizedFlag == true, will return the non localized string (retained of course), otherwise NULL
// If bundlePtr != NULL, will use *bundlePtr and will return the bundle in there; otherwise bundle is created and released

static CFStringRef _CFCopyLocalizedVersionKey(CFBundleRef *bundlePtr, CFStringRef nonLocalized) {
    CFStringRef localized = NULL;
    CFBundleRef locBundle = bundlePtr ? *bundlePtr : NULL;
    if (!locBundle) {
        CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("/System/Library/CoreServices/SystemVersion.bundle"), kCFURLPOSIXPathStyle, false);
        if (url) {
            locBundle = CFBundleCreate(kCFAllocatorDefault, url);
            CFRelease(url);
        }
    }
    if (locBundle) {
	localized = CFBundleCopyLocalizedString(locBundle, nonLocalized, nonLocalized, CFSTR("SystemVersion"));
	if (bundlePtr) *bundlePtr = locBundle; else CFRelease(locBundle);
    }
    return localized ? localized : CFRetain(nonLocalized);
}

// Note, if this function is changed to cache the computed data using frozen named data support, it should really be called once per path (that is why the results are cached below in the functions calling this).
//
static CFDictionaryRef _CFCopyVersionDictionary(CFStringRef path) {
    CFPropertyListRef plist = NULL;
    CFDataRef data;
    CFURLRef url;

    url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, path, kCFURLPOSIXPathStyle, false);
    if (url && CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, url, &data, NULL, NULL, NULL)) {
	plist = CFPropertyListCreateFromXMLData(kCFAllocatorSystemDefault, data, kCFPropertyListMutableContainers, NULL);
	CFRelease(data);
    }
    if (url) CFRelease(url);
    
    if (plist) {
	CFBundleRef locBundle = NULL;
	CFStringRef fullVersion, vers, versExtra, build;
	CFStringRef versionString = _CFCopyLocalizedVersionKey(&locBundle, _kCFSystemVersionProductVersionStringKey);
	CFStringRef buildString = _CFCopyLocalizedVersionKey(&locBundle, _kCFSystemVersionBuildStringKey);
	CFStringRef fullVersionString = _CFCopyLocalizedVersionKey(&locBundle, CFSTR("FullVersionString"));
	if (locBundle) CFRelease(locBundle);

        // Now build the full version string
        if (CFEqual(fullVersionString, CFSTR("FullVersionString"))) {
            CFRelease(fullVersionString);
            fullVersionString = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %%@ (%@ %%@)"), versionString, buildString);
        }
        vers = CFDictionaryGetValue(plist, _kCFSystemVersionProductVersionKey);
        versExtra = CFDictionaryGetValue(plist, _kCFSystemVersionProductVersionExtraKey);
        if (vers && versExtra) vers = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %@"), vers, versExtra);
        build = CFDictionaryGetValue(plist, _kCFSystemVersionBuildVersionKey);
        fullVersion = CFStringCreateWithFormat(NULL, NULL, fullVersionString, (vers ? vers : CFSTR("?")), build ? build : CFSTR("?"));
        if (vers && versExtra) CFRelease(vers);
        
	CFDictionarySetValue((CFMutableDictionaryRef)plist, _kCFSystemVersionProductVersionStringKey, versionString);
	CFDictionarySetValue((CFMutableDictionaryRef)plist, _kCFSystemVersionBuildStringKey, buildString);
	CFDictionarySetValue((CFMutableDictionaryRef)plist, CFSTR("FullVersionString"), fullVersion);
 	CFRelease(versionString);
	CFRelease(buildString);
	CFRelease(fullVersionString);
        CFRelease(fullVersion);
    }    
    return plist;
}

CFStringRef CFCopySystemVersionString(void) {
    CFStringRef versionString;
    CFDictionaryRef dict = _CFCopyServerVersionDictionary();
    if (!dict) dict = _CFCopySystemVersionDictionary();
    versionString = CFDictionaryGetValue(dict, CFSTR("FullVersionString"));
    if (versionString) CFRetain(versionString);
    CFRelease(dict);
    return versionString;
}

// These two functions cache the dictionaries to avoid calling _CFCopyVersionDictionary() more than once per dict desired

CFDictionaryRef _CFCopySystemVersionDictionary(void) {
    static CFPropertyListRef plist = NULL;	// Set to -1 for failed lookup
    if (!plist) {
	plist = _CFCopyVersionDictionary(CFSTR("/System/Library/CoreServices/SystemVersion.plist"));
        if (!plist) plist = (CFPropertyListRef)(-1);
    }
    return (plist == (CFPropertyListRef)(-1)) ? NULL : CFRetain(plist);
}

CFDictionaryRef _CFCopyServerVersionDictionary(void) {
    static CFPropertyListRef plist = NULL;	// Set to -1 for failed lookup
    if (!plist) {
	plist = _CFCopyVersionDictionary(CFSTR("/System/Library/CoreServices/ServerVersion.plist"));
        if (!plist) plist = (CFPropertyListRef)(-1);
    }
    return (plist == (CFPropertyListRef)(-1)) ? NULL : CFRetain(plist);
}

CONST_STRING_DECL(_kCFSystemVersionProductNameKey, "ProductName")
CONST_STRING_DECL(_kCFSystemVersionProductCopyrightKey, "ProductCopyright")
CONST_STRING_DECL(_kCFSystemVersionProductVersionKey, "ProductVersion")
CONST_STRING_DECL(_kCFSystemVersionProductVersionExtraKey, "ProductVersionExtra")
CONST_STRING_DECL(_kCFSystemVersionProductUserVisibleVersionKey, "ProductUserVisibleVersion")
CONST_STRING_DECL(_kCFSystemVersionBuildVersionKey, "ProductBuildVersion")
CONST_STRING_DECL(_kCFSystemVersionProductVersionStringKey, "Version")
CONST_STRING_DECL(_kCFSystemVersionBuildStringKey, "Build")

#if defined(__MACH__)
CFLibraryVersion CFGetExecutableLinkedLibraryVersion(CFStringRef libraryName) {
    CFLibraryVersion ret = {0xFFFF, 0xFF, 0xFF};
    struct mach_header *mh;
    struct load_command *lc;
    unsigned int idx;
    char library[CFMaxPathSize];	// search specs larger than this are pointless

    if (!CFStringGetCString(libraryName, library, sizeof(library), kCFStringEncodingUTF8)) return ret;
    mh = _dyld_get_image_header(0);	// image header #0 is the executable
    if (NULL == mh) return ret;
    lc = (struct load_command *)((char *)mh + sizeof(struct mach_header));
    for (idx = 0; idx < mh->ncmds; idx++) {
        if (lc->cmd == LC_LOAD_DYLIB) {
            struct dylib_command *dl = (struct dylib_command *)lc;
            char *path = (char *)lc + dl->dylib.name.offset;
            if (NULL != strstr(path, library)) {
		ret.primaryVersion = dl->dylib.current_version >> 16;
		ret.secondaryVersion = (dl->dylib.current_version >> 8) & 0xff;
		ret.tertiaryVersion = dl->dylib.current_version & 0xff;
		return ret;
            }
        }
        lc = (struct load_command *)((char *)lc + lc->cmdsize);
    }
    return ret;
}

CFLibraryVersion CFGetExecutingLibraryVersion(CFStringRef libraryName) {
    CFLibraryVersion ret = {0xFFFF, 0xFF, 0xFF};
    struct mach_header *mh;
    struct load_command *lc;
    unsigned int idx1, idx2, cnt;
    char library[CFMaxPathSize];	// search specs larger than this are pointless

    if (!CFStringGetCString(libraryName, library, sizeof(library), kCFStringEncodingUTF8)) return ret;
    cnt = _dyld_image_count();
    for (idx1 = 1; idx1 < cnt; idx1++) {
	char *image_name = _dyld_get_image_name(idx1);
	if (NULL == image_name || NULL == strstr(image_name, library)) continue;
	mh = _dyld_get_image_header(idx1);
	if (NULL == mh) return ret;
	lc = (struct load_command *)((char *)mh + sizeof(struct mach_header));
	for (idx2 = 0; idx2 < mh->ncmds; idx2++) {
	    if (lc->cmd == LC_ID_DYLIB) {
		struct dylib_command *dl = (struct dylib_command *)lc;
		ret.primaryVersion = dl->dylib.current_version >> 16;
		ret.secondaryVersion = (dl->dylib.current_version >> 8) & 0xff;
		ret.tertiaryVersion = dl->dylib.current_version & 0xff;
		return ret;
	    }
	}
    }
    return ret;
}


/*
If
   (vers != 0xFFFF): We know the version number of the library this app was linked against
   and (versionInfo[version].VERSIONFIELD != 0xFFFF): And we know what version number started the specified release
   and ((version == 0) || (versionInfo[version-1].VERSIONFIELD < versionInfo[version].VERSIONFIELD)): And it's distinct from the prev release
Then
   If the version the app is linked against is less than the version recorded for the specified release
   Then stop checking and return false
   Else stop checking and return YES
Else
   Continue checking (the next library)
*/
#define checkLibrary(LIBNAME, VERSIONFIELD) \
    {int vers = CFGetExecutableLinkedLibraryVersion(LIBNAME).primaryVersion; \
     if ((vers != 0xFFFF) && (versionInfo[version].VERSIONFIELD != 0xFFFF) && ((version == 0) || (versionInfo[version-1].VERSIONFIELD < versionInfo[version].VERSIONFIELD))) return (results[version] = ((vers < versionInfo[version].VERSIONFIELD) ? false : true)); }

CF_EXPORT Boolean _CFExecutableLinkedOnOrAfter(CFSystemVersion version) {
    // The numbers in the below table should be the numbers for any version of the framework in the release.
    // When adding new entries to this table for a new build train, it's simplest to use the versions of the
    // first new versions of projects submitted to the new train. These can later be updated. One thing to watch for is that software updates
    // for the previous release do not increase numbers beyond the number used for the next release!
    // For a given train, don't ever use the last versions submitted to the previous train! (This to assure room for software updates.)
    // If versions are the same as previous release, use 0xFFFF; this will assure the answer is a conservative NO.
    // NOTE: Also update the CFM check below, perhaps to the previous release... (???)
    static const struct {
        uint16_t libSystemVersion;
        uint16_t cocoaVersion;
        uint16_t appkitVersion;
        uint16_t fouVersion;
        uint16_t cfVersion;
        uint16_t carbonVersion;
        uint16_t applicationServicesVersion;
        uint16_t coreServicesVersion;
        uint16_t iokitVersion;
    } versionInfo[] = {
	{50, 5, 577, 397, 196, 113, 16, 9, 52},		/* CFSystemVersionCheetah (used the last versions) */
	{55, 7, 620, 425, 226, 122, 16, 10, 67},	/* CFSystemVersionPuma (used the last versions) */
        {56, 8, 631, 431, 232, 122, 17, 11, 73},	/* CFSystemVersionJaguar */
        {67, 9, 704, 481, 281, 126, 19, 16, 159},	/* CFSystemVersionPanther */
        {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}	/* CFSystemVersionMerlot */
    };
    static char results[CFSystemVersionMax] = {-2, -2, -2, -2, -2};	/* We cache the results per-release; there are only a few of these... */
    if (version >= CFSystemVersionMax) return false;	/* Actually, we don't know the answer, and something scary is going on */
    if (results[version] != -2) return results[version];

    if (_CFIsCFM()) {
        results[version] = (version <= CFSystemVersionJaguar) ? true : false;
        return results[version];
    }
    
    checkLibrary(CFSTR("/libSystem."), libSystemVersion);	// Pretty much everyone links with this
    checkLibrary(CFSTR("/Cocoa."), cocoaVersion);
    checkLibrary(CFSTR("/AppKit."), appkitVersion);
    checkLibrary(CFSTR("/Foundation."), fouVersion);
    checkLibrary(CFSTR("/CoreFoundation."), cfVersion);
    checkLibrary(CFSTR("/Carbon."), carbonVersion);
    checkLibrary(CFSTR("/ApplicationServices."), applicationServicesVersion);
    checkLibrary(CFSTR("/CoreServices."), coreServicesVersion);
    checkLibrary(CFSTR("/IOKit."), iokitVersion);
    
    /* If not found, then simply return NO to indicate earlier --- compatibility by default, unfortunately */
    return false;
}
#endif




void CFShow(const void *obj) { // ??? supposed to use stderr for Logging ?
     CFStringRef str;
     CFIndex idx, cnt;
     CFStringInlineBuffer buffer;
     bool lastNL = false;

     if (obj) {
	if (CFGetTypeID(obj) == CFStringGetTypeID()) {
	    // Makes Ali marginally happier
	    str = __CFCopyFormattingDescription(obj, NULL);
	    if (!str) str = CFCopyDescription(obj);
	} else {
	    str = CFCopyDescription(obj);
	}
     } else {
	str = CFRetain(CFSTR("(null)"));
     }
     cnt = CFStringGetLength(str);

     CFStringInitInlineBuffer(str, &buffer, CFRangeMake(0, cnt));
     for (idx = 0; idx < cnt; idx++) {
         UniChar ch = __CFStringGetCharacterFromInlineBufferQuick(&buffer, idx);
         if (ch < 128) {
             fprintf (stderr, "%c", ch);
	     lastNL = (ch == '\n');
         } else {
             fprintf (stderr, "\\u%04x", ch);
         }
     }
     if (!lastNL) fprintf(stderr, "\n");

     if (str) CFRelease(str);
}

void CFLog(int p, CFStringRef format, ...) {
    CFStringRef result;
    va_list argList;
    static CFSpinLock_t lock = 0;

    va_start(argList, format);
    result = CFStringCreateWithFormatAndArguments(NULL, NULL, format, argList);
    va_end(argList);

    __CFSpinLock(&lock); 
#if defined(__WIN32__)
    printf("*** CFLog (%d): %s[%d] ", p, __argv[0], GetCurrentProcessId());
#else
	// Date format: YYYY '-' MM '-' DD ' ' hh ':' mm ':' ss.fff
	CFTimeZoneRef tz = CFTimeZoneCopySystem();	// specifically choose system time zone for logs
	CFGregorianDate gdate = CFAbsoluteTimeGetGregorianDate(CFAbsoluteTimeGetCurrent(), tz);
	CFRelease(tz);
	gdate.second = gdate.second + 0.0005;
    fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%06.3f %s[%d] CFLog (%d): ", (int)gdate.year, gdate.month, gdate.day, gdate.hour, gdate.minute, gdate.second, *_CFGetProgname(), getpid(), p);
#endif

    CFShow(result);
    __CFSpinUnlock(&lock); 
    CFRelease(result);
}

