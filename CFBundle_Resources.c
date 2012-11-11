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
        Copyright (c) 1999-2012, Apple Inc.  All rights reserved.
        Responsibility: Tony Parker
*/

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

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

#if DEPLOYMENT_TARGET_MACOSX

#endif

#if DEPLOYMENT_TARGET_WINDOWS
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define close _close
#define write _write
#define read _read
#define open _NS_open
#define stat _NS_stat
#define fstat _fstat
#define mkdir(a,b) _NS_mkdir(a)
#define rmdir _NS_rmdir
#define unlink _NS_unlink

#endif

CF_EXPORT bool CFDictionaryGetKeyIfPresent(CFDictionaryRef dict, const void *key, const void **actualkey);

extern void _CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex len);


static inline Boolean _CFBundleSortedArrayContains(CFArrayRef arr, CFStringRef target) {
    CFRange arrRange = CFRangeMake(0, CFArrayGetCount(arr));
    CFIndex itemIdx = CFArrayBSearchValues(arr, arrRange, target, (CFComparatorFunction)CFStringCompare, NULL);
    return itemIdx < arrRange.length && CFEqual(CFArrayGetValueAtIndex(arr, itemIdx), target);
}

// The following strings are initialized 'later' (i.e., not at static initialization time) because static init time is too early for CFSTR to work, on platforms without constant CF strings
#if !__CONSTANT_STRINGS__

#define _CFBundleNumberOfPlatforms 7
static CFStringRef _CFBundleSupportedPlatforms[_CFBundleNumberOfPlatforms] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const char *_CFBundleSupportedPlatformStrings[_CFBundleNumberOfPlatforms] = { "iphoneos", "macos", "windows", "linux", "freebsd", "solaris", "hpux" };

#define _CFBundleNumberOfProducts 3
static CFStringRef _CFBundleSupportedProducts[_CFBundleNumberOfProducts] = { NULL, NULL, NULL };
static const char *_CFBundleSupportedProductStrings[_CFBundleNumberOfProducts] = { "iphone", "ipod", "ipad" };

#define _CFBundleNumberOfiPhoneOSPlatformProducts 3
static CFStringRef _CFBundleSupportediPhoneOSPlatformProducts[_CFBundleNumberOfiPhoneOSPlatformProducts] = { NULL, NULL, NULL };
static const char *_CFBundleSupportediPhoneOSPlatformProductStrings[_CFBundleNumberOfiPhoneOSPlatformProducts] = { "iphone", "ipod", "ipad" };

__private_extern__ void _CFBundleResourcesInitialize() {
    for (unsigned int i = 0; i < _CFBundleNumberOfPlatforms; i++) _CFBundleSupportedPlatforms[i] = CFStringCreateWithCString(kCFAllocatorSystemDefault, _CFBundleSupportedPlatformStrings[i], kCFStringEncodingUTF8);
    
    for (unsigned int i = 0; i < _CFBundleNumberOfProducts; i++) _CFBundleSupportedProducts[i] = CFStringCreateWithCString(kCFAllocatorSystemDefault, _CFBundleSupportedProductStrings[i], kCFStringEncodingUTF8);
    
    for (unsigned int i = 0; i < _CFBundleNumberOfiPhoneOSPlatformProducts; i++) _CFBundleSupportediPhoneOSPlatformProducts[i] = CFStringCreateWithCString(kCFAllocatorSystemDefault, _CFBundleSupportediPhoneOSPlatformProductStrings[i], kCFStringEncodingUTF8);
}

#else

#if DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
// On iOS, we only support one platform
#define _CFBundleNumberOfPlatforms 1
static CFStringRef _CFBundleSupportedPlatforms[_CFBundleNumberOfPlatforms] = { CFSTR("iphoneos") };
#else
// On other platforms, we support the following platforms
#define _CFBundleNumberOfPlatforms 7
static CFStringRef _CFBundleSupportedPlatforms[_CFBundleNumberOfPlatforms] = { CFSTR("iphoneos"), CFSTR("macos"), CFSTR("windows"), CFSTR("linux"), CFSTR("freebsd"), CFSTR("solaris"), CFSTR("hpux") };
#endif

#define _CFBundleNumberOfProducts 3
static CFStringRef _CFBundleSupportedProducts[_CFBundleNumberOfProducts] = { CFSTR("iphone"), CFSTR("ipod"), CFSTR("ipad") };

#define _CFBundleNumberOfiPhoneOSPlatformProducts 3
static CFStringRef _CFBundleSupportediPhoneOSPlatformProducts[_CFBundleNumberOfiPhoneOSPlatformProducts] = { CFSTR("iphone"), CFSTR("ipod"), CFSTR("ipad") };

__private_extern__ void _CFBundleResourcesInitialize() { }
#endif

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

CF_EXPORT CFStringRef _CFGetProductName(void) {
#if DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
CF_EXPORT CFStringRef _CFGetPlatformName(void) {
#if DEPLOYMENT_TARGET_MACOSX 
    return _CFBundleMacOSXPlatformName;
#elif DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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

CF_EXPORT CFStringRef _CFGetAlternatePlatformName(void) {
#if DEPLOYMENT_TARGET_MACOSX
    return _CFBundleAlternateMacOSXPlatformName;
#elif DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
    return _CFBundleMacOSXPlatformName;
#elif DEPLOYMENT_TARGET_WINDOWS
    return CFSTR("");
#else
#error Unknown or unspecified DEPLOYMENT_TARGET
#endif
}

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
static UniChar *_BaseUniChars = NULL;
static CFIndex _BaseLen;
static UniChar *_GlobalResourcesUniChars = NULL;
static CFIndex _GlobalResourcesLen = 0;
static UniChar *_InfoExtensionUniChars = NULL;
static CFIndex _InfoExtensionLen = 0;

#if 0
static UniChar _ResourceSuffix3[32];
static CFIndex _ResourceSuffix3Len = 0;
#endif
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
    CFStringRef baseStr = _CFBundleBaseDirectory;

    _AppSupportLen1 = CFStringGetLength(appSupportStr1);
    _AppSupportLen2 = CFStringGetLength(appSupportStr2);
    _ResourcesLen = CFStringGetLength(resourcesStr);
    _PlatformLen = CFStringGetLength(platformStr);
    _AlternatePlatformLen = CFStringGetLength(alternatePlatformStr);
    _LprojLen = CFStringGetLength(lprojStr);
    _GlobalResourcesLen = CFStringGetLength(globalResourcesStr);
    _InfoExtensionLen = CFStringGetLength(infoExtensionStr);
    _BaseLen = CFStringGetLength(baseStr);

    _AppSupportUniChars1 = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * (_AppSupportLen1 + _AppSupportLen2 + _ResourcesLen + _PlatformLen + _AlternatePlatformLen + _LprojLen + _GlobalResourcesLen + _InfoExtensionLen + _BaseLen), 0);
    _AppSupportUniChars2 = _AppSupportUniChars1 + _AppSupportLen1;
    _ResourcesUniChars = _AppSupportUniChars2 + _AppSupportLen2;
    _PlatformUniChars = _ResourcesUniChars + _ResourcesLen;
    _AlternatePlatformUniChars = _PlatformUniChars + _PlatformLen;
    _LprojUniChars = _AlternatePlatformUniChars + _AlternatePlatformLen;
    _GlobalResourcesUniChars = _LprojUniChars + _LprojLen;
    _InfoExtensionUniChars = _GlobalResourcesUniChars + _GlobalResourcesLen;
    _BaseUniChars = _InfoExtensionUniChars + _InfoExtensionLen;
    
    if (_AppSupportLen1 > 0) CFStringGetCharacters(appSupportStr1, CFRangeMake(0, _AppSupportLen1), _AppSupportUniChars1);
    if (_AppSupportLen2 > 0) CFStringGetCharacters(appSupportStr2, CFRangeMake(0, _AppSupportLen2), _AppSupportUniChars2);
    if (_ResourcesLen > 0) CFStringGetCharacters(resourcesStr, CFRangeMake(0, _ResourcesLen), _ResourcesUniChars);
    if (_PlatformLen > 0) CFStringGetCharacters(platformStr, CFRangeMake(0, _PlatformLen), _PlatformUniChars);
    if (_AlternatePlatformLen > 0) CFStringGetCharacters(alternatePlatformStr, CFRangeMake(0, _AlternatePlatformLen), _AlternatePlatformUniChars);
    if (_LprojLen > 0) CFStringGetCharacters(lprojStr, CFRangeMake(0, _LprojLen), _LprojUniChars);
    if (_GlobalResourcesLen > 0) CFStringGetCharacters(globalResourcesStr, CFRangeMake(0, _GlobalResourcesLen), _GlobalResourcesUniChars);
    if (_InfoExtensionLen > 0) CFStringGetCharacters(infoExtensionStr, CFRangeMake(0, _InfoExtensionLen), _InfoExtensionUniChars);
    if (_BaseLen > 0) CFStringGetCharacters(baseStr, CFRangeMake(0, _BaseLen), _BaseUniChars);

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
#if 0
    if (_ResourceSuffix1Len > 1 && _ResourceSuffix2Len > 1) {
        _ResourceSuffix3Len = _ResourceSuffix1Len + _ResourceSuffix2Len;
        memmove(_ResourceSuffix3, _ResourceSuffix1, sizeof(UniChar) * _ResourceSuffix1Len);
        memmove(_ResourceSuffix3 + _ResourceSuffix1Len, _ResourceSuffix2, sizeof(UniChar) * _ResourceSuffix2Len);
    }
#endif
}

CF_INLINE void _CFEnsureStaticBuffersInited(void) {
    static dispatch_once_t once = 0;
    dispatch_once(&once, ^{
        _CFBundleInitStaticUniCharBuffers();
    });
}

static CFSpinLock_t _cacheLock = CFSpinLockInit;
static CFMutableDictionaryRef _contentsCache = NULL;
static CFMutableDictionaryRef _directoryContentsCache = NULL;
static CFMutableDictionaryRef _unknownContentsCache = NULL;

typedef enum {
    _CFBundleAllContents = 0,
    _CFBundleDirectoryContents = 1,
    _CFBundleUnknownContents = 2
} _CFBundleDirectoryContentsType;

extern void _CFArraySortValues(CFMutableArrayRef array, CFComparatorFunction comparator, void *context);

