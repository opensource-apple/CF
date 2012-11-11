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

/*	CFUtilities.c
	Copyright (c) 1998-2009, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include <CoreFoundation/CFPriv.h>
#include "CFInternal.h"
#include "CFLocaleInternal.h"
#include <CoreFoundation/CFPriv.h>
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFCalendar.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <asl.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#include <unistd.h>
#include <sys/uio.h>
#include <mach/mach.h>
#include <pthread.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <vproc.h>
#include <vproc_priv.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <stdio.h>
#include <sys/errno.h>
#include <mach/mach_time.h>
#include <libkern/OSAtomic.h>
#include <Block.h>
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


#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
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

#if DEPLOYMENT_TARGET_WINDOWS
struct _args {
    void *func;
    void *arg;
    HANDLE handle;
};
static unsigned __stdcall __CFWinThreadFunc(void *arg) {
    struct _args *args = (struct _args*)arg; 
    ((void (*)(void *))args->func)(args->arg);
    CloseHandle(args->handle);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, arg);
    _endthreadex(0);
    return 0; 
}
#endif

__private_extern__ void *__CFStartSimpleThread(void *func, void *arg) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
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
#elif DEPLOYMENT_TARGET_WINDOWS
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


// Looks for localized version of "nonLocalized" in the SystemVersion bundle
// If not found, and returnNonLocalizedFlag == true, will return the non localized string (retained of course), otherwise NULL
// If bundlePtr != NULL, will use *bundlePtr and will return the bundle in there; otherwise bundle is created and released
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
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
#endif

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
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
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

CFStringRef CFCopySystemVersionString(void) {
    CFStringRef versionString;
    CFDictionaryRef dict = _CFCopyServerVersionDictionary();
    if (!dict) dict = _CFCopySystemVersionDictionary();
    if (!dict) return NULL;
    versionString = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("FullVersionString"));
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
    return (CFDictionaryRef)plist;
}

CFDictionaryRef _CFCopyServerVersionDictionary(void) {
    CFPropertyListRef plist = NULL;
	plist = _CFCopyVersionDictionary(CFSTR("/System/Library/CoreServices/ServerVersion.plist"));
    return (CFDictionaryRef)plist;
}

CONST_STRING_DECL(_kCFSystemVersionProductNameKey, "ProductName")
CONST_STRING_DECL(_kCFSystemVersionProductCopyrightKey, "ProductCopyright")
CONST_STRING_DECL(_kCFSystemVersionProductVersionKey, "ProductVersion")
CONST_STRING_DECL(_kCFSystemVersionProductVersionExtraKey, "ProductVersionExtra")
CONST_STRING_DECL(_kCFSystemVersionProductUserVisibleVersionKey, "ProductUserVisibleVersion")
CONST_STRING_DECL(_kCFSystemVersionBuildVersionKey, "ProductBuildVersion")
CONST_STRING_DECL(_kCFSystemVersionProductVersionStringKey, "Version")
CONST_STRING_DECL(_kCFSystemVersionBuildStringKey, "Build")

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || TARGET_IPHONE_SIMULATOR

typedef struct {
    uint16_t    primaryVersion;
    uint8_t     secondaryVersion;
    uint8_t     tertiaryVersion;
} CFLibraryVersion;

CFLibraryVersion CFGetExecutableLinkedLibraryVersion(CFStringRef libraryName) {
    CFLibraryVersion ret = {0xFFFF, 0xFF, 0xFF};
    char library[CFMaxPathSize];	// search specs larger than this are pointless
    if (!CFStringGetCString(libraryName, library, sizeof(library), kCFStringEncodingUTF8)) return ret;
    int32_t version = NSVersionOfLinkTimeLibrary(library);
    if (-1 != version) {
	ret.primaryVersion = version >> 16;
	ret.secondaryVersion = (version >> 8) & 0xff;
	ret.tertiaryVersion = version & 0xff;
    }
    return ret;
}

CFLibraryVersion CFGetExecutingLibraryVersion(CFStringRef libraryName) {
    CFLibraryVersion ret = {0xFFFF, 0xFF, 0xFF};
    char library[CFMaxPathSize];	// search specs larger than this are pointless
    if (!CFStringGetCString(libraryName, library, sizeof(library), kCFStringEncodingUTF8)) return ret;
    int32_t version = NSVersionOfRunTimeLibrary(library);
    if (-1 != version) {
	ret.primaryVersion = version >> 16;
	ret.secondaryVersion = (version >> 8) & 0xff;
	ret.tertiaryVersion = version & 0xff;
    }
    return ret;
}

static inline Boolean _CFLibraryVersionLessThan(CFLibraryVersion vers1, CFLibraryVersion vers2) {
    if (vers1.primaryVersion < vers2.primaryVersion) {
	return true;
    } else if (vers1.primaryVersion == vers2.primaryVersion) {
	if (vers1.secondaryVersion < vers2.secondaryVersion) {
	    return true;
	} else if (vers1.secondaryVersion == vers2.secondaryVersion) {
	    return vers1.tertiaryVersion < vers2.tertiaryVersion;
	}
    }
    return false;
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

#define resultIndex(VERSION) (VERSION)

#define checkLibrary(LIBNAME, VERSIONFIELD) { \
    uint16_t vers = (NSVersionOfLinkTimeLibrary(LIBNAME) >> 16); \
    if ((vers != 0xFFFF) && (versionInfo[version].VERSIONFIELD != 0xFFFF) && \
        ((version == 0) || (versionInfo[version-1].VERSIONFIELD < versionInfo[version].VERSIONFIELD))) \
        return (results[resultIndex(version)] = ((vers < versionInfo[version].VERSIONFIELD) ? false : true)); \
}


CF_EXPORT Boolean _CFExecutableLinkedOnOrAfter(CFSystemVersion version) {
    // The numbers in the below tables should be the numbers for any version of the framework in the release.
    // When adding new entries to these tables for a new build train, it's simplest to use the versions of the
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
        {73, 10, 750, 505, 305, 128, 22, 18, 271},	/* CFSystemVersionTiger */
        {89, 12, 840, 575, 375, 136, 34, 32, 0xFFFF},			/* CFSystemVersionLeopard */
        {112, 13, 960, 680, 480, 0xFFFF, 0xFFFF, 33, 0xFFFF},	/* CFSystemVersionSnowLeopard */
    };
    
    
    // !!! When a new release is added to the array, don't forget to bump the size of this array!
    static char results[CFSystemVersionMax] = {-2, -2, -2, -2, -2, -2};	/* We cache the results per-release; there are only a few of these... */
    if (version >= CFSystemVersionMax) return false;	/* Actually, we don't know the answer, and something scary is going on */
    
    int versionIndex = resultIndex(version);
    if (results[versionIndex] != -2) return results[versionIndex];

