/*
 * Copyright (c) 2012 Apple Inc. All rights reserved.
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

/*      CFBundle_Resources.c
        Copyright (c) 1999-2011, Apple Inc.  All rights reserved.
        Responsibility: David Smith
*/

#if DEPLOYMENT_TARGET_MACOSX
#define READ_DIRECTORIES 1
#elif DEPLOYMENT_TARGET_EMBEDDED
#define READ_DIRECTORIES 1
#elif DEPLOYMENT_TARGET_WINDOWS
#define READ_DIRECTORIES 0
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif

#define READ_DIRECTORIES_CACHE_CAPACITY 128

#include "CFBundle_Internal.h"
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFByteOrder.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFLocale.h>
#include <CoreFoundation/CFPreferences.h>
#include <string.h>
#include "CFInternal.h"
#include <CoreFoundation/CFPriv.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#include <sys/sysctl.h>
#endif

#if DEPLOYMENT_TARGET_MACOSX
#include <unistd.h>
#elif DEPLOYMENT_TARGET_EMBEDDED
#include <unistd.h>
#elif DEPLOYMENT_TARGET_WINDOWS
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif

#if READ_DIRECTORIES
#include <dirent.h>
#endif /* READ_DIRECTORIES */

CF_EXPORT bool CFDictionaryGetKeyIfPresent(CFDictionaryRef dict, const void *key, const void **actualkey);


static inline Boolean _CFBundleSortedArrayContains(CFArrayRef arr, CFStringRef target) {
    CFRange arrRange = CFRangeMake(0, CFArrayGetCount(arr));
    CFIndex itemIdx = CFArrayBSearchValues(arr, arrRange, target, (CFComparatorFunction)CFStringCompare, NULL);
    return itemIdx < arrRange.length && CFEqual(CFArrayGetValueAtIndex(arr, itemIdx), target);
}

// The following strings are initialized 'later' (i.e., not at static initialization time) because static init time is too early for CFSTR to work, on Windows
// This is here to make sure it gets updated when _CFGetPlatformName does
#define _CFBundleNumberOfPlatforms 7
static CFStringRef _CFBundleSupportedPlatforms[_CFBundleNumberOfPlatforms] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const char *_CFBundleSupportedPlatformStrings[_CFBundleNumberOfPlatforms] = { "iphoneos", "macos", "windows", "linux", "freebsd", "solaris", "hpux" };

// This is here to make sure it gets updated when _CFGetProductName does
#define _CFBundleNumberOfProducts 3
static CFStringRef _CFBundleSupportedProducts[_CFBundleNumberOfProducts] = { NULL, NULL, NULL };
static const char *_CFBundleSupportedProductStrings[_CFBundleNumberOfProducts] = { "iphone", "ipod", "ipad" };

#define _CFBundleNumberOfiPhoneOSPlatformProducts 3
static CFStringRef _CFBundleSupportediPhoneOSPlatformProducts[_CFBundleNumberOfiPhoneOSPlatformProducts] = { NULL, NULL, NULL };
static const char *_CFBundleSupportediPhoneOSPlatformProductStrings[_CFBundleNumberOfiPhoneOSPlatformProducts] = { "iphone", "ipod", "ipad" };

void _CFBundleResourcesInitialize() {
    for (unsigned int i = 0; i < _CFBundleNumberOfPlatforms; i++) _CFBundleSupportedPlatforms[i] = CFStringCreateWithCString(kCFAllocatorSystemDefault, _CFBundleSupportedPlatformStrings[i], kCFStringEncodingUTF8);
    
    for (unsigned int i = 0; i < _CFBundleNumberOfProducts; i++) _CFBundleSupportedProducts[i] = CFStringCreateWithCString(kCFAllocatorSystemDefault, _CFBundleSupportedProductStrings[i], kCFStringEncodingUTF8);
    
    for (unsigned int i = 0; i < _CFBundleNumberOfiPhoneOSPlatformProducts; i++) _CFBundleSupportediPhoneOSPlatformProducts[i] = CFStringCreateWithCString(kCFAllocatorSystemDefault, _CFBundleSupportediPhoneOSPlatformProductStrings[i], kCFStringEncodingUTF8);
}

static CFStringRef platform = NULL;

void _CFSetProductName(CFStringRef str) {
    if (str) CFRetain(str);
    platform = str;
    // Note that the previous value is leaked, which is fine normally
    // because the initial values would tend to be the constant strings
    // below. That is required for thread-safety value due to the Get
    // function [not being Copy]. It is also fine because people
    // shouldn't be screwing around with this value casually.
}

CFStringRef _CFGetProductName(void) {
#if DEPLOYMENT_TARGET_EMBEDDED
   if (!platform) {
      char buffer[256];
      memset(buffer, 0, sizeof(buffer));
      size_t buflen = sizeof(buffer);
      int ret = sysctlbyname("hw.machine", buffer, &buflen, NULL, 0);
      if (0 == ret || (-1 == ret && ENOMEM == errno)) {
          if (6 <= buflen && 0 == memcmp(buffer, "iPhone", 6)) {
              platform = CFSTR("iphone");
          } else if (4 <= buflen && 0 == memcmp(buffer, "iPod", 4)) {
              platform = CFSTR("ipod");
          } else if (4 <= buflen && 0 == memcmp(buffer, "iPad", 4)) {
              platform = CFSTR("ipad");
          } else {
              const char *env = __CFgetenv("IPHONE_SIMULATOR_DEVICE");
              if (env) {
                  if (0 == strcmp(env, "iPhone")) {
                      platform = CFSTR("iphone");
                  } else if (0 == strcmp(env, "iPad")) {
                      platform = CFSTR("ipad");
                  } else {
                      // fallback, unrecognized IPHONE_SIMULATOR_DEVICE
                  }
              } else {
                  // fallback, unrecognized hw.machine and no IPHONE_SIMULATOR_DEVICE
              }
          }
      }
      if (!platform) platform = CFSTR("iphone"); // fallback
   }
   return platform;
#endif
    return CFSTR("");
}

// All new-style bundles will have these extensions.
__private_extern__ CFStringRef _CFGetPlatformName(void) {
#if DEPLOYMENT_TARGET_MACOSX 
    return _CFBundleMacOSXPlatformName;
#elif DEPLOYMENT_TARGET_EMBEDDED
    return _CFBundleiPhoneOSPlatformName;
#elif DEPLOYMENT_TARGET_WINDOWS
    return _CFBundleWindowsPlatformName;
#elif DEPLOYMENT_TARGET_SOLARIS
    return _CFBundleSolarisPlatformName;
#elif DEPLOYMENT_TARGET_HPUX
    return _CFBundleHPUXPlatformName;
#elif DEPLOYMENT_TARGET_LINUX
    return _CFBundleLinuxPlatformName;
#elif DEPLOYMENT_TARGET_FREEBSD
    return _CFBundleFreeBSDPlatformName;
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

__private_extern__ CFStringRef _CFGetAlternatePlatformName(void) {
#if DEPLOYMENT_TARGET_MACOSX
    return _CFBundleAlternateMacOSXPlatformName;
#elif DEPLOYMENT_TARGET_EMBEDDED
    return _CFBundleMacOSXPlatformName;
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

static CFSpinLock_t CFBundleResourceGlobalDataLock = CFSpinLockInit;
static UniChar *_AppSupportUniChars1 = NULL;
static CFIndex _AppSupportLen1 = 0;
static UniChar *_AppSupportUniChars2 = NULL;
static CFIndex _AppSupportLen2 = 0;
static UniChar *_ResourcesUniChars = NULL;
static CFIndex _ResourcesLen = 0;
static UniChar *_PlatformUniChars = NULL;
static CFIndex _PlatformLen = 0;
static UniChar *_AlternatePlatformUniChars = NULL;
static CFIndex _AlternatePlatformLen = 0;
static UniChar *_LprojUniChars = NULL;
static CFIndex _LprojLen = 0;
static UniChar *_GlobalResourcesUniChars = NULL;
static CFIndex _GlobalResourcesLen = 0;
static UniChar *_InfoExtensionUniChars = NULL;
static CFIndex _InfoExtensionLen = 0;

static UniChar _ResourceSuffix3[32];
static CFIndex _ResourceSuffix3Len = 0;
static UniChar _ResourceSuffix2[16];
static CFIndex _ResourceSuffix2Len = 0;
static UniChar _ResourceSuffix1[16];
static CFIndex _ResourceSuffix1Len = 0;

static void _CFBundleInitStaticUniCharBuffers(void) {
    CFStringRef appSupportStr1 = _CFBundleSupportFilesDirectoryName1;
    CFStringRef appSupportStr2 = _CFBundleSupportFilesDirectoryName2;
    CFStringRef resourcesStr = _CFBundleResourcesDirectoryName;
    CFStringRef platformStr = _CFGetPlatformName();
    CFStringRef alternatePlatformStr = _CFGetAlternatePlatformName();
    CFStringRef lprojStr = _CFBundleLprojExtension;
    CFStringRef globalResourcesStr = _CFBundleNonLocalizedResourcesDirectoryName;
    CFStringRef infoExtensionStr = _CFBundleInfoExtension;

    _AppSupportLen1 = CFStringGetLength(appSupportStr1);
    _AppSupportLen2 = CFStringGetLength(appSupportStr2);
    _ResourcesLen = CFStringGetLength(resourcesStr);
    _PlatformLen = CFStringGetLength(platformStr);
    _AlternatePlatformLen = CFStringGetLength(alternatePlatformStr);
    _LprojLen = CFStringGetLength(lprojStr);
    _GlobalResourcesLen = CFStringGetLength(globalResourcesStr);
    _InfoExtensionLen = CFStringGetLength(infoExtensionStr);

    _AppSupportUniChars1 = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * (_AppSupportLen1 + _AppSupportLen2 + _ResourcesLen + _PlatformLen + _AlternatePlatformLen + _LprojLen + _GlobalResourcesLen + _InfoExtensionLen), 0);
    _AppSupportUniChars2 = _AppSupportUniChars1 + _AppSupportLen1;
    _ResourcesUniChars = _AppSupportUniChars2 + _AppSupportLen2;
    _PlatformUniChars = _ResourcesUniChars + _ResourcesLen;
    _AlternatePlatformUniChars = _PlatformUniChars + _PlatformLen;
    _LprojUniChars = _AlternatePlatformUniChars + _AlternatePlatformLen;
    _GlobalResourcesUniChars = _LprojUniChars + _LprojLen;
    _InfoExtensionUniChars = _GlobalResourcesUniChars + _GlobalResourcesLen;

    if (_AppSupportLen1 > 0) CFStringGetCharacters(appSupportStr1, CFRangeMake(0, _AppSupportLen1), _AppSupportUniChars1);
    if (_AppSupportLen2 > 0) CFStringGetCharacters(appSupportStr2, CFRangeMake(0, _AppSupportLen2), _AppSupportUniChars2);
    if (_ResourcesLen > 0) CFStringGetCharacters(resourcesStr, CFRangeMake(0, _ResourcesLen), _ResourcesUniChars);
    if (_PlatformLen > 0) CFStringGetCharacters(platformStr, CFRangeMake(0, _PlatformLen), _PlatformUniChars);
    if (_AlternatePlatformLen > 0) CFStringGetCharacters(alternatePlatformStr, CFRangeMake(0, _AlternatePlatformLen), _AlternatePlatformUniChars);
    if (_LprojLen > 0) CFStringGetCharacters(lprojStr, CFRangeMake(0, _LprojLen), _LprojUniChars);
    if (_GlobalResourcesLen > 0) CFStringGetCharacters(globalResourcesStr, CFRangeMake(0, _GlobalResourcesLen), _GlobalResourcesUniChars);
    if (_InfoExtensionLen > 0) CFStringGetCharacters(infoExtensionStr, CFRangeMake(0, _InfoExtensionLen), _InfoExtensionUniChars);

    _ResourceSuffix1Len = CFStringGetLength(platformStr);
    if (_ResourceSuffix1Len > 0) _ResourceSuffix1[0] = '-';
    if (_ResourceSuffix1Len > 0) CFStringGetCharacters(platformStr, CFRangeMake(0, _ResourceSuffix1Len), _ResourceSuffix1 + 1);
    if (_ResourceSuffix1Len > 0) _ResourceSuffix1Len++;
    CFStringRef productStr = _CFGetProductName();
    if (CFEqual(productStr, CFSTR("ipod"))) { // For now, for resource lookups, hide ipod distinction and make it look for iphone resources
        productStr = CFSTR("iphone");
    }
    _ResourceSuffix2Len = CFStringGetLength(productStr);
    if (_ResourceSuffix2Len > 0) _ResourceSuffix2[0] = '~';
    if (_ResourceSuffix2Len > 0) CFStringGetCharacters(productStr, CFRangeMake(0, _ResourceSuffix2Len), _ResourceSuffix2 + 1);
    if (_ResourceSuffix2Len > 0) _ResourceSuffix2Len++;
    if (_ResourceSuffix1Len > 1 && _ResourceSuffix2Len > 1) {
        _ResourceSuffix3Len = _ResourceSuffix1Len + _ResourceSuffix2Len;
        memmove(_ResourceSuffix3, _ResourceSuffix1, sizeof(UniChar) * _ResourceSuffix1Len);
        memmove(_ResourceSuffix3 + _ResourceSuffix1Len, _ResourceSuffix2, sizeof(UniChar) * _ResourceSuffix2Len);
    }
}

CF_INLINE void _CFEnsureStaticBuffersInited(void) {
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (!_AppSupportUniChars1) _CFBundleInitStaticUniCharBuffers();
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
}

#if READ_DIRECTORIES

static CFMutableDictionaryRef contentsCache = NULL;
static CFMutableDictionaryRef directoryContentsCache = NULL;
static CFMutableDictionaryRef unknownContentsCache = NULL;

typedef enum {
    _CFBundleAllContents = 0,
    _CFBundleDirectoryContents = 1,
    _CFBundleUnknownContents = 2
} _CFBundleDirectoryContentsType;

extern void _CFArraySortValues(CFMutableArrayRef array, CFComparatorFunction comparator, void *context);

static CFArrayRef _CFBundleCopySortedDirectoryContentsAtPath(CFStringRef path, _CFBundleDirectoryContentsType contentsType) {
    CFArrayRef result = NULL;
    
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (contentsType == _CFBundleUnknownContents) {
        if (unknownContentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(unknownContentsCache, path);
    } else if (contentsType == _CFBundleDirectoryContents) {
        if (directoryContentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(directoryContentsCache, path);
    } else {
        if (contentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(contentsCache, path);
    }
    if (result) CFRetain(result);
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);

    if (!result) {
        Boolean tryToOpen = false, allDots = true;
        char cpathBuff[CFMaxPathSize];
        CFIndex cpathLen = 0, idx, lastSlashIdx = 0;
        DIR *dirp = NULL;
        struct dirent *dent;
        CFMutableArrayRef contents = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks), directoryContents = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks), unknownContents = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFStringRef dirName, name;
        
        cpathBuff[0] = '\0';
        if (CFStringGetFileSystemRepresentation(path, cpathBuff, CFMaxPathSize)) {
            tryToOpen = true;
            cpathLen = strlen(cpathBuff);
            
            // First see whether we already know that the directory doesn't exist
            for (idx = cpathLen; lastSlashIdx == 0 && idx-- > 0;) {
                if (cpathBuff[idx] == '/') lastSlashIdx = idx;
                else if (cpathBuff[idx] != '.') allDots = false;
            }
            if (lastSlashIdx > 0 && lastSlashIdx + 1 < cpathLen && !allDots) {
                cpathBuff[lastSlashIdx] = '\0';
                dirName = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, cpathBuff);
                if (dirName) {
                    name = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, cpathBuff + lastSlashIdx + 1);
                    if (name) {
                        // ??? we might like to use directoryContentsCache rather than contentsCache here, but we cannot unless we resolve DT_LNKs below
                        CFArrayRef dirDirContents = NULL;
                        
                        __CFSpinLock(&CFBundleResourceGlobalDataLock);
                        if (contentsCache) dirDirContents = (CFArrayRef)CFDictionaryGetValue(contentsCache, dirName);
                        if (dirDirContents) {
                            Boolean foundIt = false;
                            CFIndex dirDirIdx, dirDirLength = CFArrayGetCount(dirDirContents);
                            for (dirDirIdx = 0; !foundIt && dirDirIdx < dirDirLength; dirDirIdx++) if (kCFCompareEqualTo == CFStringCompare(name, CFArrayGetValueAtIndex(dirDirContents, dirDirIdx), kCFCompareCaseInsensitive)) foundIt = true;
                            if (!foundIt) tryToOpen = false;
                        }
                        __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
                        CFRelease(name);
                    }
                    CFRelease(dirName);
                }
                cpathBuff[lastSlashIdx] = '/';
            }
        }
        if (tryToOpen && (dirp = opendir(cpathBuff))) {
            while ((dent = readdir(dirp))) {
                CFIndex nameLen = dent->d_namlen;
                if (0 == nameLen || 0 == dent->d_fileno || ('.' == dent->d_name[0] && (1 == nameLen || (2 == nameLen && '.' == dent->d_name[1]) || '_' == dent->d_name[1]))) continue;
                name = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, dent->d_name);
                if (name) {
                    // ??? should we follow links for DT_LNK?  unless we do, results are approximate, but for performance reasons we do not
                    // ??? likewise for DT_UNKNOWN
                    // ??? the utility of distinguishing directories from other contents is somewhat doubtful anyway
                    CFArrayAppendValue(contents, name);
                    if (dent->d_type == DT_DIR) {
                        CFArrayAppendValue(directoryContents, name);
                    } else if (dent->d_type == DT_UNKNOWN) {
                        CFArrayAppendValue(unknownContents, name);
                    }
                    CFRelease(name);
                }
            }
            (void)closedir(dirp);
        }
        
        _CFArraySortValues(contents, (CFComparatorFunction)CFStringCompare, NULL);
        _CFArraySortValues(directoryContents, (CFComparatorFunction)CFStringCompare, NULL);
        _CFArraySortValues(unknownContents, (CFComparatorFunction)CFStringCompare, NULL);
        
        __CFSpinLock(&CFBundleResourceGlobalDataLock);
        if (!contentsCache) contentsCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, READ_DIRECTORIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (READ_DIRECTORIES_CACHE_CAPACITY <= CFDictionaryGetCount(contentsCache)) CFDictionaryRemoveAllValues(contentsCache);
        CFDictionaryAddValue(contentsCache, path, contents);

        if (!directoryContentsCache) directoryContentsCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, READ_DIRECTORIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (READ_DIRECTORIES_CACHE_CAPACITY <= CFDictionaryGetCount(directoryContentsCache)) CFDictionaryRemoveAllValues(directoryContentsCache);
        CFDictionaryAddValue(directoryContentsCache, path, directoryContents);

        if (!unknownContentsCache) unknownContentsCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, READ_DIRECTORIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (READ_DIRECTORIES_CACHE_CAPACITY <= CFDictionaryGetCount(unknownContentsCache)) CFDictionaryRemoveAllValues(unknownContentsCache);
        CFDictionaryAddValue(unknownContentsCache, path, unknownContents);

        if (contentsType == _CFBundleUnknownContents) {
            result = CFRetain(unknownContents);
        } else if (contentsType == _CFBundleDirectoryContents) {
            result = CFRetain(directoryContents);
        } else {
            result = CFRetain(contents);
        }
        
        CFRelease(contents);
        CFRelease(directoryContents);
        CFRelease(unknownContents);
        __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
    }
    return result;
}