static CFArrayRef _CFBundleCopySortedDirectoryContentsAtPath(CFStringRef path, _CFBundleDirectoryContentsType contentsType) {
    CFArrayRef result = NULL;
    
    if (!path) {
        // Return an empty result. It's mutable because the other arrays returned from this function are mutable, so may as well go for maximum compatibility.
        result = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        return result;
    }

    __CFSpinLock(&_cacheLock);
    if (contentsType == _CFBundleUnknownContents) {
        if (_unknownContentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(_unknownContentsCache, path);
    } else if (contentsType == _CFBundleDirectoryContents) {
        if (_directoryContentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(_directoryContentsCache, path);
    } else {
        if (_contentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(_contentsCache, path);
    }
    if (result) CFRetain(result);
    __CFSpinUnlock(&_cacheLock);

    if (!result) {
        Boolean tryToOpen = false, allDots = true;
        char cpathBuff[CFMaxPathSize];
        CFIndex cpathLen = 0, idx, lastSlashIdx = 0;
        CFMutableArrayRef contents = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks), directoryContents = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks), unknownContents = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFStringRef dirName, name;
        
        cpathBuff[0] = '\0';
        if (CFStringGetFileSystemRepresentation(path, cpathBuff, CFMaxPathSize)) {
            tryToOpen = true;
            cpathLen = strlen(cpathBuff);
            
            // First see whether we already know that the directory doesn't exist
            for (idx = cpathLen; lastSlashIdx == 0 && idx-- > 0;) {
                if (cpathBuff[idx] == PATH_SEP) lastSlashIdx = idx;
                else if (cpathBuff[idx] != '.') allDots = false;
            }
            if (lastSlashIdx > 0 && lastSlashIdx + 1 < cpathLen && !allDots) {
                cpathBuff[lastSlashIdx] = '\0';
                dirName = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, cpathBuff);
                if (dirName) {
                    name = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, cpathBuff + lastSlashIdx + 1);
                    if (name) {
                        // ??? we might like to use _directoryContentsCache rather than _contentsCache here, but we cannot unless we resolve DT_LNKs below
                        CFArrayRef dirDirContents = NULL;
                        
                        __CFSpinLock(&_cacheLock);
                        if (_contentsCache) dirDirContents = (CFArrayRef)CFDictionaryGetValue(_contentsCache, dirName);
                        if (dirDirContents) {
                            Boolean foundIt = false;
                            CFIndex dirDirIdx, dirDirLength = CFArrayGetCount(dirDirContents);
                            for (dirDirIdx = 0; !foundIt && dirDirIdx < dirDirLength; dirDirIdx++) if (kCFCompareEqualTo == CFStringCompare(name, (CFStringRef)CFArrayGetValueAtIndex(dirDirContents, dirDirIdx), kCFCompareCaseInsensitive)) foundIt = true;
                            if (!foundIt) tryToOpen = false;
                        }
                        __CFSpinUnlock(&_cacheLock);
                        CFRelease(name);
                    }
                    CFRelease(dirName);
                }
                cpathBuff[lastSlashIdx] = PATH_SEP;
            }
        }
#if DEPLOYMENT_TARGET_WINDOWS
        // Make sure there is room for the additional space we need in the win32 api
        if (tryToOpen && cpathLen + 2 < CFMaxPathSize) {
            WIN32_FIND_DATAW file;
            HANDLE handle;
            
            cpathBuff[cpathLen++] = '\\';
            cpathBuff[cpathLen++] = '*';
            cpathBuff[cpathLen] = '\0';
            
            // Convert UTF8 buffer to windows appropriate UTF-16LE
            // Get the real length of the string in UTF16 characters
            CFStringRef cfStr = CFStringCreateWithCString(kCFAllocatorSystemDefault, cpathBuff, kCFStringEncodingUTF8);
            cpathLen = CFStringGetLength(cfStr);
            // Allocate a wide buffer to hold the converted string, including space for a NULL terminator
            wchar_t *wideBuf = (wchar_t *)malloc((cpathLen + 1) * sizeof(wchar_t));
            // Copy the string into the buffer and terminate
            CFStringGetCharacters(cfStr, CFRangeMake(0, cpathLen), (UniChar *)wideBuf);
            wideBuf[cpathLen] = 0;
            CFRelease(cfStr);
            
            handle = FindFirstFileW(wideBuf, (LPWIN32_FIND_DATAW)&file);
            if (handle != INVALID_HANDLE_VALUE) {
                do {
                    CFIndex nameLen = wcslen(file.cFileName);
                    if (0 == nameLen || ('.' == file.cFileName[0] && (1 == nameLen || (2 == nameLen && '.' == file.cFileName[1]) || '_' == file.cFileName[1]))) continue;
                    name = CFStringCreateWithBytes(kCFAllocatorSystemDefault, (const uint8_t *)file.cFileName, nameLen * sizeof(wchar_t), kCFStringEncodingUTF16, NO);
                    if (name) {
                        CFArrayAppendValue(contents, name);
                        if (file.dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY) {
                            CFArrayAppendValue(directoryContents, name);
                        } /* else if (file.dwFileAttributes == DT_UNKNOWN) {
                            CFArrayAppendValue(unknownContents, name);
                        } */
                        CFRelease(name);
                    }
                } while (FindNextFileW(handle, &file));
                
                FindClose(handle);
            }
            free(wideBuf);
        }
#else
        DIR *dirp = NULL;
        struct dirent *dent;
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
#endif
        
        _CFArraySortValues(contents, (CFComparatorFunction)CFStringCompare, NULL);
        _CFArraySortValues(directoryContents, (CFComparatorFunction)CFStringCompare, NULL);
        _CFArraySortValues(unknownContents, (CFComparatorFunction)CFStringCompare, NULL);
        
        __CFSpinLock(&_cacheLock);
        if (!_contentsCache) _contentsCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, READ_DIRECTORIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (READ_DIRECTORIES_CACHE_CAPACITY <= CFDictionaryGetCount(_contentsCache)) CFDictionaryRemoveAllValues(_contentsCache);
        CFDictionaryAddValue(_contentsCache, path, contents);

        if (!_directoryContentsCache) _directoryContentsCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, READ_DIRECTORIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (READ_DIRECTORIES_CACHE_CAPACITY <= CFDictionaryGetCount(_directoryContentsCache)) CFDictionaryRemoveAllValues(_directoryContentsCache);
        CFDictionaryAddValue(_directoryContentsCache, path, directoryContents);

        if (!_unknownContentsCache) _unknownContentsCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, READ_DIRECTORIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (READ_DIRECTORIES_CACHE_CAPACITY <= CFDictionaryGetCount(_unknownContentsCache)) CFDictionaryRemoveAllValues(_unknownContentsCache);
        CFDictionaryAddValue(_unknownContentsCache, path, unknownContents);

        if (contentsType == _CFBundleUnknownContents) {
            result = (CFArrayRef)CFRetain(unknownContents);
        } else if (contentsType == _CFBundleDirectoryContents) {
            result = (CFArrayRef)CFRetain(directoryContents);
        } else {
            result = (CFArrayRef)CFRetain(contents);
        }
        
        CFRelease(contents);
        CFRelease(directoryContents);
        CFRelease(unknownContents);
        __CFSpinUnlock(&_cacheLock);
    }
    return result;
}

static void _CFBundleFlushContentsCaches(void) {
    __CFSpinLock(&_cacheLock);
    if (_contentsCache) CFDictionaryRemoveAllValues(_contentsCache);
    if (_directoryContentsCache) CFDictionaryRemoveAllValues(_directoryContentsCache);
    if (_unknownContentsCache) CFDictionaryRemoveAllValues(_unknownContentsCache);
    __CFSpinUnlock(&_cacheLock);
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
    __CFSpinLock(&_cacheLock);
    if (_contentsCache) _CFBundleFlushContentsCacheForPath(_contentsCache, path);
    if (_directoryContentsCache) _CFBundleFlushContentsCacheForPath(_directoryContentsCache, path);
    if (_unknownContentsCache) _CFBundleFlushContentsCacheForPath(_unknownContentsCache, path);
    __CFSpinUnlock(&_cacheLock);
}

CF_EXPORT void _CFBundleFlushCachesForURL(CFURLRef url) {
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
    CFStringRef path = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    _CFBundleFlushContentsCachesForPath(path);
    CFRelease(path);
    CFRelease(absoluteURL);
}

CF_EXPORT void _CFBundleFlushCaches(void) {
    _CFBundleFlushContentsCaches();
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

__private_extern__ void _CFBundleSetResourceDir(UniChar *buffer, CFIndex *currLen, CFIndex maxLen, uint8_t version){
    if (1 == version) {
        _CFAppendPathComponent(buffer, currLen, maxLen, _AppSupportUniChars1, _AppSupportLen1);
    } else if (2 == version) {
        _CFAppendPathComponent(buffer, currLen, maxLen, _AppSupportUniChars2, _AppSupportLen2);
    }
    if (0 == version || 1 == version || 2 == version) _CFAppendPathComponent(buffer, currLen, maxLen, _ResourcesUniChars, _ResourcesLen);
}

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
        CFStringRef content = (CFStringRef)CFArrayGetValueAtIndex(contents, i);
        if (CFStringHasPrefix(content, cheapStr)) {
            //fprintf(stderr, "found ");CFShow(content);
            for (j = 0; j < numResTypes; j++) {
                CFStringRef resType = (CFStringRef)CFArrayGetValueAtIndex(resTypes, j);
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

#if DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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

#if DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
    Boolean platformGenericIsUnknown = false, platformSpecificIsUnknown = false; 
    CFStringRef platformGenericStr = NULL;

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
    
    if (nameLen > 0) appendSucceeded = _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, nameUniChars, nameLen);
    savedPathLen = pathLen;
    if (appendSucceeded && typeLen > 0) appendSucceeded = _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    if (appendSucceeded) {
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
    CFRelease(contents);
    CFRelease(directoryContents);
    CFRelease(unknownContents);
#endif
}

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
        CFStringRef candidateFilename = (CFStringRef)CFArrayGetValueAtIndex(contents, i);
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


static void _CFFindBundleResourcesInRawDir(CFAllocatorRef alloc, UniChar *workingUniChars, CFIndex workingLen, UniChar *nameUniChars, CFIndex nameLen, CFArrayRef resTypes, CFIndex limit, Boolean *stopLooking, Boolean (^predicate)(CFStringRef filename, Boolean *stop), uint8_t version, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, CFMutableArrayRef result) {
    if (predicate) {
        _CFSearchBundleDirectoryWithPredicate(alloc, result, workingUniChars, workingLen, predicate, cheapStr, tmpString, stopLooking, version);
        return;
    }
    if (nameLen > 0) {
        // If we have a resName, just call the search API.  We may have to loop over the resTypes.
        if (!resTypes) {
            _CFSearchBundleDirectory(alloc, result, workingUniChars, workingLen, nameUniChars, nameLen, NULL, 0, cheapStr, tmpString, version);
        } else {
            CFArrayRef subResTypes = resTypes;
            Boolean releaseSubResTypes = false;
            CFIndex i, c = CFArrayGetCount(resTypes);
            if (c > 2) {
                // this is an optimization we employ when searching for large numbers of types, if the directory contents are available
                // we scan the directory contents and restrict the list of resTypes to the types that might actually occur with the specified name
                subResTypes = _CFCopyTypesForSearchBundleDirectory(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, cheapStr, tmpString, version);
                c = CFArrayGetCount(subResTypes);
                releaseSubResTypes = true;
            }
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
    
    // Now search the first localized resources (user language).
    CFIndex langCount = (searchLanguages ? CFArrayGetCount(searchLanguages) : 0);
    UniChar curLangUniChars[255];
    
    workingLen = savedWorkingLen;
    if (CFArrayGetCount(result) < limit && !stopLooking && langCount >= 1) {
        CFIndex numResults = CFArrayGetCount(result);
        CFStringRef curLangStr = (CFStringRef)CFArrayGetValueAtIndex(searchLanguages, 0);
        CFIndex curLangLen = MIN(CFStringGetLength(curLangStr), 255);
        CFStringGetCharacters(curLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
         
        savedWorkingLen = workingLen;
        if (_CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, curLangUniChars, curLangLen) &&
            _CFAppendPathExtension(workingUniChars, &workingLen, CFMaxPathSize, _LprojUniChars, _LprojLen) &&
            (subDirLen == 0 || (subDirLen > 0 && _CFAppendPathExtension(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen)))) {
            
            _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, &stopLooking, predicate, version, cheapStr, tmpString, result);
            
            if (CFArrayGetCount(result) != numResults) {
                // We found resources in a language we already searched.  Don't look any farther.
                // We also don't need to check the limit, since if the count changed at all, we are bailing.
                return;
            }
        }
    }
    
    workingLen = savedWorkingLen;
    // Now search the Base.lproj directory
    if (CFArrayGetCount(result) < limit && !stopLooking) {
        CFIndex numResults = CFArrayGetCount(result);
        savedWorkingLen = workingLen;
        if (_CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _BaseUniChars, _BaseLen) &&
            _CFAppendPathExtension(workingUniChars, &workingLen, CFMaxPathSize, _LprojUniChars, _LprojLen) &&
            (subDirLen == 0 || (subDirLen > 0 && _CFAppendPathExtension(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen)))) {
            
            _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, &stopLooking, predicate, version, cheapStr, tmpString, result);
            
            if (CFArrayGetCount(result) != numResults) {
                // We found resources in a language we already searched.  Don't look any farther.
                // We also don't need to check the limit, since if the count changed at all, we are bailing.
                return;
            }
        }
    }

    // Now search remaining localized resources (developer language)
    workingLen = savedWorkingLen;
    if (CFArrayGetCount(result) < limit && !stopLooking && langCount >= 2) {
        // MF:??? OK to hard-wire this length?
        CFIndex numResults = CFArrayGetCount(result);
        
        // start after 1st language
        for (CFIndex langIndex = 1; langIndex < langCount; langIndex++) {
            CFStringRef curLangStr = (CFStringRef)CFArrayGetValueAtIndex(searchLanguages, langIndex);
            CFIndex curLangLen = MIN(CFStringGetLength(curLangStr), 255);
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
        _CFBundleSetResourceDir(workingUniChars, &workingLen, CFMaxPathSize, version);

        // both of these used for temp string operations, for slightly different purposes, where each type is appropriate
        cheapStr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
        _CFStrSetDesiredCapacity(cheapStr, CFMaxPathSize);
        tmpString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);

        _CFFindBundleResourcesInResourcesDir(kCFAllocatorSystemDefault, workingUniChars, workingLen, subDirUniChars, subDirLen, searchLanguages, nameUniChars, nameLen, resTypes, limit, predicate, version, cheapStr, tmpString, result);
        
        CFRelease(cheapStr);
        CFRelease(tmpString);
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, nameUniChars);
    }
    if (resName) CFRelease(resName);
    if (basePath) CFRelease(basePath);
    return result;
}