#if DEPLOYMENT_TARGET_MACOSX
    if (_CFIsCFM()) {
        results[versionIndex] = (version <= CFSystemVersionJaguar) ? true : false;
        return results[versionIndex];
    }
#endif
    
    // Do a sanity check, since sometimes System framework is screwed up, which confuses everything. 
    // If the currently executing System framework has a version less than that of Leopard, warn.
    static Boolean called = false;
    if (!called) {	// We do a check here in case CFLog() recursively calls this function.
	called = true;
	int32_t vers = NSVersionOfRunTimeLibrary("System");
	if ((vers != -1) && (((unsigned int)vers) >> 16) < 89) {    // 89 is the version of libSystem for first version of Leopard
	    CFLog(__kCFLogAssertion, CFSTR("System.framework version (%x) is wrong, this will break CF and up"), vers);
	}
	if (results[versionIndex] != -2) return results[versionIndex];	// If there was a recursive call that figured this out, return
    }
    
#if DEPLOYMENT_TARGET_MACOSX
    if (version < CFSystemVersionMax) {
	// Compare the linked library versions of a Mac OS X app to framework versions found on Mac OS X.
	checkLibrary("System", libSystemVersion);	// Pretty much everyone links with this
	checkLibrary("Cocoa", cocoaVersion);
	checkLibrary("AppKit", appkitVersion);
	checkLibrary("Foundation", fouVersion);
	checkLibrary("CoreFoundation", cfVersion);
	checkLibrary("Carbon", carbonVersion);
	checkLibrary("ApplicationServices", applicationServicesVersion);
	checkLibrary("CoreServices", coreServicesVersion);
	checkLibrary("IOKit", iokitVersion);
    } else {
    }
