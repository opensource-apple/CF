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
/*	CFPreferences.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Chris Parker
*/

#include <CoreFoundation/CFPreferences.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFUserNotification.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFPriv.h>
#include <CoreFoundation/CFUtilitiesPriv.h>
#include "CFInternal.h"
#include <sys/stat.h>

#if defined(__WIN32__)
#include <windows.h>
#endif
#if DEBUG_PREFERENCES_MEMORY
#include "../Tests/CFCountingAllocator.c"
#endif

struct __CFPreferencesDomain {
    CFRuntimeBase _base;
    /* WARNING - not copying the callbacks; we know they are always static structs */
    const _CFPreferencesDomainCallBacks *_callBacks;
    CFTypeRef _context;
    void *_domain;
};

CONST_STRING_DECL(kCFPreferencesAnyApplication, "kCFPreferencesAnyApplication")
CONST_STRING_DECL(kCFPreferencesAnyHost, "kCFPreferencesAnyHost")
CONST_STRING_DECL(kCFPreferencesAnyUser, "kCFPreferencesAnyUser")
CONST_STRING_DECL(kCFPreferencesCurrentApplication, "kCFPreferencesCurrentApplication")
CONST_STRING_DECL(kCFPreferencesCurrentHost, "kCFPreferencesCurrentHost")
CONST_STRING_DECL(kCFPreferencesCurrentUser, "kCFPreferencesCurrentUser")


static CFAllocatorRef _preferencesAllocator = NULL;
__private_extern__ CFAllocatorRef __CFPreferencesAllocator(void) {
    if (!_preferencesAllocator) {
#if DEBUG_PREFERENCES_MEMORY
        _preferencesAllocator = CFCountingAllocatorCreate(NULL);
#else
        _preferencesAllocator = __CFGetDefaultAllocator();
        CFRetain(_preferencesAllocator);
#endif
    }
    return _preferencesAllocator;
}

// declaration for telling the 
void _CFApplicationPreferencesDomainHasChanged(CFPreferencesDomainRef);

#if DEBUG_PREFERENCES_MEMORY
#warning Preferences debugging on
CF_EXPORT void CFPreferencesDumpMem(void) {
    if (_preferencesAllocator) {
//        CFCountingAllocatorPrintSummary(_preferencesAllocator);
        CFCountingAllocatorPrintPointers(_preferencesAllocator);
    }
//    CFCountingAllocatorReset(_preferencesAllocator);
}
#endif

static CFURLRef _CFPreferencesURLForStandardDomainWithSafetyLevel(CFStringRef domainName, CFStringRef userName, CFStringRef hostName, unsigned long safeLevel);

static unsigned long __CFSafeLaunchLevel = 0;

static CFURLRef _preferencesDirectoryForUserHostSafetyLevel(CFStringRef userName, CFStringRef hostName, unsigned long safeLevel) {
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    CFURLRef  home = NULL;
    CFURLRef  url;
    int levels = 0;
    //    if (hostName != kCFPreferencesCurrentHost && hostName != kCFPreferencesAnyHost) return NULL; // Arbitrary host access not permitted
    if (userName == kCFPreferencesAnyUser) {
        if (!home) home = CFURLCreateWithFileSystemPath(alloc, CFSTR("/Library/Preferences/"), kCFURLPOSIXPathStyle, true);
        levels = 1;
        if (hostName == kCFPreferencesCurrentHost) url = home;
        else {
            url = CFURLCreateWithFileSystemPathRelativeToBase(alloc, CFSTR("Network/"), kCFURLPOSIXPathStyle, true, home);
            levels ++;
            CFRelease(home);
        }
    } else {
        home = CFCopyHomeDirectoryURLForUser((userName == kCFPreferencesCurrentUser) ? NULL : userName);
        if (home) {
            url = (safeLevel > 0) ? CFURLCreateWithFileSystemPathRelativeToBase(alloc, CFSTR("Library/Safe Preferences/"), kCFURLPOSIXPathStyle, true, home) :
            CFURLCreateWithFileSystemPathRelativeToBase(alloc, CFSTR("Library/Preferences/"), kCFURLPOSIXPathStyle, true, home);
            levels = 2;
            CFRelease(home);
            if (hostName != kCFPreferencesAnyHost) {
                home = url;
                url = CFURLCreateWithFileSystemPathRelativeToBase(alloc, CFSTR("ByHost/"), kCFURLPOSIXPathStyle, true, home);
                levels ++;
                CFRelease(home);
            }
        } else {
            url = NULL;
        }
    }
    return url;
}

static CFURLRef  _preferencesDirectoryForUserHost(CFStringRef  userName, CFStringRef  hostName) {
    return _preferencesDirectoryForUserHostSafetyLevel(userName, hostName, __CFSafeLaunchLevel);
}