__private_extern__ CFArrayRef _CFFindBundleResourcesNoBlock(CFBundleRef bundle, CFURLRef bundleURL, CFStringRef subDirName, CFArrayRef searchLanguages, CFStringRef resName, CFArrayRef resTypes, CFIndex limit, uint8_t version){
    return _CFFindBundleResources(bundle, bundleURL, subDirName, searchLanguages, resName, resTypes, limit, NULL, version);
}

CF_EXPORT CFURLRef CFBundleCopyResourceURL(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName) {
    if (!bundle) return NULL;
#ifdef CFBUNDLE_NEWLOOKUP
    CFURLRef result = (CFURLRef) _CFBundleCopyFindResources(bundle, NULL, NULL, resourceName, resourceType, subDirName, NULL, NO, NO, NULL);
    return result;
#else
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
#endif
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfType(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName) {
#ifdef CFBUNDLE_NEWLOOKUP
    if (!bundle) return CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFArrayRef result = (CFArrayRef) _CFBundleCopyFindResources(bundle, NULL, NULL, NULL, resourceType, subDirName, NULL, YES, NO, NULL);
    return result;
#else
    CFArrayRef languages = _CFBundleGetLanguageSearchList(bundle), types = NULL, array;
    if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
    // MF:!!! Better "limit" than 1,000,000?
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, NULL, types, 1000000, NULL, _CFBundleLayoutVersion(bundle));
    if (types) CFRelease(types);
    
    return array;
#endif
}

CF_EXPORT CFURLRef _CFBundleCopyResourceURLForLanguage(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {
    return CFBundleCopyResourceURLForLocalization(bundle, resourceName, resourceType, subDirName, language);
}

CF_EXPORT CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
#ifdef CFBUNDLE_NEWLOOKUP
    if (!bundle) return NULL;
    CFURLRef result = (CFURLRef) _CFBundleCopyFindResources(bundle, NULL, NULL, resourceName, resourceType, subDirName, localizationName, NO, YES, NULL);
    return result;
#else
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
#endif
}

CF_EXPORT CFArrayRef _CFBundleCopyResourceURLsOfTypeForLanguage(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {
    return CFBundleCopyResourceURLsOfTypeForLocalization(bundle, resourceType, subDirName, language);
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfTypeForLocalization(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
#ifdef CFBUNDLE_NEWLOOKUP
    if (!bundle) return CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFArrayRef result = (CFArrayRef) _CFBundleCopyFindResources(bundle, NULL, NULL, NULL, resourceType, subDirName, localizationName, YES, YES, NULL);
    return result;
#else
    CFArrayRef languages = NULL, types = NULL, array;

    if (localizationName) languages = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&localizationName, 1, &kCFTypeArrayCallBacks);
    if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
    // MF:!!! Better "limit" than 1,000,000?
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, NULL, types, 1000000, NULL, _CFBundleLayoutVersion(bundle));
    if (types) CFRelease(types);
    if (languages) CFRelease(languages);
    return array;
#endif
}


static Boolean CFBundleAllowMixedLocalizations(void);

CF_EXPORT CFStringRef CFBundleCopyLocalizedString(CFBundleRef bundle, CFStringRef key, CFStringRef value, CFStringRef tableName) {
    CFStringRef result = NULL;
    CFDictionaryRef stringTable = NULL;
    static CFSpinLock_t CFBundleLocalizedStringLock = CFSpinLockInit;

    if (!key) return (value ? (CFStringRef)CFRetain(value) : (CFStringRef)CFRetain(CFSTR("")));

    // Make sure to check the mixed localizations key early -- if the main bundle has not yet been cached, then we need to create the cache of the Info.plist before we start asking for resources (11172381)
    (void)CFBundleAllowMixedLocalizations();
    
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
        if (!value) {
            result = (CFStringRef)CFRetain(key);
        } else if (CFEqual(value, CFSTR(""))) {
            result = (CFStringRef)CFRetain(key);
        } else {
            result = (CFStringRef)CFRetain(value);
        }
        __block Boolean capitalize = false;
        if (capitalize) {
            CFMutableStringRef capitalizedResult = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, result);
            CFLog(__kCFLogBundle, CFSTR("Localizable string \"%@\" not found in strings table \"%@\" of bundle %@."), key, tableName, bundle);
            CFStringUppercase(capitalizedResult, NULL);
            CFRelease(result);
            result = capitalizedResult;
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
#ifdef CFBUNDLE_NEWLOOKUP
        result = (CFURLRef) _CFBundleCopyFindResources(NULL, bundleURL, NULL, resourceName, resourceType, subDirName, NULL, NO, NO, NULL);
#else
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
#endif
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
#ifdef CFBUNDLE_NEWLOOKUP
        array = (CFArrayRef) _CFBundleCopyFindResources(NULL, bundleURL, NULL, NULL, resourceType, subDirName, NULL, YES, NO, NULL);
#else
        uint8_t version = 0;
        CFArrayRef languages = _CFBundleCopyLanguageSearchListInDirectory(kCFAllocatorSystemDefault, newURL, &version), types = NULL;
        if (resourceType) types = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&resourceType, 1, &kCFTypeArrayCallBacks);
        // MF:!!! Better "limit" than 1,000,000?
        array = _CFFindBundleResources(NULL, newURL, subDirName, languages, NULL, types, 1000000, NULL, version);
        if (types) CFRelease(types);
        if (languages) CFRelease(languages);
#endif
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


__private_extern__ CFArrayRef _CFBundleCopyUserLanguages(Boolean useBackstops) {
    static CFArrayRef _CFBundleUserLanguages = NULL;
    static dispatch_once_t once = 0;
    dispatch_once(&once, ^{
        CFArrayRef preferencesArray = NULL;
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
        if (preferencesArray) CFRelease(preferencesArray);
    });
    
    if (_CFBundleUserLanguages) {
        CFRetain(_CFBundleUserLanguages);
        return _CFBundleUserLanguages;
    } else {
        return NULL;
    }
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
    CFArrayRef contents;
    CFRange contentsRange;

    // both of these used for temp string operations, for slightly
    // different purposes, where each type is appropriate
    cheapStr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    _CFStrSetDesiredCapacity(cheapStr, CFMaxPathSize);
    tmpString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);    
    
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    contents = _CFBundleCopySortedDirectoryContentsAtPath(cheapStr, _CFBundleAllContents);
    contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
    
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
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, curLangStr)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), curLangStr)) CFArrayAppendValue(lprojNames, curLangStr);
            foundOne = true;
            if (CFStringGetLength(curLangStr) <= 2) {
                CFRelease(cheapStr);
                CFRelease(tmpString);
                CFRelease(contents);
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
        CFRelease(contents);
        return foundOne;
    }
    if (altLangStr) {
        curLangLen = CFStringGetLength(altLangStr);
        if (curLangLen > 255) curLangLen = 255;
        CFStringGetCharacters(altLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, altLangStr)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
                if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), altLangStr)) CFArrayAppendValue(lprojNames, altLangStr);
                foundOne = true;
                CFRelease(cheapStr);
                CFRelease(tmpString);
                CFRelease(contents);
                return foundOne;
            }
        }
    }
    if (!foundOne && (!predefinedLocalizations || CFArrayGetCount(predefinedLocalizations) == 0)) {
        Boolean hasLocalizations = false;
        CFIndex idx;
        for (idx = 0; !hasLocalizations && idx < contentsRange.length; idx++) {
            CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(contents, idx);
            if (CFStringHasSuffix(name, _CFBundleLprojExtensionWithDot)) hasLocalizations = true;
        }
        if (!hasLocalizations) {
            CFRelease(cheapStr);
            CFRelease(tmpString);
            CFRelease(contents);
            return foundOne;
        }
    }
    if (!altLangStr && (modifiedLangStr = _CFBundleCopyModifiedLocalization(curLangStr))) {
        curLangLen = CFStringGetLength(modifiedLangStr);
        if (curLangLen > 255) curLangLen = 255;
        CFStringGetCharacters(modifiedLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        if (_CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen) && _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen)) {
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, modifiedLangStr)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
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
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageAbbreviation)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
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
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageName)) || (version != 4 && _CFBundleSortedArrayContains(contents, cheapStr))) {
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
    CFRelease(contents);
    return foundOne;
}