#else 
#endif
    
    /* If not found, then simply return NO to indicate earlier --- compatibility by default, unfortunately */
    return false;
}
#else
CF_EXPORT Boolean _CFExecutableLinkedOnOrAfter(CFSystemVersion version) {
    return true;
}
#endif


#if DEPLOYMENT_TARGET_MACOSX
__private_extern__ void *__CFLookupCarbonCoreFunction(const char *name) {
    static void *image = NULL;
    if (NULL == image) {
	image = dlopen("/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/CarbonCore.framework/Versions/A/CarbonCore", RTLD_LAZY | RTLD_LOCAL);
    }
    void *dyfunc = NULL;
    if (image) {
	dyfunc = dlsym(image, name);
    }
    return dyfunc;
}
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
__private_extern__ void *__CFLookupCFNetworkFunction(const char *name) {
    static void *image = NULL;
    if (NULL == image) {
	const char *path = NULL;
	if (!issetugid()) {
	    path = __CFgetenv("CFNETWORK_LIBRARY_PATH");
	}
	if (!path) {
#if DEPLOYMENT_TARGET_MACOSX
	    path = "/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/CFNetwork.framework/Versions/A/CFNetwork";
#else
	    path = "/System/Library/Frameworks/CFNetwork.framework/CFNetwork";
#endif
	}
	image = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    }
    void *dyfunc = NULL;
    if (image) {
	dyfunc = dlsym(image, name);
    }
    return dyfunc;
}
#endif


#ifndef __CFGetSessionID_defined

__private_extern__ uint32_t __CFGetSessionID(void) {
    return 0;
}

#endif

__private_extern__ CFIndex __CFActiveProcessorCount() {
    int32_t pcnt;
#if DEPLOYMENT_TARGET_WINDOWS
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD_PTR activeProcessorMask = sysInfo.dwActiveProcessorMask;
    // assumes sizeof(DWORD_PTR) is 64 bits or less
    uint64_t v = activeProcessorMask;
    v = v - ((v >> 1) & 0x5555555555555555ULL);
    v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
    v = (v + (v >> 4)) & 0xf0f0f0f0f0f0f0fULL;
    pcnt = (v * 0x0101010101010101ULL) >> ((sizeof(v) - 1) * 8);
#else
    int32_t mib[] = {CTL_HW, HW_AVAILCPU};
    size_t len = sizeof(pcnt);
    int32_t result = sysctl(mib, sizeof(mib) / sizeof(int32_t), &pcnt, &len, NULL, 0);
    if (result != 0) {
        pcnt = 0;
    }
#endif
    return pcnt;
}

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
#if DEPLOYMENT_TARGET_WINDOWS
    wchar_t *accumulatedBuffer = (wchar_t *)malloc((cnt+1) * sizeof(wchar_t));
#endif
     for (idx = 0; idx < cnt; idx++) {
         UniChar ch = __CFStringGetCharacterFromInlineBufferQuick(&buffer, idx);
#if DEPLOYMENT_TARGET_WINDOWS
         if (file == stderr || file == stdout) {
             accumulatedBuffer[idx] = ch;
	         lastNL = (ch == L'\n');
             if (idx == (cnt - 1)) {
                accumulatedBuffer[idx+1] = L'\0'; 
                OutputDebugStringW(accumulatedBuffer);
                free(accumulatedBuffer);
             }
         } else {
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
		 if (ch < 128) {
             fprintf_l(file, NULL, "%c", ch);
	     lastNL = (ch == '\n');
         } else {
             fprintf_l(file, NULL, "\\u%04x", ch);
         }
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
		 if (ch < 128) {
             _fprintf_l(file, "%c", NULL, ch);
	     lastNL = (ch == '\n');
         } else {
             _fprintf_l(file, "\\u%04x", NULL, ch);
         }
#endif
     }
#if  DEPLOYMENT_TARGET_WINDOWS
     }