static void _CFBundleFlushContentsCaches(void) {
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (contentsCache) CFDictionaryRemoveAllValues(contentsCache);
    if (directoryContentsCache) CFDictionaryRemoveAllValues(directoryContentsCache);
    if (unknownContentsCache) CFDictionaryRemoveAllValues(unknownContentsCache);
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
}

static void _CFBundleFlushContentsCacheForPath(CFMutableDictionaryRef cache, CFStringRef path) {
    CFStringRef keys[READ_DIRECTORIES_CACHE_CAPACITY];
    unsigned i, count = CFDictionaryGetCount(cache);
    if (count <= READ_DIRECTORIES_CACHE_CAPACITY) {
        CFDictionaryGetKeysAndValues(cache, (const void **)keys, NULL);
        for (i = 0; i < count; i++) {
            if (CFStringFindWithOptions(keys[i], path, CFRangeMake(0, CFStringGetLength(keys[i])), kCFCompareAnchored|kCFCompareCaseInsensitive, NULL)) CFDictionaryRemoveValue(cache, keys[i]);
        }
    }
}

static void _CFBundleFlushContentsCachesForPath(CFStringRef path) {
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (contentsCache) _CFBundleFlushContentsCacheForPath(contentsCache, path);
    if (directoryContentsCache) _CFBundleFlushContentsCacheForPath(directoryContentsCache, path);
    if (unknownContentsCache) _CFBundleFlushContentsCacheForPath(unknownContentsCache, path);
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
}

#endif /* READ_DIRECTORIES */

CF_EXPORT void _CFBundleFlushCachesForURL(CFURLRef url) {
#if READ_DIRECTORIES
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
    CFStringRef path = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    _CFBundleFlushContentsCachesForPath(path);
    CFRelease(path);
    CFRelease(absoluteURL);
#endif /* READ_DIRECTORIES */
}

CF_EXPORT void _CFBundleFlushCaches(void) {
#if READ_DIRECTORIES
    _CFBundleFlushContentsCaches();
#endif /* READ_DIRECTORIES */
}

static inline Boolean _CFIsResourceCommon(char *path, Boolean *isDir) {
    Boolean exists;
    SInt32 mode;
    if (_CFGetPathProperties(kCFAllocatorSystemDefault, path, &exists, &mode, NULL, NULL, NULL, NULL) == 0) {
        if (isDir) *isDir = ((exists && ((mode & S_IFMT) == S_IFDIR)) ? true : false);
        return (exists && (mode & 0444));
    }
    return false;
}

__private_extern__ Boolean _CFIsResourceAtURL(CFURLRef url, Boolean *isDir) {
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathLength)) return false;
    
    return _CFIsResourceCommon(path, isDir);
}

__private_extern__ Boolean _CFIsResourceAtPath(CFStringRef path, Boolean *isDir) {
    char pathBuf[CFMaxPathSize];
    if (!CFStringGetFileSystemRepresentation(path, pathBuf, CFMaxPathSize)) return false;
    
    return _CFIsResourceCommon(pathBuf, isDir);
}

#if READ_DIRECTORIES
static CFArrayRef _CFCopyTypesForSearchBundleDirectory(CFAllocatorRef alloc, UniChar *pathUniChars, CFIndex pathLen, UniChar *nameUniChars, CFIndex nameLen, CFArrayRef resTypes, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, uint8_t version) {
    CFMutableArrayRef result = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    CFArrayRef contents;
    CFRange contentsRange, resultRange = CFRangeMake(0, 0);
    CFIndex dirPathLen = pathLen, numResTypes = CFArrayGetCount(resTypes), i, j;
    
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, dirPathLen, dirPathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    //fprintf(stderr, "looking in ");CFShow(cheapStr);
    contents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleAllContents);
    contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
    
    CFStringSetExternalCharactersNoCopy(tmpString, nameUniChars, nameLen, nameLen);
    CFStringReplaceAll(cheapStr, tmpString);
    for (i = 0; i < contentsRange.length; i++) {
        CFStringRef content = CFArrayGetValueAtIndex(contents, i);
        if (CFStringHasPrefix(content, cheapStr)) {
            //fprintf(stderr, "found ");CFShow(content);
            for (j = 0; j < numResTypes; j++) {
                CFStringRef resType = CFArrayGetValueAtIndex(resTypes, j);
                if (!CFArrayContainsValue(result, resultRange, resType) && CFStringHasSuffix(content, resType)) {
                    CFArrayAppendValue(result, resType);
                    resultRange.length = CFArrayGetCount(result);
                }
            }
        }
    }
    //fprintf(stderr, "result ");CFShow(result);
    CFRelease(contents);
    return result;
}
#endif /* READ_DIRECTORIES */

#if DEPLOYMENT_TARGET_EMBEDDED
static void _CFSearchBundleDirectory2(CFAllocatorRef alloc, CFMutableArrayRef result, UniChar *pathUniChars, CFIndex pathLen, UniChar *nameUniChars, CFIndex nameLen, UniChar *typeUniChars, CFIndex typeLen, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, uint8_t version) {
    // pathUniChars is the full path to the directory we are searching.
    // nameUniChars is what we are looking for.
    // typeUniChars is the type we are looking for.
    // platformUniChars is the platform name.
    // cheapStr is available for our use for whatever we want.
    // URLs for found resources get added to result.

    Boolean appendSucceeded = true;
    if (nameLen > 0) appendSucceeded = _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, nameUniChars, nameLen);
    if (! appendSucceeded) return;
    CFIndex savedPathLen = pathLen;

    // Try in order:
    // NAME-PLATFORM~PRODUCT.TYPE (disabled for now)
    // NAME~PRODUCT.TYPE
    // NAME-PLATFORM.TYPE (disabled for now)
    // NAME.TYPE

#if 0
    appendSucceeded = (pathLen + _ResourceSuffix3Len < CFMaxPathSize);
    if (appendSucceeded) {
        memmove(pathUniChars + pathLen, _ResourceSuffix3, _ResourceSuffix3Len * sizeof(UniChar));
        pathLen += _ResourceSuffix3Len;
    }
    if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    if (appendSucceeded) {
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        Boolean Found = false, IsDir = false;
        Found = _CFIsResourceAtPath(cheapStr, &IsDir);
        if (Found) {
            CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, IsDir);
            CFArrayAppendValue(result, url);
            CFRelease(url);
            return;
        }
    }
#endif

    pathLen = savedPathLen;
    appendSucceeded = (pathLen + _ResourceSuffix2Len < CFMaxPathSize);
    if (appendSucceeded) {
        memmove(pathUniChars + pathLen, _ResourceSuffix2, _ResourceSuffix2Len * sizeof(UniChar));
        pathLen += _ResourceSuffix2Len;
    }
    if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    if (appendSucceeded) {
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        Boolean Found = false, IsDir = false;
        Found = _CFIsResourceAtPath(cheapStr, &IsDir);
        if (Found) {
            CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, IsDir);
            CFArrayAppendValue(result, url);
            CFRelease(url);
            return;
        }
    }

#if 0
    pathLen = savedPathLen;
    appendSucceeded = (pathLen + _ResourceSuffix1Len < CFMaxPathSize);
    if (appendSucceeded) {
        memmove(pathUniChars + pathLen, _ResourceSuffix1, _ResourceSuffix1Len * sizeof(UniChar));
        pathLen += _ResourceSuffix1Len;
    }
    if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    if (appendSucceeded) {
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        Boolean Found = false, IsDir = false;
        Found = _CFIsResourceAtPath(cheapStr, &IsDir);
        if (Found) {
            CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, IsDir);
            CFArrayAppendValue(result, url);
            CFRelease(url);
            return;
        }
    }
#endif

    pathLen = savedPathLen;
    appendSucceeded = true;
    if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    if (appendSucceeded) {
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        Boolean Found = false, IsDir = false;
        Found = _CFIsResourceAtPath(cheapStr, &IsDir);
        if (Found) {
            CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, IsDir);
            CFArrayAppendValue(result, url);
            CFRelease(url);
            return;
        }
    }
}
#endif

static void _CFSearchBundleDirectory(CFAllocatorRef alloc, CFMutableArrayRef result, UniChar *pathUniChars, CFIndex pathLen, UniChar *nameUniChars, CFIndex nameLen, UniChar *typeUniChars, CFIndex typeLen, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, uint8_t version) {

#if DEPLOYMENT_TARGET_EMBEDDED
    _CFSearchBundleDirectory2(alloc, result, pathUniChars, pathLen, nameUniChars, nameLen, typeUniChars, typeLen, cheapStr, tmpString, version);
#else
    // pathUniChars is the full path to the directory we are searching.
    // nameUniChars is what we are looking for.
    // typeUniChars is the type we are looking for.
    // platformUniChars is the platform name.
    // cheapStr is available for our use for whatever we want.
    // URLs for found resources get added to result.
    CFIndex savedPathLen;
    Boolean appendSucceeded = true, platformGenericFound = false, platformSpecificFound = false, platformGenericIsDir = false, platformSpecificIsDir = false;
#if READ_DIRECTORIES
    Boolean platformGenericIsUnknown = false, platformSpecificIsUnknown = false; 
#endif
    CFStringRef platformGenericStr = NULL;

#if READ_DIRECTORIES
    CFIndex dirPathLen = pathLen;
    CFArrayRef contents, directoryContents, unknownContents;
    CFRange contentsRange, directoryContentsRange, unknownContentsRange;
    
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, dirPathLen, dirPathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    //fprintf(stderr, "looking in ");CFShow(cheapStr);
    contents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleAllContents);
    contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
    directoryContents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleDirectoryContents);
    directoryContentsRange = CFRangeMake(0, CFArrayGetCount(directoryContents));
    unknownContents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleUnknownContents);
    unknownContentsRange = CFRangeMake(0, CFArrayGetCount(unknownContents));
#endif /* READ_DIRECTORIES */
    
    if (nameLen > 0) appendSucceeded = _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, nameUniChars, nameLen);
    savedPathLen = pathLen;
    if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    if (appendSucceeded) {
#if READ_DIRECTORIES
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + dirPathLen + 1, pathLen - dirPathLen - 1, pathLen - dirPathLen - 1);
        CFStringReplaceAll(cheapStr, tmpString);
        platformGenericFound = _CFBundleSortedArrayContains(contents, cheapStr);
        platformGenericIsDir = _CFBundleSortedArrayContains(directoryContents, cheapStr);
        platformGenericIsUnknown =  _CFBundleSortedArrayContains(unknownContents, cheapStr);
        //fprintf(stderr, "looking for ");CFShow(cheapStr);if (platformGenericFound) fprintf(stderr, "found it\n"); if (platformGenericIsDir) fprintf(stderr, "a directory\n");
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        if (platformGenericFound && platformGenericIsUnknown) {
            (void)_CFIsResourceAtPath(cheapStr, &platformGenericIsDir);
            //if (platformGenericIsDir) fprintf(stderr, "a directory after all\n"); else fprintf(stderr, "not a directory after all\n"); 
        }
#else /* READ_DIRECTORIES */
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        platformGenericFound = _CFIsResourceAtPath(cheapStr, &platformGenericIsDir);
#endif /* READ_DIRECTORIES */
    }
    
    // Check for platform specific.
    if (platformGenericFound) {
        platformGenericStr = (CFStringRef)CFStringCreateCopy(kCFAllocatorSystemDefault, cheapStr);
        if (!platformSpecificFound && (_PlatformLen > 0)) {
            pathLen = savedPathLen;
            pathUniChars[pathLen++] = (UniChar)'-';
            memmove(pathUniChars + pathLen, _PlatformUniChars, _PlatformLen * sizeof(UniChar));
            pathLen += _PlatformLen;
            if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
            if (appendSucceeded) {
#if READ_DIRECTORIES
                CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + dirPathLen + 1, pathLen - dirPathLen - 1, pathLen - dirPathLen - 1);
                CFStringReplaceAll(cheapStr, tmpString);
                platformSpecificFound = _CFBundleSortedArrayContains(contents, cheapStr);
                platformSpecificIsDir = _CFBundleSortedArrayContains(directoryContents, cheapStr);
                platformSpecificIsUnknown = _CFBundleSortedArrayContains(unknownContents, cheapStr);
                //fprintf(stderr, "looking for ");CFShow(cheapStr);if (platformSpecificFound) fprintf(stderr, "found it\n"); if (platformSpecificIsDir) fprintf(stderr, "a directory\n");
                CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
                CFStringReplaceAll(cheapStr, tmpString);
                if (platformSpecificFound && platformSpecificIsUnknown) {
                    (void)_CFIsResourceAtPath(cheapStr, &platformSpecificIsDir);
                    //if (platformSpecificIsDir) fprintf(stderr, "a directory after all\n"); else fprintf(stderr, "not a directory after all\n"); 
                }
#else /* READ_DIRECTORIES */
                CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
                CFStringReplaceAll(cheapStr, tmpString);
                platformSpecificFound = _CFIsResourceAtPath(cheapStr, &platformSpecificIsDir);
#endif /* READ_DIRECTORIES */
            }
        }
    }
    if (platformSpecificFound) {
        CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, platformSpecificIsDir);
        CFArrayAppendValue(result, url);
        CFRelease(url);
    } else if (platformGenericFound) {
        CFURLRef url = CFURLCreateWithFileSystemPath(alloc, platformGenericStr ? platformGenericStr : cheapStr, PLATFORM_PATH_STYLE, platformGenericIsDir);
        CFArrayAppendValue(result, url);
        CFRelease(url);
    }
    if (platformGenericStr) CFRelease(platformGenericStr);
#if READ_DIRECTORIES
    CFRelease(contents);
    CFRelease(directoryContents);
    CFRelease(unknownContents);
#endif /* READ_DIRECTORIES */
#endif
}

#if READ_DIRECTORIES
static void _CFSearchBundleDirectoryWithPredicate(CFAllocatorRef alloc, CFMutableArrayRef result, UniChar *pathUniChars, CFIndex dirPathLen, Boolean (^predicate)(CFStringRef filename, Boolean *stop), CFMutableStringRef cheapStr, CFMutableStringRef tmpString, Boolean *stopLooking, uint8_t version) {

    // pathUniChars is the full path to the directory we are searching.
    // platformUniChars is the platform name.
    // predicate is a block that evaluates a given filename to see if it's a match.
    // cheapStr is available for our use for whatever we want.
    // URLs for found resources get added to result.
    
    // get the contents of the directory
    CFArrayRef contents, directoryContents, unknownContents;
    CFRange contentsRange;
    
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, dirPathLen, dirPathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    
    if (!_CFAppendTrailingPathSlash(pathUniChars, &dirPathLen, CFMaxPathSize)) {
        return;
    }
    
    //fprintf(stderr, "looking in ");CFShow(cheapStr);
    contents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleAllContents);
    contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
    directoryContents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleDirectoryContents);
    unknownContents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleUnknownContents);
        
    // scan directory contents for matches against predicate
    for (int i = 0; i < contentsRange.length; i++) {
        CFStringRef candidateFilename = CFArrayGetValueAtIndex(contents, i);
        if (predicate(candidateFilename, stopLooking)) {
            // we want this resource, though possibly a platform specific version of it
            // unpack candidateFilename string into pathUniChars after verifying that we have enough space in the buffer
            CFIndex candidateFilenameLength = CFStringGetLength(candidateFilename);
            if ((dirPathLen + candidateFilenameLength < CFMaxPathSize)) {
                CFStringGetCharacters(candidateFilename, CFRangeMake(0, candidateFilenameLength), pathUniChars + dirPathLen);
                
                // is there a platform specific version available? if so update pathUniChars to contain it and candidateFilenameLength to describe its length.
                static const int platformSeparatorLen = 1; // the length of '-', as appears in foo-macos.tiff.  sugar to make the following easier to read.
                if (_PlatformLen && (dirPathLen + candidateFilenameLength + platformSeparatorLen + _PlatformLen < CFMaxPathSize)) {
                    CFIndex candidateFilenameWithoutExtensionLen = _CFLengthAfterDeletingPathExtension(pathUniChars + dirPathLen, candidateFilenameLength);
                    CFIndex extensionLen = candidateFilenameLength - candidateFilenameWithoutExtensionLen;
                    // shift the extension over to make space for the platform
                    memmove(pathUniChars + dirPathLen + candidateFilenameWithoutExtensionLen + platformSeparatorLen + _PlatformLen, pathUniChars + dirPathLen + candidateFilenameWithoutExtensionLen, extensionLen * sizeof(UniChar));
                    // write the platform into the middle of the string
                    pathUniChars[dirPathLen + candidateFilenameWithoutExtensionLen] = (UniChar)'-';
                    memcpy(pathUniChars + dirPathLen + candidateFilenameWithoutExtensionLen + platformSeparatorLen, _PlatformUniChars, _PlatformLen * sizeof(UniChar));
                    // pack it up as a CFStringRef
                    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + dirPathLen, candidateFilenameLength + platformSeparatorLen + _PlatformLen, candidateFilenameLength + _PlatformLen);
                    CFStringReplaceAll(cheapStr, tmpString);
                    // is the platform specialized version there?
                    if (_CFBundleSortedArrayContains(contents, cheapStr)) {
                        // woo. update the candidateFilenameLength.  we'll update the candidateFilename too for consistency, but we don't actually use it again.  
                        // the pathUniChars now contains the full path to the file
                        candidateFilename = cheapStr;
                        candidateFilenameLength = candidateFilenameLength + _PlatformLen + platformSeparatorLen;
                    } else {
                        // nope, no platform specific resource.  Put the pathUniChars back how they were before, without the platform.
                        memmove(pathUniChars + dirPathLen + candidateFilenameWithoutExtensionLen, pathUniChars + dirPathLen + candidateFilenameWithoutExtensionLen + platformSeparatorLen + _PlatformLen, extensionLen * sizeof(UniChar));
                    }
                }
                
                // get the full path into cheapStr
                CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, dirPathLen + candidateFilenameLength, dirPathLen + candidateFilenameLength);
                CFStringReplaceAll(cheapStr, tmpString);
                
                // is the resource a directory?  we need to know so that we can avoid file access when making a URL.
                Boolean isDir = 0;
                if (_CFBundleSortedArrayContains(directoryContents, cheapStr)) {
                    isDir = 1;
                } else if (_CFBundleSortedArrayContains(unknownContents, cheapStr)) {
                    _CFIsResourceAtPath(cheapStr, &isDir);
                }
                
                CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, isDir);
                CFArrayAppendValue(result, url);
                CFRelease(url);
            }
        }
        
        if (*stopLooking) break;
    }
    
    CFRelease(contents);
    CFRelease(directoryContents);
    CFRelease(unknownContents);
}
#endif