static Boolean CFBundleAllowMixedLocalizations(void) {
    static Boolean allowMixed = false;
    static dispatch_once_t once = 0;
    dispatch_once(&once, ^{
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
    });
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
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
    CFStringRef directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFArrayRef contents = _CFBundleCopySortedDirectoryContentsAtPath(directoryPath, _CFBundleAllContents);
    Boolean hasFrameworkSuffix = CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework/"));
#if DEPLOYMENT_TARGET_WINDOWS
    hasFrameworkSuffix = hasFrameworkSuffix || CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework\\"));
#endif
    
    if (hasFrameworkSuffix) {
        if (_CFBundleSortedArrayContains(contents, _CFBundleResourcesDirectoryName)) localVersion = 0;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName2)) localVersion = 2;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName1)) localVersion = 1;
    } else {
        if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName2)) localVersion = 2;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleResourcesDirectoryName)) localVersion = 0;
        else if (_CFBundleSortedArrayContains(contents, _CFBundleSupportFilesDirectoryName1)) localVersion = 1;
    }
    if (contents) CFRelease(contents);
    if (directoryPath) CFRelease(directoryPath);
    if (absoluteURL) CFRelease(absoluteURL);
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_WINDOWS
    if (localVersion == 3) {
        if (hasFrameworkSuffix) {
            if (_CFBundleURLHasSubDir(url, _CFBundleResourcesURLFromBase0)) localVersion = 0;
            else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase2)) localVersion = 2;
            else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase1)) localVersion = 1;
        } else {
            if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase2)) localVersion = 2;
            else if (_CFBundleURLHasSubDir(url, _CFBundleResourcesURLFromBase0)) localVersion = 0;
            else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase1)) localVersion = 1;
        }
    }
#endif
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
#if __CONSTANT_STRINGS__
#define _CFBundleNumberOfBlacklistedInfoDictionaryKeys 2
    static const CFStringRef _CFBundleBlacklistedInfoDictionaryKeys[_CFBundleNumberOfBlacklistedInfoDictionaryKeys] = { CFSTR("CFBundleExecutable"), CFSTR("CFBundleIdentifier") };
    
    for (CFIndex idx = 0; idx < _CFBundleNumberOfBlacklistedInfoDictionaryKeys; idx++) {
        if (CFEqual(keyName, _CFBundleBlacklistedInfoDictionaryKeys[idx])) return true;
    }
#endif
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
        CFURLRef directoryURL = NULL, absoluteURL;
        CFStringRef directoryPath;
        CFArrayRef contents = NULL;
        CFRange contentsRange = CFRangeMake(0, 0);

        _CFEnsureStaticBuffersInited();

        if (0 == version) {
            directoryURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundleResourcesURLFromBase0, url);
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension0;
            infoURLFromBase = _CFBundleInfoURLFromBase0;
        } else if (1 == version) {
            directoryURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundleSupportFilesURLFromBase1, url);
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension1;
            infoURLFromBase = _CFBundleInfoURLFromBase1;
        } else if (2 == version) {
            directoryURL = CFURLCreateWithString(kCFAllocatorSystemDefault, _CFBundleSupportFilesURLFromBase2, url);
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension2;
            infoURLFromBase = _CFBundleInfoURLFromBase2;
        } else if (3 == version) {
            CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
            // this test is necessary to exclude the case where a bundle is spuriously created from the innards of another bundle
            if (path) {
                if (!(CFStringHasSuffix(path, _CFBundleSupportFilesDirectoryName1) || CFStringHasSuffix(path, _CFBundleSupportFilesDirectoryName2) || CFStringHasSuffix(path, _CFBundleResourcesDirectoryName))) {
                    directoryURL = (CFURLRef)CFRetain(url);
                    infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension3;
                    infoURLFromBase = _CFBundleInfoURLFromBase3;
                }
                CFRelease(path);
            }
        }
        if (directoryURL) {
            absoluteURL = CFURLCopyAbsoluteURL(directoryURL);
            directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
            contents = _CFBundleCopySortedDirectoryContentsAtPath(directoryPath, _CFBundleAllContents);
            contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
            if (directoryPath) CFRelease(directoryPath);
            if (absoluteURL) CFRelease(absoluteURL);
            if (directoryURL) CFRelease(directoryURL);
        }

        len = CFStringGetLength(infoURLFromBaseNoExtension);
        CFStringGetCharacters(infoURLFromBaseNoExtension, CFRangeMake(0, len), buff);
        buff[len++] = (UniChar)'-';
        memmove(buff + len, _PlatformUniChars, _PlatformLen * sizeof(UniChar));
        len += _PlatformLen;
        _CFAppendPathExtension(buff, &len, CFMaxPathSize, _InfoExtensionUniChars, _InfoExtensionLen);
        cheapStr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
        CFStringAppendCharacters(cheapStr, buff, len);
        infoURL = CFURLCreateWithString(kCFAllocatorSystemDefault, cheapStr, url);
        if (contents) {
            CFIndex resourcesLen, idx;
            for (resourcesLen = len; resourcesLen > 0; resourcesLen--) if (buff[resourcesLen - 1] == PATH_SEP) break;
            CFStringDelete(cheapStr, CFRangeMake(0, CFStringGetLength(cheapStr)));
            CFStringAppendCharacters(cheapStr, buff + resourcesLen, len - resourcesLen);
            for (tryPlatformSpecific = false, idx = 0; !tryPlatformSpecific && idx < contentsRange.length; idx++) {
                // Need to do this case-insensitive to accommodate Palm
                if (kCFCompareEqualTo == CFStringCompare(cheapStr, (CFStringRef)CFArrayGetValueAtIndex(contents, idx), kCFCompareCaseInsensitive)) tryPlatformSpecific = true;
            }
        }
        if (tryPlatformSpecific) CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, infoURL, &infoData, NULL, NULL, NULL);
        //fprintf(stderr, "looking for ");CFShow(infoURL);fprintf(stderr, infoData ? "found it\n" : (tryPlatformSpecific ? "missed it\n" : "skipped it\n"));
        CFRelease(cheapStr);
        if (!infoData) {
            // Check for global Info.plist
            CFRelease(infoURL);
            infoURL = CFURLCreateWithString(kCFAllocatorSystemDefault, infoURLFromBase, url);
            if (contents) {
                CFIndex idx;
                for (tryGlobal = false, idx = 0; !tryGlobal && idx < contentsRange.length; idx++) {
                    // Need to do this case-insensitive to accommodate Palm
                    if (kCFCompareEqualTo == CFStringCompare(_CFBundleInfoFileName, (CFStringRef)CFArrayGetValueAtIndex(contents, idx), kCFCompareCaseInsensitive)) tryGlobal = true;
                }
            }
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
        if (contents) CFRelease(contents);
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
                if ((strLen - startOfExtension == 4 || strLen - startOfExtension == 5) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'a' && buff[startOfExtension+2] == (UniChar)'p' && buff[startOfExtension+3] == (UniChar)'p' && (strLen - startOfExtension == 4 || buff[startOfExtension+4] == (UniChar)PATH_SEP)) {
                    // This is an app
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 6 || strLen - startOfExtension == 7) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'d' && buff[startOfExtension+2] == (UniChar)'e' && buff[startOfExtension+3] == (UniChar)'b' && buff[startOfExtension+4] == (UniChar)'u' && buff[startOfExtension+5] == (UniChar)'g' && (strLen - startOfExtension == 6 || buff[startOfExtension+6] == (UniChar)PATH_SEP)) {
                    // This is an app (debug version)
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 8 || strLen - startOfExtension == 9) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'p' && buff[startOfExtension+2] == (UniChar)'r' && buff[startOfExtension+3] == (UniChar)'o' && buff[startOfExtension+4] == (UniChar)'f' && buff[startOfExtension+5] == (UniChar)'i' && buff[startOfExtension+6] == (UniChar)'l' && buff[startOfExtension+7] == (UniChar)'e' && (strLen - startOfExtension == 8 || buff[startOfExtension+8] == (UniChar)PATH_SEP)) {
                    // This is an app (profile version)
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 8 || strLen - startOfExtension == 9) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'s' && buff[startOfExtension+2] == (UniChar)'e' && buff[startOfExtension+3] == (UniChar)'r' && buff[startOfExtension+4] == (UniChar)'v' && buff[startOfExtension+5] == (UniChar)'i' && buff[startOfExtension+6] == (UniChar)'c' && buff[startOfExtension+7] == (UniChar)'e' && (strLen - startOfExtension == 8 || buff[startOfExtension+8] == (UniChar)PATH_SEP)) {
                    // This is a service
                    *packageType = 0x4150504c;  // 'APPL'
                } else if ((strLen - startOfExtension == 10 || strLen - startOfExtension == 11) && buff[startOfExtension] == (UniChar)'.' && buff[startOfExtension+1] == (UniChar)'f' && buff[startOfExtension+2] == (UniChar)'r' && buff[startOfExtension+3] == (UniChar)'a' && buff[startOfExtension+4] == (UniChar)'m' && buff[startOfExtension+5] == (UniChar)'e' && buff[startOfExtension+6] == (UniChar)'w' && buff[startOfExtension+7] == (UniChar)'o' && buff[startOfExtension+8] == (UniChar)'r' && buff[startOfExtension+9] == (UniChar)'k' && (strLen - startOfExtension == 10 || buff[startOfExtension+10] == (UniChar)PATH_SEP)) {
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
#elif DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
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
    CFURLRef absoluteURL;
    CFStringRef directoryPath;
    CFArrayRef contents;
    CFRange contentsRange;
    CFIndex idx;
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

    if (resourcesURL) {
        absoluteURL = CFURLCopyAbsoluteURL(resourcesURL);
        directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
        contents = _CFBundleCopySortedDirectoryContentsAtPath(directoryPath, _CFBundleAllContents);
        contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
        for (idx = 0; idx < contentsRange.length; idx++) {
            CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(contents, idx);
            if (CFStringHasSuffix(name, _CFBundleLprojExtensionWithDot)) {
                CFStringRef localization = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, name, CFRangeMake(0, CFStringGetLength(name) - 6));
                if (!result) result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
                CFArrayAppendValue(result, localization);
                CFRelease(localization);
            }
        }
        if (contents) CFRelease(contents);
        if (directoryPath) CFRelease(directoryPath);
        if (absoluteURL) CFRelease(absoluteURL);
    }
    
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

 
    
CF_INLINE Boolean _CFBundleFindCharacterInStr(const UniChar *str, UniChar c, Boolean backward, CFIndex start, CFIndex length, CFRange *result){
    *result = CFRangeMake(kCFNotFound, 0);
    Boolean found = false;
    if (backward) {
        for (CFIndex i = start; i > start-length; i--) {
            if (c == str[i]) {
                result->location = i;
                found = true;
                break;
            }
        }
    } else {
        for (CFIndex i = start; i < start+length; i++) {
            if (c == str[i]) {
                result->location = i;
                found = true;
                break;
            }
        }
    }
    return found;
}