#endif
     if (!lastNL) {
#if DEPLOYMENT_TARGET_WINDOWS
         if (file == stderr || file == stdout) {
             char outStr[2];
             outStr[0] = '\n';
             outStr[1] = '\0';
             OutputDebugStringA(outStr);
         } else {
		 _fprintf_l(file, "\n", NULL);
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
         fprintf_l(file, NULL, "\n");
#endif
#if DEPLOYMENT_TARGET_WINDOWS
         }
#endif
         if (flush) fflush(file);
     }

     if (str) CFRelease(str);
}

void CFShow(const void *obj) {
     _CFShowToFile(stderr, true, obj);
}


// message must be a UTF8-encoded, null-terminated, byte buffer with at least length bytes
typedef void (*CFLogFunc)(int32_t lev, const char *message, size_t length, char withBanner);

static Boolean also_do_stderr() {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if (!issetugid() && __CFgetenv("CFLOG_FORCE_STDERR")) {
	return true;
    }
    struct stat sb;
    int ret = fstat(STDERR_FILENO, &sb);
    if (ret < 0) return false;
    mode_t m = sb.st_mode & S_IFMT;
    if (S_IFREG == m || S_IFSOCK == m) return true;
    if (!(S_IFIFO == m || S_IFCHR == m)) return false; // disallow any whacky stuff
    // if it could be a pipe back to launchd, fail
    int64_t val = 0;
    // assumes val is not written to on error
    vproc_swap_integer(NULL, VPROC_GSK_IS_MANAGED, NULL, &val);
    if (val) return false;
#endif
    return true;
}

