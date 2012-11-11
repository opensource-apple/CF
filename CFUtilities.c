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
/*	CFUtilities.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFPriv.h"
#include "CFInternal.h"
#include "CFPriv.h"
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFCalendar.h>
#if (DEPLOYMENT_TARGET_MACOSX) 
#include <CoreFoundation/CFLogUtilities.h>
#include <asl.h>
#include <sys/uio.h>
#endif
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if DEPLOYMENT_TARGET_MACOSX
    #include <mach/mach.h>
    #include <pthread.h>
    #include <mach-o/loader.h>
    #include <mach-o/dyld.h>
    #include <crt_externs.h>
    #include <dlfcn.h>
    #include <vproc.h>
    #include <vproc_priv.h>
    #include <sys/stat.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#endif
#if DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
    #include <string.h>
    #include <pthread.h>
#endif


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
   
*/
__private_extern__ CFIndex CFBSearch(const void *element, CFIndex elementSize, const void *list, CFIndex count, CFComparatorFunction comparator, void *context) {
    const char *ptr = (const char *)list;
    while (0 < count) {
        CFIndex half = count / 2;
        const char *probe = ptr + elementSize * half;
        CFComparisonResult cr = comparator(element, probe, context);
	if (0 == cr) return (probe - (const char *)list) / elementSize;
        ptr = (cr < 0) ? ptr : probe + elementSize;
        count = (cr < 0) ? half : (half + (count & 1) - 1);
    }
    return (ptr - (const char *)list) / elementSize;
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


#if DEPLOYMENT_TARGET_MACOSX
__private_extern__ uintptr_t __CFFindPointer(uintptr_t ptr, uintptr_t start) {
    vm_map_t task = mach_task_self();
    mach_vm_address_t address = start;
    for (;;) {
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object_name;
        kern_return_t ret = mach_vm_region(task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object_name);
        if (KERN_SUCCESS != ret) break;
	boolean_t scan = (info.protection & VM_PROT_WRITE) ? 1 : 0;
	if (scan) {
	    uintptr_t *addr = (uintptr_t *)((uintptr_t)address);
	    uintptr_t *end = (uintptr_t *)((uintptr_t)address + (uintptr_t)size);
	    while (addr < end) {
	        if ((uintptr_t *)start <= addr && *addr == ptr) {
		    return (uintptr_t)addr;
	        }
	        addr++;
	    }
	}
        address += size;
    }
    return 0;
}
#endif


__private_extern__ void *__CFStartSimpleThread(void *func, void *arg) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
    pthread_attr_t attr;
    pthread_t tid = 0;
    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 60 * 1024);	// 60K stack for our internal threads is sufficient
    OSMemoryBarrier(); // ensure arg is fully initialized and set in memory
    pthread_create(&tid, &attr, func, arg);
    pthread_attr_destroy(&attr);
//warning CF: we dont actually know that a pthread_t is the same size as void *
    return (void *)tid;
#else
    unsigned tid;
    struct _args *args = (struct _args*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(struct _args), 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(args, "CFUtilities (thread-args)");
    HANDLE handle;
    args->func = func;
    args->arg = arg;
    /* The thread is created suspended, because otherwise there would be a race between the assignment below of the handle field, and it's possible use in the thread func above. */
    args->handle = (HANDLE)_beginthreadex(NULL, 0, __CFWinThreadFunc, args, CREATE_SUSPENDED, &tid);
    handle = args->handle;
    ResumeThread(handle);
    return handle;
#endif
}

__private_extern__ CFStringRef _CFCreateLimitedUniqueString() {
    /* this unique string is only unique to the current host during the current boot */
    uint64_t tsr = __CFReadTSR();
    UInt32 tsrh = (UInt32)(tsr >> 32), tsrl = (UInt32)(tsr & (int64_t)0xFFFFFFFF);
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("CFUniqueString-%lu%lu$"), tsrh, tsrl);
}


// Looks for localized version of "nonLocalized" in the SystemVersion bundle
// If not found, and returnNonLocalizedFlag == true, will return the non localized string (retained of course), otherwise NULL
// If bundlePtr != NULL, will use *bundlePtr and will return the bundle in there; otherwise bundle is created and released

static CFStringRef _CFCopyLocalizedVersionKey(CFBundleRef *bundlePtr, CFStringRef nonLocalized) {
    CFStringRef localized = NULL;
    CFBundleRef locBundle = bundlePtr ? *bundlePtr : NULL;
    if (!locBundle) {
        CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, CFSTR("/System/Library/CoreServices/SystemVersion.bundle"), kCFURLPOSIXPathStyle, false);
        if (url) {
            locBundle = CFBundleCreate(kCFAllocatorSystemDefault, url);
            CFRelease(url);
        }
    }
    if (locBundle) {
	localized = CFBundleCopyLocalizedString(locBundle, nonLocalized, nonLocalized, CFSTR("SystemVersion"));
	if (bundlePtr) *bundlePtr = locBundle; else CFRelease(locBundle);
    }
    return localized ? localized : (CFStringRef)CFRetain(nonLocalized);
}