typedef enum {
    _CFBundleFileVersionNoProductNoPlatform = 1,
    _CFBundleFileVersionWithProductNoPlatform,
    _CFBundleFileVersionNoProductWithPlatform,
    _CFBundleFileVersionWithProductWithPlatform,
    _CFBundleFileVersionUnmatched
} _CFBundleFileVersion;

static _CFBundleFileVersion _CFBundleCheckFileProductAndPlatform(CFStringRef file, UniChar *fileBuffer1, CFIndex fileLen, CFRange searchRange, CFStringRef product, CFStringRef platform, CFRange* prodp, CFRange* platp, CFIndex prodLen)
{
    _CFBundleFileVersion version;
    CFRange found;
    Boolean foundprod, foundplat;
    foundplat = foundprod = NO;
    UniChar fileBuffer2[CFMaxPathSize];
    UniChar *fileBuffer;
    Boolean wrong = false;
    
    if (fileBuffer1) {
        fileBuffer = fileBuffer1;
    }else{
        fileLen = CFStringGetLength(file);
        if (fileLen > CFMaxPathSize) fileLen = CFMaxPathSize;
        CFStringGetCharacters(file, CFRangeMake(0, fileLen), fileBuffer2);
        fileBuffer = fileBuffer2;
    }
        
    if (_CFBundleFindCharacterInStr(fileBuffer, '~', NO, searchRange.location, searchRange.length, &found)) {
        if (prodLen != 1) {
            if (CFStringFindWithOptions(file, product, searchRange, kCFCompareEqualTo, prodp)) {
                foundprod = YES;
            }
        }
        if (!foundprod) {
            for (CFIndex i = 0; i < _CFBundleNumberOfProducts; i++) {
                if (CFStringFindWithOptions(file, _CFBundleSupportedProducts[i], searchRange, kCFCompareEqualTo, &found)) {
                    wrong = true;
                    break;
                }
            }
        }
    }

    if (!wrong && _CFBundleFindCharacterInStr(fileBuffer, '-', NO, searchRange.location, searchRange.length, &found)) {
        if (CFStringFindWithOptions(file, platform, searchRange, kCFCompareEqualTo, platp)) {
            foundplat = YES;
        }
        if (!foundplat) {
            for (CFIndex i = 0; i < _CFBundleNumberOfPlatforms; i++) {
                if (CFStringFindWithOptions(file, _CFBundleSupportedPlatforms[i], searchRange, kCFCompareEqualTo, &found)) {
                    wrong = true;
                    break;
                }
            }
        }
    }
    
    if (wrong) {
        version = _CFBundleFileVersionUnmatched;
    } else if (foundplat && foundprod) {
        version = _CFBundleFileVersionWithProductWithPlatform;
    } else if (foundplat) {
        version = _CFBundleFileVersionNoProductWithPlatform;
    } else if (foundprod) {
        version = _CFBundleFileVersionWithProductNoPlatform;
    } else {
        version = _CFBundleFileVersionNoProductNoPlatform;
    }
    return version;
}

    
// ZFH
    
    
static void _CFBundleAddValueForType(CFMutableStringRef type, UniChar* fileNameBuffer, CFMutableDictionaryRef queryTable, CFRange dotPosition, CFIndex fileLen, CFMutableDictionaryRef typeDir, CFTypeRef value, CFMutableDictionaryRef addedTypes, Boolean firstLproj){
    CFIndex typeLen = fileLen - dotPosition.location - 1;
    CFStringSetExternalCharactersNoCopy(type, fileNameBuffer+dotPosition.location+1, typeLen, typeLen);
    CFMutableArrayRef tFiles = (CFMutableArrayRef) CFDictionaryGetValue(typeDir, type);
    if (!tFiles) {
        CFStringRef key = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%@.%@"), _CFBundleTypeIndicator, type);
        tFiles = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(queryTable, key, tFiles);
        CFDictionarySetValue(typeDir, type, tFiles);
        CFRelease(tFiles);
        CFRelease(key);
    }
    if (!addedTypes) {
        CFArrayAppendValue(tFiles, value);        
    } else if (firstLproj) {
        CFDictionarySetValue(addedTypes, type, type);
        CFArrayAppendValue(tFiles, value);
    } else if (!(CFDictionaryGetValue(addedTypes, type))) {
        CFArrayAppendValue(tFiles, value);
    }
}

static Boolean _CFBundleReadDirectory(CFStringRef pathOfDir, CFBundleRef bundle, CFURLRef bundleURL, UniChar *resDir, UniChar *subDir, CFIndex subDirLen, CFMutableArrayRef allFiles, Boolean hasFileAdded, CFMutableStringRef type, CFMutableDictionaryRef queryTable, CFMutableDictionaryRef typeDir, CFMutableDictionaryRef addedTypes, Boolean firstLproj, CFStringRef product, CFStringRef platform, CFStringRef lprojName, Boolean appendLprojCharacters) {

    Boolean result = true;
    
    const CFIndex cPathBuffLen = CFStringGetMaximumSizeOfFileSystemRepresentation(pathOfDir) + 1;
    const CFIndex valueBufferLen = cPathBuffLen + 1 + CFMaxPathSize;
    UniChar *valueBuff = (UniChar *) CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * valueBufferLen, 0);
    CFMutableStringRef valueStr = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);
    CFIndex pathOfDirWithSlashLen = 0; 
    
    CFIndex productLen = CFStringGetLength(product);
    CFIndex platformLen = CFStringGetLength(platform);
    
    if (lprojName) {
        // valueBuff is allocated with the actual length of lprojTarget
        CFRange lprojRange = CFRangeMake(0, CFStringGetLength(lprojName));
        CFStringGetCharacters(lprojName, lprojRange, valueBuff);
        pathOfDirWithSlashLen += lprojRange.length;
        if (appendLprojCharacters) _CFAppendPathExtension(valueBuff, &pathOfDirWithSlashLen, valueBufferLen, _LprojUniChars, _LprojLen);
        _CFAppendTrailingPathSlash(valueBuff, &pathOfDirWithSlashLen, valueBufferLen);
    }
    
    if (subDirLen) {
        memmove(valueBuff+pathOfDirWithSlashLen, subDir, subDirLen*sizeof(UniChar));
        pathOfDirWithSlashLen += subDirLen;
        if (subDir[subDirLen-1] != _CFGetSlash()) {
            _CFAppendTrailingPathSlash(valueBuff, &pathOfDirWithSlashLen, valueBufferLen);
        }
    }
    
    UniChar *fileNameBuffer = valueBuff + pathOfDirWithSlashLen;
    char *cPathBuff = (char *)malloc(sizeof(char) * cPathBuffLen);

    if (CFStringGetFileSystemRepresentation(pathOfDir, cPathBuff, cPathBuffLen)) {
// this is a fix for traversing ouside of a bundle security issue: 8302591
// it will be enabled after the bug 10956699 gets fixed
#ifdef CFBUNDLE_NO_TRAVERSE_OUTSIDE
#endif // CFBUNDLE_NO_TRAVERSE_OUTSIDE
        
#if DEPLOYMENT_TARGET_WINDOWS
        wchar_t pathBuf[CFMaxPathSize];
        CFStringRef pathInUTF8 = CFStringCreateWithCString(kCFAllocatorSystemDefault, cPathBuff, kCFStringEncodingUTF8);
        CFIndex pathInUTF8Len = CFStringGetLength(pathInUTF8);
        if (pathInUTF8Len > CFMaxPathSize) pathInUTF8Len = CFMaxPathSize;
        
        CFStringGetCharacters(pathInUTF8, CFRangeMake(0, pathInUTF8Len), (UniChar *)pathBuf);
        pathBuf[pathInUTF8Len] = 0;
        CFRelease(pathInUTF8);
        WIN32_FIND_DATAW filePt;
        HANDLE handle;
        
        if (pathInUTF8Len + 2 >= CFMaxPathLength) {
            result = false;
        }
        
        pathBuf[pathInUTF8Len] = '\\';
        pathBuf[pathInUTF8Len + 1] = '*';
        pathBuf[pathInUTF8Len + 2] = '\0';
        handle = FindFirstFileW(pathBuf, (LPWIN32_FIND_DATAW)&filePt);
        if (INVALID_HANDLE_VALUE == handle) {
            pathBuf[pathInUTF8Len] = '\0';
            result = false;
        }
        if (!result) {
            free(cPathBuff);
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, valueBuff);
            CFRelease(valueStr);
            return result;
        }
        
        do {
            CFIndex nameLen = wcslen(filePt.cFileName);
            if (filePt.cFileName[0] == '.' && (nameLen == 1 || (nameLen == 2  && filePt.cFileName[1] == '.'))) {
                continue;
            }
            CFStringRef file = CFStringCreateWithBytes(kCFAllocatorSystemDefault, (const uint8_t *)filePt.cFileName, nameLen * sizeof(wchar_t), kCFStringEncodingUTF16, NO);
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
        DIR *dirp = NULL;
        struct dirent* dent;
        if ( result && (dirp = opendir(cPathBuff))) {
            
            while ((dent = readdir(dirp))) {

#if DEPLOYMENT_TARGET_LINUX
                CFIndex nameLen = strlen(dent->d_name);
#else
                CFIndex nameLen = dent->d_namlen;
#endif
                if (0 == nameLen || 0 == dent->d_fileno || ('.' == dent->d_name[0] && (1 == nameLen || (2 == nameLen && '.' == dent->d_name[1]) || '_' == dent->d_name[1]))) 
                    continue;
                
                CFStringRef file = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, dent->d_name);
#else
#error unknown architecture, not implemented
#endif
                if (file) {
                    
                    CFIndex fileNameLen = CFStringGetLength(file);
                    if (fileNameLen > CFMaxPathSize) fileNameLen = CFMaxPathSize;
                    CFStringGetCharacters(file, CFRangeMake(0, fileNameLen), fileNameBuffer);
                    CFIndex valueTotalLen = pathOfDirWithSlashLen + fileNameLen;
                    
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
                    // construct the path for a file, which is the value in the query table
                    // if it is a dir
                    if (dent->d_type == DT_DIR) {
                        _CFAppendTrailingPathSlash(valueBuff, &valueTotalLen, valueBufferLen);
                    } else if (dent->d_type == DT_UNKNOWN) {
                        Boolean isDir = false;
                        char subdirPath[CFMaxPathLength];
                        struct stat statBuf;
                        strlcpy(subdirPath, cPathBuff, sizeof(subdirPath));
                        strlcat(subdirPath, "/", sizeof(subdirPath));
                        strlcat(subdirPath, dent->d_name, sizeof(subdirPath));
                        if (stat(subdirPath, &statBuf) == 0) {
                             isDir = ((statBuf.st_mode & S_IFMT) == S_IFDIR);
                        }
                        if (isDir) {
                            _CFAppendTrailingPathSlash(valueBuff, &valueTotalLen, valueBufferLen);
                        }
                    } 
#elif DEPLOYMENT_TARGET_WINDOWS
                    if ((filePt.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        _CFAppendTrailingPathSlash(valueBuff, &valueTotalLen, valueBufferLen);   
                    }
#endif
                    CFStringSetExternalCharactersNoCopy(valueStr, valueBuff, valueTotalLen, valueBufferLen);
                    CFTypeRef value = CFStringCreateCopy(kCFAllocatorSystemDefault, valueStr);
                    
                    // put it into all file array
                    if (!hasFileAdded) {
                        CFArrayAppendValue(allFiles, value);
                    }
                    
                    // put it into type array
                    // search the type from the end
                    CFRange backDotPosition, dotPosition; 
                    Boolean foundDot = _CFBundleFindCharacterInStr(fileNameBuffer, '.', YES, fileNameLen-1, fileNameLen, &backDotPosition);

                    if (foundDot && backDotPosition.location != (fileNameLen-1)) {
                        _CFBundleAddValueForType(type, fileNameBuffer, queryTable, backDotPosition, fileNameLen, typeDir, value, addedTypes, firstLproj);
                    }
                    
                    // search the type from the beginning
                    //CFRange dotPosition = CFStringFind(file, _CFBundleDot, kCFCompareEqualTo);
                    foundDot = _CFBundleFindCharacterInStr(fileNameBuffer, '.', NO, 0, fileNameLen, &dotPosition);
                    if (dotPosition.location != backDotPosition.location && foundDot) {
                        _CFBundleAddValueForType(type, fileNameBuffer, queryTable, dotPosition, fileNameLen, typeDir, value, addedTypes, firstLproj);
                    }
                    
                    // check if the file is product and platform specific
                    CFRange productRange, platformRange;
                    _CFBundleFileVersion fileVersion = _CFBundleCheckFileProductAndPlatform(file, fileNameBuffer, fileNameLen, CFRangeMake(0, fileNameLen), product, platform, &productRange, &platformRange, productLen);
                    
                    if (fileVersion == _CFBundleFileVersionNoProductNoPlatform || fileVersion == _CFBundleFileVersionUnmatched) {                        
                        // No product/no platform, or unmatched files get added directly to the query table.
                        CFStringRef prevPath = (CFStringRef)CFDictionaryGetValue(queryTable, file);
                        if (!prevPath) {
                            CFDictionarySetValue(queryTable, file, value);
                        }
                    } else {
                        // If the file has a product or platform extension, we add the full name to the query table so that it may be found using that name.
                        // Then we add the more specific name as well.
                        CFDictionarySetValue(queryTable, file, value);
                        
                        CFIndex searchOffset = platformLen;
                        CFStringRef key = NULL;
                        
                        // set the key accordining to the version of the file (product and platform)
                        switch (fileVersion) {
                            case _CFBundleFileVersionWithProductNoPlatform:
                                platformRange = productRange;
                                searchOffset = productLen;
                            case _CFBundleFileVersionNoProductWithPlatform:
                            case _CFBundleFileVersionWithProductWithPlatform:
                                foundDot = _CFBundleFindCharacterInStr(fileNameBuffer, '.', NO, platformRange.location+searchOffset, fileNameLen-platformRange.location-searchOffset, &dotPosition);
                                if (foundDot) {
                                    CFMutableStringRef mutableKey = CFStringCreateMutable(kCFAllocatorSystemDefault, platformRange.location + (fileNameLen - dotPosition.location));
                                    CFStringAppendCharacters(mutableKey, fileNameBuffer, platformRange.location);
                                    CFStringAppendCharacters(mutableKey, fileNameBuffer+dotPosition.location, fileNameLen - dotPosition.location);
                                    key = (CFStringRef)mutableKey;
                                } else {
                                    key = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, fileNameBuffer, platformRange.location);
                                }
                                break;
                            default:
                                CFLog(kCFLogLevelError, CFSTR("CFBundle: Unknown kind of file (%d) when creating CFBundle: %@"), pathOfDir);
                                break;
                        }
                        
                        if (key) {
                            // add the path of the key into the query table
                            CFStringRef prevPath = (CFStringRef) CFDictionaryGetValue(queryTable, key);
                            if (!prevPath) {
                                CFDictionarySetValue(queryTable, key, value);
                            } else {
                                if (!lprojName || CFStringHasPrefix(prevPath, lprojName)) {
                                    // we need to know the version of exisiting path to see if we can replace it by the current path
                                    CFRange searchRange;
                                    if (lprojName) {
                                        searchRange.location = CFStringGetLength(lprojName);
                                        searchRange.length = CFStringGetLength(prevPath) - searchRange.location;
                                    } else {
                                        searchRange.location = 0;
                                        searchRange.length = CFStringGetLength(prevPath);
                                    }
                                    _CFBundleFileVersion prevFileVersion = _CFBundleCheckFileProductAndPlatform(prevPath, NULL, 0, searchRange, product, platform, &productRange, &platformRange, productLen);
                                    switch (prevFileVersion) {
                                        case _CFBundleFileVersionNoProductNoPlatform:
                                            CFDictionarySetValue(queryTable, key, value);
                                            break;
                                        case _CFBundleFileVersionWithProductNoPlatform:
                                            if (fileVersion == _CFBundleFileVersionWithProductWithPlatform) CFDictionarySetValue(queryTable, key, value);
                                            break;
                                        case _CFBundleFileVersionNoProductWithPlatform:
                                            CFDictionarySetValue(queryTable, key, value);
                                            break;
                                        default:
                                            break;
                                    }
                                }
                            }
                            
                            CFRelease(key);
                        }
                    }
                    
                    CFRelease(value);
                    CFRelease(file);
                }
                
                
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
            }
            closedir(dirp);
        } else { // opendir
            result = false;
        }            