static void _CFFindBundleResourcesInRawDir(CFAllocatorRef alloc, UniChar *workingUniChars, CFIndex workingLen, UniChar *nameUniChars, CFIndex nameLen, CFArrayRef resTypes, CFIndex limit, Boolean *stopLooking, Boolean (^predicate)(CFStringRef filename, Boolean *stop), uint8_t version, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, CFMutableArrayRef result) {
    if (predicate) {
#if READ_DIRECTORIES
        _CFSearchBundleDirectoryWithPredicate(alloc, result, workingUniChars, workingLen, predicate, cheapStr, tmpString, stopLooking, version);
        return;
#else
        CFLog(kCFLogLevelCritical, CFSTR("_CFFindBundleResourcesInRawDir: predicate blocks are not supported on this platform"));
        HALT;
#endif
    }
    if (nameLen > 0) {
        // If we have a resName, just call the search API.  We may have to loop over the resTypes.
        if (!resTypes) {
            _CFSearchBundleDirectory(alloc, result, workingUniChars, workingLen, nameUniChars, nameLen, NULL, 0, cheapStr, tmpString, version);
        } else {
            CFArrayRef subResTypes = resTypes;
            Boolean releaseSubResTypes = false;
            CFIndex i, c = CFArrayGetCount(resTypes);
#if READ_DIRECTORIES
            if (c > 2) {
                // this is an optimization we employ when searching for large numbers of types, if the directory contents are available
                // we scan the directory contents and restrict the list of resTypes to the types that might actually occur with the specified name
                subResTypes = _CFCopyTypesForSearchBundleDirectory(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, cheapStr, tmpString, version);
                c = CFArrayGetCount(subResTypes);
                releaseSubResTypes = true;
            }
#endif /* READ_DIRECTORIES */
            for (i = 0; i < c; i++) {
                CFStringRef curType = (CFStringRef)CFArrayGetValueAtIndex(subResTypes, i);
                CFIndex typeLen = CFStringGetLength(curType);
                STACK_BUFFER_DECL(UniChar, typeChars, typeLen);
                CFStringGetCharacters(curType, CFRangeMake(0, typeLen), typeChars);
                _CFSearchBundleDirectory(alloc, result, workingUniChars, workingLen, nameUniChars, nameLen, typeChars, typeLen, cheapStr, tmpString, version);
                if (limit <= CFArrayGetCount(result)) break;
            }
            if (releaseSubResTypes) CFRelease(subResTypes);
        }
    } else {
        // If we have no resName, do it by hand. We may have to loop over the resTypes.
        char cpathBuff[CFMaxPathSize];
        CFIndex cpathLen;
        CFMutableArrayRef children;

        CFStringSetExternalCharactersNoCopy(tmpString, workingUniChars, workingLen, workingLen);
        if (!CFStringGetFileSystemRepresentation(tmpString, cpathBuff, CFMaxPathSize)) return;
        cpathLen = strlen(cpathBuff);

        if (!resTypes) {
            // ??? should this use _CFBundleCopyDirectoryContentsAtPath?
            children = _CFContentsOfDirectory(alloc, cpathBuff, NULL, NULL, NULL);
            if (children) {
                CFIndex childIndex, childCount = CFArrayGetCount(children);
                for (childIndex = 0; childIndex < childCount; childIndex++) CFArrayAppendValue(result, CFArrayGetValueAtIndex(children, childIndex));
                CFRelease(children);
            }
        } else {
            CFIndex i, c = CFArrayGetCount(resTypes);
            for (i = 0; i < c; i++) {
                CFStringRef curType = (CFStringRef)CFArrayGetValueAtIndex(resTypes, i);

                // ??? should this use _CFBundleCopyDirectoryContentsAtPath?
                children = _CFContentsOfDirectory(alloc, cpathBuff, NULL, NULL, curType);
                if (children) {
                    CFIndex childIndex, childCount = CFArrayGetCount(children);
                    for (childIndex = 0; childIndex < childCount; childIndex++) CFArrayAppendValue(result, CFArrayGetValueAtIndex(children, childIndex));
                    CFRelease(children);
                }
                if (limit <= CFArrayGetCount(result)) break;
            }
        }
    }
}

static void _CFFindBundleResourcesInResourcesDir(CFAllocatorRef alloc, UniChar *workingUniChars, CFIndex workingLen, UniChar *subDirUniChars, CFIndex subDirLen, CFArrayRef searchLanguages, UniChar *nameUniChars, CFIndex nameLen, CFArrayRef resTypes, CFIndex limit, Boolean (^predicate)(CFStringRef filename, Boolean *stop), uint8_t version, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, CFMutableArrayRef result) {
    CFIndex savedWorkingLen = workingLen;
    Boolean stopLooking = false; // for predicate based-queries, we set stopLooking instead of using a limit
    // Look directly in the directory specified in workingUniChars. as if it is a Resources directory.
    if (1 == version) {
        // Add the non-localized resource directory.
        Boolean appendSucceeded = _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _GlobalResourcesUniChars, _GlobalResourcesLen);
        if (appendSucceeded && subDirLen > 0) appendSucceeded = _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen);
        if (appendSucceeded) _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, &stopLooking, predicate, version, cheapStr, tmpString, result);
        // Strip the non-localized resource directory.
        workingLen = savedWorkingLen;
    }
    if (CFArrayGetCount(result) < limit && !stopLooking) {
        Boolean appendSucceeded = true;
        if (subDirLen > 0) appendSucceeded = _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen);
        if (appendSucceeded) _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, &stopLooking, predicate, version, cheapStr, tmpString, result);
    }
    
    // Now search the local resources.
    workingLen = savedWorkingLen;
    if (CFArrayGetCount(result) < limit && !stopLooking) {
        CFIndex langCount = (searchLanguages ? CFArrayGetCount(searchLanguages) : 0);
        // MF:??? OK to hard-wire this length?
        UniChar curLangUniChars[255];
        CFIndex numResults = CFArrayGetCount(result);

        for (CFIndex langIndex = 0; langIndex < langCount; langIndex++) {
            CFStringRef curLangStr = (CFStringRef)CFArrayGetValueAtIndex(searchLanguages, langIndex);
            CFIndex curLangLen = CFStringGetLength(curLangStr);
            if (curLangLen > 255) curLangLen = 255;
            CFStringGetCharacters(curLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
            savedWorkingLen = workingLen;
            if (!_CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, curLangUniChars, curLangLen)) {
                workingLen = savedWorkingLen;
                continue;
            }
            if (!_CFAppendPathExtension(workingUniChars, &workingLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
                workingLen = savedWorkingLen;
                continue;
            }
            if (subDirLen > 0) {
                if (!_CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen)) {
                    workingLen = savedWorkingLen;
                    continue;
                }
            }
            _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, &stopLooking, predicate, version, cheapStr, tmpString, result);
            
            // Back off this lproj component
            workingLen = savedWorkingLen;
            if (CFArrayGetCount(result) != numResults) {
                // We found resources in a language we already searched.  Don't look any farther.
                // We also don't need to check the limit, since if the count changed at all, we are bailing.
                break;
            }
        }
    }
}

extern void _CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex len);

CFArrayRef _CFFindBundleResources(CFBundleRef bundle, CFURLRef bundleURL, CFStringRef subDirName, CFArrayRef searchLanguages, CFStringRef resName, CFArrayRef resTypes, CFIndex limit, Boolean (^predicate)(CFStringRef filename, Boolean *stop), uint8_t version) {
    CFMutableArrayRef result = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);

    // Build an absolute path to the base directory.
    // If no URL was passed, we get it from the bundle.
    CFURLRef baseURL = bundleURL ? (CFURLRef)CFRetain(bundleURL) : (bundle ? CFBundleCopyBundleURL(bundle) : NULL);
    CFURLRef absoluteURL = baseURL ? CFURLCopyAbsoluteURL(baseURL) : NULL;
    CFStringRef basePath = absoluteURL ? CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE) : NULL;
    if (absoluteURL) CFRelease(absoluteURL);
    if (baseURL) CFRelease(baseURL);
    baseURL = absoluteURL = bundleURL = NULL;
    bundle = NULL;
    // bundle and bundleURL arguments are not used any further

    if (!basePath) return result;


    UniChar *workingUniChars, *nameUniChars, *subDirUniChars;
    CFIndex nameLen = 0;
    CFIndex workingLen, savedWorkingLen;
    CFMutableStringRef cheapStr, tmpString;

    if (resName) {  
        char buff[CFMaxPathSize];
        CFStringRef newResName = NULL;
        if (CFStringGetFileSystemRepresentation(resName, buff, CFMaxPathSize)) newResName = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, buff);
        resName = newResName ? newResName : (CFStringRef)CFRetain(resName);
        nameLen = CFStringGetLength(resName);
    }

    // Init the one-time-only unichar buffers.
    _CFEnsureStaticBuffersInited();

    // Build UniChar buffers for some of the string pieces we need.
    CFIndex subDirLen = (subDirName ? CFStringGetLength(subDirName) : 0);
    nameUniChars = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * (nameLen + subDirLen + CFMaxPathSize), 0);
    if (nameUniChars) {
        subDirUniChars = nameUniChars + nameLen;
        workingUniChars = subDirUniChars + subDirLen;

        if (nameLen > 0) CFStringGetCharacters(resName, CFRangeMake(0, nameLen), nameUniChars);
        if (subDirLen > 0) CFStringGetCharacters(subDirName, CFRangeMake(0, subDirLen), subDirUniChars);

        if ((workingLen = CFStringGetLength(basePath)) > 0) CFStringGetCharacters(basePath, CFRangeMake(0, workingLen), workingUniChars);
        savedWorkingLen = workingLen;
        if (1 == version) {
            _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _AppSupportUniChars1, _AppSupportLen1);
        } else if (2 == version) {
            _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _AppSupportUniChars2, _AppSupportLen2);
        }
        if (0 == version || 1 == version || 2 == version) _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _ResourcesUniChars, _ResourcesLen);

        // both of these used for temp string operations, for slightly different purposes, where each type is appropriate
        cheapStr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
        _CFStrSetDesiredCapacity(cheapStr, CFMaxPathSize);
        tmpString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);

        _CFFindBundleResourcesInResourcesDir(kCFAllocatorSystemDefault, workingUniChars, workingLen, subDirUniChars, subDirLen, searchLanguages, nameUniChars, nameLen, resTypes, limit, predicate, version, cheapStr, tmpString, result);
        
        // drd: This unfortunate hack is still necessary because of installer packages and Spotlight importers
        if (CFArrayGetCount(result) == 0 && (0 == version || (2 == version && CFEqual(CFSTR("/Library/Spotlight"), basePath)))) {
            // Try looking directly in the bundle path
            workingLen = savedWorkingLen;
            _CFFindBundleResourcesInResourcesDir(kCFAllocatorSystemDefault, workingUniChars, workingLen, subDirUniChars, subDirLen, searchLanguages, nameUniChars, nameLen, resTypes, limit, predicate, version, cheapStr, tmpString, result);
        }

        CFRelease(cheapStr);
        CFRelease(tmpString);
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, nameUniChars);
    }
    if (resName) CFRelease(resName);
    if (basePath) CFRelease(basePath);
    return result;
}

CF_EXPORT CFURLRef CFBundleCopyResourceURL(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName) {
    if (!bundle)
        return NULL;
    CFURLRef result = NULL;
    CFArrayRef languages = _CFBundleGetLanguageSearchList(bundle), types = NULL, array;
    if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, resourceName, types, 1, NULL, _CFBundleLayoutVersion(bundle));
    if (types) CFRelease(types);
    if (array) {
        if (CFArrayGetCount(array) > 0) result = (CFURLRef)CFRetain(CFArrayGetValueAtIndex(array, 0));
        CFRelease(array);
    }
    return result;
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfType(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName) {
    CFArrayRef languages = _CFBundleGetLanguageSearchList(bundle), types = NULL, array;
    if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
    // MF:!!! Better "limit" than 1,000,000?
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, NULL, types, 1000000, NULL, _CFBundleLayoutVersion(bundle));
    if (types) CFRelease(types);
    
    return array;
}

CF_EXPORT CFURLRef _CFBundleCopyResourceURLForLanguage(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {
    return CFBundleCopyResourceURLForLocalization(bundle, resourceName, resourceType, subDirName, language);
}

CF_EXPORT CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
    CFURLRef result = NULL;
    CFArrayRef languages = NULL, types = NULL, array;

    if (localizationName) languages = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&localizationName, 1, &kCFTypeArrayCallBacks);
    if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, resourceName, types, 1, NULL, _CFBundleLayoutVersion(bundle));
    if (array) {
        if (CFArrayGetCount(array) > 0) result = (CFURLRef)CFRetain(CFArrayGetValueAtIndex(array, 0));
        CFRelease(array);
    }
    if (types) CFRelease(types);
    if (languages) CFRelease(languages);
    return result;
}

CF_EXPORT CFArrayRef _CFBundleCopyResourceURLsOfTypeForLanguage(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {
    return CFBundleCopyResourceURLsOfTypeForLocalization(bundle, resourceType, subDirName, language);
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfTypeForLocalization(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
    CFArrayRef languages = NULL, types = NULL, array;

    if (localizationName) languages = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&localizationName, 1, &kCFTypeArrayCallBacks);
    if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
    // MF:!!! Better "limit" than 1,000,000?
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, NULL, types, 1000000, NULL, _CFBundleLayoutVersion(bundle));
    if (types) CFRelease(types);
    if (languages) CFRelease(languages);
    return array;
}


CF_EXPORT CFStringRef CFBundleCopyLocalizedString(CFBundleRef bundle, CFStringRef key, CFStringRef value, CFStringRef tableName) {
    CFStringRef result = NULL;
    CFDictionaryRef stringTable = NULL;
    static CFSpinLock_t CFBundleLocalizedStringLock = CFSpinLockInit;

    if (!key) return (value ? (CFStringRef)CFRetain(value) : (CFStringRef)CFRetain(CFSTR("")));

    if (!tableName || CFEqual(tableName, CFSTR(""))) tableName = _CFBundleDefaultStringTableName;

    __CFSpinLock(&CFBundleLocalizedStringLock);
    if (__CFBundleGetResourceData(bundle)->_stringTableCache) {
        stringTable = (CFDictionaryRef)CFDictionaryGetValue(__CFBundleGetResourceData(bundle)->_stringTableCache, tableName);
        if (stringTable) CFRetain(stringTable);
    }
    __CFSpinUnlock(&CFBundleLocalizedStringLock);

    if (!stringTable) {
        // Go load the table.
        CFURLRef tableURL = CFBundleCopyResourceURL(bundle, tableName, _CFBundleStringTableType, NULL);
        if (tableURL) {
            CFStringRef nameForSharing = NULL;
            if (!stringTable) {
                CFDataRef tableData = NULL;
                SInt32 errCode;
                CFStringRef errStr;
                if (CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, tableURL, &tableData, NULL, NULL, &errCode)) {
                    stringTable = (CFDictionaryRef)CFPropertyListCreateFromXMLData(CFGetAllocator(bundle), tableData, kCFPropertyListImmutable, &errStr);
                    if (errStr) {
                        CFRelease(errStr);
                        errStr = NULL;
                    }
                    if (stringTable && CFDictionaryGetTypeID() != CFGetTypeID(stringTable)) {
                        CFRelease(stringTable);
                        stringTable = NULL;
                    }
                    CFRelease(tableData);

                }
            }
            if (nameForSharing) CFRelease(nameForSharing);
            if (tableURL) CFRelease(tableURL);
        }
        if (!stringTable) stringTable = CFDictionaryCreate(CFGetAllocator(bundle), NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        if (!CFStringHasSuffix(tableName, CFSTR(".nocache")) || !_CFExecutableLinkedOnOrAfter(CFSystemVersionLeopard)) {
            __CFSpinLock(&CFBundleLocalizedStringLock);
            if (!__CFBundleGetResourceData(bundle)->_stringTableCache) __CFBundleGetResourceData(bundle)->_stringTableCache = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(__CFBundleGetResourceData(bundle)->_stringTableCache, tableName, stringTable);
            __CFSpinUnlock(&CFBundleLocalizedStringLock);
        }
    }

    result = (CFStringRef)CFDictionaryGetValue(stringTable, key);
    if (!result) {
        static int capitalize = -1;
        if (!value) {
            result = (CFStringRef)CFRetain(key);
        } else if (CFEqual(value, CFSTR(""))) {
            result = (CFStringRef)CFRetain(key);
        } else {
            result = (CFStringRef)CFRetain(value);
        }
        if (capitalize != 0) {
            if (capitalize != 0) {
                CFMutableStringRef capitalizedResult = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, result);
                CFLog(__kCFLogBundle, CFSTR("Localizable string \"%@\" not found in strings table \"%@\" of bundle %@."), key, tableName, bundle);
                CFStringUppercase(capitalizedResult, NULL);
                CFRelease(result);
                result = capitalizedResult;
            }
        }
    } else {
        CFRetain(result);
    }
    CFRelease(stringTable);
    return result;
}

CF_EXPORT CFURLRef CFBundleCopyResourceURLInDirectory(CFURLRef bundleURL, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName) {
    CFURLRef result = NULL;
    unsigned char buff[CFMaxPathSize];
    CFURLRef newURL = NULL;

    if (!CFURLGetFileSystemRepresentation(bundleURL, true, buff, CFMaxPathSize)) return NULL;

    newURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, buff, strlen((char *)buff), true);
    if (!newURL) newURL = (CFURLRef)CFRetain(bundleURL);
    if (_CFBundleCouldBeBundle(newURL)) {
        uint8_t version = 0;
        CFArrayRef languages = _CFBundleCopyLanguageSearchListInDirectory(kCFAllocatorSystemDefault, newURL, &version), types = NULL, array;
        if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
        array = _CFFindBundleResources(NULL, newURL, subDirName, languages, resourceName, types, 1, NULL, version);
        if (types) CFRelease(types);
        if (languages) CFRelease(languages);
        if (array) {
            if (CFArrayGetCount(array) > 0) result = (CFURLRef)CFRetain(CFArrayGetValueAtIndex(array, 0));
            CFRelease(array);
        }
    }
    if (newURL) CFRelease(newURL);
    return result;
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfTypeInDirectory(CFURLRef bundleURL, CFStringRef resourceType, CFStringRef subDirName) {
    CFArrayRef array = NULL;
    unsigned char buff[CFMaxPathSize];
    CFURLRef newURL = NULL;

    if (!CFURLGetFileSystemRepresentation(bundleURL, true, buff, CFMaxPathSize)) return NULL;

    newURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, buff, strlen((char *)buff), true);
    if (!newURL) newURL = (CFURLRef)CFRetain(bundleURL);
    if (_CFBundleCouldBeBundle(newURL)) {
        uint8_t version = 0;
        CFArrayRef languages = _CFBundleCopyLanguageSearchListInDirectory(kCFAllocatorSystemDefault, newURL, &version), types = NULL;
        if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
        // MF:!!! Better "limit" than 1,000,000?
        array = _CFFindBundleResources(NULL, newURL, subDirName, languages, NULL, types, 1000000, NULL, version);
        if (types) CFRelease(types);
        if (languages) CFRelease(languages);
    }
    if (newURL) CFRelease(newURL);
    return array;
}