static CFDictionaryRef _CFCopyVersionDictionary(CFStringRef path) {
    CFPropertyListRef plist = NULL;
    CFDataRef data;
    CFURLRef url;

    url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, path, kCFURLPOSIXPathStyle, false);
    if (url && CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, url, &data, NULL, NULL, NULL)) {
	plist = CFPropertyListCreateFromXMLData(kCFAllocatorSystemDefault, data, kCFPropertyListMutableContainers, NULL);
	CFRelease(data);
    }
    if (url) CFRelease(url);
    
    if (plist) {
#if DEPLOYMENT_TARGET_MACOSX
	CFBundleRef locBundle = NULL;
	CFStringRef fullVersion, vers, versExtra, build;
	CFStringRef versionString = _CFCopyLocalizedVersionKey(&locBundle, _kCFSystemVersionProductVersionStringKey);
	CFStringRef buildString = _CFCopyLocalizedVersionKey(&locBundle, _kCFSystemVersionBuildStringKey);
	CFStringRef fullVersionString = _CFCopyLocalizedVersionKey(&locBundle, CFSTR("FullVersionString"));
	if (locBundle) CFRelease(locBundle);

        // Now build the full version string
        if (CFEqual(fullVersionString, CFSTR("FullVersionString"))) {
            CFRelease(fullVersionString);
            fullVersionString = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%@ %%@ (%@ %%@)"), versionString, buildString);
        }
        vers = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)plist, _kCFSystemVersionProductVersionKey);
        versExtra = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)plist, _kCFSystemVersionProductVersionExtraKey);
        if (vers && versExtra) vers = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%@ %@"), vers, versExtra);
        build = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)plist, _kCFSystemVersionBuildVersionKey);
        fullVersion = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, fullVersionString, (vers ? vers : CFSTR("?")), build ? build : CFSTR("?"));
        if (vers && versExtra) CFRelease(vers);
        
	CFDictionarySetValue((CFMutableDictionaryRef)plist, _kCFSystemVersionProductVersionStringKey, versionString);
	CFDictionarySetValue((CFMutableDictionaryRef)plist, _kCFSystemVersionBuildStringKey, buildString);
	CFDictionarySetValue((CFMutableDictionaryRef)plist, CFSTR("FullVersionString"), fullVersion);
 	CFRelease(versionString);
	CFRelease(buildString);
	CFRelease(fullVersionString);
        CFRelease(fullVersion);
#endif
    }    
    return (CFDictionaryRef)plist;
}

#if defined (__MACH__) || 0
CFStringRef CFCopySystemVersionString(void) {
    CFStringRef versionString;
    CFDictionaryRef dict = _CFCopyServerVersionDictionary();
    if (!dict) dict = _CFCopySystemVersionDictionary();
    versionString = CFDictionaryGetValue(dict, CFSTR("FullVersionString"));
    if (versionString) CFRetain(versionString);
    CFRelease(dict);
    return versionString;
}

// Obsolete: These two functions cache the dictionaries to avoid calling _CFCopyVersionDictionary() more than once per dict desired
// In fact, they do not cache any more, because the file can change after
// apps are running in some situations, and apps need the new info.
// Proper caching and testing to see if the file has changed, without race
// conditions, would require semi-convoluted use of fstat().

CFDictionaryRef _CFCopySystemVersionDictionary(void) {
    CFPropertyListRef plist = NULL;
	plist = _CFCopyVersionDictionary(CFSTR("/System/Library/CoreServices/SystemVersion.plist"));
    return plist;
}

CFDictionaryRef _CFCopyServerVersionDictionary(void) {
    CFPropertyListRef plist = NULL;
	plist = _CFCopyVersionDictionary(CFSTR("/System/Library/CoreServices/ServerVersion.plist"));
    return plist;
}

CONST_STRING_DECL(_kCFSystemVersionProductNameKey, "ProductName")
CONST_STRING_DECL(_kCFSystemVersionProductCopyrightKey, "ProductCopyright")
CONST_STRING_DECL(_kCFSystemVersionProductVersionKey, "ProductVersion")
CONST_STRING_DECL(_kCFSystemVersionProductVersionExtraKey, "ProductVersionExtra")
CONST_STRING_DECL(_kCFSystemVersionProductUserVisibleVersionKey, "ProductUserVisibleVersion")
CONST_STRING_DECL(_kCFSystemVersionBuildVersionKey, "ProductBuildVersion")
CONST_STRING_DECL(_kCFSystemVersionProductVersionStringKey, "Version")
CONST_STRING_DECL(_kCFSystemVersionBuildStringKey, "Build")
#endif //__MACH__