#elif DEPLOYMENT_TARGET_WINDOWS
        } while ((FindNextFileW(handle, &filePt)));    
        FindClose(handle);
        pathBuf[pathInUTF8Len] = '\0';
#endif

    } else { // the path counld not be resolved to be a file system representation
        result = false;
    }
    
    free(cPathBuff);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, valueBuff);
    CFRelease(valueStr);
    return result;
}

__private_extern__ CFDictionaryRef _CFBundleCreateQueryTableAtPath(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, UniChar *resDir, CFIndex resDirLen, UniChar *subDir, CFIndex subDirLen)
{
    const CFIndex pathBufferSize = 2*CFMaxPathSize+resDirLen+subDirLen+2;
    UniChar *pathBuffer = (UniChar *) CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * pathBufferSize, 0);
    
    CFMutableDictionaryRef queryTable = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef allFiles = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef typeDir = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableStringRef type = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull); 
    
    CFStringRef productName = _CFGetProductName();//CFSTR("iphone");
    CFStringRef platformName = _CFGetPlatformName();//CFSTR("iphoneos");
    if (CFEqual(productName, CFSTR("ipod"))) {
        productName = CFSTR("iphone");
    }
    CFStringRef product = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("~%@"), productName);
    CFStringRef platform = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("-%@"), platformName);
    
    CFStringRef bundlePath = NULL;
    if (bundle) {
        bundlePath = _CFBundleGetBundlePath(bundle);
        CFRetain(bundlePath);
    } else {
        CFURLRef url = CFURLCopyAbsoluteURL(bundleURL);
        bundlePath = CFURLCopyFileSystemPath(url, PLATFORM_PATH_STYLE);
        CFRelease(url);
    }
    // bundlePath is an actual path, so it should not have a length greater than CFMaxPathSize
    CFIndex pathLen = CFStringGetLength(bundlePath);
    CFStringGetCharacters(bundlePath, CFRangeMake(0, pathLen), pathBuffer);
    CFRelease(bundlePath);
    
    Boolean appendSucc = true;
    if (resDirLen > 0) { // should not fail, buffer has enought space
        appendSucc = _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, resDir, resDirLen);
    }
    
    CFStringRef basePath = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, pathBuffer, pathLen);
    
    if (subDirLen > 0) { // should not fail, buffer has enought space
        appendSucc = _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, subDir, subDirLen);
    }
    
    CFStringRef pathToRead = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, pathBuffer, pathLen);
    
    // read the content in sub dir and put them into query table
    _CFBundleReadDirectory(pathToRead, bundle, bundleURL, resDir, subDir, subDirLen, allFiles, false, type, queryTable, typeDir, NULL, false, product, platform, NULL, false);
    
    CFRelease(pathToRead);
    
    CFIndex numOfAllFiles = CFArrayGetCount(allFiles);
    
    if (bundle && !languages) {
        languages = _CFBundleGetLanguageSearchList(bundle);
    }
    CFIndex numLprojs = languages ? CFArrayGetCount(languages) : 0;
    CFMutableDictionaryRef addedTypes = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    
    CFIndex basePathLen = CFStringGetLength(basePath);
    Boolean hasFileAdded = false;
    Boolean firstLproj = true;

    // First, search lproj for user's chosen language
    if (numLprojs >= 1) {
        CFStringRef lprojTarget = (CFStringRef)CFArrayGetValueAtIndex(languages, 0);
        // lprojTarget is from _CFBundleGetLanguageSearchList, so it should not have a length greater than CFMaxPathSize
        UniChar lprojBuffer[CFMaxPathSize];
        CFIndex lprojLen = CFStringGetLength(lprojTarget);
        CFStringGetCharacters(lprojTarget, CFRangeMake(0, lprojLen), lprojBuffer);
        
        pathLen = basePathLen;
        _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, lprojBuffer, lprojLen);
        _CFAppendPathExtension(pathBuffer, &pathLen, pathBufferSize, _LprojUniChars, _LprojLen);
        if (subDirLen > 0) {
            _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, subDir, subDirLen);
        }
        pathToRead = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, pathBuffer, pathLen);        
        _CFBundleReadDirectory(pathToRead, bundle, bundleURL, resDir, subDir, subDirLen, allFiles, hasFileAdded, type, queryTable, typeDir, addedTypes, firstLproj, product, platform, lprojTarget, true);
        CFRelease(pathToRead);
        
        if (!hasFileAdded && numOfAllFiles < CFArrayGetCount(allFiles)) {
            hasFileAdded = true;
        }
        firstLproj = false;
    }
    
    // Next, search Base.lproj folder
    pathLen = basePathLen;
    _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, _BaseUniChars, _BaseLen);
    _CFAppendPathExtension(pathBuffer, &pathLen, pathBufferSize, _LprojUniChars, _LprojLen);
    if (subDirLen > 0) {
        _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, subDir, subDirLen);
    }
    pathToRead = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, pathBuffer, pathLen);
    _CFBundleReadDirectory(pathToRead, bundle, bundleURL, resDir, subDir, subDirLen, allFiles, hasFileAdded, type, queryTable, typeDir, addedTypes, YES, product, platform, _CFBundleBaseDirectory, true);
    CFRelease(pathToRead);
    
    if (!hasFileAdded && numOfAllFiles < CFArrayGetCount(allFiles)) {
        hasFileAdded = true;
    }

    // Finally, search remaining languages (development language first)
    if (numLprojs >= 2) {
        // for each lproj we are interested in, read the content and put them into query table
        for (CFIndex i = 1; i < CFArrayGetCount(languages); i++) {
            CFStringRef lprojTarget = (CFStringRef) CFArrayGetValueAtIndex(languages, i);
            // lprojTarget is from _CFBundleGetLanguageSearchList, so it should not have a length greater than CFMaxPathSize
            UniChar lprojBuffer[CFMaxPathSize];
            CFIndex lprojLen = CFStringGetLength(lprojTarget);
            CFStringGetCharacters(lprojTarget, CFRangeMake(0, lprojLen), lprojBuffer);
            
            pathLen = basePathLen;
            _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, lprojBuffer, lprojLen);
            _CFAppendPathExtension(pathBuffer, &pathLen, pathBufferSize, _LprojUniChars, _LprojLen);
            if (subDirLen > 0) {
                _CFAppendPathComponent(pathBuffer, &pathLen, pathBufferSize, subDir, subDirLen);
            }
            pathToRead = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, pathBuffer, pathLen);
            _CFBundleReadDirectory(pathToRead, bundle, bundleURL, resDir, subDir, subDirLen, allFiles, hasFileAdded, type, queryTable, typeDir, addedTypes, false, product, platform, lprojTarget, true);            
            CFRelease(pathToRead);
            
            if (!hasFileAdded && numOfAllFiles < CFArrayGetCount(allFiles)) {
                hasFileAdded = true;
            }
        }
    }
    
    CFRelease(addedTypes);
    
    // put the array of all files in sub dir to the query table
    if (CFArrayGetCount(allFiles) > 0) {
        CFDictionarySetValue(queryTable, _CFBundleAllFiles, allFiles);
    }
    
    CFRelease(platform);
    CFRelease(product);
    CFRelease(allFiles);
    CFRelease(typeDir);
    CFRelease(type);
    CFRelease(basePath);
    
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, pathBuffer);
    return queryTable;
}   
    