// string, with groups of 6 characters being 1 element in the array of locale abbreviations
const char * __CFBundleLocaleAbbreviationsArray =
    "en_US\0"      "fr_FR\0"      "en_GB\0"      "de_DE\0"      "it_IT\0"      "nl_NL\0"      "nl_BE\0"      "sv_SE\0"
    "es_ES\0"      "da_DK\0"      "pt_PT\0"      "fr_CA\0"      "nb_NO\0"      "he_IL\0"      "ja_JP\0"      "en_AU\0"
    "ar\0\0\0\0"   "fi_FI\0"      "fr_CH\0"      "de_CH\0"      "el_GR\0"      "is_IS\0"      "mt_MT\0"      "el_CY\0"
    "tr_TR\0"      "hr_HR\0"      "nl_NL\0"      "nl_BE\0"      "en_CA\0"      "en_CA\0"      "pt_PT\0"      "nb_NO\0"
    "da_DK\0"      "hi_IN\0"      "ur_PK\0"      "tr_TR\0"      "it_CH\0"      "en\0\0\0\0"   "\0\0\0\0\0\0" "ro_RO\0"
    "grc\0\0\0"    "lt_LT\0"      "pl_PL\0"      "hu_HU\0"      "et_EE\0"      "lv_LV\0"      "se\0\0\0\0"   "fo_FO\0"
    "fa_IR\0"      "ru_RU\0"      "ga_IE\0"      "ko_KR\0"      "zh_CN\0"      "zh_TW\0"      "th_TH\0"      "\0\0\0\0\0\0"
    "cs_CZ\0"      "sk_SK\0"      "\0\0\0\0\0\0" "hu_HU\0"      "bn\0\0\0\0"   "be_BY\0"      "uk_UA\0"      "\0\0\0\0\0\0"
    "el_GR\0"      "sr_CS\0"      "sl_SI\0"      "mk_MK\0"      "hr_HR\0"      "\0\0\0\0\0\0" "de_DE\0"      "pt_BR\0"
    "bg_BG\0"      "ca_ES\0"      "\0\0\0\0\0\0" "gd\0\0\0\0"   "gv\0\0\0\0"   "br\0\0\0\0"   "iu_CA\0"      "cy\0\0\0\0"
    "en_CA\0"      "ga_IE\0"      "en_CA\0"      "dz_BT\0"      "hy_AM\0"      "ka_GE\0"      "es_XL\0"      "es_ES\0"
    "to_TO\0"      "pl_PL\0"      "ca_ES\0"      "fr\0\0\0\0"   "de_AT\0"      "es_XL\0"      "gu_IN\0"      "pa\0\0\0\0"
    "ur_IN\0"      "vi_VN\0"      "fr_BE\0"      "uz_UZ\0"      "en_SG\0"      "nn_NO\0"      "af_ZA\0"      "eo\0\0\0\0"
    "mr_IN\0"      "bo\0\0\0\0"   "ne_NP\0"      "kl\0\0\0\0"   "en_IE\0";

#define NUM_LOCALE_ABBREVIATIONS        109
#define LOCALE_ABBREVIATION_LENGTH      6

static const char * const __CFBundleLanguageNamesArray[] = {
    "English",      "French",       "German",       "Italian",      "Dutch",        "Swedish",      "Spanish",      "Danish",
    "Portuguese",   "Norwegian",    "Hebrew",       "Japanese",     "Arabic",       "Finnish",      "Greek",        "Icelandic",
    "Maltese",      "Turkish",      "Croatian",     "Chinese",      "Urdu",         "Hindi",        "Thai",         "Korean",
    "Lithuanian",   "Polish",       "Hungarian",    "Estonian",     "Latvian",      "Sami",         "Faroese",      "Farsi",
    "Russian",      "Chinese",      "Dutch",        "Irish",        "Albanian",     "Romanian",     "Czech",        "Slovak",
    "Slovenian",    "Yiddish",      "Serbian",      "Macedonian",   "Bulgarian",    "Ukrainian",    "Byelorussian", "Uzbek",
    "Kazakh",       "Azerbaijani",  "Azerbaijani",  "Armenian",     "Georgian",     "Moldavian",    "Kirghiz",      "Tajiki",
    "Turkmen",      "Mongolian",    "Mongolian",    "Pashto",       "Kurdish",      "Kashmiri",     "Sindhi",       "Tibetan",
    "Nepali",       "Sanskrit",     "Marathi",      "Bengali",      "Assamese",     "Gujarati",     "Punjabi",      "Oriya",
    "Malayalam",    "Kannada",      "Tamil",        "Telugu",       "Sinhalese",    "Burmese",      "Khmer",        "Lao",
    "Vietnamese",   "Indonesian",   "Tagalog",      "Malay",        "Malay",        "Amharic",      "Tigrinya",     "Oromo",
    "Somali",       "Swahili",      "Kinyarwanda",  "Rundi",        "Nyanja",       "Malagasy",     "Esperanto",    "",
    "",             "",             "",             "",             "",             "",             "",             "",
    "",             "",             "",             "",             "",             "",             "",             "",
    "",             "",             "",             "",             "",             "",             "",             "",
    "",             "",             "",             "",             "",             "",             "",             "",
    "Welsh",        "Basque",       "Catalan",      "Latin",        "Quechua",      "Guarani",      "Aymara",       "Tatar",
    "Uighur",       "Dzongkha",     "Javanese",     "Sundanese",    "Galician",     "Afrikaans",    "Breton",       "Inuktitut",
    "Scottish",     "Manx",         "Irish",        "Tongan",       "Greek",        "Greenlandic",  "Azerbaijani",  "Nynorsk"
};

#define NUM_LANGUAGE_NAMES      152
#define LANGUAGE_NAME_LENGTH    13

// string, with groups of 3 characters being 1 element in the array of abbreviations
const char * __CFBundleLanguageAbbreviationsArray =
    "en\0"   "fr\0"   "de\0"   "it\0"   "nl\0"   "sv\0"   "es\0"   "da\0"
    "pt\0"   "nb\0"   "he\0"   "ja\0"   "ar\0"   "fi\0"   "el\0"   "is\0"
    "mt\0"   "tr\0"   "hr\0"   "zh\0"   "ur\0"   "hi\0"   "th\0"   "ko\0"
    "lt\0"   "pl\0"   "hu\0"   "et\0"   "lv\0"   "se\0"   "fo\0"   "fa\0"
    "ru\0"   "zh\0"   "nl\0"   "ga\0"   "sq\0"   "ro\0"   "cs\0"   "sk\0"
    "sl\0"   "yi\0"   "sr\0"   "mk\0"   "bg\0"   "uk\0"   "be\0"   "uz\0"
    "kk\0"   "az\0"   "az\0"   "hy\0"   "ka\0"   "mo\0"   "ky\0"   "tg\0"
    "tk\0"   "mn\0"   "mn\0"   "ps\0"   "ku\0"   "ks\0"   "sd\0"   "bo\0"
    "ne\0"   "sa\0"   "mr\0"   "bn\0"   "as\0"   "gu\0"   "pa\0"   "or\0"
    "ml\0"   "kn\0"   "ta\0"   "te\0"   "si\0"   "my\0"   "km\0"   "lo\0"
    "vi\0"   "id\0"   "tl\0"   "ms\0"   "ms\0"   "am\0"   "ti\0"   "om\0"
    "so\0"   "sw\0"   "rw\0"   "rn\0"   "\0\0\0" "mg\0"   "eo\0"   "\0\0\0"
    "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0"
    "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0"
    "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0"
    "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0" "\0\0\0"
    "cy\0"   "eu\0"   "ca\0"   "la\0"   "qu\0"   "gn\0"   "ay\0"   "tt\0"
    "ug\0"   "dz\0"   "jv\0"   "su\0"   "gl\0"   "af\0"   "br\0"   "iu\0"
    "gd\0"   "gv\0"   "ga\0"   "to\0"   "el\0"   "kl\0"   "az\0"   "nn\0";

#define NUM_LANGUAGE_ABBREVIATIONS      152
#define LANGUAGE_ABBREVIATION_LENGTH    3

#if defined(__CONSTANT_CFSTRINGS__)

// These are not necessarily common localizations per se, but localizations for which the full language name is still in common use.
// These are used to provide a fast path for it (other localizations usually use the abbreviation, which is even faster).
static CFStringRef const __CFBundleCommonLanguageNamesArray[] = {CFSTR("English"), CFSTR("French"), CFSTR("German"), CFSTR("Italian"), CFSTR("Dutch"), CFSTR("Spanish"), CFSTR("Japanese")};
static CFStringRef const __CFBundleCommonLanguageAbbreviationsArray[] = {CFSTR("en"), CFSTR("fr"), CFSTR("de"), CFSTR("it"), CFSTR("nl"), CFSTR("es"), CFSTR("ja")};

#define NUM_COMMON_LANGUAGE_NAMES 7

#endif /* __CONSTANT_CFSTRINGS__ */

static const SInt32 __CFBundleScriptCodesArray[] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  1,  4,  0,  6,  0,
     0,  0,  0,  2,  4,  9, 21,  3, 29, 29, 29, 29, 29,  0,  0,  4,
     7, 25,  0,  0,  0,  0, 29, 29,  0,  5,  7,  7,  7,  7,  7,  7,
     7,  7,  4, 24, 23,  7,  7,  7,  7, 27,  7,  4,  4,  4,  4, 26,
     9,  9,  9, 13, 13, 11, 10, 12, 17, 16, 14, 15, 18, 19, 20, 22,
    30,  0,  0,  0,  4, 28, 28, 28,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  7,  4, 26,  0,  0,  0,  0,  0, 28,
     0,  0,  0,  0,  6,  0,  0,  0
};

static const CFStringEncoding __CFBundleStringEncodingsArray[] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  1,  4,  0,  6, 37,
     0, 35, 36,  2,  4,  9, 21,  3, 29, 29, 29, 29, 29,  0, 37, 0x8C,
     7, 25,  0, 39,  0, 38, 29, 29, 36,  5,  7,  7,  7, 0x98,  7,  7,
     7,  7,  4, 24, 23,  7,  7,  7,  7, 27,  7,  4,  4,  4,  4, 26,
     9,  9,  9, 13, 13, 11, 10, 12, 17, 16, 14, 15, 18, 19, 20, 22,
    30,  0,  0,  0,  4, 28, 28, 28,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    39,  0,  0,  0,  0,  0,  0,  7,  4, 26,  0,  0,  0,  0, 39, 0xEC,
    39, 39, 40,  0,  6,  0,  0,  0
};

static SInt32 _CFBundleGetLanguageCodeForLocalization(CFStringRef localizationName) {
    SInt32 result = -1, i;
    char buff[256];
    CFIndex length = CFStringGetLength(localizationName);
    if (length >= LANGUAGE_ABBREVIATION_LENGTH - 1 && length <= 255 && CFStringGetCString(localizationName, buff, 255, kCFStringEncodingASCII)) {
        buff[255] = '\0';
        for (i = 0; -1 == result && i < NUM_LANGUAGE_NAMES; i++) {
            if (0 == strcmp(buff, __CFBundleLanguageNamesArray[i])) result = i;
        }
        if (0 == strcmp(buff, "zh_TW") || 0 == strcmp(buff, "zh-Hant")) result = 19; else if (0 == strcmp(buff, "zh_CN") || 0 == strcmp(buff, "zh-Hans")) result = 33; // hack for mixed-up Chinese language codes
        if (-1 == result && (length == LANGUAGE_ABBREVIATION_LENGTH - 1 || !isalpha(buff[LANGUAGE_ABBREVIATION_LENGTH - 1]))) {
            buff[LANGUAGE_ABBREVIATION_LENGTH - 1] = '\0';
            if ('n' == buff[0] && 'o' == buff[1]) result = 9;  // hack for Norwegian
            for (i = 0; -1 == result && i < NUM_LANGUAGE_ABBREVIATIONS * LANGUAGE_ABBREVIATION_LENGTH; i += LANGUAGE_ABBREVIATION_LENGTH) {
                if (buff[0] == *(__CFBundleLanguageAbbreviationsArray + i + 0) && buff[1] == *(__CFBundleLanguageAbbreviationsArray + i + 1)) result = i / LANGUAGE_ABBREVIATION_LENGTH;
            }
        }
    }
    return result;
}

static CFStringRef _CFBundleCopyLanguageAbbreviationForLanguageCode(SInt32 languageCode) {
    CFStringRef result = NULL;
    if (0 <= languageCode && languageCode < NUM_LANGUAGE_ABBREVIATIONS) {
        const char *languageAbbreviation = __CFBundleLanguageAbbreviationsArray + languageCode * LANGUAGE_ABBREVIATION_LENGTH;
        if (languageAbbreviation && *languageAbbreviation != '\0') result = CFStringCreateWithCStringNoCopy(kCFAllocatorSystemDefault, languageAbbreviation, kCFStringEncodingASCII, kCFAllocatorNull);
    }
    return result;
}

CF_INLINE CFStringRef _CFBundleCopyLanguageNameForLanguageCode(SInt32 languageCode) {
    CFStringRef result = NULL;
    if (0 <= languageCode && languageCode < NUM_LANGUAGE_NAMES) {
        const char *languageName = __CFBundleLanguageNamesArray[languageCode];
        if (languageName && *languageName != '\0') result = CFStringCreateWithCStringNoCopy(kCFAllocatorSystemDefault, languageName, kCFStringEncodingASCII, kCFAllocatorNull);
    }
    return result;
}

CF_INLINE CFStringRef _CFBundleCopyLanguageAbbreviationForLocalization(CFStringRef localizationName) {
    CFStringRef result = NULL;
    SInt32 languageCode = _CFBundleGetLanguageCodeForLocalization(localizationName);
    if (languageCode >= 0) {
        result = _CFBundleCopyLanguageAbbreviationForLanguageCode(languageCode);
    } else {
        CFIndex length = CFStringGetLength(localizationName);
        if (length == LANGUAGE_ABBREVIATION_LENGTH - 1 || (length > LANGUAGE_ABBREVIATION_LENGTH - 1 && CFStringGetCharacterAtIndex(localizationName, LANGUAGE_ABBREVIATION_LENGTH - 1) == '_')) {
            result = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, localizationName, CFRangeMake(0, LANGUAGE_ABBREVIATION_LENGTH - 1));
        }
    }
    return result;
}

CF_INLINE CFStringRef _CFBundleCopyModifiedLocalization(CFStringRef localizationName) {
    CFMutableStringRef result = NULL;
    CFIndex length = CFStringGetLength(localizationName);
    if (length >= 4) {
        UniChar c = CFStringGetCharacterAtIndex(localizationName, 2);
        if ('-' == c || '_' == c) {
            result = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, length, localizationName);
            CFStringReplace(result, CFRangeMake(2, 1), ('-' == c) ? CFSTR("_") : CFSTR("-"));
        }
    }
    return result;
}

CF_INLINE CFStringRef _CFBundleCopyLanguageNameForLocalization(CFStringRef localizationName) {
    CFStringRef result = NULL;
    SInt32 languageCode = _CFBundleGetLanguageCodeForLocalization(localizationName);
    if (languageCode >= 0) {
        result = _CFBundleCopyLanguageNameForLanguageCode(languageCode);
    } else {
        result = (CFStringRef)CFStringCreateCopy(kCFAllocatorSystemDefault, localizationName);
    }
    return result;
}

static SInt32 _CFBundleGetLanguageCodeForRegionCode(SInt32 regionCode) {
    SInt32 result = -1, i;
    if (52 == regionCode) {     // hack for mixed-up Chinese language codes
        result = 33;
    } else if (0 <= regionCode && regionCode < NUM_LOCALE_ABBREVIATIONS) {
        const char *localeAbbreviation = __CFBundleLocaleAbbreviationsArray + regionCode * LOCALE_ABBREVIATION_LENGTH;
        if (localeAbbreviation && *localeAbbreviation != '\0') {
            for (i = 0; -1 == result && i < NUM_LANGUAGE_ABBREVIATIONS * LANGUAGE_ABBREVIATION_LENGTH; i += LANGUAGE_ABBREVIATION_LENGTH) {
                if (localeAbbreviation[0] == *(__CFBundleLanguageAbbreviationsArray + i + 0) && localeAbbreviation[1] == *(__CFBundleLanguageAbbreviationsArray + i + 1)) result = i / LANGUAGE_ABBREVIATION_LENGTH;
            }
        }
    }
    return result;
}

static SInt32 _CFBundleGetRegionCodeForLanguageCode(SInt32 languageCode) {
    SInt32 result = -1, i;
    if (19 == languageCode) {   // hack for mixed-up Chinese language codes
        result = 53;
    } else if (0 <= languageCode && languageCode < NUM_LANGUAGE_ABBREVIATIONS) {
        const char *languageAbbreviation = __CFBundleLanguageAbbreviationsArray + languageCode * LANGUAGE_ABBREVIATION_LENGTH;
        if (languageAbbreviation && *languageAbbreviation != '\0') {
            for (i = 0; -1 == result && i < NUM_LOCALE_ABBREVIATIONS * LOCALE_ABBREVIATION_LENGTH; i += LOCALE_ABBREVIATION_LENGTH) {
                if (*(__CFBundleLocaleAbbreviationsArray + i + 0) == languageAbbreviation[0] && *(__CFBundleLocaleAbbreviationsArray + i + 1) == languageAbbreviation[1]) result = i / LOCALE_ABBREVIATION_LENGTH;
            }
        }
    }
    if (25 == result) result = 68;
    if (28 == result) result = 82;
    return result;
}

static SInt32 _CFBundleGetRegionCodeForLocalization(CFStringRef localizationName) {
    SInt32 result = -1, i;
    char buff[LOCALE_ABBREVIATION_LENGTH];
    CFIndex length = CFStringGetLength(localizationName);
    if (length >= LANGUAGE_ABBREVIATION_LENGTH - 1 && length <= LOCALE_ABBREVIATION_LENGTH - 1 && CFStringGetCString(localizationName, buff, LOCALE_ABBREVIATION_LENGTH, kCFStringEncodingASCII)) {
        buff[LOCALE_ABBREVIATION_LENGTH - 1] = '\0';
        for (i = 0; -1 == result && i < NUM_LOCALE_ABBREVIATIONS * LOCALE_ABBREVIATION_LENGTH; i += LOCALE_ABBREVIATION_LENGTH) {
            if (0 == strcmp(buff, __CFBundleLocaleAbbreviationsArray + i)) result = i / LOCALE_ABBREVIATION_LENGTH;
        }
    }
    if (25 == result) result = 68;
    if (28 == result) result = 82;
    if (37 == result) result = 0;
    if (-1 == result) {
        SInt32 languageCode = _CFBundleGetLanguageCodeForLocalization(localizationName);
        result = _CFBundleGetRegionCodeForLanguageCode(languageCode);
    }
    return result;
}