static void __CFLogCString(int32_t lev, const char *message, size_t length, char withBanner) {
    char *banner = NULL;
    char *time = NULL;
    char *thread = NULL;
    char *uid = NULL;
#if !(DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED)
    int bannerLen = 0;
#endif
    if (withBanner) {
	CFAbsoluteTime at = CFAbsoluteTimeGetCurrent();
	CFCalendarRef calendar = CFCalendarCreateWithIdentifier(kCFAllocatorSystemDefault, kCFCalendarIdentifierGregorian);
	if (!calendar) goto after_banner;
	CFTimeZoneRef tz = CFTimeZoneCopySystem();
	if (!tz) {
	    CFRelease(calendar);
	    goto after_banner;
	}
	CFCalendarSetTimeZone(calendar, tz);
	CFRelease(tz);
	int32_t year, month, day, hour, minute, second;
	Boolean dec = CFCalendarDecomposeAbsoluteTime(calendar, at, "yMdHms", &year, &month, &day, &hour, &minute, &second);
	CFRelease(calendar);
	if (!dec) goto after_banner;
	double atf;
	int32_t ms = (int32_t)floor(1000.0 * modf(at, &atf));
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        asprintf(&banner, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s[%d:%x] ", year, month, day, hour, minute, second, ms, *_CFGetProgname(), getpid(), pthread_mach_thread_np(pthread_self()));
	asprintf(&thread, "%x", pthread_mach_thread_np(pthread_self()));
#else
	bannerLen = asprintf(&banner, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s[%d:%x] ", year, month, day, hour, minute, second, ms, *_CFGetProgname(), getpid(), GetCurrentThreadId());
	asprintf(&thread, "%x", GetCurrentThreadId());
#endif
	asprintf(&time, "%04d-%02d-%02d %02d:%02d:%02d.%03d", year, month, day, hour, minute, second, ms);

    }
    after_banner:;
    asprintf(&uid, "%d", geteuid());
    aslclient asl = asl_open(NULL, "com.apple.console", ASL_OPT_NO_DELAY);
    aslmsg msg = asl_new(ASL_TYPE_MSG);
    asl_set(msg, "CFLog Local Time", time); // not to be documented, not public API
    asl_set(msg, "CFLog Thread", thread);   // not to be documented, not public API
    asl_set(msg, "ReadUID", uid);
    static const char *levstr[] = {"0", "1", "2", "3", "4", "5", "6", "7"};
    asl_set(msg, ASL_KEY_LEVEL, levstr[lev]);
    asl_set(msg, ASL_KEY_MSG, message);
    asl_send(asl, msg);
    asl_free(msg);
    asl_close(asl);

    if (also_do_stderr()) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
	struct iovec v[3];
	v[0].iov_base = banner;
	v[0].iov_len = banner ? strlen(banner) : 0;
	v[1].iov_base = (char *)message;
	v[1].iov_len = length;
	v[2].iov_base = "\n";
	v[2].iov_len = (message[length - 1] != '\n') ? 1 : 0;
	int nv = (v[0].iov_base ? 1 : 0) + 1 + (v[2].iov_len ? 1 : 0);
	static CFSpinLock_t lock = CFSpinLockInit;
	__CFSpinLock(&lock);
	writev(STDERR_FILENO, v[0].iov_base ? v : v + 1, nv);
	__CFSpinUnlock(&lock);
#else
        size_t bufLen = bannerLen + length + 1;
        char *buf = (char *)malloc(sizeof(char) * bufLen);
        if (banner) {
            // Copy the banner into the debug string
            memmove_s(buf, bufLen, banner, bannerLen);
            
            // Copy the message into the debug string
            strcpy_s(buf + bannerLen, bufLen - bannerLen, message);
        } else {
            strcpy_s(buf, bufLen, message);
        }
        buf[bufLen - 1] = '\0';
	fprintf_s(stderr, "%s\n", buf);
	// This Win32 API call only prints when a debugger is active
	// OutputDebugStringA(buf);
        free(buf);
#endif
    }
    
    if (thread) free(thread);
    if (time) free(time);
    if (banner) free(banner);
    if (uid) free(uid);
}

CF_EXPORT void _CFLogvEx(CFLogFunc logit, CFStringRef (*copyDescFunc)(void *, const void *), CFDictionaryRef formatOptions, int32_t lev, CFStringRef format, va_list args) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if (pthread_getspecific(__CFTSDKeyIsInCFLog)) return;
    pthread_setspecific(__CFTSDKeyIsInCFLog, (void *)1);
#endif
    CFStringRef str = format ? _CFStringCreateWithFormatAndArgumentsAux(kCFAllocatorSystemDefault, copyDescFunc, formatOptions, (CFStringRef)format, args) : 0;
    CFIndex blen = str ? CFStringGetMaximumSizeForEncoding(CFStringGetLength(str), kCFStringEncodingUTF8) + 1 : 0;
    char *buf = str ? (char *)malloc(blen) : 0;
    if (str && buf) {
	Boolean converted = CFStringGetCString(str, buf, blen, kCFStringEncodingUTF8);
	size_t len = strlen(buf);
	// silently ignore 0-length or really large messages, and levels outside the valid range
	if (converted && !(len <= 0 || (1 << 24) < len) && !(lev < ASL_LEVEL_EMERG || ASL_LEVEL_DEBUG < lev)) {
	    (logit ? logit : __CFLogCString)(lev, buf, len, 1);
	}
    }
    if (buf) free(buf);
    if (str) CFRelease(str);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    pthread_setspecific(__CFTSDKeyIsInCFLog, 0);
#endif
}

void CFLog(int32_t lev, CFStringRef format, ...) {
    va_list args;
    va_start(args, format); 
    _CFLogvEx(NULL, NULL, NULL, lev, format, args);
    va_end(args);
}



#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED

kern_return_t _CFDiscorporateMemoryAllocate(CFDiscorporateMemory *hm, size_t size, bool purgeable) {
    kern_return_t ret = KERN_SUCCESS;
    size = round_page(size);
    if (0 == size) size = vm_page_size;
    memset(hm, 0, sizeof(CFDiscorporateMemory));
    void *addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, VM_MAKE_TAG(0) | (purgeable ? VM_FLAGS_PURGABLE : 0), 0);
    if ((uintptr_t)addr == -1) {
        ret = KERN_NO_SPACE;
    }
    if (KERN_SUCCESS == ret) {
        hm->address = (mach_vm_address_t)(uintptr_t)addr;
        hm->size = (mach_vm_size_t)size;
        hm->port = MACH_PORT_NULL;
        hm->corporeal = true;
        hm->purgeable = purgeable;
    }
    if (KERN_SUCCESS == ret) ret = mach_make_memory_entry_64(mach_task_self(), &hm->size, hm->address, VM_PROT_DEFAULT, &hm->port, MACH_PORT_NULL);
    if (KERN_SUCCESS == ret) hm->corporeal = true;
    return ret;
}