static CFURLRef _CFBundleCreateURLFromPath(CFStringRef path, UniChar slash, UniChar *urlBuffer, CFIndex urlBufferLen, CFMutableStringRef urlStr)
{
    CFURLRef url = NULL;
    // path is a part of an actual path in the query table, so it should not have a length greater than the buffer size
    CFIndex pathLen = CFStringGetLength(path);
    CFStringGetCharacters(path, CFRangeMake(0, pathLen), urlBuffer+urlBufferLen);
    CFStringSetExternalCharactersNoCopy(urlStr, urlBuffer, urlBufferLen+pathLen, CFMaxPathSize);
    if (CFStringGetCharacterAtIndex(path, pathLen-1) == slash) {
        url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, YES);
    } else {
        url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, NO);
    }
    
    return url;
}
    
static CFURLRef _CFBundleCreateRelativeURLFromBaseAndPath(CFStringRef path, CFURLRef base, UniChar slash, CFStringRef slashStr)
{
    CFURLRef url = NULL;
    CFRange resultRange;
    Boolean needToRelease = false;
    if (CFStringFindWithOptions(path, slashStr, CFRangeMake(0, CFStringGetLength(path)-1), kCFCompareBackwards, &resultRange)) {
        CFStringRef subPathCom = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, path, CFRangeMake(0, resultRange.location));
        base = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, base, subPathCom, YES);
        path = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, path, CFRangeMake(resultRange.location+1, CFStringGetLength(path)-resultRange.location-1));
        CFRelease(subPathCom);
        needToRelease = true;
    }
    if (CFStringGetCharacterAtIndex(path, CFStringGetLength(path)-1) == slash) {
        url = (CFURLRef)CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, path, PLATFORM_PATH_STYLE, YES, base);
    } else {
        url = (CFURLRef)CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, path, PLATFORM_PATH_STYLE, NO, base);
    }
    if (needToRelease) {
        CFRelease(base);
        CFRelease(path);
    }
    return url;
}
    
static void _CFBundleFindResourcesWithPredicate(CFMutableArrayRef interResult, CFDictionaryRef queryTable, Boolean (^predicate)(CFStringRef filename, Boolean *stop), Boolean *stop)
{
    CFIndex dictSize = CFDictionaryGetCount(queryTable);
    if (dictSize == 0) {
        return;
    }
    STACK_BUFFER_DECL(CFTypeRef, keys, dictSize);
    STACK_BUFFER_DECL(CFTypeRef, values, dictSize);
    CFDictionaryGetKeysAndValues(queryTable, keys, values);
    for (CFIndex i = 0; i < dictSize; i++) {
        if (predicate((CFStringRef)keys[i], stop)) {
            if (CFGetTypeID(values[i]) == CFStringGetTypeID()) {
                CFArrayAppendValue(interResult, values[i]);
            } else {
                CFArrayAppendArray(interResult, (CFArrayRef)values[i], CFRangeMake(0, CFArrayGetCount((CFArrayRef)values[i])));
            }
        }
        
        if (*stop) break;
    }
}
    
static CFTypeRef _CFBundleCopyURLsOfKey(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, UniChar *resDir, CFIndex resDirLen, UniChar *subDirBuffer, CFIndex subDirLen, CFStringRef subDir, CFStringRef key, CFStringRef lproj, UniChar *lprojBuff, Boolean returnArray, Boolean localized, uint8_t bundleVersion, Boolean (^predicate)(CFStringRef filename, Boolean *stop))
{
    CFTypeRef value = NULL;
    Boolean stop = false; // for predicate
    CFMutableArrayRef interResult = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFDictionaryRef subTable = NULL;
        
    if (1 == bundleVersion) {
        CFIndex savedResDirLen = resDirLen;
        // add the non-localized resource dir
        Boolean appendSucc = _CFAppendPathComponent(resDir, &resDirLen, CFMaxPathSize, _GlobalResourcesUniChars, _GlobalResourcesLen);
        if (appendSucc) {
            subTable = _CFBundleCopyQueryTable(bundle, bundleURL, languages, resDir, resDirLen, subDirBuffer, subDirLen);
            if (predicate) {
                _CFBundleFindResourcesWithPredicate(interResult, subTable, predicate, &stop);
            } else {
                value = CFDictionaryGetValue(subTable, key);
            }
        }
        resDirLen = savedResDirLen;
    }
    
    if (!value && !stop) {
        if (subTable) CFRelease(subTable);
        subTable = _CFBundleCopyQueryTable(bundle, bundleURL, languages, resDir, resDirLen, subDirBuffer, subDirLen);
        if (predicate) {
            _CFBundleFindResourcesWithPredicate(interResult, subTable, predicate, &stop);
        } else {
            // get the path or paths for the given key
            value = CFDictionaryGetValue(subTable, key);
        }
    }
    
    // if localization is needed, we filter out the paths for the localization and put the valid ones in the interResult
    Boolean checkLP = true;
    CFIndex lpLen = lproj ? CFStringGetLength(lproj) : 0;
    if (localized && value) {
        
        if (CFGetTypeID(value) == CFStringGetTypeID()){
            // We had one result, but since we are going to do a search in a different localization, we will convert the one result into an array of results.
            value = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&value, 1, &kCFTypeArrayCallBacks);
        } else {
            CFRetain(value);
        }
        
        CFRange resultRange, searchRange;
        CFIndex pathValueLen;
        CFIndex limit = returnArray ? CFArrayGetCount((CFArrayRef)value) : 1;
        searchRange.location = 0;
        for (CFIndex i = 0; i < limit; i++) {
            CFStringRef pathValue = (CFStringRef) CFArrayGetValueAtIndex((CFArrayRef)value, i);
            pathValueLen = CFStringGetLength(pathValue);
            searchRange.length = pathValueLen;
            
            // if we have subdir, we find the subdir and see if it is after the base path (bundle path + res dir)
            Boolean searchForLocalization = false;
            if (subDirLen) {
                if (CFStringFindWithOptions(pathValue, subDir, searchRange, kCFCompareEqualTo, &resultRange) && resultRange.location != searchRange.location) {
                    searchForLocalization = true;
                }
            } else if (!subDirLen && searchRange.length != 0) {
                if (CFStringFindWithOptions(pathValue, _CFBundleLprojExtensionWithDot, searchRange, kCFCompareEqualTo, &resultRange) && resultRange.location + 7 < pathValueLen) {
                    searchForLocalization = true;
                }
            }
            
            if (searchForLocalization) {
                if (!lpLen || !(CFStringFindWithOptions(pathValue, lproj, searchRange, kCFCompareEqualTo | kCFCompareAnchored, &resultRange) && CFStringFindWithOptions(pathValue, CFSTR("."), CFRangeMake(resultRange.location + resultRange.length, 1), kCFCompareEqualTo, &resultRange))) {
                    break;
                }
                checkLP = false;
            }
            
            CFArrayAppendValue(interResult, pathValue);
        }
        
        CFRelease(value);
        
        if (!returnArray && CFArrayGetCount(interResult) != 0) {
            checkLP = false;
        }
    } else if (value) {
        if (CFGetTypeID(value) == CFArrayGetTypeID()) {
            CFArrayAppendArray(interResult, (CFArrayRef)value, CFRangeMake(0, CFArrayGetCount((CFArrayRef)value)));
        } else {
            CFArrayAppendValue(interResult, value);
        }
    }
    
    value = NULL;
    CFRelease(subTable);
    
    // we fetch the result for a given lproj and join them with the nonlocalized result fetched above
    if (lpLen && checkLP) {
        CFIndex lprojBuffLen = lpLen;
        // lprojBuff is allocated with the actual size of lproj
        CFStringGetCharacters(lproj, CFRangeMake(0, lpLen), lprojBuff);
        _CFAppendPathExtension(lprojBuff, &lprojBuffLen, lprojBuffLen+7, _LprojUniChars, _LprojLen);
        
        if (subDirLen) {
            _CFAppendTrailingPathSlash(lprojBuff, &lprojBuffLen, lprojBuffLen+1);
        }
        subTable = _CFBundleCopyQueryTable(bundle, bundleURL, languages, resDir, resDirLen, lprojBuff, subDirLen+lprojBuffLen);
        
        value = CFDictionaryGetValue(subTable, key);
        
        if (value) {
            if (CFGetTypeID(value) == CFStringGetTypeID()) {
                CFArrayAppendValue(interResult, value);
            } else {
                CFArrayAppendArray(interResult, (CFArrayRef)value, CFRangeMake(0, CFArrayGetCount((CFArrayRef)value)));
            }
        }
        
        CFRelease(subTable);
    }
    
    // after getting paths, we create urls from the paths
    CFTypeRef result = NULL;
    if (CFArrayGetCount(interResult) > 0) {
        UniChar slash = _CFGetSlash();
        UniChar *urlBuffer = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * CFMaxPathSize, 0);
        CFMutableStringRef urlStr = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);
        CFStringRef bundlePath = NULL;
        if (bundle) {
            bundlePath = _CFBundleGetBundlePath(bundle);
            CFRetain(bundlePath);
        } else {
            CFURLRef url = CFURLCopyAbsoluteURL(bundleURL);
            bundlePath = CFURLCopyFileSystemPath(url, PLATFORM_PATH_STYLE);
            CFRelease(url);
        }
        CFIndex urlBufferLen = CFStringGetLength(bundlePath);
        CFStringGetCharacters(bundlePath, CFRangeMake(0, urlBufferLen), urlBuffer);
        CFRelease(bundlePath);
        
        if (resDirLen) {
            _CFAppendPathComponent(urlBuffer, &urlBufferLen, CFMaxPathSize, resDir, resDirLen);
        }
        _CFAppendTrailingPathSlash(urlBuffer, &urlBufferLen, CFMaxPathSize);
        
        if (!returnArray) {
            Boolean isOnlyTypeOrAllFiles = CFStringHasPrefix(key, _CFBundleTypeIndicator);
            isOnlyTypeOrAllFiles |= CFStringHasPrefix(key, _CFBundleAllFiles);
            
            CFStringRef resultPath = (CFStringRef)CFArrayGetValueAtIndex((CFArrayRef)interResult, 0);
            if (!isOnlyTypeOrAllFiles) {
                result = (CFURLRef)_CFBundleCreateURLFromPath((CFStringRef)resultPath, slash, urlBuffer, urlBufferLen, urlStr);
            } else { // need to create relative URLs for binary compatibility issues
                CFStringSetExternalCharactersNoCopy(urlStr, urlBuffer, urlBufferLen, CFMaxPathSize);
                CFURLRef base = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, YES);
                CFStringRef slashStr = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, &slash, 1);
                result = (CFURLRef)_CFBundleCreateRelativeURLFromBaseAndPath(resultPath, base, slash, slashStr);
                CFRelease(slashStr);
                CFRelease(base);
            }
        } else {
            // need to create relative URLs for binary compatibility issues
            CFIndex numOfPaths = CFArrayGetCount((CFArrayRef)interResult);
            CFStringSetExternalCharactersNoCopy(urlStr, urlBuffer, urlBufferLen, CFMaxPathSize);
            CFURLRef base = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, YES);
            CFStringRef slashStr = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, &slash, 1);
            CFMutableArrayRef urls = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            for (CFIndex i = 0; i < numOfPaths; i++) {
                CFStringRef path = (CFStringRef)CFArrayGetValueAtIndex((CFArrayRef)interResult, i);
                CFURLRef url = _CFBundleCreateRelativeURLFromBaseAndPath(path, base, slash, slashStr);
                CFArrayAppendValue(urls, url);
                CFRelease(url);
            }
            result = urls;
            CFRelease(base);
        }
        CFRelease(urlStr);
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, urlBuffer);
    } else if (returnArray) {
        result = CFRetain(interResult);
    }
    
    CFRelease(interResult);
    return result;
}