static CFStringRef _CFBundleCopyLocaleAbbreviationForRegionCode(SInt32 regionCode) {
    CFStringRef result = NULL;
    if (0 <= regionCode && regionCode < NUM_LOCALE_ABBREVIATIONS) {
        const char *localeAbbreviation = __CFBundleLocaleAbbreviationsArray + regionCode * LOCALE_ABBREVIATION_LENGTH;
        if (localeAbbreviation && *localeAbbreviation != '\0') {
            result = CFStringCreateWithCStringNoCopy(kCFAllocatorSystemDefault, localeAbbreviation, kCFStringEncodingASCII, kCFAllocatorNull);
        }
    }
    return result;
}

Boolean CFBundleGetLocalizationInfoForLocalization(CFStringRef localizationName, SInt32 *languageCode, SInt32 *regionCode, SInt32 *scriptCode, CFStringEncoding *stringEncoding) {
    Boolean retval = false;
    SInt32 language = -1, region = -1, script = 0;
    CFStringEncoding encoding = kCFStringEncodingMacRoman;
    if (!localizationName) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        CFArrayRef languages = NULL;
        if (mainBundle) {
            languages = _CFBundleGetLanguageSearchList(mainBundle);
            if (languages) CFRetain(languages);
        }
        if (!languages) languages = _CFBundleCopyUserLanguages(false);
        if (languages && CFArrayGetCount(languages) > 0) localizationName = (CFStringRef)CFArrayGetValueAtIndex(languages, 0);
    }
    if (localizationName) {
        LangCode langCode = -1;
        RegionCode regCode = -1;
        ScriptCode scrCode = 0;
        CFStringEncoding enc = kCFStringEncodingMacRoman;
        retval = CFLocaleGetLanguageRegionEncodingForLocaleIdentifier(localizationName, &langCode, &regCode, &scrCode, &enc);
        if (retval) {
            language = langCode;
            region = regCode;
            script = scrCode;
            encoding = enc;
        }
    }  
    if (!retval) {
        if (localizationName) {
            language = _CFBundleGetLanguageCodeForLocalization(localizationName);
            region = _CFBundleGetRegionCodeForLocalization(localizationName);
        } else {
            _CFBundleGetLanguageAndRegionCodes(&language, &region);
        }
        if ((language < 0 || language > (int)(sizeof(__CFBundleScriptCodesArray)/sizeof(SInt32))) && region != -1) language = _CFBundleGetLanguageCodeForRegionCode(region);
        if (region == -1 && language != -1) region = _CFBundleGetRegionCodeForLanguageCode(language);
        if (language >= 0 && language < (int)(sizeof(__CFBundleScriptCodesArray)/sizeof(SInt32))) {
            script = __CFBundleScriptCodesArray[language];
        }
        if (language >= 0 && language < (int)(sizeof(__CFBundleStringEncodingsArray)/sizeof(CFStringEncoding))) {
            encoding = __CFBundleStringEncodingsArray[language];
        }
        retval = (language != -1 || region != -1);
    }
    if (languageCode) *languageCode = language;
    if (regionCode) *regionCode = region;
    if (scriptCode) *scriptCode = script;
    if (stringEncoding) *stringEncoding = encoding;
    return retval;
}

CFStringRef CFBundleCopyLocalizationForLocalizationInfo(SInt32 languageCode, SInt32 regionCode, SInt32 scriptCode, CFStringEncoding stringEncoding) {
    CFStringRef localizationName = NULL;
    if (!localizationName) localizationName = _CFBundleCopyLocaleAbbreviationForRegionCode(regionCode);
#if DEPLOYMENT_TARGET_MACOSX
    if (!localizationName && 0 <= languageCode && languageCode < SHRT_MAX) localizationName = CFLocaleCreateCanonicalLocaleIdentifierFromScriptManagerCodes(kCFAllocatorSystemDefault, (LangCode)languageCode, (RegionCode)-1);
#endif
    if (!localizationName) localizationName = _CFBundleCopyLanguageAbbreviationForLanguageCode(languageCode);
    if (!localizationName) {
        SInt32 language = -1, scriptLanguage = -1, encodingLanguage = -1;
        unsigned int i;
        for (i = 0; language == -1 && i < (sizeof(__CFBundleScriptCodesArray)/sizeof(SInt32)); i++) {
            if (__CFBundleScriptCodesArray[i] == scriptCode && __CFBundleStringEncodingsArray[i] == stringEncoding) language = i;
        }
        for (i = 0; scriptLanguage == -1 && i < (sizeof(__CFBundleScriptCodesArray)/sizeof(SInt32)); i++) {
            if (__CFBundleScriptCodesArray[i] == scriptCode) scriptLanguage = i;
        }
        for (i = 0; encodingLanguage == -1 && i < (sizeof(__CFBundleStringEncodingsArray)/sizeof(CFStringEncoding)); i++) {
            if (__CFBundleStringEncodingsArray[i] == stringEncoding) encodingLanguage = i;
        }
        localizationName = _CFBundleCopyLanguageAbbreviationForLanguageCode(language);
        if (!localizationName) localizationName = _CFBundleCopyLanguageAbbreviationForLanguageCode(encodingLanguage);
        if (!localizationName) localizationName = _CFBundleCopyLanguageAbbreviationForLanguageCode(scriptLanguage);
    }
    return localizationName;
}

extern void *__CFAppleLanguages;

#if DEPLOYMENT_TARGET_WINDOWS

extern CFStringRef copyLocaleLanguageName(void);
extern CFStringRef copyLocaleCountryName(void);

static CFArrayRef copyWindowsLanguagePrefsArray() {
    CFArrayRef result;
    CFStringRef locales[4];
    CFStringRef languageName = copyLocaleLanguageName(), countryName = copyLocaleCountryName();
    if (!languageName) languageName = CFSTR("en");
    if (!countryName) countryName = CFSTR("");
    CFIndex i, localesCount = 0;
    if (CFStringGetLength(countryName) > 0) locales[localesCount++] = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%@_%@"), languageName, countryName);
    if (CFStringGetLength(languageName) != 0) {
        // special-case for zh since we don't have a generic zh localization
        if (CFStringCompare(languageName, CFSTR("zh"), kCFCompareCaseInsensitive) != 0) {
            locales[localesCount++] = CFStringCreateCopy(kCFAllocatorSystemDefault, languageName);//languageName;
        } else {
            CFStringRef moreSpecificLanguageName;

            // See http://intrigue-build.apple.com/changeset/14948 for the details on the change.  Copied below is the snippet of the code change.
            // According to http://www.microsoft.com/globaldev/reference/win2k/setup/lcid.mspx, the locales that use 
            // 126          // simplified chinese are CN (PRC) and SG (Singapore).  The rest use traditional chinese. 
            // 127          languageName = (countryName == TEXT("CN") || countryName == TEXT("SG")) ? TEXT("zh_CN") : TEXT("zh_TW"); 

            // Compare for CN or SG
            if (CFStringCompare(countryName, CFSTR("CN"), kCFCompareCaseInsensitive) == 0 || CFStringCompare(countryName, CFSTR("SG"), kCFCompareCaseInsensitive) == 0) {
                moreSpecificLanguageName = CFSTR("zh_CN");
            } else {
                moreSpecificLanguageName = CFSTR("zh_TW");
            }
            locales[localesCount++] = CFStringCreateCopy(kCFAllocatorSystemDefault, moreSpecificLanguageName);
        }
        // Don't need this now
        if (languageName) CFRelease(languageName);
        if (countryName) CFRelease(countryName);
    }
    if (localesCount == 0) locales[localesCount++] = CFStringCreateCopy(kCFAllocatorSystemDefault, CFSTR("en"));
    result = CFArrayCreate(kCFAllocatorDefault, (const void **)locales, localesCount, &kCFTypeArrayCallBacks);
    for (i = 0; i < localesCount; i++) CFRelease(locales[i]);
    return result;
}

#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
    
static CFArrayRef _CFBundleUserLanguages = NULL;

__private_extern__ CFArrayRef _CFBundleCopyUserLanguages(Boolean useBackstops) {
    CFArrayRef result = NULL;
    static Boolean didit = false;
    CFArrayRef preferencesArray = NULL;
    // This is a temporary solution, until the argument domain is moved down into CFPreferences
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (!didit) {
        if (__CFAppleLanguages) {
            CFDataRef data;
            CFIndex length = strlen((const char *)__CFAppleLanguages);
            if (length > 0) {
                data = CFDataCreateWithBytesNoCopy(kCFAllocatorSystemDefault, (const UInt8 *)__CFAppleLanguages, length, kCFAllocatorNull);
                if (data) {
                    _CFBundleUserLanguages = (CFArrayRef)CFPropertyListCreateFromXMLData(kCFAllocatorSystemDefault, data, kCFPropertyListImmutable, NULL);
                    CFRelease(data);
                }
            }
        }
        if (!_CFBundleUserLanguages && preferencesArray) _CFBundleUserLanguages = (CFArrayRef)CFRetain(preferencesArray);
        Boolean useEnglishAsBackstop = true;
        // could perhaps read out of LANG environment variable
        if (useEnglishAsBackstop && !_CFBundleUserLanguages) {
            CFStringRef english = CFSTR("en");
            _CFBundleUserLanguages = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&english, 1, &kCFTypeArrayCallBacks);
        }
        if (_CFBundleUserLanguages && CFGetTypeID(_CFBundleUserLanguages) != CFArrayGetTypeID()) {
            CFRelease(_CFBundleUserLanguages);
            _CFBundleUserLanguages = NULL;
        }
        didit = true;
    }
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
    if (preferencesArray) CFRelease(preferencesArray);
    if (!result && _CFBundleUserLanguages) result = (CFArrayRef)CFRetain(_CFBundleUserLanguages);
    return result;
}

CF_EXPORT void _CFBundleGetLanguageAndRegionCodes(SInt32 *languageCode, SInt32 *regionCode) {
    // an attempt to answer the question, "what language are we running in?"
    // note that the question cannot be answered fully since it may depend on the bundle
    SInt32 language = -1, region = -1;
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFArrayRef languages = NULL;
    if (mainBundle) {
        languages = _CFBundleGetLanguageSearchList(mainBundle);
        if (languages) CFRetain(languages);
    }
    if (!languages) languages = _CFBundleCopyUserLanguages(false);
    if (languages && CFArrayGetCount(languages) > 0) {
        CFStringRef localizationName = (CFStringRef)CFArrayGetValueAtIndex(languages, 0);
        Boolean retval = false;
        LangCode langCode = -1;
        RegionCode regCode = -1;
        retval = CFLocaleGetLanguageRegionEncodingForLocaleIdentifier(localizationName, &langCode, &regCode, NULL, NULL);
        if (retval) {
            language = langCode;
            region = regCode;
        }
        if (!retval) {
            language = _CFBundleGetLanguageCodeForLocalization(localizationName);
            region = _CFBundleGetRegionCodeForLocalization(localizationName);
        }
    } else {
        language = 0;
        region = 0;
    }
    if (language == -1 && region != -1) language = _CFBundleGetLanguageCodeForRegionCode(region);
    if (region == -1 && language != -1) region = _CFBundleGetRegionCodeForLanguageCode(language);
    if (languages) CFRelease(languages);
    if (languageCode) *languageCode = language;
    if (regionCode) *regionCode = region;
}


static Boolean _CFBundleTryOnePreferredLprojNameInDirectory(CFAllocatorRef alloc, UniChar *pathUniChars, CFIndex pathLen, uint8_t version, CFDictionaryRef infoDict, CFStringRef curLangStr, CFMutableArrayRef lprojNames, Boolean fallBackToLanguage) {
    CFIndex curLangLen = CFStringGetLength(curLangStr), savedPathLen;
    UniChar curLangUniChars[255];
    CFStringRef altLangStr = NULL, modifiedLangStr = NULL, languageAbbreviation = NULL, languageName = NULL, canonicalLanguageIdentifier = NULL, canonicalLanguageAbbreviation = NULL;
    CFMutableDictionaryRef canonicalLanguageIdentifiers = NULL, predefinedCanonicalLanguageIdentifiers = NULL;
    Boolean foundOne = false, specifiesScript = false;
    CFArrayRef predefinedLocalizations = NULL;
    CFRange predefinedLocalizationsRange;
    CFMutableStringRef cheapStr, tmpString;
#if READ_DIRECTORIES
    CFArrayRef contents;
    CFRange contentsRange;
#else /* READ_DIRECTORIES */
    Boolean isDir = false;
#endif /* READ_DIRECTORIES */

    // both of these used for temp string operations, for slightly
    // different purposes, where each type is appropriate
    cheapStr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    _CFStrSetDesiredCapacity(cheapStr, CFMaxPathSize);
    tmpString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);    
    
#if READ_DIRECTORIES
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    contents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleAllContents);
    contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
#endif /* READ_DIRECTORIES */
    
    if (infoDict) {
        predefinedLocalizations = (CFArrayRef)CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
        if (predefinedLocalizations && CFGetTypeID(predefinedLocalizations) != CFArrayGetTypeID()) {
            predefinedLocalizations = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleLocalizationsKey);
        }
    }
    predefinedLocalizationsRange = CFRangeMake(0, predefinedLocalizations ? CFArrayGetCount(predefinedLocalizations) : 0);
    
    if (curLangLen > 255) curLangLen = 255;
    CFStringGetCharacters(curLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
    savedPathLen = pathLen;
    if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
#if READ_DIRECTORIES
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, curLangStr)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
#else /* READ_DIRECTORIES */
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, curLangStr)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif /* READ_DIRECTORIES */
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), curLangStr)) CFArrayAppendValue(lprojNames, curLangStr);
            foundOne = true;
            if (CFStringGetLength(curLangStr) <= 2) {
                CFRelease(cheapStr);
                CFRelease(tmpString);
#if READ_DIRECTORIES
                CFRelease(contents);
#endif /* READ_DIRECTORIES */
                return foundOne;
            }
        }
    }
#if defined(__CONSTANT_CFSTRINGS__)
    if (!altLangStr) {
        CFIndex idx;
        for (idx = 0; !altLangStr && idx < NUM_COMMON_LANGUAGE_NAMES; idx++) {
            if (CFEqual(curLangStr, __CFBundleCommonLanguageAbbreviationsArray[idx])) altLangStr = __CFBundleCommonLanguageNamesArray[idx];
            else if (CFEqual(curLangStr, __CFBundleCommonLanguageNamesArray[idx])) altLangStr = __CFBundleCommonLanguageAbbreviationsArray[idx];
        }
    }
#endif /* __CONSTANT_CFSTRINGS__ */
    if (foundOne && altLangStr) {
        CFRelease(cheapStr);
        CFRelease(tmpString);
#if READ_DIRECTORIES
        CFRelease(contents);
#endif /* READ_DIRECTORIES */
        return foundOne;
    }
    if (altLangStr) {
        curLangLen = CFStringGetLength(altLangStr);
        if (curLangLen > 255) curLangLen = 255;
        CFStringGetCharacters(altLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
#if READ_DIRECTORIES
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, altLangStr)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
#else /* READ_DIRECTORIES */
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, altLangStr)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif /* READ_DIRECTORIES */
                if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), altLangStr)) CFArrayAppendValue(lprojNames, altLangStr);
                foundOne = true;
                CFRelease(cheapStr);
                CFRelease(tmpString);
#if READ_DIRECTORIES
                CFRelease(contents);
#endif /* READ_DIRECTORIES */
                return foundOne;
            }
        }
    }
#if READ_DIRECTORIES
    if (!foundOne && (!predefinedLocalizations || CFArrayGetCount(predefinedLocalizations) == 0)) {
        Boolean hasLocalizations = false;
        CFIndex idx;
        for (idx = 0; !hasLocalizations && idx < contentsRange.length; idx++) {
            CFStringRef name = CFArrayGetValueAtIndex(contents, idx);
            if (CFStringHasSuffix(name, _CFBundleLprojExtensionWithDot)) hasLocalizations = true;
        }
        if (!hasLocalizations) {
            CFRelease(cheapStr);
            CFRelease(tmpString);
            CFRelease(contents);
            return foundOne;
        }
    }
#endif /* READ_DIRECTORIES */
    if (!altLangStr && (modifiedLangStr = _CFBundleCopyModifiedLocalization(curLangStr))) {
        curLangLen = CFStringGetLength(modifiedLangStr);
        if (curLangLen > 255) curLangLen = 255;
        CFStringGetCharacters(modifiedLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
#if READ_DIRECTORIES
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, modifiedLangStr)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
#else /* READ_DIRECTORIES */
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, modifiedLangStr)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif /* READ_DIRECTORIES */
                if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), modifiedLangStr)) CFArrayAppendValue(lprojNames, modifiedLangStr);
                foundOne = true;
            }
        }
    }
    if (!specifiesScript && (foundOne || fallBackToLanguage) && !altLangStr && (languageAbbreviation = _CFBundleCopyLanguageAbbreviationForLocalization(curLangStr)) && !CFEqual(curLangStr, languageAbbreviation)) {
        curLangLen = CFStringGetLength(languageAbbreviation);
        if (curLangLen > 255) curLangLen = 255;
        CFStringGetCharacters(languageAbbreviation, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
#if READ_DIRECTORIES
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageAbbreviation)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
#else /* READ_DIRECTORIES */
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageAbbreviation)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif /* READ_DIRECTORIES */
                if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), languageAbbreviation)) CFArrayAppendValue(lprojNames, languageAbbreviation);
                foundOne = true;
            }
        }
    }
    if (!specifiesScript && (foundOne || fallBackToLanguage) && !altLangStr && (languageName = _CFBundleCopyLanguageNameForLocalization(curLangStr)) && !CFEqual(curLangStr, languageName)) {
        curLangLen = CFStringGetLength(languageName);
        if (curLangLen > 255) curLangLen = 255;
        CFStringGetCharacters(languageName, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
#if READ_DIRECTORIES
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageName)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
#else /* READ_DIRECTORIES */
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageName)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif /* READ_DIRECTORIES */
                if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), languageName)) CFArrayAppendValue(lprojNames, languageName);
                foundOne = true;
            }
        }
    }
    if (modifiedLangStr) CFRelease(modifiedLangStr);
    if (languageAbbreviation) CFRelease(languageAbbreviation);
    if (languageName) CFRelease(languageName);
    if (canonicalLanguageIdentifier) CFRelease(canonicalLanguageIdentifier);
    if (canonicalLanguageIdentifiers) CFRelease(canonicalLanguageIdentifiers);
    if (predefinedCanonicalLanguageIdentifiers) CFRelease(predefinedCanonicalLanguageIdentifiers);
    if (canonicalLanguageAbbreviation) CFRelease(canonicalLanguageAbbreviation);
    CFRelease(cheapStr);
    CFRelease(tmpString);