kern_return_t _CFDiscorporateMemoryDeallocate(CFDiscorporateMemory *hm) {
    kern_return_t ret1 = KERN_SUCCESS, ret2 = KERN_SUCCESS;
    if (hm->corporeal) ret1 = mach_vm_deallocate(mach_task_self(), hm->address, hm->size);
    hm->address = MACH_VM_MIN_ADDRESS;
    hm->corporeal = false;
    ret2 = mach_port_deallocate(mach_task_self(), hm->port);
    hm->port = MACH_PORT_NULL;
    return ret1 != KERN_SUCCESS ? ret1 : ret2;
}

kern_return_t _CFDiscorporateMemoryDematerialize(CFDiscorporateMemory *hm) {
    kern_return_t ret = KERN_SUCCESS;
    if (!hm->corporeal) ret = KERN_INVALID_MEMORY_CONTROL;
    int state = VM_PURGABLE_VOLATILE;
    if (KERN_SUCCESS == ret) vm_purgable_control(mach_task_self(), (vm_address_t)hm->address, VM_PURGABLE_SET_STATE, &state);
    if (KERN_SUCCESS == ret) ret = mach_vm_deallocate(mach_task_self(), hm->address, hm->size);
    if (KERN_SUCCESS == ret) hm->address = MACH_VM_MIN_ADDRESS;
    if (KERN_SUCCESS == ret) hm->corporeal = false;
    return ret;
}

kern_return_t _CFDiscorporateMemoryMaterialize(CFDiscorporateMemory *hm) {
    kern_return_t ret = KERN_SUCCESS;
    if (hm->corporeal) ret = KERN_INVALID_MEMORY_CONTROL;
    if (KERN_SUCCESS == ret) ret = mach_vm_map(mach_task_self(), &hm->address, hm->size, 0, VM_FLAGS_ANYWHERE, hm->port, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_DEFAULT);
    if (KERN_SUCCESS == ret) hm->corporeal = true;
    int state = VM_PURGABLE_NONVOLATILE;
    if (KERN_SUCCESS == ret) ret = vm_purgable_control(mach_task_self(), (vm_address_t)hm->address, VM_PURGABLE_SET_STATE, &state);
    if (KERN_SUCCESS == ret) if (VM_PURGABLE_EMPTY == state) ret = KERN_PROTECTION_FAILURE; // same as VM_PURGABLE_EMPTY
    return ret;
}

#endif

#if DEPLOYMENT_TARGET_MACOSX

#define SUDDEN_TERMINATION_ENABLE_VPROC 1

#if SUDDEN_TERMINATION_ENABLE_VPROC

static CFSpinLock_t __CFProcessKillingLock = CFSpinLockInit;
static CFIndex __CFProcessKillingDisablingCount = 1;
static Boolean __CFProcessKillingWasTurnedOn = false;

void _CFSuddenTerminationDisable(void) {
    __CFSpinLock(&__CFProcessKillingLock);
    __CFProcessKillingDisablingCount++;
    _vproc_transaction_begin();
    __CFSpinUnlock(&__CFProcessKillingLock);
}