// Bindings internals
__private_extern__ CFSpinLock_t userDefaultsLock = 0;
__private_extern__ void *userDefaults = NULL;

void _CFPreferencesSetStandardUserDefaults(void *sudPtr) {
    __CFSpinLock(&userDefaultsLock);
    userDefaults = sudPtr;
    __CFSpinUnlock(&userDefaultsLock);
}


#define CF_OBJC_KVO_WILLCHANGE(obj, sel)
#define CF_OBJC_KVO_DIDCHANGE(obj, sel)


static Boolean __CFPreferencesWritesXML = false;

Boolean __CFPreferencesShouldWriteXML(void) {
    return __CFPreferencesWritesXML;
}

void __CFPreferencesCheckFormatType(void) {
    static int checked = 0;
    if (!checked) {
        checked = 1;
        __CFPreferencesWritesXML = CFPreferencesGetAppBooleanValue(CFSTR("CFPreferencesWritesXML"), kCFPreferencesCurrentApplication, NULL);
    }
}

static CFSpinLock_t domainCacheLock = 0;
static CFMutableDictionaryRef  domainCache = NULL; // mutable

// Public API

CFTypeRef  CFPreferencesCopyValue(CFStringRef  key, CFStringRef  appName, CFStringRef  user, CFStringRef  host) {
    CFPreferencesDomainRef domain;
    CFAssert1(appName != NULL && user != NULL && host != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL application name, user, or host", __PRETTY_FUNCTION__);
    CFAssert1(key != NULL, __kCFLogAssertion, "%s(): Cannot access preferences with a NULL key", __PRETTY_FUNCTION__);
    
    domain = _CFPreferencesStandardDomain(appName, user, host);
    if (domain) {
        return _CFPreferencesDomainCreateValueForKey(domain, key);
    } else {
        return NULL;
    }
}

CFDictionaryRef CFPreferencesCopyMultiple(CFArrayRef keysToFetch, CFStringRef appName, CFStringRef userName, CFStringRef hostName) {
    CFPreferencesDomainRef domain;
    CFMutableDictionaryRef result;
    CFIndex idx, count;

    CFAssert1(appName != NULL && userName != NULL && hostName != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL application name, user, or host", __PRETTY_FUNCTION__);
    __CFGenericValidateType(appName, CFStringGetTypeID());
    __CFGenericValidateType(userName, CFStringGetTypeID());
    __CFGenericValidateType(hostName, CFStringGetTypeID());

    domain = _CFPreferencesStandardDomain(appName, userName, hostName);
    if (!domain) return NULL;
    if (!keysToFetch) {
        return _CFPreferencesDomainDeepCopyDictionary(domain);
    } else {
        __CFGenericValidateType(keysToFetch, CFArrayGetTypeID());
        count = CFArrayGetCount(keysToFetch);
        result = CFDictionaryCreateMutable(CFGetAllocator(domain), count, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!result) return NULL;
        for (idx = 0; idx < count; idx ++) {
            CFStringRef key = CFArrayGetValueAtIndex(keysToFetch, idx);
            CFPropertyListRef value;
            __CFGenericValidateType(key, CFStringGetTypeID());
            value = _CFPreferencesDomainCreateValueForKey(domain, key);
            if (value) {
                CFDictionarySetValue(result, key, value);
                CFRelease(value);
            }
        }
    }
    return result;
}

void CFPreferencesSetValue(CFStringRef  key, CFTypeRef  value, CFStringRef  appName, CFStringRef  user, CFStringRef  host) {
    CFPreferencesDomainRef domain;
    CFAssert1(appName != NULL && user != NULL && host != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL application name, user, or host", __PRETTY_FUNCTION__);
    CFAssert1(key != NULL, __kCFLogAssertion, "%s(): Cannot access preferences with a NULL key", __PRETTY_FUNCTION__);

    domain = _CFPreferencesStandardDomain(appName, user, host);
    if (domain) {
        void *defs = NULL;
        __CFSpinLock(&userDefaultsLock);
        defs = userDefaults;
        __CFSpinUnlock(&userDefaultsLock);
        CF_OBJC_KVO_WILLCHANGE(defs, key);
        _CFPreferencesDomainSet(domain, key, value);
        _CFApplicationPreferencesDomainHasChanged(domain);
        CF_OBJC_KVO_DIDCHANGE(defs, key);
    }
}


void CFPreferencesSetMultiple(CFDictionaryRef keysToSet, CFArrayRef keysToRemove, CFStringRef appName, CFStringRef userName, CFStringRef hostName) {
    CFPreferencesDomainRef domain;
    CFIndex idx, count;
    CFAssert1(appName != NULL && userName != NULL && hostName != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL application name, user, or host", __PRETTY_FUNCTION__);
    if (keysToSet) __CFGenericValidateType(keysToSet, CFDictionaryGetTypeID());
    if (keysToRemove) __CFGenericValidateType(keysToRemove, CFArrayGetTypeID());
    __CFGenericValidateType(appName, CFStringGetTypeID());
    __CFGenericValidateType(userName, CFStringGetTypeID());
    __CFGenericValidateType(hostName, CFStringGetTypeID());

    CFTypeRef *keys = NULL;
    CFTypeRef *values;
    CFIndex numOfKeysToSet = 0;
    
    domain = _CFPreferencesStandardDomain(appName, userName, hostName);
    if (!domain) return;

    CFAllocatorRef alloc = CFGetAllocator(domain);
    void *defs = NULL;
    
    __CFSpinLock(&userDefaultsLock);
    defs = userDefaults;
    __CFSpinUnlock(&userDefaultsLock);
    
    if (keysToSet && (count = CFDictionaryGetCount(keysToSet))) {
        numOfKeysToSet = count;
        keys = CFAllocatorAllocate(alloc, 2*count*sizeof(CFTypeRef), 0);
        if (keys) {
            values = &(keys[count]);
            CFDictionaryGetKeysAndValues(keysToSet, keys, values);
            for (idx = 0; idx < count; idx ++) {
                CF_OBJC_KVO_WILLCHANGE(defs, keys[idx]);
                _CFPreferencesDomainSet(domain, keys[idx], values[idx]);
            }
        }
    }
    if (keysToRemove && (count = CFArrayGetCount(keysToRemove))) {
        for (idx = 0; idx < count; idx ++) {
            CFStringRef removedKey = CFArrayGetValueAtIndex(keysToRemove, idx);
            CF_OBJC_KVO_WILLCHANGE(defs, removedKey);
            _CFPreferencesDomainSet(domain, removedKey, NULL);
        }
    }


    _CFApplicationPreferencesDomainHasChanged(domain);
    
    // here, we have to do things in reverse order.
    if(keysToRemove) {
        count = CFArrayGetCount(keysToRemove);
        for(idx = count - 1; idx >= 0; idx--) {
            CF_OBJC_KVO_DIDCHANGE(defs, CFArrayGetValueAtIndex(keysToRemove, idx));
        }
    }
    
    if(numOfKeysToSet > 0) {
        for(idx = numOfKeysToSet - 1; idx >= 0; idx--) {
            CF_OBJC_KVO_DIDCHANGE(defs, keys[idx]);
        }
    }
    
    if(keys) CFAllocatorDeallocate(alloc, keys);
}

Boolean CFPreferencesSynchronize(CFStringRef  appName, CFStringRef  user, CFStringRef  host) {
    CFPreferencesDomainRef domain;
    CFAssert1(appName != NULL && user != NULL && host != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL application name, user, or host", __PRETTY_FUNCTION__);

    __CFPreferencesCheckFormatType();
    
    domain = _CFPreferencesStandardDomain(appName, user, host);
    if(domain) _CFApplicationPreferencesDomainHasChanged(domain);
    
    return domain ? _CFPreferencesDomainSynchronize(domain) : false;
}

CFArrayRef  CFPreferencesCopyApplicationList(CFStringRef  userName, CFStringRef  hostName) {
    CFArrayRef  array;
    CFAssert1(userName != NULL && hostName != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL user or host", __PRETTY_FUNCTION__);
    array = _CFPreferencesCreateDomainList(userName, hostName);
    return array;
}

CFArrayRef  CFPreferencesCopyKeyList(CFStringRef  appName, CFStringRef  userName, CFStringRef  hostName) {
    CFPreferencesDomainRef domain;
    CFAssert1(appName != NULL && userName != NULL && hostName != NULL, __kCFLogAssertion, "%s(): Cannot access preferences for a NULL application name, user, or host", __PRETTY_FUNCTION__);

    domain = _CFPreferencesStandardDomain(appName, userName, hostName);
    if (!domain) {
        return NULL;
    } else {
        void **buf = NULL;
        CFAllocatorRef alloc = __CFPreferencesAllocator();
        CFArrayRef  result;
        CFIndex numPairs = 0;
        _CFPreferencesDomainGetKeysAndValues(alloc, domain, &buf, &numPairs);
        if (numPairs == 0) {
            result = NULL;
        } else {
            // It would be nice to avoid this allocation....
            result = CFArrayCreate(alloc, (const void **)buf, numPairs, &kCFTypeArrayCallBacks);
            CFAllocatorDeallocate(alloc, buf);
        }
        return result;
    }
}


/****************************/
/*  CFPreferencesDomain     */
/****************************/

static CFStringRef __CFPreferencesDomainCopyDescription(CFTypeRef cf) {
    return CFStringCreateWithFormat(__CFPreferencesAllocator(), NULL, CFSTR("<Private CFType 0x%x>\n"), (UInt32)cf);
}

static void __CFPreferencesDomainDeallocate(CFTypeRef cf) {
    const struct __CFPreferencesDomain *domain = cf;
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    domain->_callBacks->freeDomain(alloc, domain->_context, domain->_domain);
    if (domain->_context) CFRelease(domain->_context);
}

static CFTypeID __kCFPreferencesDomainTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFPreferencesDomainClass = {
    0,
    "CFPreferencesDomain",
    NULL,      // init
    NULL,      // copy
    __CFPreferencesDomainDeallocate,
    NULL,
    NULL,
    NULL,      // 
    __CFPreferencesDomainCopyDescription
};

/* This is called once at CFInitialize() time. */
__private_extern__ void __CFPreferencesDomainInitialize(void) {
    __kCFPreferencesDomainTypeID = _CFRuntimeRegisterClass(&__CFPreferencesDomainClass);
}

/* We spend a lot of time constructing these prefixes; we should cache.  REW, 7/19/99 */
__private_extern__ CFStringRef  _CFPreferencesCachePrefixForUserHost(CFStringRef  userName, CFStringRef  hostName) {
    Boolean freeHost = false;
    CFStringRef  result;
    if (userName == kCFPreferencesCurrentUser) {
        userName = CFGetUserName();
    } else if (userName == kCFPreferencesAnyUser) {
        userName = CFSTR("*");
    }

    if (hostName == kCFPreferencesCurrentHost) {
        hostName = __CFCopyEthernetAddrString();
        if (!hostName) hostName = _CFStringCreateHostName();
        freeHost = true;
    } else if (hostName == kCFPreferencesAnyHost) {
        hostName = CFSTR("*");
    }
    result = CFStringCreateWithFormat(__CFPreferencesAllocator(), NULL, CFSTR("%@/%@/"), userName, hostName);
    if (freeHost && hostName != NULL) CFRelease(hostName);
    return result;
}

// It would be nice if we could remember the key for "well-known" combinations, so we're not constantly allocing more strings....  - REW 2/3/99
static CFStringRef  _CFPreferencesStandardDomainCacheKey(CFStringRef  domainName, CFStringRef  userName, CFStringRef  hostName) {
    CFStringRef  prefix = _CFPreferencesCachePrefixForUserHost(userName, hostName);
    CFStringRef  result = NULL;
    
    if (prefix) {
        result = CFStringCreateWithFormat(__CFPreferencesAllocator(), NULL, CFSTR("%@%@"), prefix, domainName);
        CFRelease(prefix);
    }
    return result;
}

#if defined(__MACOS8__)
// Define a custom hash function so that we don't inadvertantly make the
// result of CFHash() on a string persistent, and locked-in for all time.
static UInt16 hashString(CFStringRef str) {
    UInt32 h = 0;
    CFIndex idx, cnt;
    cnt = CFStringGetLength(str);
    h = cnt;
    for (idx = 0; idx < cnt; idx++) {
	h <<= 2;
	h += CFStringGetCharacterAtIndex(str, idx);
    }
    return (h >> 16) ^ (h & 0xFFFF);
}
#endif

static CFURLRef _CFPreferencesURLForStandardDomainWithSafetyLevel(CFStringRef domainName, CFStringRef userName, CFStringRef hostName, unsigned long safeLevel) {
    CFURLRef theURL = NULL;
    CFAllocatorRef prefAlloc = __CFPreferencesAllocator();
#if defined(__MACH__)
    CFURLRef prefDir = _preferencesDirectoryForUserHostSafetyLevel(userName, hostName, safeLevel);
    CFStringRef  appName;
    CFStringRef  fileName;
    Boolean mustFreeAppName = false;
    
    if (!prefDir) return NULL;
    if (domainName == kCFPreferencesAnyApplication) {
        appName = CFSTR(".GlobalPreferences");
    } else if (domainName == kCFPreferencesCurrentApplication) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        appName = mainBundle ? CFBundleGetIdentifier(mainBundle) : NULL;
        if (!appName || CFStringGetLength(appName) == 0) {
            appName = _CFProcessNameString();
        }
    } else {
        appName = domainName;
    }
    if (userName != kCFPreferencesAnyUser) {
        if (hostName == kCFPreferencesAnyHost) {
            fileName = CFStringCreateWithFormat(prefAlloc, NULL, CFSTR("%@.plist"), appName);
        } else if (hostName == kCFPreferencesCurrentHost) {
            CFStringRef host = __CFCopyEthernetAddrString();
            if (!host) host = _CFStringCreateHostName();
            fileName = CFStringCreateWithFormat(prefAlloc, NULL, CFSTR("%@.%@.plist"), appName, host);
            CFRelease(host);
        } else {
            fileName = CFStringCreateWithFormat(prefAlloc, NULL, CFSTR("%@.%@.plist"), appName, hostName);
        }
    } else {
        fileName = CFStringCreateWithFormat(prefAlloc, NULL, CFSTR("%@.plist"), appName);
    }
    if (mustFreeAppName) {
	CFRelease(appName);
    }
    if (fileName) {
        theURL = CFURLCreateWithFileSystemPathRelativeToBase(prefAlloc, fileName, kCFURLPOSIXPathStyle, false, prefDir);
        if (prefDir) CFRelease(prefDir);
        CFRelease(fileName);
    }
#else
#error Do not know where to store NSUserDefaults on this platform
#endif
    return theURL;
}

static CFURLRef _CFPreferencesURLForStandardDomain(CFStringRef domainName, CFStringRef userName, CFStringRef hostName) {
    return _CFPreferencesURLForStandardDomainWithSafetyLevel(domainName, userName, hostName, __CFSafeLaunchLevel);
}

CFPreferencesDomainRef _CFPreferencesStandardDomain(CFStringRef  domainName, CFStringRef  userName, CFStringRef  hostName) {
    CFPreferencesDomainRef domain;
    CFStringRef  domainKey;
    Boolean shouldReleaseDomain = true;
     domainKey = _CFPreferencesStandardDomainCacheKey(domainName, userName, hostName);
    __CFSpinLock(&domainCacheLock);
    if (!domainCache) {
        CFAllocatorRef alloc = __CFPreferencesAllocator();
        domainCache = CFDictionaryCreateMutable(alloc, 0, & kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    domain = (CFPreferencesDomainRef)CFDictionaryGetValue(domainCache, domainKey);
    __CFSpinUnlock(&domainCacheLock);
    if (!domain) {
        // Domain's not in the cache; load from permanent storage
        CFURLRef  theURL = _CFPreferencesURLForStandardDomain(domainName, userName, hostName);
        if (theURL) {
            domain = _CFPreferencesDomainCreate(theURL, &__kCFXMLPropertyListDomainCallBacks);
            if (userName == kCFPreferencesAnyUser) {
                _CFPreferencesDomainSetIsWorldReadable(domain, true);
            }
            CFRelease(theURL);
        }
	__CFSpinLock(&domainCacheLock);
        if (domain && domainCache) {
            // We've just synthesized a domain & we're about to throw it in the domain cache. The problem is that someone else might have gotten in here behind our backs, so we can't just blindly set the domain (3021920). We'll need to check to see if this happened, and compensate.
            CFPreferencesDomainRef checkDomain = (CFPreferencesDomainRef)CFDictionaryGetValue(domainCache, domainKey);
            if(checkDomain) {
                // Someone got in here ahead of us, so we shouldn't smash the domain we're given. checkDomain is the current version, we should use that.
                // checkDomain was retrieved with a Get, so we don't want to over-release.
                shouldReleaseDomain = false;
                CFRelease(domain);	// release the domain we synthesized earlier.
                domain = checkDomain;	// repoint it at the domain picked up out of the cache.
            } else {
                // We must not have found the domain in the cache, so it's ok for us to put this in.
                CFDictionarySetValue(domainCache, domainKey, domain);                
            }
            if(shouldReleaseDomain) CFRelease(domain);
        }
	__CFSpinUnlock(&domainCacheLock);
    }
    CFRelease(domainKey);
    return domain;
}

static void __CFPreferencesPerformSynchronize(const void *key, const void *value, void *context) {
    CFPreferencesDomainRef domain = (CFPreferencesDomainRef)value;
    Boolean *cumulativeResult = (Boolean *)context;
    if (!_CFPreferencesDomainSynchronize(domain)) *cumulativeResult = false;
}

__private_extern__ Boolean _CFSynchronizeDomainCache(void) {
    Boolean result = true;
    __CFSpinLock(&domainCacheLock);
    if (domainCache) {
        CFDictionaryApplyFunction(domainCache, __CFPreferencesPerformSynchronize, &result);
    }
    __CFSpinUnlock(&domainCacheLock);
    return result;
}

__private_extern__ void _CFPreferencesPurgeDomainCache(void) {
    _CFSynchronizeDomainCache();
    __CFSpinLock(&domainCacheLock);
    if (domainCache) {
        CFRelease(domainCache);
        domainCache = NULL;
    }
    __CFSpinUnlock(&domainCacheLock);
}

__private_extern__ CFArrayRef  _CFPreferencesCreateDomainList(CFStringRef  userName, CFStringRef  hostName) {
#if 0 && defined(__WIN32__)
    DWORD idx, numSubkeys, maxSubKey, cnt;
    CFMutableArrayRef  retVal;
    LONG result;
    id *list, buffer[512];
    result = RegQueryInfoKeyA(_masterKey, NULL, NULL, NULL, &numSubkeys, &maxSubKey, NULL, NULL, NULL, NULL, NULL, NULL);
    if (result != ERROR_SUCCESS) {
        NSLog(@"%@: cannot query master key info; %d", _NSMethodExceptionProem(self, _cmd), result);
        return [NSArray array];
    }
    maxSubKey++;
    list = (numSubkeys <= 512) ? buffer : NSZoneMalloc(NULL, numSubkeys * sizeof(void *));
    if (_useCStringDomains < 0)
        _useCStringDomains = (NSWindows95OperatingSystem == [[NSProcessInfo processInfo] operatingSystem]);
    if (_useCStringDomains) {
        for (idx = 0, cnt = 0; idx < numSubkeys; idx++) {
            char name[maxSubKey + 1];
            DWORD nameSize = maxSubKey;
            if (RegEnumKeyExA(_masterKey, idx, name, &nameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
                list[cnt++] = [NSString stringWithCString:name length:nameSize];
        }
    } else {
        for (idx = 0, cnt = 0; idx < numSubkeys; idx++) {
            unichar name[maxSubKey + 1];
            DWORD nameSize = maxSubKey;
            if (RegEnumKeyExW(_masterKey, idx, name, &nameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
                list[cnt++] = [NSString stringWithCharacters:name length:nameSize];
        }
    }
    retVal = [NSArray arrayWithObjects:list count:cnt];
    if (list != buffer) NSZoneFree(NULL, list);
    return retVal;
#elif defined(__MACH__) || defined(__svr4__) || defined(__hpux__)
    CFAllocatorRef prefAlloc = __CFPreferencesAllocator();
    CFArrayRef  domains;
    CFMutableArrayRef  marray;
    CFStringRef  *cachedDomainKeys;
    CFPreferencesDomainRef *cachedDomains;
    SInt32 idx, cnt;
    CFStringRef  suffix;
    UInt32 suffixLen;
    CFURLRef prefDir = _preferencesDirectoryForUserHost(userName, hostName);

    if (!prefDir) {
        return NULL;
    }
    if (hostName == kCFPreferencesAnyHost) {
        suffix = CFStringCreateWithCString(prefAlloc, ".plist", kCFStringEncodingASCII);
    } else if (hostName == kCFPreferencesCurrentHost) {
        CFStringRef host = __CFCopyEthernetAddrString();
        if (!host) host = _CFStringCreateHostName();
        suffix = CFStringCreateWithFormat(prefAlloc, NULL, CFSTR(".%@.plist"), host);
        CFRelease(host);
    } else {
        suffix = CFStringCreateWithFormat(prefAlloc, NULL, CFSTR(".%@.plist"), hostName);
    }
    suffixLen = CFStringGetLength(suffix);

    domains = CFURLCreatePropertyFromResource(prefAlloc, prefDir, kCFURLFileDirectoryContents, NULL);
    CFRelease(prefDir);
    if (domains){
        marray = CFArrayCreateMutableCopy(prefAlloc, 0, domains);
        CFRelease(domains);
    } else {
        marray = CFArrayCreateMutable(prefAlloc, 0, & kCFTypeArrayCallBacks);
    }
    for (idx = CFArrayGetCount(marray)-1; idx >= 0; idx --) {
        CFURLRef  url = CFArrayGetValueAtIndex(marray, idx);
        CFStringRef string = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        if (!CFStringHasSuffix(string, suffix)) {
            CFArrayRemoveValueAtIndex(marray, idx);
        } else {
            CFStringRef  dom = CFStringCreateWithSubstring(prefAlloc, string, CFRangeMake(0, CFStringGetLength(string) - suffixLen));
            if (CFEqual(dom, CFSTR(".GlobalPreferences"))) {
                CFArraySetValueAtIndex(marray, idx, kCFPreferencesAnyApplication);
            } else {
                CFArraySetValueAtIndex(marray, idx, dom);
            }
            CFRelease(dom);
        }
        CFRelease(string);
    }
    CFRelease(suffix);

    // Now add any domains added in the cache; delete any that have been deleted in the cache
    __CFSpinLock(&domainCacheLock);
    if (!domainCache) {
        __CFSpinUnlock(&domainCacheLock);
        return marray;
    }
    cnt = CFDictionaryGetCount(domainCache);
    cachedDomainKeys = CFAllocatorAllocate(prefAlloc, 2 * cnt * sizeof(CFStringRef), 0);
    cachedDomains = (CFPreferencesDomainRef *)(cachedDomainKeys + cnt);
    CFDictionaryGetKeysAndValues(domainCache, (const void **)cachedDomainKeys, (const void **)cachedDomains);
    __CFSpinUnlock(&domainCacheLock);
    suffix = _CFPreferencesCachePrefixForUserHost(userName, hostName);
    suffixLen = CFStringGetLength(suffix);
    
    for (idx = 0; idx < cnt; idx ++) {
        CFStringRef  domainKey = cachedDomainKeys[idx];
        CFPreferencesDomainRef domain = cachedDomains[idx];
        CFStringRef  domainName;
        CFIndex keyCount = 0;

        if (!CFStringHasPrefix(domainKey, suffix)) continue;
        domainName = CFStringCreateWithSubstring(prefAlloc, domainKey, CFRangeMake(suffixLen, CFStringGetLength(domainKey) - suffixLen));
        if (CFEqual(domainName, CFSTR("*"))) {
            CFRelease(domainName);
            domainName = CFRetain(kCFPreferencesAnyApplication);
        } else if (CFEqual(domainName, kCFPreferencesCurrentApplication)) {
            CFRelease(domainName);
            domainName = CFRetain(_CFProcessNameString());
        }
        _CFPreferencesDomainGetKeysAndValues(kCFAllocatorNull, domain, NULL, &keyCount);
        if (keyCount == 0) {
            // Domain was deleted
            SInt32 firstIndexOfValue = CFArrayGetFirstIndexOfValue(marray, CFRangeMake(0, CFArrayGetCount(marray)), domainName);
            if (0 <= firstIndexOfValue) {
                CFArrayRemoveValueAtIndex(marray, firstIndexOfValue);
            }
        } else if (!CFArrayContainsValue(marray, CFRangeMake(0, CFArrayGetCount(marray)), domainName)) {
            CFArrayAppendValue(marray, domainName);
        }
        CFRelease(domainName);
    }
    CFRelease(suffix);
    CFAllocatorDeallocate(prefAlloc, cachedDomainKeys);
    return marray;
#else
#endif
}

//
// CFPreferencesDomain functions
//

CFPreferencesDomainRef _CFPreferencesDomainCreate(CFTypeRef  context, const _CFPreferencesDomainCallBacks *callBacks) {
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    CFPreferencesDomainRef newDomain;
    CFAssert(callBacks != NULL && callBacks->createDomain != NULL && callBacks->freeDomain != NULL && callBacks->fetchValue != NULL && callBacks->writeValue != NULL, __kCFLogAssertion, "Cannot create a domain with NULL callbacks");
    newDomain = (CFPreferencesDomainRef)_CFRuntimeCreateInstance(alloc, __kCFPreferencesDomainTypeID, sizeof(struct __CFPreferencesDomain) - sizeof(CFRuntimeBase), NULL);
    if (newDomain) {
        newDomain->_callBacks = callBacks;
        if (context) CFRetain(context);
        newDomain->_context = context;
        newDomain->_domain = callBacks->createDomain(alloc, context);
    }
    return newDomain;
}

CFTypeRef  _CFPreferencesDomainCreateValueForKey(CFPreferencesDomainRef domain, CFStringRef key) {
    return domain->_callBacks->fetchValue(domain->_context, domain->_domain, key);
}

void _CFPreferencesDomainSet(CFPreferencesDomainRef domain, CFStringRef  key, CFTypeRef  value) {
    domain->_callBacks->writeValue(domain->_context, domain->_domain, key, value);
}

__private_extern__ Boolean _CFPreferencesDomainSynchronize(CFPreferencesDomainRef domain) {
    return domain->_callBacks->synchronize(domain->_context, domain->_domain);
}

__private_extern__ void _CFPreferencesDomainGetKeysAndValues(CFAllocatorRef alloc, CFPreferencesDomainRef domain, void **buf[], CFIndex *numKeyValuePairs) {
    domain->_callBacks->getKeysAndValues(alloc, domain->_context, domain->_domain, buf, numKeyValuePairs);
}

__private_extern__ void _CFPreferencesDomainSetIsWorldReadable(CFPreferencesDomainRef domain, Boolean isWorldReadable) {
    if (domain->_callBacks->setIsWorldReadable) {
        domain->_callBacks->setIsWorldReadable(domain->_context, domain->_domain, isWorldReadable);
    }
}

void _CFPreferencesDomainSetDictionary(CFPreferencesDomainRef domain, CFDictionaryRef dict) {
    CFTypeRef buf[32], *keys = buf;
    CFIndex idx, count = 16;
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    
    _CFPreferencesDomainGetKeysAndValues(kCFAllocatorNull, domain, (void ***)(&keys), &count);
    if (count > 16) {
        // Have to allocate
        keys = NULL;
        count = 0;
        _CFPreferencesDomainGetKeysAndValues(alloc, domain, (void ***)(&keys), &count);
    }
    for (idx = 0; idx < count; idx ++) {
        _CFPreferencesDomainSet(domain, (CFStringRef)keys[idx], NULL);
    }
    if (keys != buf) {
        CFAllocatorDeallocate(alloc, keys);
    }

    if (dict && (count = CFDictionaryGetCount(dict)) != 0) {
        CFStringRef *newKeys = (count < 32) ? buf : CFAllocatorAllocate(alloc, count * sizeof(CFStringRef), 0);
        CFDictionaryGetKeysAndValues(dict, (const void **)newKeys, NULL);
        for (idx = 0; idx < count; idx ++) {
            CFStringRef key = newKeys[idx];
            _CFPreferencesDomainSet(domain, key, (CFTypeRef)CFDictionaryGetValue(dict, key));
        }
        if (((CFTypeRef)newKeys) != buf) {
            CFAllocatorDeallocate(alloc, newKeys);
        }
    }
}

CFDictionaryRef _CFPreferencesDomainCopyDictionary(CFPreferencesDomainRef domain) {
    CFTypeRef *keys = NULL;
    CFIndex count = 0;
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    CFDictionaryRef dict = NULL;
    _CFPreferencesDomainGetKeysAndValues(alloc, domain, (void ***)&keys, &count);
    if (count && keys) {
        CFTypeRef *values = keys + count;
        dict = CFDictionaryCreate(alloc, keys, values, count, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAllocatorDeallocate(alloc, keys);
    }
    return dict;
}

CFDictionaryRef _CFPreferencesDomainDeepCopyDictionary(CFPreferencesDomainRef domain) {
    CFDictionaryRef result = domain->_callBacks->copyDomainDictionary(domain->_context, domain->_domain);
    if(result && CFDictionaryGetCount(result) == 0) {
        CFRelease(result);
        result = NULL;
    }
    return result;
}

Boolean _CFPreferencesDomainExists(CFStringRef domainName, CFStringRef userName, CFStringRef hostName) {
    CFPreferencesDomainRef domain;
    CFIndex count = 0;
    domain = _CFPreferencesStandardDomain(domainName, userName, hostName);
    if (domain) {
        _CFPreferencesDomainGetKeysAndValues(kCFAllocatorNull, domain, NULL, &count);
        return (count > 0);
    } else {
        return false;
    }
}

/* Volatile domains - context is ignored; domain is a CFDictionary (mutable) */
static void *createVolatileDomain(CFAllocatorRef allocator, CFTypeRef  context) {
    return CFDictionaryCreateMutable(allocator, 0, & kCFTypeDictionaryKeyCallBacks, & kCFTypeDictionaryValueCallBacks);
}

static void freeVolatileDomain(CFAllocatorRef allocator, CFTypeRef  context, void *domain) {
    CFRelease((CFTypeRef)domain);
}

static CFTypeRef  fetchVolatileValue(CFTypeRef  context, void *domain, CFStringRef  key) {
    CFTypeRef  result = CFDictionaryGetValue((CFMutableDictionaryRef  )domain, key);
    if (result) CFRetain(result);
    return result;
}

static void writeVolatileValue(CFTypeRef  context, void *domain, CFStringRef  key, CFTypeRef  value) {
    if (value)
        CFDictionarySetValue((CFMutableDictionaryRef  )domain, key, value);
    else
        CFDictionaryRemoveValue((CFMutableDictionaryRef  )domain, key);
}

static Boolean synchronizeVolatileDomain(CFTypeRef  context, void *domain) {
    return true;
}

static void getVolatileKeysAndValues(CFAllocatorRef alloc, CFTypeRef context, void *domain, void **buf[], CFIndex *numKeyValuePairs) {
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)domain;
    CFIndex count = CFDictionaryGetCount(dict);

    if (buf) {
        void **values;
        if ( count < *numKeyValuePairs ) {
            values = *buf + count;
            CFDictionaryGetKeysAndValues(dict, (const void **)*buf, (const void **)values);
        } else if (alloc != kCFAllocatorNull) {
            if (*buf) {
                *buf = CFAllocatorReallocate(alloc, *buf, count * 2 * sizeof(void *), 0);
            } else {
                *buf = CFAllocatorAllocate(alloc, count*2*sizeof(void *), 0);
            }
            if (*buf) {
                values = *buf + count;
                CFDictionaryGetKeysAndValues(dict, (const void **)*buf, (const void **)values);
            }
        }
    }
    *numKeyValuePairs = count;
}

static CFDictionaryRef copyVolatileDomainDictionary(CFTypeRef context, void *volatileDomain) {
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)volatileDomain;
    
    CFDictionaryRef result = (CFDictionaryRef)CFPropertyListCreateDeepCopy(__CFPreferencesAllocator(), dict, kCFPropertyListImmutable);
    return result;
}

const _CFPreferencesDomainCallBacks __kCFVolatileDomainCallBacks = {createVolatileDomain, freeVolatileDomain, fetchVolatileValue, writeVolatileValue, synchronizeVolatileDomain, getVolatileKeysAndValues, copyVolatileDomainDictionary, NULL};