#if READ_DIRECTORIES
    CFRelease(contents);
#endif /* READ_DIRECTORIES */
    return foundOne;
}

static Boolean CFBundleAllowMixedLocalizations(void) {
    static Boolean allowMixed = false, examinedMain = false;
    if (!examinedMain) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        CFDictionaryRef infoDict = mainBundle ? CFBundleGetInfoDictionary(mainBundle) : NULL;
        CFTypeRef allowMixedValue = infoDict ? CFDictionaryGetValue(infoDict, _kCFBundleAllowMixedLocalizationsKey) : NULL;
        if (allowMixedValue) {
            CFTypeID typeID = CFGetTypeID(allowMixedValue);
            if (typeID == CFBooleanGetTypeID()) {
                allowMixed = CFBooleanGetValue((CFBooleanRef)allowMixedValue);
            } else if (typeID == CFStringGetTypeID()) {
                allowMixed = (CFStringCompare((CFStringRef)allowMixedValue, CFSTR("true"), kCFCompareCaseInsensitive) == kCFCompareEqualTo || CFStringCompare((CFStringRef)allowMixedValue, CFSTR("YES"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
            } else if (typeID == CFNumberGetTypeID()) {
                SInt32 val = 0;
                if (CFNumberGetValue((CFNumberRef)allowMixedValue, kCFNumberSInt32Type, &val)) allowMixed = (val != 0);
            }
        }
        examinedMain = true;
    }
    return allowMixed;
}

static Boolean _CFBundleLocalizationsHaveCommonPrefix(CFStringRef loc1, CFStringRef loc2) {
    Boolean result = false;
    CFIndex length1 = CFStringGetLength(loc1), length2 = CFStringGetLength(loc2), idx;
    if (length1 > 3 && length2 > 3) {
        for (idx = 0; idx < length1 && idx < length2; idx++) {
            UniChar c1 = CFStringGetCharacterAtIndex(loc1, idx), c2 = CFStringGetCharacterAtIndex(loc2, idx);
            if (idx >= 2 && (c1 == '-' || c1 == '_') && (c2 == '-' || c2 == '_')) {
                result = true;
                break;
            } else if (c1 != c2) {
                break;
            }
        }
    }
    return result;
}

__private_extern__ void _CFBundleAddPreferredLprojNamesInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, uint8_t version, CFDictionaryRef infoDict, CFMutableArrayRef lprojNames, CFStringRef devLang) {
    // This function will add zero, one or two elements to the lprojNames array.
    // It examines the users preferred language list and the lproj directories inside the bundle directory.  It picks the lproj directory that is highest on the users list.
    // The users list can contain region names (like "en_US" for US English).  In this case, if the region lproj exists, it will be added, and, if the region's associated language lproj exists that will be added.
    CFURLRef resourcesURL = _CFBundleCopyResourcesDirectoryURLInDirectory(bundleURL, version);
    CFURLRef absoluteURL;
    CFIndex idx, startIdx;
    CFIndex count;
    CFStringRef resourcesPath;
    UniChar pathUniChars[CFMaxPathSize];
    CFIndex pathLen;
    CFStringRef curLangStr, nextLangStr;
    Boolean foundOne = false;
    CFArrayRef userLanguages;
    
    // Init the one-time-only unichar buffers.
    _CFEnsureStaticBuffersInited();

    // Get the path to the resources and extract into a buffer.
    absoluteURL = CFURLCopyAbsoluteURL(resourcesURL);
    resourcesPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFRelease(absoluteURL);
    pathLen = CFStringGetLength(resourcesPath);
    if (pathLen > CFMaxPathSize) pathLen = CFMaxPathSize;
    CFStringGetCharacters(resourcesPath, CFRangeMake(0, pathLen), pathUniChars);
    CFRelease(resourcesURL);
    CFRelease(resourcesPath);

    // First check the main bundle.
    if (!CFBundleAllowMixedLocalizations()) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            CFURLRef mainBundleURL = CFBundleCopyBundleURL(mainBundle);
            if (mainBundleURL) {
                if (!CFEqual(bundleURL, mainBundleURL)) {
                    // If there is a main bundle, and it isn't this one, try to use the language it prefers.
                    CFArrayRef mainBundleLangs = _CFBundleGetLanguageSearchList(mainBundle);
                    if (mainBundleLangs && (CFArrayGetCount(mainBundleLangs) > 0)) {
                        curLangStr = (CFStringRef)CFArrayGetValueAtIndex(mainBundleLangs, 0);
                        foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames, true);
                    }
                }
                CFRelease(mainBundleURL);
            }
        }
    }

    if (!foundOne) {
        // If we didn't find the main bundle's preferred language, look at the users' prefs again and find the best one.
        userLanguages = _CFBundleCopyUserLanguages(true);
        count = (userLanguages ? CFArrayGetCount(userLanguages) : 0);
        for (idx = 0, startIdx = -1; !foundOne && idx < count; idx++) {
            curLangStr = (CFStringRef)CFArrayGetValueAtIndex(userLanguages, idx);
            nextLangStr = (idx + 1 < count) ? (CFStringRef)CFArrayGetValueAtIndex(userLanguages, idx + 1) : NULL;
            if (nextLangStr && _CFBundleLocalizationsHaveCommonPrefix(curLangStr, nextLangStr)) {
                foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames, false);
                if (startIdx < 0) startIdx = idx;
            } else if (startIdx >= 0 && startIdx <= idx) {
                foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames, false);
                for (; !foundOne && startIdx <= idx; startIdx++) {
                    curLangStr = (CFStringRef)CFArrayGetValueAtIndex(userLanguages, startIdx);
                    foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames, true);
                }
                startIdx = -1;
            } else {
                foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames, true);
                startIdx = -1;
            }
        }
        // use development region and U.S. English as backstops
        if (!foundOne && devLang) foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, devLang, lprojNames, true);
        if (!foundOne) foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(kCFAllocatorSystemDefault, pathUniChars, pathLen, version, infoDict, CFSTR("en_US"), lprojNames, true);
        if (userLanguages) CFRelease(userLanguages);
    }
}

static Boolean _CFBundleTryOnePreferredLprojNameInArray(CFArrayRef array, CFStringRef curLangStr, CFMutableArrayRef lprojNames, Boolean fallBackToLanguage) {
    Boolean foundOne = false, specifiesScript = false;
    CFRange range = CFRangeMake(0, CFArrayGetCount(array));
    CFStringRef altLangStr = NULL, modifiedLangStr = NULL, languageAbbreviation = NULL, languageName = NULL, canonicalLanguageIdentifier = NULL, canonicalLanguageAbbreviation = NULL;
    CFMutableDictionaryRef canonicalLanguageIdentifiers = NULL;

    if (range.length == 0) return foundOne;
    if (CFArrayContainsValue(array, range, curLangStr)) {
        if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), curLangStr)) CFArrayAppendValue(lprojNames, curLangStr);
        foundOne = true;
        if (range.length == 1 || CFStringGetLength(curLangStr) <= 2) return foundOne;
    }
    if (range.length == 1 && CFArrayContainsValue(array, range, CFSTR("default"))) return foundOne;
#if defined(__CONSTANT_CFSTRINGS__)
    if (!altLangStr) {
        CFIndex idx;
        for (idx = 0; !altLangStr && idx < NUM_COMMON_LANGUAGE_NAMES; idx++) {
            if (CFEqual(curLangStr, __CFBundleCommonLanguageAbbreviationsArray[idx])) altLangStr = __CFBundleCommonLanguageNamesArray[idx];
            else if (CFEqual(curLangStr, __CFBundleCommonLanguageNamesArray[idx])) altLangStr = __CFBundleCommonLanguageAbbreviationsArray[idx];
        }
    }
#endif /* __CONSTANT_CFSTRINGS__ */
    if (foundOne && altLangStr) return foundOne;
    if (altLangStr && CFArrayContainsValue(array, range, altLangStr)) {
        if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), altLangStr)) CFArrayAppendValue(lprojNames, altLangStr);
        foundOne = true;
        return foundOne;
    }
    if (!altLangStr && (modifiedLangStr = _CFBundleCopyModifiedLocalization(curLangStr))) {
        if (CFArrayContainsValue(array, range, modifiedLangStr)) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), modifiedLangStr)) CFArrayAppendValue(lprojNames, modifiedLangStr);
            foundOne = true;
        }
    }
    if (!specifiesScript && (foundOne || fallBackToLanguage) && !altLangStr && (languageAbbreviation = _CFBundleCopyLanguageAbbreviationForLocalization(curLangStr)) && !CFEqual(curLangStr, languageAbbreviation)) {
        if (CFArrayContainsValue(array, range, languageAbbreviation)) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), languageAbbreviation)) CFArrayAppendValue(lprojNames, languageAbbreviation);
            foundOne = true;
        }
    }
    if (!specifiesScript && (foundOne || fallBackToLanguage) && !altLangStr && (languageName = _CFBundleCopyLanguageNameForLocalization(curLangStr)) && !CFEqual(curLangStr, languageName)) {
        if (CFArrayContainsValue(array, range, languageName)) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), languageName)) CFArrayAppendValue(lprojNames, languageName);
            foundOne = true;
        }
    }
    if (modifiedLangStr) CFRelease(modifiedLangStr);
    if (languageAbbreviation) CFRelease(languageAbbreviation);
    if (languageName) CFRelease(languageName);
    if (canonicalLanguageIdentifier) CFRelease(canonicalLanguageIdentifier);
    if (canonicalLanguageIdentifiers) CFRelease(canonicalLanguageIdentifiers);
    if (canonicalLanguageAbbreviation) CFRelease(canonicalLanguageAbbreviation);
    return foundOne;
}

static CFArrayRef _CFBundleCopyLocalizationsForPreferences(CFArrayRef locArray, CFArrayRef prefArray, Boolean considerMain) {
    CFMutableArrayRef lprojNames = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    Boolean foundOne = false, releasePrefArray = false;
    CFIndex idx, count, startIdx;
    
    if (considerMain && !CFBundleAllowMixedLocalizations()) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            // If there is a main bundle, try to use the language it prefers.
            CFArrayRef mainBundleLangs = _CFBundleGetLanguageSearchList(mainBundle);
            if (mainBundleLangs && (CFArrayGetCount(mainBundleLangs) > 0)) foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, (CFStringRef)CFArrayGetValueAtIndex(mainBundleLangs, 0), lprojNames, true);
        }
    }
    
    if (!foundOne) {
        CFStringRef curLangStr, nextLangStr;
        if (!prefArray) {
            prefArray = _CFBundleCopyUserLanguages(true);
            if (prefArray) releasePrefArray = true;
        }
        count = (prefArray ? CFArrayGetCount(prefArray) : 0);
        for (idx = 0, startIdx = -1; !foundOne && idx < count; idx++) {
            curLangStr = (CFStringRef)CFArrayGetValueAtIndex(prefArray, idx);
            nextLangStr = (idx + 1 < count) ? (CFStringRef)CFArrayGetValueAtIndex(prefArray, idx + 1) : NULL;
            if (nextLangStr && _CFBundleLocalizationsHaveCommonPrefix(curLangStr, nextLangStr)) {
                foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, curLangStr, lprojNames, false);
                if (startIdx < 0) startIdx = idx;
            } else if (startIdx >= 0 && startIdx <= idx) {
                foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, curLangStr, lprojNames, false);
                for (; !foundOne && startIdx <= idx; startIdx++) {
                    curLangStr = (CFStringRef)CFArrayGetValueAtIndex(prefArray, startIdx);
                    foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, curLangStr, lprojNames, true);
                }
                startIdx = -1;
            } else {
                foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, curLangStr, lprojNames, true);
                startIdx = -1;
            }
        }
        // use U.S. English as backstop
        if (!foundOne) foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, CFSTR("en_US"), lprojNames, true);
        // use random entry as backstop
        if (!foundOne && CFArrayGetCount(locArray) > 0) foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, (CFStringRef)CFArrayGetValueAtIndex(locArray, 0), lprojNames, true);
    }
    if (CFArrayGetCount(lprojNames) == 0) {
        // Total backstop behavior to avoid having an empty array. 
        CFArrayAppendValue(lprojNames, CFSTR("en"));
    }
    if (releasePrefArray) {
        CFRelease(prefArray);
    }
    return lprojNames;
}

CF_EXPORT CFArrayRef CFBundleCopyLocalizationsForPreferences(CFArrayRef locArray, CFArrayRef prefArray) {
    return _CFBundleCopyLocalizationsForPreferences(locArray, prefArray, false);
}

CF_EXPORT CFArrayRef CFBundleCopyPreferredLocalizationsFromArray(CFArrayRef locArray) {
    return _CFBundleCopyLocalizationsForPreferences(locArray, NULL, true);
}

__private_extern__ CFArrayRef _CFBundleCopyLanguageSearchListInDirectory(CFAllocatorRef alloc, CFURLRef url, uint8_t *version) {
    CFMutableArrayRef langs = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    uint8_t localVersion = 0;
    CFDictionaryRef infoDict = _CFBundleCopyInfoDictionaryInDirectory(kCFAllocatorSystemDefaultGCRefZero, url, &localVersion);
    CFStringRef devLang = NULL;
    if (infoDict) devLang = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
    if (devLang && (CFGetTypeID(devLang) != CFStringGetTypeID() || CFStringGetLength(devLang) == 0)) devLang = NULL;

    _CFBundleAddPreferredLprojNamesInDirectory(alloc, url, localVersion, infoDict, langs, devLang);
    
    if (devLang && CFArrayGetFirstIndexOfValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), devLang) < 0) CFArrayAppendValue(langs, devLang);

    // Total backstop behavior to avoid having an empty array. 
    if (CFArrayGetCount(langs) == 0) CFArrayAppendValue(langs, CFSTR("en"));
    
    if (infoDict && !_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(infoDict);
    if (version) *version = localVersion;
    return langs;
}

CF_EXPORT Boolean _CFBundleURLLooksLikeBundle(CFURLRef url) {
    Boolean result = false;
    CFBundleRef bundle = _CFBundleCreateIfLooksLikeBundle(kCFAllocatorSystemDefault, url);
    if (bundle) {
        result = true;
        CFRelease(bundle);
    }
    return result;
}

// Note that subDirName is expected to be the string for a URL
CF_INLINE Boolean _CFBundleURLHasSubDir(CFURLRef url, CFStringRef subDirName) {
    Boolean isDir = false, result = false;
    CFURLRef dirURL = CFURLCreateWithString(kCFAllocatorSystemDefault, subDirName, url);
    if (dirURL) {
        if (_CFIsResourceAtURL(dirURL, &isDir) && isDir) result = true;
        CFRelease(dirURL);
    }
    return result;
}

__private_extern__ Boolean _CFBundleURLLooksLikeBundleVersion(CFURLRef url, uint8_t *version) {
    // check for existence of "Resources" or "Contents" or "Support Files"
    // but check for the most likely one first
    // version 0:  old-style "Resources" bundles
    // version 1:  obsolete "Support Files" bundles
    // version 2:  modern "Contents" bundles
    // version 3:  none of the above (see below)
    // version 4:  not a bundle (for main bundle only)
    uint8_t localVersion = 3;
#if READ_DIRECTORIES
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
    CFStringRef directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFArrayRef contents = _CFBundleCopySortedDirectoryContentsAtPath(directoryPath, _CFBundleAllContents);
    if (CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework/"))) {
        if (_CFBundleSortedArrayContains(contents, _CFBundleResourcesDirectoryName)) localVersion = 0;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName2)) localVersion = 2;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName1)) localVersion = 1;
    } else {
        if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName2)) localVersion = 2;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleResourcesDirectoryName)) localVersion = 0;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName1)) localVersion = 1;
    }
    CFRelease(contents);
    CFRelease(directoryPath);
    CFRelease(absoluteURL);
#endif /* READ_DIRECTORIES */
    if (localVersion == 3) {
#if DEPLOYMENT_TARGET_EMBEDDED
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_WINDOWS
#if DEPLOYMENT_TARGET_WINDOWS
        if (CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework/")) || CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework\\"))) {
#else
        if (CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework/"))) {
#endif
            if (_CFBundleURLHasSubDir(url, _CFBundleResourcesURLFromBase0)) localVersion = 0;
            else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase2)) localVersion = 2;
            else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase1)) localVersion = 1;
        } else {
            if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase2)) localVersion = 2;
            else if (_CFBundleURLHasSubDir(url, _CFBundleResourcesURLFromBase0)) localVersion = 0;
            else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase1)) localVersion = 1;
        }
#endif
    }
    if (version) *version = localVersion;
    return (localVersion != 3);
}

static Boolean _isValidPlatformSuffix(CFStringRef suffix) {
    for (CFIndex idx = 0; idx < _CFBundleNumberOfPlatforms; idx++) {
        if (CFEqual(suffix, _CFBundleSupportedPlatforms[idx])) return true;
    }
    return false;
}

static Boolean _isValidProductSuffix(CFStringRef suffix) {
    for (CFIndex idx = 0; idx < _CFBundleNumberOfProducts; idx++) {
        if (CFEqual(suffix, _CFBundleSupportedProducts[idx])) return true;
    }
    return false;
}

static Boolean _isValidiPhoneOSPlatformProductSuffix(CFStringRef suffix) {
    for (CFIndex idx = 0; idx < _CFBundleNumberOfiPhoneOSPlatformProducts; idx++) {
        if (CFEqual(suffix, _CFBundleSupportediPhoneOSPlatformProducts[idx])) return true;
    }
    return false;
}

static Boolean _isValidPlatformAndProductSuffixPair(CFStringRef platform, CFStringRef product) {
    if (!platform && !product) return true;
    if (!platform) {
        return _isValidProductSuffix(product);
    }
    if (!product) {
        return _isValidPlatformSuffix(platform);
    }
    if (CFEqual(platform, _CFBundleiPhoneOSPlatformName)) {
        return _isValidiPhoneOSPlatformProductSuffix(product);
    }
    return false;
}

static Boolean _isBlacklistedKey(CFStringRef keyName) {
#define _CFBundleNumberOfBlacklistedInfoDictionaryKeys 2
    static const CFStringRef _CFBundleBlacklistedInfoDictionaryKeys[_CFBundleNumberOfBlacklistedInfoDictionaryKeys] = { CFSTR("CFBundleExecutable"), CFSTR("CFBundleIdentifier") };
    
    for (CFIndex idx = 0; idx < _CFBundleNumberOfBlacklistedInfoDictionaryKeys; idx++) {
        if (CFEqual(keyName, _CFBundleBlacklistedInfoDictionaryKeys[idx])) return true;
    }
    return false;
}