void _CFSuddenTerminationEnable(void) {
    // In our model the first call of _CFSuddenTerminationEnable() that does not balance a previous call of _CFSuddenTerminationDisable() actually enables sudden termination so we have to keep a count that's almost redundant with vproc's.
    __CFSpinLock(&__CFProcessKillingLock);
    __CFProcessKillingDisablingCount--;
    if (__CFProcessKillingDisablingCount==0 && !__CFProcessKillingWasTurnedOn) {
	int64_t transactionsAreToBeEnabled = 1;
	int64_t transactionsWereAlreadyEnabled = 0;
	vproc_err_t verr = vproc_swap_integer(NULL, VPROC_GSK_TRANSACTIONS_ENABLED, &transactionsAreToBeEnabled, &transactionsWereAlreadyEnabled);
	if (!verr) {
	    if (!transactionsWereAlreadyEnabled) {
		// We set __CFProcessKillingWasTurnedOn below regardless of success because there's no point in retrying.
	    } // else this process was launched by launchd with transactions already enabled because EnableTransactions was set to true in the launchd .plist file.
	} // else this process was not launched by launchd and the fix for 6416724 is not in the build yet.
	__CFProcessKillingWasTurnedOn = true;
    } else {
	// Mail seems to have sudden termination disabling/enabling imbalance bugs that make _vproc_transaction_end() kill the app but we don't want that to prevent our submission of the fix 6382488.
	if (__CFProcessKillingDisablingCount>=0) {
	    _vproc_transaction_end();
	} else {
	    CFLog(kCFLogLevelError, CFSTR("-[NSProcessInfo enableSuddenTermination] has been invoked more times than necessary to balance invocations of -[NSProcessInfo disableSuddenTermination]. Ignoring."));
	}
    }
    __CFSpinUnlock(&__CFProcessKillingLock);
}

void _CFSuddenTerminationExitIfTerminationEnabled(int exitStatus) {
    // This is for when the caller wants to try to exit quickly if possible but not automatically exit the process when it next becomes clean, because quitting might still be cancelled by the user.
    __CFSpinLock(&__CFProcessKillingLock);
    // Check _vproc_transaction_count() because other code in the process might go straight to the vproc APIs but also check __CFProcessKillingWasTurnedOn because  _vproc_transaction_count() can return 0 when transactions didn't even get enabled.
    if (_vproc_transaction_count()==0 && __CFProcessKillingWasTurnedOn) {
        _exit(exitStatus);
    }
    __CFSpinUnlock(&__CFProcessKillingLock);
}

void _CFSuddenTerminationExitWhenTerminationEnabled(int exitStatus) {
    // The user has had their final opportunity to cancel quitting. Exit as soon as the process is clean. Same carefulness as in _CFSuddenTerminationExitIfTerminationEnabled().
    __CFSpinLock(&__CFProcessKillingLock);
    if (__CFProcessKillingWasTurnedOn) {
	_vproc_transaction_try_exit(exitStatus);
    }
    __CFSpinUnlock(&__CFProcessKillingLock);
}

size_t _CFSuddenTerminationDisablingCount(void) {
    // Until sudden termination has been really enabled vproc's notion of the count is off by one but we can't just return __CFProcessKillingDisablingCount() because that doesn't take into account stuff that calls the vproc_transaction functions behind our back.
    return _vproc_transaction_count() + (__CFProcessKillingWasTurnedOn ? 0 : 1);
}

#else

#warning Building with vproc sudden termination API disabled.

static CFSpinLock_t __CFProcessKillingLock = CFSpinLockInit;
static size_t __CFProcessKillingDisablingCount = 1;
static Boolean __CFProcessExitNextTimeKillingIsEnabled = false;
static int32_t __CFProcessExitStatus = 0;
static int __CFProcessIsKillableNotifyToken;
static Boolean __CFProcessIsKillableNotifyTokenIsFigured = false;

__private_extern__ void _CFSetSuddenTerminationEnabled(Boolean isEnabled) {
    if (!__CFProcessIsKillableNotifyTokenIsFigured) {
        char *notificationName = NULL;
        asprintf(&notificationName, "com.apple.isKillable.%i", getpid());
        uint32_t notifyResult = notify_register_check(notificationName, &__CFProcessIsKillableNotifyToken);
        if (notifyResult != NOTIFY_STATUS_OK) {
            CFLog(kCFLogLevelError, CFSTR("%s: notify_register_check() returned %i."), __PRETTY_FUNCTION__, notifyResult);
        }
        free(notificationName);
        __CFProcessIsKillableNotifyTokenIsFigured = true;
    }
    uint32_t notifyResult = notify_set_state(__CFProcessIsKillableNotifyToken, isEnabled);
    if (notifyResult != NOTIFY_STATUS_OK) {
        CFLog(kCFLogLevelError, CFSTR("%s: notify_set_state() returned %i"), __PRETTY_FUNCTION__, notifyResult);
    }
}