CFTypeRef _CFBundleCopyFindResources(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subPath, CFStringRef lproj, Boolean returnArray, Boolean localized, Boolean (^predicate)(CFStringRef filename, Boolean *stop))
{
    CFIndex rnameLen = 0;
    CFIndex typeLen = resourceType ? CFStringGetLength(resourceType) : 0;
    CFIndex lprojLen = lproj ? CFStringGetLength(lproj) + 7 : 0; // 7 is the length of ".lproj/"
    CFStringRef subPathFromResourceName = NULL;
    CFIndex subPathFromResourceNameLen = 0;
    if (resourceName) {
        UniChar tmpNameBuffer[CFMaxPathSize];
        rnameLen = CFStringGetLength(resourceName);
        if (rnameLen > CFMaxPathSize) rnameLen = CFMaxPathSize;
        CFStringGetCharacters(resourceName, CFRangeMake(0, rnameLen), tmpNameBuffer);
        CFIndex rnameIndex = _CFStartOfLastPathComponent(tmpNameBuffer, rnameLen);
        if (rnameIndex != 0) {
            resourceName = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, tmpNameBuffer+rnameIndex, rnameLen - rnameIndex);
            subPathFromResourceName = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, tmpNameBuffer, rnameIndex-1);
            subPathFromResourceNameLen = rnameIndex-1;
        } else {
            CFRetain(resourceName);
        }
        
        char buff[CFMaxPathSize];
        CFStringRef newResName = NULL;
        if (CFStringGetFileSystemRepresentation(resourceName, buff, CFMaxPathSize)) {
            newResName = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, buff);
        }
        CFStringRef tmpStr = resourceName;
        resourceName = newResName ? newResName : (CFStringRef)CFRetain(resourceName);
        rnameLen = CFStringGetLength(resourceName);
        CFRelease(tmpStr);
    }
    CFIndex subDirLen = subPath ? CFStringGetLength(subPath) : 0;
    if (subDirLen == 0) subPath = NULL;
    if (subDirLen && subPathFromResourceName) {
        subDirLen += (subPathFromResourceNameLen + 1);
    } else if (subPathFromResourceNameLen) {
        subDirLen = subPathFromResourceNameLen;
    }
    
    // the nameBuff have the format: [resourceName].[resourceType][lprojName.lproj/][subPath][resource dir]
    UniChar *nameBuff = (UniChar *) CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * (rnameLen + 1 + typeLen + subDirLen + lprojLen + CFMaxPathSize), 0);
    UniChar *typeBuff = rnameLen ? nameBuff + rnameLen + 1 : nameBuff + CFStringGetLength(_CFBundleTypeIndicator) + 1;
    UniChar *lprojBuffer = typeBuff + typeLen;
    UniChar *subDirBuffer = lprojBuffer + lprojLen;
    UniChar *resDir = subDirBuffer + subDirLen;
    
    CFStringRef key = NULL;
    
    CFIndex typeP = 0;
    if (typeLen && CFStringGetCharacterAtIndex(resourceType, 0) == '.') {
        typeP = 1;
        typeLen--;
    }
    if (rnameLen && typeLen) {
        CFStringGetCharacters(resourceName, CFRangeMake(0, rnameLen), nameBuff);
        nameBuff[rnameLen] = '.';
        CFStringGetCharacters(resourceType, CFRangeMake(typeP, typeLen), typeBuff);
        key = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, nameBuff, rnameLen+typeLen+1);
    } else if (rnameLen) {
        CFStringGetCharacters(resourceName, CFRangeMake(0, rnameLen), nameBuff);
        key = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, nameBuff, rnameLen);
    } else if (typeLen) {
        rnameLen = CFStringGetLength(_CFBundleTypeIndicator);
        CFStringGetCharacters(_CFBundleTypeIndicator, CFRangeMake(0, rnameLen), nameBuff);
        nameBuff[rnameLen] = '.';
        CFStringGetCharacters(resourceType, CFRangeMake(typeP, typeLen), typeBuff);
        key = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, nameBuff, rnameLen+typeLen+1);
    } else {
        key = (CFStringRef)CFRetain(_CFBundleAllFiles);
    }
    
    if (subDirLen) {
        CFIndex subPathLen = 0;
        if (subPath) {
            subPathLen = CFStringGetLength(subPath);
            CFStringGetCharacters(subPath, CFRangeMake(0, subPathLen), subDirBuffer);
            if (subPathFromResourceName) _CFAppendTrailingPathSlash(subDirBuffer, &subPathLen, subDirLen);
        }
        if (subPathFromResourceName) {
            CFStringGetCharacters(subPathFromResourceName, CFRangeMake(0, subPathFromResourceNameLen), subDirBuffer+subPathLen);
            subPathLen += subPathFromResourceNameLen;
            subPath = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, subDirBuffer, subPathLen);
        } else {
            CFRetain(subPath);
        }
    }
    
    // Init the one-time-only unichar buffers.
    _CFEnsureStaticBuffersInited();
    
    CFIndex resDirLen = 0;
    uint8_t bundleVersion = bundle ? _CFBundleLayoutVersion(bundle) : 0;
    if (bundleURL && !languages) {
        languages = _CFBundleCopyLanguageSearchListInDirectory(kCFAllocatorSystemDefault, bundleURL, &bundleVersion);
    } else if (languages) {
        CFRetain(languages);
    }

    _CFBundleSetResourceDir(resDir, &resDirLen, CFMaxPathSize, bundleVersion);
    
    CFTypeRef returnValue = _CFBundleCopyURLsOfKey(bundle, bundleURL, languages, resDir, resDirLen, subDirBuffer, subDirLen, subPath, key, lproj, lprojBuffer, returnArray, localized, bundleVersion, predicate);
        
    if ((!returnValue || (CFGetTypeID(returnValue) == CFArrayGetTypeID() && CFArrayGetCount((CFArrayRef)returnValue) == 0)) && (0 == bundleVersion || 2 == bundleVersion)) {
        CFStringRef bundlePath = NULL;
        if (bundle) {
            bundlePath = _CFBundleGetBundlePath(bundle);
            CFRetain(bundlePath);
        } else {
            CFURLRef absoluteURL = CFURLCopyAbsoluteURL(bundleURL);
            bundlePath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
            CFRelease(absoluteURL);
        }
        if ((0 == bundleVersion) || CFEqual(CFSTR("/Library/Spotlight"), bundlePath)){
            if (returnValue) CFRelease(returnValue);
            CFRange found;
            // 9 is the length of "Resources"
            if ((bundleVersion == 0 && subPath && CFEqual(subPath, CFSTR("Resources"))) || (bundleVersion == 2 && subPath && CFEqual(subPath, CFSTR("Contents/Resources")))){
                subDirLen = 0;
            } else if ((bundleVersion == 0 && subPath && CFStringFindWithOptions(subPath, CFSTR("Resources/"), CFRangeMake(0, 10), kCFCompareEqualTo, &found) && found.location+10 < subDirLen)) {
                subDirBuffer = subDirBuffer + 10;
                subDirLen -= 10;
            } else if ((bundleVersion == 2 && subPath && CFStringFindWithOptions(subPath, CFSTR("Contents/Resources/"), CFRangeMake(0, 19), kCFCompareEqualTo, &found) && found.location+19 < subDirLen)) {
                subDirBuffer = subDirBuffer + 19;
                subDirLen -= 19;
            } else {
                resDirLen = 0;
            }
            if (subDirLen > 0) {
                CFRelease(subPath);
                subPath = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, subDirBuffer, subDirLen);
            }
            returnValue = _CFBundleCopyURLsOfKey(bundle, bundleURL, languages, resDir, resDirLen, subDirBuffer, subDirLen, subPath, key, lproj, lprojBuffer, returnArray, localized, bundleVersion, predicate);
        }
        CFRelease(bundlePath);
    }

    if (resourceName) CFRelease(resourceName);
    if (subPath) CFRelease(subPath);
    if (subPathFromResourceName) CFRelease(subPathFromResourceName);
    if (languages) CFRelease(languages);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, nameBuff);
    CFRelease(key);
    return returnValue;
}

__private_extern__ CFTypeRef _CFBundleCopyFindResourcesWithNoBlock(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subPath, CFStringRef lproj, Boolean returnArray, Boolean localized)
{
    return _CFBundleCopyFindResources(bundle, bundleURL, languages, resourceName, resourceType, subPath, lproj, returnArray, localized, NULL);
}