static Boolean _isOverrideKey(CFStringRef fullKey, CFStringRef *outBaseKey, CFStringRef *outPlatformSuffix, CFStringRef *outProductSuffix) {
    if (outBaseKey) {
        *outBaseKey = NULL;
    }
    if (outPlatformSuffix) {
        *outPlatformSuffix = NULL;
    }
    if (outProductSuffix) {
        *outProductSuffix = NULL;
    }
    if (!fullKey)
        return false;
    CFRange minusRange = CFStringFind(fullKey, CFSTR("-"), kCFCompareBackwards);
    CFRange tildeRange = CFStringFind(fullKey, CFSTR("~"), kCFCompareBackwards);
    if (minusRange.location == kCFNotFound && tildeRange.location == kCFNotFound) return false;
    // minus must come before tilde if both are present
    if (minusRange.location != kCFNotFound && tildeRange.location != kCFNotFound && tildeRange.location <= minusRange.location) return false;
    
    CFIndex strLen = CFStringGetLength(fullKey);
    CFRange baseKeyRange = (minusRange.location != kCFNotFound) ? CFRangeMake(0, minusRange.location) : CFRangeMake(0, tildeRange.location);
    CFRange platformRange = CFRangeMake(kCFNotFound, 0);
    CFRange productRange = CFRangeMake(kCFNotFound, 0);
    if (minusRange.location != kCFNotFound) {
        platformRange.location = minusRange.location + minusRange.length;
        platformRange.length = ((tildeRange.location != kCFNotFound) ? tildeRange.location : strLen) - platformRange.location;
    }
    if (tildeRange.location != kCFNotFound) {
        productRange.location = tildeRange.location + tildeRange.length;
        productRange.length = strLen - productRange.location;
    }
    if (baseKeyRange.length < 1) return false;
    if (platformRange.location != kCFNotFound && platformRange.length < 1) return false;
    if (productRange.location != kCFNotFound && productRange.length < 1) return false;
    
    CFStringRef platform = (platformRange.location != kCFNotFound) ? CFStringCreateWithSubstring(kCFAllocatorSystemDefaultGCRefZero, fullKey, platformRange) : NULL;
    CFStringRef product = (productRange.location != kCFNotFound) ? CFStringCreateWithSubstring(kCFAllocatorSystemDefaultGCRefZero, fullKey, productRange) : NULL;
    Boolean result = _isValidPlatformAndProductSuffixPair(platform, product);
    
    if (result) {
        if (outBaseKey) {
            *outBaseKey = CFStringCreateWithSubstring(kCFAllocatorSystemDefaultGCRefZero, fullKey, baseKeyRange);
        }
        if (outPlatformSuffix) {
            *outPlatformSuffix = platform;
        } else {
            if (platform && !_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(platform);
        }
        if (outProductSuffix) {
            *outProductSuffix = product;
        } else {
            if (product && !_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(product);
        }
    } else {
        if (platform && !_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(platform);
        if (product && !_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(product);
    }
    return result;
}
    
static Boolean _isCurrentPlatformAndProduct(CFStringRef platform, CFStringRef product) {
    if (!platform && !product) return true;
    if (!platform) {
        return CFEqual(_CFGetProductName(), product);
    }
    if (!product) {
        return CFEqual(_CFGetPlatformName(), platform);
    }
    
    return CFEqual(_CFGetProductName(), product) && CFEqual(_CFGetPlatformName(), platform);
}
    
static CFArrayRef _CopySortedOverridesForBaseKey(CFStringRef keyName, CFDictionaryRef dict) {
    CFMutableArrayRef overrides = CFArrayCreateMutable(kCFAllocatorSystemDefaultGCRefZero, 0, &kCFTypeArrayCallBacks);
    CFStringRef keyNameWithBoth = CFStringCreateWithFormat(kCFAllocatorSystemDefaultGCRefZero, NULL, CFSTR("%@-%@~%@"), keyName, _CFGetPlatformName(), _CFGetProductName());
    CFStringRef keyNameWithProduct = CFStringCreateWithFormat(kCFAllocatorSystemDefaultGCRefZero, NULL, CFSTR("%@~%@"), keyName, _CFGetProductName());
    CFStringRef keyNameWithPlatform = CFStringCreateWithFormat(kCFAllocatorSystemDefaultGCRefZero, NULL, CFSTR("%@-%@"), keyName, _CFGetPlatformName());

    CFIndex count = CFDictionaryGetCount(dict);
    
    if (count > 0) {
        CFTypeRef *keys = (CFTypeRef *)CFAllocatorAllocate(kCFAllocatorSystemDefaultGCRefZero, 2 * count * sizeof(CFTypeRef), 0);
        CFTypeRef *values = &(keys[count]);
        
        CFDictionaryGetKeysAndValues(dict, keys, values);
        for (CFIndex idx = 0; idx < count; idx++) {
			if (CFEqual(keys[idx], keyNameWithBoth)) {
				CFArrayAppendValue(overrides, keys[idx]);
				break;
			}
		}
        for (CFIndex idx = 0; idx < count; idx++) {
			if (CFEqual(keys[idx], keyNameWithProduct)) {
				CFArrayAppendValue(overrides, keys[idx]);
				break;
			}
		}
        for (CFIndex idx = 0; idx < count; idx++) {
			if (CFEqual(keys[idx], keyNameWithPlatform)) {
				CFArrayAppendValue(overrides, keys[idx]);
				break;
			}
		}
        for (CFIndex idx = 0; idx < count; idx++) {
			if (CFEqual(keys[idx], keyName)) {
				CFArrayAppendValue(overrides, keys[idx]);
				break;
			}
		}

        if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) {
            CFAllocatorDeallocate(kCFAllocatorSystemDefaultGCRefZero, keys);
		}
	}

	if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) {
		CFRelease(keyNameWithProduct);
		CFRelease(keyNameWithPlatform);
		CFRelease(keyNameWithBoth);
	}

    return overrides;
}

__private_extern__ void _processInfoDictionary(CFMutableDictionaryRef dict, CFStringRef platformSuffix, CFStringRef productSuffix) {
    CFIndex count = CFDictionaryGetCount(dict);
    
    if (count > 0) {
        CFTypeRef *keys = (CFTypeRef *)CFAllocatorAllocate(kCFAllocatorSystemDefaultGCRefZero, 2 * count * sizeof(CFTypeRef), 0);
        CFTypeRef *values = &(keys[count]);
        CFMutableArrayRef guard = CFArrayCreateMutable(kCFAllocatorSystemDefaultGCRefZero, 0, &kCFTypeArrayCallBacks);
        
        CFDictionaryGetKeysAndValues(dict, keys, values);
        for (CFIndex idx = 0; idx < count; idx++) {
            CFStringRef keyPlatformSuffix, keyProductSuffix, keyName;
            if (_isOverrideKey((CFStringRef)keys[idx], &keyName, &keyPlatformSuffix, &keyProductSuffix)) {
                CFArrayRef keysForBaseKey = NULL;
                if (_isCurrentPlatformAndProduct(keyPlatformSuffix, keyProductSuffix) && !_isBlacklistedKey(keyName) && CFDictionaryContainsKey(dict, keys[idx])) {
                    keysForBaseKey = _CopySortedOverridesForBaseKey(keyName, dict);
                    CFIndex keysForBaseKeyCount = CFArrayGetCount(keysForBaseKey);
                    
                    //make sure the other keys for this base key don't get released out from under us until we're done
                    CFArrayAppendValue(guard, keysForBaseKey); 
                    
                    //the winner for this base key will be sorted to the front, do the override with it
                    CFTypeRef highestPriorityKey = CFArrayGetValueAtIndex(keysForBaseKey, 0);
                    CFDictionarySetValue(dict, keyName, CFDictionaryGetValue(dict, highestPriorityKey));
                    
                    //remove everything except the now-overridden key; this will cause them to fail the CFDictionaryContainsKey(dict, keys[idx]) check in the enclosing if() and not be reprocessed
                    for (CFIndex presentKeysIdx = 0; presentKeysIdx < keysForBaseKeyCount; presentKeysIdx++) {
                        CFStringRef currentKey = (CFStringRef)CFArrayGetValueAtIndex(keysForBaseKey, presentKeysIdx);
                        if (!CFEqual(currentKey, keyName))
                            CFDictionaryRemoveValue(dict, currentKey);
                    }
                } else {
                    CFDictionaryRemoveValue(dict, keys[idx]);
                }

                
                if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) {
                    if (keyPlatformSuffix) CFRelease(keyPlatformSuffix);
                    if (keyProductSuffix) CFRelease(keyProductSuffix);
                    CFRelease(keyName);
                    if (keysForBaseKey) CFRelease(keysForBaseKey);
                }
            }
        }
        
        if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) {
            CFAllocatorDeallocate(kCFAllocatorSystemDefaultGCRefZero, keys);
            CFRelease(guard);
        }
    }
}      

// returns zero-ref dictionary under GC if given kCFAllocatorSystemDefaultGCRefZero
__private_extern__ CFDictionaryRef _CFBundleCopyInfoDictionaryInDirectory(CFAllocatorRef alloc, CFURLRef url, uint8_t *version) {
    CFDictionaryRef dict = NULL;
    unsigned char buff[CFMaxPathSize];
    uint8_t localVersion = 0;
    
    if (CFURLGetFileSystemRepresentation(url, true, buff, CFMaxPathSize)) {
        CFURLRef newURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, buff, strlen((char *)buff), true);
        if (!newURL) newURL = (CFURLRef)CFRetain(url);

        // version 3 is for flattened pseudo-bundles with no Contents, Support Files, or Resources directories
        if (!_CFBundleURLLooksLikeBundleVersion(newURL, &localVersion)) localVersion = 3;
        
        dict = _CFBundleCopyInfoDictionaryInDirectoryWithVersion(alloc, newURL, localVersion);
        CFRelease(newURL);
    }
    if (version) *version = localVersion;
    return dict;
}

// returns zero-ref dictionary under GC if given kCFAllocatorSystemDefaultGCRefZero
__private_extern__ CFDictionaryRef _CFBundleCopyInfoDictionaryInDirectoryWithVersion(CFAllocatorRef alloc, CFURLRef url, uint8_t version) {
    CFDictionaryRef result = NULL;
    if (url) {
        CFURLRef infoURL = NULL, rawInfoURL = NULL;
        CFDataRef infoData = NULL;
        UniChar buff[CFMaxPathSize];
        CFIndex len;
        CFMutableStringRef cheapStr;
        CFStringRef infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension0, infoURLFromBase = _CFBundleInfoURLFromBase0;
        Boolean tryPlatformSpecific = true, tryGlobal = true;
#if READ_DIRECTORIES
        CFURLRef directoryURL = NULL, absoluteURL;
        CFStringRef directoryPath;
        CFArrayRef contents = NULL;
        CFRange contentsRange = CFRangeMake(0, 0);
#endif /* READ_DIRECTORIES */    

        _CFEnsureStaticBuffersInited();

        if (0 == version) {
#if READ_DIRECTORIES
            directoryURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundleResourcesURLFromBase0, url);
#endif /* READ_DIRECTORIES */    
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension0;
            infoURLFromBase = _CFBundleInfoURLFromBase0;
        } else if (1 == version) {
#if READ_DIRECTORIES
            directoryURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundleSupportFilesURLFromBase1, url);
#endif /* READ_DIRECTORIES */    
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension1;
            infoURLFromBase = _CFBundleInfoURLFromBase1;
        } else if (2 == version) {
#if READ_DIRECTORIES
            directoryURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundleSupportFilesURLFromBase2, url);
#endif /* READ_DIRECTORIES */    
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension2;
            infoURLFromBase = _CFBundleInfoURLFromBase2;
        } else if (3 == version) {
            CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
            // this test is necessary to exclude the case where a bundle is spuriously created from the innards of another bundle
            if (path) {
                if (!(CFStringHasSuffix(path, _CFBundleSupportFilesDirectoryName1) || CFStringHasSuffix(path, _CFBundleSupportFilesDirectoryName2) || CFStringHasSuffix(path, _CFBundleResourcesDirectoryName))) {
#if READ_DIRECTORIES
                    directoryURL = CFRetain(url);
#endif /* READ_DIRECTORIES */    
                    infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension3;
                    infoURLFromBase = _CFBundleInfoURLFromBase3;
                }
                CFRelease(path);
            }
        }
#if READ_DIRECTORIES
        if (directoryURL) {
            absoluteURL = CFURLCopyAbsoluteURL(directoryURL);
            directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
            contents = _CFBundleCopySortedDirectoryContentsAtPath(directoryPath, _CFBundleAllContents);
            contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
            CFRelease(directoryPath);
            CFRelease(absoluteURL);
            CFRelease(directoryURL);
        }
#endif /* READ_DIRECTORIES */    

        len = CFStringGetLength(infoURLFromBaseNoExtension);
        CFStringGetCharacters(infoURLFromBaseNoExtension, CFRangeMake(0, len), buff);
        buff[len++] = (UniChar)'-';
        memmove(buff + len, _PlatformUniChars, _PlatformLen * sizeof(UniChar));
        len += _PlatformLen;
        _CFAppendPathExtension(buff, &len, CFMaxPathSize, _InfoExtensionUniChars, _InfoExtensionLen);
        cheapStr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
        CFStringAppendCharacters(cheapStr, buff, len);
        infoURL = CFURLCreateWithString(kCFAllocatorSystemDefault, cheapStr, url);
#if READ_DIRECTORIES
        if (contents) {
            CFIndex resourcesLen, idx;
            for (resourcesLen = len; resourcesLen > 0; resourcesLen--) if (buff[resourcesLen - 1] == '/') break;
            CFStringDelete(cheapStr, CFRangeMake(0, CFStringGetLength(cheapStr)));
            CFStringAppendCharacters(cheapStr, buff + resourcesLen, len - resourcesLen);
            for (tryPlatformSpecific = false, idx = 0; !tryPlatformSpecific && idx < contentsRange.length; idx++) {
                // Need to do this case-insensitive to accommodate Palm
                if (kCFCompareEqualTo == CFStringCompare(cheapStr, CFArrayGetValueAtIndex(contents, idx), kCFCompareCaseInsensitive)) tryPlatformSpecific = true;
            }
        }
#endif /* READ_DIRECTORIES */    
        if (tryPlatformSpecific) CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, infoURL, &infoData, NULL, NULL, NULL);
        //fprintf(stderr, "looking for ");CFShow(infoURL);fprintf(stderr, infoData ? "found it\n" : (tryPlatformSpecific ? "missed it\n" : "skipped it\n"));
        CFRelease(cheapStr);
        if (!infoData) {
            // Check for global Info.plist
            CFRelease(infoURL);
            infoURL = CFURLCreateWithString(kCFAllocatorSystemDefault, infoURLFromBase, url);
#if READ_DIRECTORIES
            if (contents) {
                CFIndex idx;
                for (tryGlobal = false, idx = 0; !tryGlobal && idx < contentsRange.length; idx++) {
                    // Need to do this case-insensitive to accommodate Palm
                    if (kCFCompareEqualTo == CFStringCompare(_CFBundleInfoFileName, CFArrayGetValueAtIndex(contents, idx), kCFCompareCaseInsensitive)) tryGlobal = true;
                }
            }
#endif /* READ_DIRECTORIES */    
            if (tryGlobal) CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, infoURL, &infoData, NULL, NULL, NULL);
            //fprintf(stderr, "looking for ");CFShow(infoURL);fprintf(stderr, infoData ? "found it\n" : (tryGlobal ? "missed it\n" : "skipped it\n"));
        }
        
        if (infoData) {
            result = (CFDictionaryRef)CFPropertyListCreateFromXMLData(alloc, infoData, kCFPropertyListMutableContainers, NULL);
            if (result) {
                if (CFDictionaryGetTypeID() == CFGetTypeID(result)) {
                    CFDictionarySetValue((CFMutableDictionaryRef)result, _kCFBundleInfoPlistURLKey, infoURL);
                } else {
                    if (!_CFAllocatorIsGCRefZero(alloc)) CFRelease(result);
                    result = NULL;
                }
            }
            if (!result) rawInfoURL = infoURL;
            CFRelease(infoData);
        }
        if (!result) {
            result = CFDictionaryCreateMutable(alloc, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (rawInfoURL) CFDictionarySetValue((CFMutableDictionaryRef)result, _kCFBundleRawInfoPlistURLKey, rawInfoURL);
        }

        CFRelease(infoURL);
#if READ_DIRECTORIES
        if (contents) CFRelease(contents);
#endif /* READ_DIRECTORIES */    
    }
    _processInfoDictionary((CFMutableDictionaryRef)result, _CFGetPlatformName(), _CFGetProductName());
    return result;
}