void _CFSuddenTerminationDisable(void) {
    __CFSpinLock(&__CFProcessKillingLock);
    if (__CFProcessKillingDisablingCount == 0) {
        _CFSetSuddenTerminationEnabled(false);
    }
    __CFProcessKillingDisablingCount++;
    __CFSpinUnlock(&__CFProcessKillingLock);
}

void _CFSuddenTerminationEnable(void) {
    __CFSpinLock(&__CFProcessKillingLock);
    __CFProcessKillingDisablingCount--;
    if (__CFProcessKillingDisablingCount == 0) {
        if (__CFProcessExitNextTimeKillingIsEnabled) {
            _exit(__CFProcessExitStatus);
        } else {
            _CFSetSuddenTerminationEnabled(true);
        }
    }
    __CFSpinUnlock(&__CFProcessKillingLock);
}

void _CFSuddenTerminationExitIfTerminationEnabled(int exitStatus) {
    __CFSpinLock(&__CFProcessKillingLock);
    if (__CFProcessKillingDisablingCount == 0) {
        _exit(exitStatus);
    }
    __CFSpinUnlock(&__CFProcessKillingLock);
}

void _CFSuddenTerminationExitWhenTerminationEnabled(int exitStatus) {
    __CFSpinLock(&__CFProcessKillingLock);
    if (__CFProcessKillingDisablingCount == 0) {
        _exit(exitStatus);
    } else {
        __CFProcessExitNextTimeKillingIsEnabled = YES;
        __CFProcessExitStatus = exitStatus;
    }
    __CFSpinUnlock(&__CFProcessKillingLock);
}

size_t _CFSuddenTerminationDisablingCount(void) {
    return __CFProcessKillingDisablingCount;
}

#endif

#endif

#if 0
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED

typedef void (^ThrottleTypeA)(void);		// allows calls per nanoseconds
typedef void (^ThrottleTypeB)(uint64_t amt);	// allows amount per nanoseconds

__private_extern__ ThrottleTypeA __CFCreateThrottleTypeA(uint16_t calls, uint64_t nanoseconds) {
   struct mach_timebase_info info;
   mach_timebase_info(&info);
   uint64_t period = nanoseconds / info.numer * info.denom;

   if (0 == calls || 0 == period) return NULL;

   __block OSSpinLock b_lock = OS_SPINLOCK_INIT;
   __block uint64_t b_values[calls];
   __block uint64_t *b_oldest = b_values;
   memset(b_values, 0, sizeof(b_values));

   return Block_copy(^{
               uint64_t curr_time = mach_absolute_time();
               OSSpinLockLock(&b_lock);
               uint64_t next_time = *b_oldest + period;
               *b_oldest = (curr_time < next_time) ? next_time : curr_time;
               b_oldest++;
               if (b_values + calls <= b_oldest) b_oldest = b_values;
               OSSpinLockUnlock(&b_lock);
               if (curr_time < next_time) {
                   mach_wait_until(next_time);
               }
           });
}

__private_extern__ ThrottleTypeB __CFCreateThrottleTypeB(uint64_t amount, uint64_t nanoseconds) {
   struct mach_timebase_info info;
   mach_timebase_info(&info);
   uint64_t period = nanoseconds / info.numer * info.denom;

   if (0 == amount || 0 == period) return NULL;

   __block OSSpinLock b_lock = OS_SPINLOCK_INIT;
   __block uint64_t b_sum = 0ULL;
   __block uint16_t b_num_values = 8;
   __block uint64_t *b_values = calloc(b_num_values, 2 * sizeof(uint64_t));
   __block uint64_t *b_oldest = b_values;

   return Block_copy(^(uint64_t amt){
               OSSpinLockLock(&b_lock);
// unimplemented
               OSSpinLockUnlock(&b_lock);
           });
}

#endif
#endif