CF_EXPORT Boolean _CFExecutableLinkedOnOrAfter(CFSystemVersion version) {
    return true;
}

#if DEPLOYMENT_TARGET_MACOSX
__private_extern__ void *__CFLookupCFNetworkFunction(const char *name) {
    static void *image = NULL;
    if (NULL == image) {
	const char *path = NULL;
	if (!issetugid()) {
	    path = getenv("CFNETWORK_LIBRARY_PATH");
	}
	if (!path) {
	    path = "/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/CFNetwork.framework/Versions/A/CFNetwork";
	}
	image = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    }
    void *dyfunc = NULL;
    if (image) {
	dyfunc = dlsym(image, name);
    }
    return dyfunc;
}
#endif //__MACH__


#ifndef __CFGetSessionID_defined

__private_extern__ uint32_t __CFGetSessionID(void) {
    return 0;
}

#endif

const char *_CFPrintForDebugger(const void *obj) {
	static char *result = NULL;
	CFStringRef str;
	CFIndex cnt = 0;

	free(result);	// Let go of result from previous call.
	result = NULL;
	if (obj) {
		if (CFGetTypeID(obj) == CFStringGetTypeID()) {
			// Makes Ali marginally happier
			str = __CFCopyFormattingDescription(obj, NULL);
			if (!str) str = CFCopyDescription(obj);
		} else {
			str = CFCopyDescription(obj);
		}
	} else {
		str = (CFStringRef)CFRetain(CFSTR("(null)"));
	}
	
	if (str != NULL) {
		CFStringGetBytes(str, CFRangeMake(0, CFStringGetLength(str)), kCFStringEncodingUTF8, 0, FALSE, NULL, 0, &cnt);
	}
	result = (char *) malloc(cnt + 2);	// 1 for '\0', 1 for an optional '\n'
	if (str != NULL) {
		CFStringGetBytes(str, CFRangeMake(0, CFStringGetLength(str)), kCFStringEncodingUTF8, 0, FALSE, (UInt8 *) result, cnt, &cnt);
	}
	result[cnt] = '\0';

	if (str) CFRelease(str);
	return result;
}

static void _CFShowToFile(FILE *file, Boolean flush, const void *obj) {
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
	str = (CFStringRef)CFRetain(CFSTR("(null)"));
     }
     cnt = CFStringGetLength(str);

     // iTunes used OutputDebugStringW(theString);

     CFStringInitInlineBuffer(str, &buffer, CFRangeMake(0, cnt));
#if defined (__WIN32__)
    TCHAR *accumulatedBuffer = (TCHAR *)malloc((cnt+1) * sizeof(TCHAR));
#endif
     for (idx = 0; idx < cnt; idx++) {
         UniChar ch = __CFStringGetCharacterFromInlineBufferQuick(&buffer, idx);

#if DEPLOYMENT_TARGET_MACOSX
		 if (ch < 128) {
             fprintf_l(file, NULL, "%c", ch);
	     lastNL = (ch == '\n');
         } else {
             fprintf_l(file, NULL, "\\u%04x", ch);
         }
#elif defined (__WIN32__)
		 if (ch < 128) {
             _fprintf_l(file, "%c", NULL, ch);
	     lastNL = (ch == '\n');
         } else {
             _fprintf_l(file, "\\u%04x", NULL, ch);
         }
#endif
     }
#if  defined(__WIN32__)
     }
#endif
     if (!lastNL) {
#if DEPLOYMENT_TARGET_MACOSX
         fprintf_l(file, NULL, "\n");
#endif
         if (flush) fflush(file);
     }

     if (str) CFRelease(str);
}

void CFShow(const void *obj) {
     _CFShowToFile(stderr, true, obj);
}



void CFLog(int32_t lev, CFStringRef format, ...) {
    CFStringRef result;
    va_list argList;
    static CFSpinLock_t lock = CFSpinLockInit;

    va_start(argList, format);
    result = CFStringCreateWithFormatAndArguments(kCFAllocatorSystemDefault, NULL, format, argList);
    va_end(argList);

    __CFSpinLock(&lock); 
    CFTimeZoneRef tz = CFTimeZoneCopySystem();	// specifically choose system time zone for logs
    CFGregorianDate gdate = CFAbsoluteTimeGetGregorianDate(CFAbsoluteTimeGetCurrent(), tz);
    CFRelease(tz);
    gdate.second = gdate.second + 0.0005;
    // Date format: YYYY '-' MM '-' DD ' ' hh ':' mm ':' ss.fff
    fprintf_l(stderr, NULL, "%04d-%02d-%02d %02d:%02d:%06.3f %s[%d:%x] CFLog: ", (int)gdate.year, gdate.month, gdate.day, gdate.hour, gdate.minute, gdate.second, *_CFGetProgname(), getpid(), pthread_mach_thread_np(pthread_self()));
    CFShow(result);

    __CFSpinUnlock(&lock); 
    CFRelease(result);
}