static Boolean _CFBundleGetPackageInfoInDirectoryWithInfoDictionary(CFAllocatorRef alloc, CFURLRef url, CFDictionaryRef infoDict, UInt32 *packageType, UInt32 *packageCreator) {
    Boolean retVal = false, hasType = false, hasCreator = false, releaseInfoDict = false;
    CFURLRef tempURL;
    CFDataRef pkgInfoData = NULL;

    // Check for a "real" new bundle
    tempURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundlePkgInfoURLFromBase2, url);
    CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, tempURL, &pkgInfoData, NULL, NULL, NULL);
    CFRelease(tempURL);
    if (!pkgInfoData) {
        tempURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundlePkgInfoURLFromBase1, url);
        CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, tempURL, &pkgInfoData, NULL, NULL, NULL);
        CFRelease(tempURL);
    }
    if (!pkgInfoData) {
        // Check for a "pseudo" new bundle
        tempURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundlePseudoPkgInfoURLFromBase, url);
        CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, tempURL, &pkgInfoData, NULL, NULL, NULL);
        CFRelease(tempURL);
    }

    // Now, either we have a pkgInfoData or not.  If not, then is it because this is a new bundle without one (do we allow this?), or is it dbecause it is an old bundle.
    // If we allow new bundles to not have a PkgInfo (because they already have the same data in the Info.plist), then we have to go read the info plist which makes failure expensive.
    // drd: So we assume that a new bundle _must_ have a PkgInfo if they have this data at all, otherwise we manufacture it from the extension.
    
    if (pkgInfoData && CFDataGetLength(pkgInfoData) >= (int)(sizeof(UInt32) * 2)) {
        UInt32 *pkgInfo = (UInt32 *)CFDataGetBytePtr(pkgInfoData);
        if (packageType) *packageType = CFSwapInt32BigToHost(pkgInfo[0]);
        if (packageCreator) *packageCreator = CFSwapInt32BigToHost(pkgInfo[1]);
        retVal = hasType = hasCreator = true;
    }
    if (pkgInfoData) CFRelease(pkgInfoData);
    if (!retVal) {
        if (!infoDict) {
            infoDict = _CFBundleCopyInfoDictionaryInDirectory(kCFAllocatorSystemDefaultGCRefZero, url, NULL);
            releaseInfoDict = true;
        }
        if (infoDict) {
            CFStringRef typeString = (CFStringRef)CFDictionaryGetValue(infoDict, _kCFBundlePackageTypeKey), creatorString = (CFStringRef)CFDictionaryGetValue(infoDict, _kCFBundleSignatureKey);
            UInt32 tmp;
            CFIndex usedBufLen = 0;
            if (typeString && CFGetTypeID(typeString) == CFStringGetTypeID() && CFStringGetLength(typeString) == 4 && 4 == CFStringGetBytes(typeString, CFRangeMake(0, 4), kCFStringEncodingMacRoman, 0, false, (UInt8 *)&tmp, 4, &usedBufLen) && 4 == usedBufLen) {
                if (packageType) *packageType = CFSwapInt32BigToHost(tmp);
                retVal = hasType = true;
            }
            if (creatorString && CFGetTypeID(creatorString) == CFStringGetTypeID() && CFStringGetLength(creatorString) == 4 && 4 == CFStringGetBytes(creatorString, CFRangeMake(0, 4), kCFStringEncodingMacRoman, 0, false, (UInt8 *)&tmp, 4, &usedBufLen) && 4 == usedBufLen) {
                if (packageCreator) *packageCreator = CFSwapInt32BigToHost(tmp);
                retVal = hasCreator = true;
            }
            if (releaseInfoDict && !_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(infoDict);
        }
    }
    if (!hasType || !hasCreator) {
        // If this looks like a bundle then manufacture the type and creator.
        if (retVal || _CFBundleURLLooksLikeBundle(url)) {
            if (packageCreator && !hasCreator) *packageCreator = 0x3f3f3f3f;  // '????'
            if (packageType && !hasType) {
                CFStringRef urlStr;
                UniChar buff[CFMaxPathSize];
                CFIndex strLen, startOfExtension;
                CFURLRef absoluteURL;
                
                // Detect "app", "debug", "profile", or "framework" extensions
                absoluteURL = CFURLCopyAbsoluteURL(url);
                urlStr = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
                CFRelease(absoluteURL);
                strLen = CFStringGetLength(urlStr);
                if (strLen > CFMaxPathSize) strLen = CFMaxPathSize;
                CFStringGetCharacters(urlStr, CFRangeMake(0, strLen), buff);
                CFRelease(urlStr);
                startOfExtension = _CFStartOfPathExtension(buff, strLen);
                if ((strLen - startOfExtension == 4 || strLen - startOfExtension == 5) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'a' && buff[startOfExtension+2] == (UniChar)'p' && buff[startOfExtension+3] == (UniChar)'p' && (strLen - startOfExtension == 4 || buff[startOfExtension+4] == (UniChar)'/')) {
                    // This is an app
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 6 || strLen - startOfExtension == 7) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'d' && buff[startOfExtension+2] == (UniChar)'e' && buff[startOfExtension+3] == (UniChar)'b' && buff[startOfExtension+4] == (UniChar)'u' && buff[startOfExtension+5] == (UniChar)'g' && (strLen - startOfExtension == 6 || buff[startOfExtension+6] == (UniChar)'/')) {
                    // This is an app (debug version)
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 8 || strLen - startOfExtension == 9) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'p' && buff[startOfExtension+2] == (UniChar)'r' && buff[startOfExtension+3] == (UniChar)'o' && buff[startOfExtension+4] == (UniChar)'f' && buff[startOfExtension+5] == (UniChar)'i' && buff[startOfExtension+6] == (UniChar)'l' && buff[startOfExtension+7] == (UniChar)'e' && (strLen - startOfExtension == 8 || buff[startOfExtension+8] == (UniChar)'/')) {
                    // This is an app (profile version)
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 8 || strLen - startOfExtension == 9) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'s' && buff[startOfExtension+2] == (UniChar)'e' && buff[startOfExtension+3] == (UniChar)'r' && buff[startOfExtension+4] == (UniChar)'v' && buff[startOfExtension+5] == (UniChar)'i' && buff[startOfExtension+6] == (UniChar)'c' && buff[startOfExtension+7] == (UniChar)'e' && (strLen - startOfExtension == 8 || buff[startOfExtension+8] == (UniChar)'/')) {
                    // This is a service
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 10 || strLen - startOfExtension == 11) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'f' && buff[startOfExtension+2] == (UniChar)'r' && buff[startOfExtension+3] == (UniChar)'a' && buff[startOfExtension+4] == (UniChar)'m' && buff[startOfExtension+5] == (UniChar)'e' && buff[startOfExtension+6] == (UniChar)'w' && buff[startOfExtension+7] == (UniChar)'o' && buff[startOfExtension+8] == (UniChar)'r' && buff[startOfExtension+9] == (UniChar)'k' && (strLen - startOfExtension == 10 || buff[startOfExtension+10] == (UniChar)'/')) {
                    // This is a framework
                    *packageType = 0x464d574b;  // 'FMWK'
                } else {
                    // Default to BNDL for generic bundle
                    *packageType = 0x424e444c;  // 'BNDL'
                }
            }
            retVal = true;
        }
    }
    return retVal;
}

CF_EXPORT Boolean _CFBundleGetPackageInfoInDirectory(CFAllocatorRef alloc, CFURLRef url, UInt32 *packageType, UInt32 *packageCreator) {
    return _CFBundleGetPackageInfoInDirectoryWithInfoDictionary(alloc, url, NULL, packageType, packageCreator);
}

CF_EXPORT void CFBundleGetPackageInfo(CFBundleRef bundle, UInt32 *packageType, UInt32 *packageCreator) {
    CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
    if (!_CFBundleGetPackageInfoInDirectoryWithInfoDictionary(kCFAllocatorSystemDefault, bundleURL, CFBundleGetInfoDictionary(bundle), packageType, packageCreator)) {
        if (packageType) *packageType = 0x424e444c;  // 'BNDL'
        if (packageCreator) *packageCreator = 0x3f3f3f3f;  // '????'
    }
    if (bundleURL) CFRelease(bundleURL);
}

CF_EXPORT Boolean CFBundleGetPackageInfoInDirectory(CFURLRef url, UInt32 *packageType, UInt32 *packageCreator) {
    return _CFBundleGetPackageInfoInDirectory(kCFAllocatorSystemDefault, url, packageType, packageCreator);
}

static void _CFBundleCheckSupportedPlatform(CFMutableArrayRef mutableArray, UniChar *buff, CFIndex startLen, CFStringRef platformName, CFStringRef platformIdentifier) {
    CFIndex buffLen = startLen, platformLen = CFStringGetLength(platformName), extLen = CFStringGetLength(_CFBundleInfoExtension);
    CFMutableStringRef str;
    Boolean isDir;
    if (buffLen + platformLen + extLen < CFMaxPathSize) {
        CFStringGetCharacters(platformName, CFRangeMake(0, platformLen), buff + buffLen);
        buffLen += platformLen;
        buff[buffLen++] = (UniChar)'.';
        CFStringGetCharacters(_CFBundleInfoExtension, CFRangeMake(0, extLen), buff + buffLen);
        buffLen += extLen;
        str = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
        CFStringAppendCharacters(str, buff, buffLen);
        if (_CFIsResourceAtPath(str, &isDir) && !isDir && CFArrayGetFirstIndexOfValue(mutableArray, CFRangeMake(0, CFArrayGetCount(mutableArray)), platformIdentifier) < 0) CFArrayAppendValue(mutableArray, platformIdentifier);
        CFRelease(str);
    }
}

CF_EXPORT CFArrayRef _CFBundleGetSupportedPlatforms(CFBundleRef bundle) {
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFArrayRef platformArray = infoDict ? (CFArrayRef)CFDictionaryGetValue(infoDict, _kCFBundleSupportedPlatformsKey) : NULL;
    if (platformArray && CFGetTypeID(platformArray) != CFArrayGetTypeID()) {
        platformArray = NULL;
        CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, _kCFBundleSupportedPlatformsKey);
    }
    if (!platformArray) {
        CFURLRef infoPlistURL = infoDict ? (CFURLRef)CFDictionaryGetValue(infoDict, _kCFBundleInfoPlistURLKey) : NULL, absoluteURL;
        CFStringRef infoPlistPath;
        UniChar buff[CFMaxPathSize];
        CFIndex buffLen, infoLen = CFStringGetLength(_CFBundleInfoURLFromBaseNoExtension3), startLen, extLen = CFStringGetLength(_CFBundleInfoExtension);
        if (infoPlistURL) {
            CFMutableArrayRef mutableArray = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            absoluteURL = CFURLCopyAbsoluteURL(infoPlistURL);
            infoPlistPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
            CFRelease(absoluteURL);
            buffLen = CFStringGetLength(infoPlistPath);
            if (buffLen > CFMaxPathSize) buffLen = CFMaxPathSize;
            CFStringGetCharacters(infoPlistPath, CFRangeMake(0, buffLen), buff);
            CFRelease(infoPlistPath);
            if (buffLen > 0) {
                buffLen = _CFStartOfLastPathComponent(buff, buffLen);
                if (buffLen > 0 && buffLen + infoLen + extLen < CFMaxPathSize) {
                    CFStringGetCharacters(_CFBundleInfoURLFromBaseNoExtension3, CFRangeMake(0, infoLen), buff + buffLen);
                    buffLen += infoLen;
                    buff[buffLen++] = (UniChar)'-';
                    startLen = buffLen;
                    _CFBundleCheckSupportedPlatform(mutableArray, buff, startLen, CFSTR("macos"), CFSTR("MacOS"));
                    _CFBundleCheckSupportedPlatform(mutableArray, buff, startLen, CFSTR("macosx"), CFSTR("MacOS"));
                    _CFBundleCheckSupportedPlatform(mutableArray, buff, startLen, CFSTR("iphoneos"), CFSTR("iPhoneOS"));
                    _CFBundleCheckSupportedPlatform(mutableArray, buff, startLen, CFSTR("windows"), CFSTR("Windows"));
                }
            }
            if (CFArrayGetCount(mutableArray) > 0) {
                platformArray = (CFArrayRef)mutableArray;
                CFDictionarySetValue((CFMutableDictionaryRef)infoDict, _kCFBundleSupportedPlatformsKey, platformArray);
            }
            CFRelease(mutableArray);
        }
    }
    return platformArray;
}

CF_EXPORT CFStringRef _CFBundleGetCurrentPlatform(void) {
#if DEPLOYMENT_TARGET_MACOSX
    return CFSTR("MacOS");
#elif DEPLOYMENT_TARGET_EMBEDDED
    return CFSTR("iPhoneOS");
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("Windows");
#elif DEPLOYMENT_TARGET_SOLARIS
    return CFSTR("Solaris");
#elif DEPLOYMENT_TARGET_HPUX
    return CFSTR("HPUX");
#elif DEPLOYMENT_TARGET_LINUX
    return CFSTR("Linux");
#elif DEPLOYMENT_TARGET_FREEBSD
    return CFSTR("FreeBSD");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

__private_extern__ CFStringRef _CFBundleGetPlatformExecutablesSubdirectoryName(void) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    return CFSTR("MacOS");
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("Windows");
#elif DEPLOYMENT_TARGET_SOLARIS
    return CFSTR("Solaris");
#elif DEPLOYMENT_TARGET_HPUX
    return CFSTR("HPUX");
#elif DEPLOYMENT_TARGET_LINUX
    return CFSTR("Linux");
#elif DEPLOYMENT_TARGET_FREEBSD
    return CFSTR("FreeBSD");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

__private_extern__ CFStringRef _CFBundleGetAlternatePlatformExecutablesSubdirectoryName(void) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    return CFSTR("Mac OS X");
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("WinNT");
#elif DEPLOYMENT_TARGET_SOLARIS
    return CFSTR("Solaris");
#elif DEPLOYMENT_TARGET_HPUX
    return CFSTR("HP-UX");
#elif DEPLOYMENT_TARGET_LINUX
    return CFSTR("Linux");
#elif DEPLOYMENT_TARGET_FREEBSD
    return CFSTR("FreeBSD");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

__private_extern__ CFStringRef _CFBundleGetOtherPlatformExecutablesSubdirectoryName(void) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    return CFSTR("MacOSClassic");
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_HPUX
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_SOLARIS
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_LINUX
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_FREEBSD
    return CFSTR("Other");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

__private_extern__ CFStringRef _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName(void) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    return CFSTR("Mac OS 8");
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_HPUX
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_SOLARIS
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_LINUX
    return CFSTR("Other");
#elif DEPLOYMENT_TARGET_FREEBSD
    return CFSTR("Other");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

__private_extern__ CFArrayRef _CFBundleCopyBundleRegionsArray(CFBundleRef bundle) {
    return CFBundleCopyBundleLocalizations(bundle);
}

CF_EXPORT CFArrayRef CFBundleCopyBundleLocalizations(CFBundleRef bundle) {
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
#if READ_DIRECTORIES
    CFURLRef absoluteURL;
    CFStringRef directoryPath;
    CFArrayRef contents;
    CFRange contentsRange;
    CFIndex idx;
#else /* READ_DIRECTORIES */
    CFArrayRef urls = ((_CFBundleLayoutVersion(bundle) != 4) ? _CFContentsOfDirectory(CFGetAllocator(bundle), NULL, NULL, resourcesURL, _CFBundleLprojExtension) : NULL);
#endif /* READ_DIRECTORIES */
    CFArrayRef predefinedLocalizations = NULL;
    CFMutableArrayRef result = NULL;

    if (infoDict) {
        predefinedLocalizations = (CFArrayRef)CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
        if (predefinedLocalizations && CFGetTypeID(predefinedLocalizations) != CFArrayGetTypeID()) {
            predefinedLocalizations = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleLocalizationsKey);
        }
        if (predefinedLocalizations) {
            CFIndex i, c = CFArrayGetCount(predefinedLocalizations);
            if (c > 0 && !result) result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
            for (i = 0; i < c; i++) CFArrayAppendValue(result, CFArrayGetValueAtIndex(predefinedLocalizations, i));
        }
    }

#if READ_DIRECTORIES
    if (resourcesURL) {
        absoluteURL = CFURLCopyAbsoluteURL(resourcesURL);
        directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
        contents = _CFBundleCopySortedDirectoryContentsAtPath(directoryPath, _CFBundleAllContents);
        contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
        for (idx = 0; idx < contentsRange.length; idx++) {
            CFStringRef name = CFArrayGetValueAtIndex(contents, idx);
            if (CFStringHasSuffix(name, _CFBundleLprojExtensionWithDot)) {
                CFStringRef localization = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, name, CFRangeMake(0, CFStringGetLength(name) - 6));
                if (!result) result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
                CFArrayAppendValue(result, localization);
                CFRelease(localization);
            }
        }
        CFRelease(contents);
        CFRelease(directoryPath);
        CFRelease(absoluteURL);
    }
#else /* READ_DIRECTORIES */
    if (urls) {
        CFIndex i, c = CFArrayGetCount(urls);
        CFURLRef curURL, curAbsoluteURL;
        CFStringRef curStr, regionStr;
        UniChar buff[CFMaxPathSize];
        CFIndex strLen, startOfLastPathComponent, regionLen;

        if (c > 0 && !result) result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
        for (i = 0; i < c; i++) {
            curURL = (CFURLRef)CFArrayGetValueAtIndex(urls, i);
            curAbsoluteURL = CFURLCopyAbsoluteURL(curURL);
            curStr = CFURLCopyFileSystemPath(curAbsoluteURL, PLATFORM_PATH_STYLE);
            CFRelease(curAbsoluteURL);
            strLen = CFStringGetLength(curStr);
            if (strLen > CFMaxPathSize) strLen = CFMaxPathSize;
            CFStringGetCharacters(curStr, CFRangeMake(0, strLen), buff);

            startOfLastPathComponent = _CFStartOfLastPathComponent(buff, strLen);
            regionLen = _CFLengthAfterDeletingPathExtension(&(buff[startOfLastPathComponent]), strLen - startOfLastPathComponent);
            regionStr = CFStringCreateWithCharacters(CFGetAllocator(bundle), &(buff[startOfLastPathComponent]), regionLen);
            CFArrayAppendValue(result, regionStr);
            CFRelease(regionStr);
            CFRelease(curStr);
        }
        CFRelease(urls);
    }
#endif /* READ_DIRECTORIES */
    
    if (!result) {
        CFStringRef developmentLocalization = CFBundleGetDevelopmentRegion(bundle);
        if (developmentLocalization) {
            result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
            CFArrayAppendValue(result, developmentLocalization);
        }
    }
    if (resourcesURL) CFRelease(resourcesURL);
    return result;
}


CF_EXPORT CFDictionaryRef CFBundleCopyInfoDictionaryForURL(CFURLRef url) {
    CFDictionaryRef result = NULL;
    Boolean isDir = false;
    if (_CFIsResourceAtURL(url, &isDir)) {
        if (isDir) {
            result = _CFBundleCopyInfoDictionaryInDirectory(kCFAllocatorSystemDefaultGCRefZero, url, NULL);
        } else {
            result = _CFBundleCopyInfoDictionaryInExecutable(url);  // return zero-ref dictionary under GC
        }
    }
    if (result && _CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRetain(result); // conditionally put on a retain for a Copy function
    return result;
}

CFArrayRef CFBundleCopyExecutableArchitecturesForURL(CFURLRef url) {
    CFArrayRef result = NULL;
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorSystemDefault, url);
    if (bundle) {
        result = CFBundleCopyExecutableArchitectures(bundle);
        CFRelease(bundle);
    } else {
        result = _CFBundleCopyArchitecturesForExecutable(url);
    }
    return result;
}

CFArrayRef CFBundleCopyLocalizationsForURL(CFURLRef url) {
    CFArrayRef result = NULL;
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorSystemDefault, url);
    CFStringRef devLang = NULL;
    if (bundle) {
        result = CFBundleCopyBundleLocalizations(bundle);
        CFRelease(bundle);
    } else {
        CFDictionaryRef infoDict = _CFBundleCopyInfoDictionaryInExecutable(url);  // return zero-ref dictionary under GC
        if (infoDict) {
            CFArrayRef predefinedLocalizations = (CFArrayRef)CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
            if (predefinedLocalizations && CFGetTypeID(predefinedLocalizations) == CFArrayGetTypeID()) result = (CFArrayRef)CFRetain(predefinedLocalizations);
            if (!result) {
                devLang = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
                if (devLang && (CFGetTypeID(devLang) == CFStringGetTypeID() && CFStringGetLength(devLang) > 0)) result = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&devLang, 1, &kCFTypeArrayCallBacks);
            }
            if (!_CFAllocatorIsGCRefZero(kCFAllocatorSystemDefaultGCRefZero)) CFRelease(infoDict);
        }
    }
    return result;
}
