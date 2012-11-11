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
/*	CFBundle_Resources.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Doug Davidson
*/

#if defined(__MACOS8__) || defined(__WIN32__)
#define USE_GETDIRENTRIES 0
#else
#define USE_GETDIRENTRIES 1
#endif
#define GETDIRENTRIES_CACHE_CAPACITY 100

#include "CFBundle_Internal.h"
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFByteOrder.h>
#include <CoreFoundation/CFNumber.h>
#include <string.h>
#include "CFInternal.h"
#include "CFPriv.h"

#if defined(__MACOS8__)
/* MacOS8 Headers */
#include <Script.h>
#include <stat.h>
#include <Files.h>
#include <Resources.h>
#include <CodeFragments.h>
#include <Errors.h>
#include <Gestalt.h>
#else
/* Unixy & Windows Headers */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#if USE_GETDIRENTRIES
#include <dirent.h>
#endif
#endif



// All new-style bundles will have these extensions.
CF_INLINE CFStringRef _CFGetPlatformName(void) {
    // MF:!!! This used to be based on NSInterfaceStyle, not hard-wired by compiler.
#if defined(__WIN32__)
    return _CFBundleWindowsPlatformName;
#elif defined(__MACOS8__)
    return _CFBundleMacOS8PlatformName;
#elif defined (__MACH__)
    return _CFBundleMacOSXPlatformName;
#elif defined(__svr4__)
    return _CFBundleSolarisPlatformName;
#elif defined(__hpux__)
    return _CFBundleHPUXPlatformName;
#elif defined(__LINUX__)
    return _CFBundleLinuxPlatformName;
#elif defined(__FREEBSD__)
    return _CFBundleFreeBSDPlatformName;
#else
    return CFSTR("");
#endif
}

CF_INLINE CFStringRef _CFGetAlternatePlatformName(void) {
#if defined (__MACH__)
    return _CFBundleAlternateMacOSXPlatformName;
#elif defined(__MACOS8__)
    return _CFBundleAlternateMacOS8PlatformName;
#else
    return CFSTR("");
#endif
}

static CFSpinLock_t CFBundleResourceGlobalDataLock = 0;
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

static void _CFBundleInitStaticUniCharBuffers(void) {
    CFStringRef appSupportStr1 = _CFBundleSupportFilesDirectoryName1;
    CFStringRef appSupportStr2 = _CFBundleSupportFilesDirectoryName2;
    CFStringRef resourcesStr = _CFBundleResourcesDirectoryName;
    CFStringRef platformStr = _CFGetPlatformName();
    CFStringRef alternatePlatformStr = _CFGetAlternatePlatformName();
    CFStringRef lprojStr = _CFBundleLprojExtension;
    CFStringRef globalResourcesStr = _CFBundleNonLocalizedResourcesDirectoryName;
    CFStringRef infoExtensionStr = _CFBundleInfoExtension;

    CFAllocatorRef alloc = __CFGetDefaultAllocator();

    _AppSupportLen1 = CFStringGetLength(appSupportStr1);
    _AppSupportLen2 = CFStringGetLength(appSupportStr2);
    _ResourcesLen = CFStringGetLength(resourcesStr);
    _PlatformLen = CFStringGetLength(platformStr);
    _AlternatePlatformLen = CFStringGetLength(alternatePlatformStr);
    _LprojLen = CFStringGetLength(lprojStr);
    _GlobalResourcesLen = CFStringGetLength(globalResourcesStr);
    _InfoExtensionLen = CFStringGetLength(infoExtensionStr);

    _AppSupportUniChars1 = CFAllocatorAllocate(alloc, sizeof(UniChar) * (_AppSupportLen1 + _AppSupportLen2 + _ResourcesLen + _PlatformLen + _AlternatePlatformLen + _LprojLen + _GlobalResourcesLen + _InfoExtensionLen), 0);
    _AppSupportUniChars2 = _AppSupportUniChars1 + _AppSupportLen1;
    _ResourcesUniChars = _AppSupportUniChars2 + _AppSupportLen2;
    _PlatformUniChars = _ResourcesUniChars + _ResourcesLen;
    _AlternatePlatformUniChars = _PlatformUniChars + _PlatformLen;
    _LprojUniChars = _AlternatePlatformUniChars + _AlternatePlatformLen;
    _GlobalResourcesUniChars = _LprojUniChars + _LprojLen;
    _InfoExtensionUniChars = _GlobalResourcesUniChars + _GlobalResourcesLen;

    if (_AppSupportLen1 > 0) {
        CFStringGetCharacters(appSupportStr1, CFRangeMake(0, _AppSupportLen1), _AppSupportUniChars1);
    }
    if (_AppSupportLen2 > 0) {
        CFStringGetCharacters(appSupportStr2, CFRangeMake(0, _AppSupportLen2), _AppSupportUniChars2);
    }
    if (_ResourcesLen > 0) {
        CFStringGetCharacters(resourcesStr, CFRangeMake(0, _ResourcesLen), _ResourcesUniChars);
    }
    if (_PlatformLen > 0) {
        CFStringGetCharacters(platformStr, CFRangeMake(0, _PlatformLen), _PlatformUniChars);
    }
    if (_AlternatePlatformLen > 0) {
        CFStringGetCharacters(alternatePlatformStr, CFRangeMake(0, _AlternatePlatformLen), _AlternatePlatformUniChars);
    }
    if (_LprojLen > 0) {
        CFStringGetCharacters(lprojStr, CFRangeMake(0, _LprojLen), _LprojUniChars);
    }
    if (_GlobalResourcesLen > 0) {
        CFStringGetCharacters(globalResourcesStr, CFRangeMake(0, _GlobalResourcesLen), _GlobalResourcesUniChars);
    }
    if (_InfoExtensionLen > 0) {
        CFStringGetCharacters(infoExtensionStr, CFRangeMake(0, _InfoExtensionLen), _InfoExtensionUniChars);
    }
}

CF_INLINE void _CFEnsureStaticBuffersInited(void) {
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (_AppSupportUniChars1 == NULL) {
        _CFBundleInitStaticUniCharBuffers();
    }
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
}

#if USE_GETDIRENTRIES

static CFMutableDictionaryRef contentsCache = NULL;
static CFMutableDictionaryRef directoryContentsCache = NULL;

static CFArrayRef _CFBundleCopyDirectoryContentsAtPath(CFStringRef path, Boolean directoriesOnly) {
    CFArrayRef result = NULL;
    
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (directoriesOnly) {
        if (directoryContentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(directoryContentsCache, path);
    } else {
        if (contentsCache) result = (CFMutableArrayRef)CFDictionaryGetValue(contentsCache, path);
    }
    if (result) CFRetain(result);
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);

    if (!result) {
        char cpathBuff[CFMaxPathSize], dirge[8192];
        CFIndex cpathLen = 0;
        int fd = -1, numread;
        long basep = 0;
        CFMutableArrayRef contents = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks), directoryContents = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        CFStringRef name;
        
        if (_CFStringGetFileSystemRepresentation(path, cpathBuff, CFMaxPathSize)) {
            cpathLen = strlen(cpathBuff);
            fd = open(cpathBuff, O_RDONLY, 0777);
        }
        if (fd >= 0) {
            while ((numread = getdirentries(fd, dirge, sizeof(dirge), &basep)) > 0) {
                struct dirent *dent;
                for (dent = (struct dirent *)dirge; dent < (struct dirent *)(dirge + numread); dent = (struct dirent *)((char *)dent + dent->d_reclen)) {
                    CFIndex nameLen = strlen(dent->d_name);
                    if (0 == dent->d_fileno || (dent->d_name[0] == '.' && (nameLen == 1 || (nameLen == 2 && dent->d_name[1] == '.')))) continue;
                    name = CFStringCreateWithCString(NULL, dent->d_name, CFStringFileSystemEncoding());
                    if (NULL != name) {
                        CFArrayAppendValue(contents, name);
                        if (dent->d_type == DT_DIR) {
                            CFArrayAppendValue(directoryContents, name);
                        } else if (dent->d_type == DT_UNKNOWN) {
                            struct stat statBuf;
                            cpathBuff[cpathLen] = '/';
                            strncpy(cpathBuff + cpathLen + 1, dent->d_name, nameLen);
                            cpathBuff[cpathLen + nameLen + 1] = '\0';
                            if (stat(cpathBuff, &statBuf) == 0 && (statBuf.st_mode & S_IFMT) == S_IFDIR) CFArrayAppendValue(directoryContents, name);
                            cpathBuff[cpathLen] = '\0';
                        }
                        CFRelease(name);
                    }
                }
            }
            close(fd);
        }
        
        __CFSpinLock(&CFBundleResourceGlobalDataLock);
        if (!contentsCache) contentsCache = CFDictionaryCreateMutable(NULL, GETDIRENTRIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (GETDIRENTRIES_CACHE_CAPACITY == CFDictionaryGetCount(contentsCache)) CFDictionaryRemoveAllValues(contentsCache);
        CFDictionaryAddValue(contentsCache, path, contents);

        if (!directoryContentsCache) directoryContentsCache = CFDictionaryCreateMutable(NULL, GETDIRENTRIES_CACHE_CAPACITY, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (GETDIRENTRIES_CACHE_CAPACITY == CFDictionaryGetCount(directoryContentsCache)) CFDictionaryRemoveAllValues(directoryContentsCache);
        CFDictionaryAddValue(directoryContentsCache, path, directoryContents);

        result = CFRetain(directoriesOnly ? directoryContents : contents);
        CFRelease(contents);
        CFRelease(directoryContents);
        __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
    }
    
    return result;
}

static void _CFBundleFlushContentsCaches(void) {
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (contentsCache) CFDictionaryRemoveAllValues(contentsCache);
    if (directoryContentsCache) CFDictionaryRemoveAllValues(directoryContentsCache);
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
}

#endif /* USE_GETDIRENTRIES */

CF_EXPORT void _CFBundleFlushCaches(void) {
#if USE_GETDIRENTRIES
    _CFBundleFlushContentsCaches();
#endif /* USE_GETDIRENTRIES */
}

__private_extern__ Boolean _CFIsResourceAtURL(CFURLRef url, Boolean *isDir) {
    Boolean exists;
    SInt32 mode;
    if (_CFGetFileProperties(NULL, url, &exists, &mode, NULL, NULL, NULL, NULL) == 0) {
        if (isDir) {
            *isDir = ((exists && ((mode & S_IFMT) == S_IFDIR)) ? true : false);
        }
#if defined(__MACOS8__)
        return (exists);
#else
        return (exists && (mode & 0444));
#endif /* __MACOS8__ */
    } else {
        return false;
    }
}

__private_extern__ Boolean _CFIsResourceAtPath(CFStringRef path, Boolean *isDir) {
    Boolean result = false;
    CFURLRef url = CFURLCreateWithFileSystemPath(CFGetAllocator(path), path, PLATFORM_PATH_STYLE, false);
    if (url != NULL) {
        result = _CFIsResourceAtURL(url, isDir);
        CFRelease(url);
    }
    return result;
}

static void _CFSearchBundleDirectory(CFAllocatorRef alloc, CFMutableArrayRef result, UniChar *pathUniChars, CFIndex pathLen, UniChar *nameUniChars, CFIndex nameLen, UniChar *typeUniChars, CFIndex typeLen, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, uint8_t version) {
    // pathUniChars is the full path to the directory we are searching.
    // nameUniChars is what we are looking for.
    // typeUniChars is the type we are looking for.
    // platformUniChars is the platform name.
    // cheapStr is available for our use for whatever we want.
    // URLs for found resources get added to result.
    CFIndex savedPathLen;
    Boolean platformGenericFound = false, platformSpecificFound = false;
    Boolean platformGenericIsDir = false, platformSpecificIsDir = false;
    CFStringRef platformGenericStr = NULL;

#if USE_GETDIRENTRIES
    CFIndex dirPathLen = pathLen;
    CFArrayRef contents, directoryContents;
    CFRange contentsRange, directoryContentsRange;

    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, dirPathLen, dirPathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    //fprintf(stderr, "looking in ");CFShow(cheapStr);
    contents = _CFBundleCopyDirectoryContentsAtPath(cheapStr, false);
    contentsRange = CFRangeMake(0, CFArrayGetCount(contents));
    directoryContents = _CFBundleCopyDirectoryContentsAtPath(cheapStr, true);
    directoryContentsRange = CFRangeMake(0, CFArrayGetCount(directoryContents));
#endif

    if (nameLen > 0) {
        _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, nameUniChars, nameLen);
    }
    // Save length with just name appended.
    savedPathLen = pathLen;

    // Check platform generic
    if (typeLen > 0) {
        _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
    }
#if USE_GETDIRENTRIES
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + dirPathLen + 1, pathLen - dirPathLen - 1, pathLen - dirPathLen - 1);
    CFStringReplaceAll(cheapStr, tmpString);
    platformGenericFound = CFArrayContainsValue(contents, contentsRange, cheapStr);
    platformGenericIsDir = CFArrayContainsValue(directoryContents, directoryContentsRange, cheapStr);
    //fprintf(stderr, "looking for ");CFShow(cheapStr);if (platformGenericFound) fprintf(stderr, "found it\n"); if (platformGenericIsDir) fprintf(stderr, "a directory\n");
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
    CFStringReplaceAll(cheapStr, tmpString);
#else
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    platformGenericFound = _CFIsResourceAtPath(cheapStr, &platformGenericIsDir);
#endif
    
    // Check for platform specific.
    if (platformGenericFound) {
        platformGenericStr = CFStringCreateCopy(alloc, cheapStr);
        if (!platformSpecificFound && (_PlatformLen > 0)) {
            pathLen = savedPathLen;
            pathUniChars[pathLen++] = (UniChar)'-';
            memmove(pathUniChars + pathLen, _PlatformUniChars, _PlatformLen * sizeof(UniChar));
            pathLen += _PlatformLen;
            if (typeLen > 0) {
                _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, typeUniChars, typeLen);
            }
#if USE_GETDIRENTRIES
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + dirPathLen + 1, pathLen - dirPathLen - 1, pathLen - dirPathLen - 1);
            CFStringReplaceAll(cheapStr, tmpString);
            platformSpecificFound = CFArrayContainsValue(contents, contentsRange, cheapStr);
            platformSpecificIsDir = CFArrayContainsValue(directoryContents, directoryContentsRange, cheapStr);
            //fprintf(stderr, "looking for ");CFShow(cheapStr);if (platformSpecificFound) fprintf(stderr, "found it\n"); if (platformSpecificIsDir) fprintf(stderr, "a directory\n");
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
            CFStringReplaceAll(cheapStr, tmpString);
#else
            CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
            CFStringReplaceAll(cheapStr, tmpString);
            platformSpecificFound = _CFIsResourceAtPath(cheapStr, &platformSpecificIsDir);
#endif
        }
    }
    if (platformSpecificFound) {
        CFURLRef url = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, platformSpecificIsDir);
        CFArrayAppendValue(result, url);
        CFRelease(url);
    } else if (platformGenericFound) {
        CFURLRef url = CFURLCreateWithFileSystemPath(alloc, ((platformGenericStr != NULL) ? platformGenericStr : cheapStr), PLATFORM_PATH_STYLE, platformGenericIsDir);
        CFArrayAppendValue(result, url);
        CFRelease(url);
    }
    if (platformGenericStr != NULL) {
        CFRelease(platformGenericStr);
    }
#if USE_GETDIRENTRIES
    CFRelease(contents);
    CFRelease(directoryContents);
#endif
}

static void _CFFindBundleResourcesInRawDir(CFAllocatorRef alloc, UniChar *workingUniChars, CFIndex workingLen, UniChar *nameUniChars, CFIndex nameLen, CFArrayRef resTypes, CFIndex limit, uint8_t version, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, CFMutableArrayRef result) {

    if (nameLen > 0) {
        // If we have a resName, just call the search API.  We may have to loop over the resTypes.
        if (!resTypes) {
            _CFSearchBundleDirectory(alloc, result, workingUniChars, workingLen, nameUniChars, nameLen, NULL, 0, cheapStr, tmpString, version);
        } else {
            CFIndex i, c = CFArrayGetCount(resTypes);
            for (i=0; i<c; i++) {
                CFStringRef curType = (CFStringRef)CFArrayGetValueAtIndex(resTypes, i);
                CFIndex typeLen = CFStringGetLength(curType);
#if defined(__MACOS8__) || defined(__WIN32__)
                UniChar *typeChars = CFAllocatorAllocate(alloc, sizeof(UniChar) * typeLen, 0);
                if (typeChars) {
#else
                UniChar typeChars[typeLen];
#endif /* __MACOS8__ */
                CFStringGetCharacters(curType, CFRangeMake(0, typeLen), typeChars);
                _CFSearchBundleDirectory(alloc, result, workingUniChars, workingLen, nameUniChars, nameLen, typeChars, typeLen, cheapStr, tmpString, version);
                if (limit <= CFArrayGetCount(result)) {
                    break;
                }
#if defined(__MACOS8__) || defined(__WIN32__)
                        CFAllocatorDeallocate(alloc, typeChars);
                }
#endif /* __MACOS8__ */
            }
        }
    } else {
        // If we have no resName, do it by hand. We may have to loop over the resTypes.
        unsigned char cpathBuff[CFMaxPathSize];
        CFIndex cpathLen;
        CFMutableArrayRef children;

        CFStringSetExternalCharactersNoCopy(tmpString, workingUniChars, workingLen, workingLen);
        if (!_CFStringGetFileSystemRepresentation(tmpString, cpathBuff, CFMaxPathSize)) return;
        cpathLen = strlen(cpathBuff);

        if (!resTypes) {
            children = _CFContentsOfDirectory(alloc, cpathBuff, NULL, NULL, NULL);
            if (children) {
                CFIndex childIndex, childCount = CFArrayGetCount(children);
                for (childIndex = 0; childIndex < childCount; childIndex++) {
                    CFArrayAppendValue(result, CFArrayGetValueAtIndex(children, childIndex));
                }
                CFRelease(children);
            }
        } else {
            CFIndex i, c = CFArrayGetCount(resTypes);
            for (i=0; i<c; i++) {
                CFStringRef curType = (CFStringRef)CFArrayGetValueAtIndex(resTypes, i);

                children = _CFContentsOfDirectory(alloc, cpathBuff, NULL, NULL, curType);
                if (children) {
                    CFIndex childIndex, childCount = CFArrayGetCount(children);
                    for (childIndex = 0; childIndex < childCount; childIndex++) {
                        CFArrayAppendValue(result, CFArrayGetValueAtIndex(children, childIndex));
                    }
                    CFRelease(children);
                }
                if (limit <= CFArrayGetCount(result)) {
                    break;
                }
            }
        }
    }
}

static void _CFFindBundleResourcesInResourcesDir(CFAllocatorRef alloc, UniChar *workingUniChars, CFIndex workingLen, UniChar *subDirUniChars, CFIndex subDirLen, CFArrayRef searchLanguages, UniChar *nameUniChars, CFIndex nameLen, CFArrayRef resTypes, CFIndex limit, uint8_t version, CFMutableStringRef cheapStr, CFMutableStringRef tmpString, CFMutableArrayRef result) {
    CFIndex savedWorkingLen = workingLen;

    // Look directly in the directory specified in workingUniChars. as if it is a Resources directory.
    if (1 == version) {
        // Add the non-localized resource directory.
        _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _GlobalResourcesUniChars, _GlobalResourcesLen);
        if (subDirLen > 0) {
            _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen);
        }
        _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, version, cheapStr, tmpString, result);

        // Strip the non-localized resource directory.
        workingLen = savedWorkingLen;
    }
    if (CFArrayGetCount(result) < limit) {
        if (subDirLen > 0) {
            _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, subDirUniChars, subDirLen);
        }
        _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, version, cheapStr, tmpString, result);
    }
    
    // Now search the local resources.
    workingLen = savedWorkingLen;
    if (CFArrayGetCount(result) < limit) {
        CFIndex langIndex;
        CFIndex langCount = (searchLanguages ? CFArrayGetCount(searchLanguages) : 0);
        CFStringRef curLangStr;
        CFIndex curLangLen;
        // MF:??? OK to hard-wire this length?
        UniChar curLangUniChars[255];
        CFIndex numResults = CFArrayGetCount(result);

        for (langIndex = 0; langIndex < langCount; langIndex++) {
            curLangStr = CFArrayGetValueAtIndex(searchLanguages, langIndex);
            curLangLen = CFStringGetLength(curLangStr);
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
            _CFFindBundleResourcesInRawDir(alloc, workingUniChars, workingLen, nameUniChars, nameLen, resTypes, limit, version, cheapStr, tmpString, result);
            
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

CFArrayRef _CFFindBundleResources(CFBundleRef bundle, CFURLRef bundleURL, CFStringRef subDirName, CFArrayRef searchLanguages, CFStringRef resName, CFArrayRef resTypes, CFIndex limit, uint8_t version) {
    CFAllocatorRef alloc = ((bundle != NULL) ? CFGetAllocator(bundle) : CFRetain(__CFGetDefaultAllocator()));
    CFMutableArrayRef result;
    UniChar *workingUniChars, *nameUniChars, *subDirUniChars;
    CFIndex nameLen = (resName ? CFStringGetLength(resName) : 0);
    CFIndex subDirLen = (subDirName ? CFStringGetLength(subDirName) : 0);
    CFIndex workingLen, savedWorkingLen;
    CFURLRef absoluteURL;
    CFStringRef bundlePath;
    CFMutableStringRef cheapStr, tmpString;

    result = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    // Init the one-time-only unichar buffers.
    _CFEnsureStaticBuffersInited();

    // Build UniChar buffers for some of the string pieces we need.
    // One malloc will do.
    nameUniChars = CFAllocatorAllocate(alloc, sizeof(UniChar) * (nameLen + subDirLen + CFMaxPathSize), 0);
    subDirUniChars = nameUniChars + nameLen;
    workingUniChars = subDirUniChars + subDirLen;

    if (nameLen > 0) {
        CFStringGetCharacters(resName, CFRangeMake(0, nameLen), nameUniChars);
    }
    if (subDirLen > 0) {
        CFStringGetCharacters(subDirName, CFRangeMake(0, subDirLen), subDirUniChars);
    }
    // Build a UniChar buffer with the absolute path to the bundle's resources directory.
    // If no URL was passed, we get it from the bundle.
    bundleURL = ((bundleURL != NULL) ? CFRetain(bundleURL) : CFBundleCopyBundleURL(bundle));
    absoluteURL = CFURLCopyAbsoluteURL(bundleURL);
    bundlePath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFRelease(absoluteURL);
    if ((workingLen = CFStringGetLength(bundlePath)) > 0) {
        CFStringGetCharacters(bundlePath, CFRangeMake(0, workingLen), workingUniChars);
    }
    CFRelease(bundlePath);
    CFRelease(bundleURL);
    savedWorkingLen = workingLen;
    if (1 == version) {
        _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _AppSupportUniChars1, _AppSupportLen1);
    } else if (2 == version) {
        _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _AppSupportUniChars2, _AppSupportLen2);
    }
    if (0 == version || 1 == version || 2 == version) {
        _CFAppendPathComponent(workingUniChars, &workingLen, CFMaxPathSize, _ResourcesUniChars, _ResourcesLen);
    }

    // both of these used for temp string operations, for slightly
    // different purposes, where each type is appropriate
    cheapStr = CFStringCreateMutable(alloc, 0);
    _CFStrSetDesiredCapacity(cheapStr, CFMaxPathSize);
    tmpString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);

    _CFFindBundleResourcesInResourcesDir(alloc, workingUniChars, workingLen, subDirUniChars, subDirLen, searchLanguages, nameUniChars, nameLen, resTypes, limit, version, cheapStr, tmpString, result);
    
    // drd: This unfortunate hack is still necessary because of installer packages
    if (0 == version && CFArrayGetCount(result) == 0) {
        // Try looking directly in the bundle path
        workingLen = savedWorkingLen;
        _CFFindBundleResourcesInResourcesDir(alloc, workingUniChars, workingLen, subDirUniChars, subDirLen, searchLanguages, nameUniChars, nameLen, resTypes, limit, version, cheapStr, tmpString, result);
    }

    CFRelease(cheapStr);
    CFRelease(tmpString);
    CFAllocatorDeallocate(alloc, nameUniChars);
    if (bundle == NULL) {
        CFRelease(alloc);
    }

    return result;
}

CF_EXPORT CFURLRef CFBundleCopyResourceURL(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName) {
    CFURLRef result = NULL;
    CFArrayRef languages = _CFBundleGetLanguageSearchList(bundle);
    CFMutableArrayRef types = NULL;
    CFArrayRef array;

    if (resourceType) {
        types = CFArrayCreateMutable(CFGetAllocator(bundle), 1, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(types, resourceType);
    }
    
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, resourceName, types, 1, _CFBundleLayoutVersion(bundle));
    
    if (types) {
        CFRelease(types);
    }
    
    if (array) {
        if (CFArrayGetCount(array) > 0) {
            result = CFRetain(CFArrayGetValueAtIndex(array, 0));
        }
        CFRelease(array);
    }
    return result;
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfType(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName) {
    CFArrayRef languages = _CFBundleGetLanguageSearchList(bundle);
    CFMutableArrayRef types = NULL;
    CFArrayRef array;

    if (resourceType) {
        types = CFArrayCreateMutable(CFGetAllocator(bundle), 1, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(types, resourceType);
    }
    
    // MF:!!! Better "limit" than 1,000,000?
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, NULL, types, 1000000, _CFBundleLayoutVersion(bundle));
    
    if (types) {
        CFRelease(types);
    }
    
    return array;
}

CF_EXPORT CFURLRef _CFBundleCopyResourceURLForLanguage(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {return CFBundleCopyResourceURLForLocalization(bundle, resourceName, resourceType, subDirName, language);}

CF_EXPORT CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
    CFURLRef result = NULL;
    CFMutableArrayRef languages = CFArrayCreateMutable(CFGetAllocator(bundle), 1, &kCFTypeArrayCallBacks);
    CFMutableArrayRef types = NULL;
    CFArrayRef array;

    if (localizationName) CFArrayAppendValue(languages, localizationName);

    if (resourceType) {
        types = CFArrayCreateMutable(CFGetAllocator(bundle), 1, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(types, resourceType);
    }
    
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, resourceName, types, 1, _CFBundleLayoutVersion(bundle));
    
    if (types) {
        CFRelease(types);
    }
    
    if (array) {
        if (CFArrayGetCount(array) > 0) {
            result = CFRetain(CFArrayGetValueAtIndex(array, 0));
        }
        CFRelease(array);
    }

    CFRelease(languages);
    
    return result;
}

CF_EXPORT CFArrayRef _CFBundleCopyResourceURLsOfTypeForLanguage(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {return CFBundleCopyResourceURLsOfTypeForLocalization(bundle, resourceType, subDirName, language);}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfTypeForLocalization(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
    CFMutableArrayRef languages = CFArrayCreateMutable(CFGetAllocator(bundle), 1, &kCFTypeArrayCallBacks);
    CFMutableArrayRef types = NULL;
    CFArrayRef array;

    if (localizationName) CFArrayAppendValue(languages, localizationName);

    if (resourceType) {
        types = CFArrayCreateMutable(CFGetAllocator(bundle), 1, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(types, resourceType);
    }
    
    // MF:!!! Better "limit" than 1,000,000?
    array = _CFFindBundleResources(bundle, NULL, subDirName, languages, NULL, types, 1000000, _CFBundleLayoutVersion(bundle));

    if (types) {
        CFRelease(types);
    }
    
    CFRelease(languages);

    return array;
}

CF_EXPORT CFStringRef CFBundleCopyLocalizedString(CFBundleRef bundle, CFStringRef key, CFStringRef value, CFStringRef tableName) {
    CFStringRef result = NULL;
    CFDictionaryRef stringTable = NULL;

    if (key == NULL) return (value ? CFRetain(value) : CFRetain(CFSTR("")));

    if ((tableName == NULL) || CFEqual(tableName, CFSTR(""))) {
        tableName = _CFBundleDefaultStringTableName;
    }
    if (__CFBundleGetResourceData(bundle)->_stringTableCache != NULL) {
        // See if we have the table cached.
        stringTable = CFDictionaryGetValue(__CFBundleGetResourceData(bundle)->_stringTableCache, tableName);
    }
    if (stringTable == NULL) {
        // Go load the table.
        CFURLRef tableURL = CFBundleCopyResourceURL(bundle, tableName, _CFBundleStringTableType, NULL);
        if (tableURL) {
            CFStringRef nameForSharing = NULL;
            if (stringTable == NULL) {
                CFDataRef tableData = NULL;
                SInt32 errCode;
                CFStringRef errStr;
                if (CFURLCreateDataAndPropertiesFromResource(CFGetAllocator(bundle), tableURL, &tableData, NULL, NULL, &errCode)) {
                    stringTable = CFPropertyListCreateFromXMLData(CFGetAllocator(bundle), tableData, kCFPropertyListImmutable, &errStr);
                    if (errStr != NULL) {
                        CFRelease(errStr);
                        errStr = NULL;
                    }
                    CFRelease(tableData);
                }
            }
            if (nameForSharing) CFRelease(nameForSharing);
            CFRelease(tableURL);
        }
        if (stringTable == NULL) {
            stringTable = CFDictionaryCreate(CFGetAllocator(bundle), NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        if (__CFBundleGetResourceData(bundle)->_stringTableCache == NULL) {
            __CFBundleGetResourceData(bundle)->_stringTableCache = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        CFDictionarySetValue(__CFBundleGetResourceData(bundle)->_stringTableCache, tableName, stringTable);
        CFRelease(stringTable);
    }

    result = CFDictionaryGetValue(stringTable, key);
    if (result == NULL) {
        if (value == NULL) {
            result = CFRetain(key);
        } else if (CFEqual(value, CFSTR(""))) {
            result = CFRetain(key);
        } else {
            result = CFRetain(value);
        }
    } else {
        CFRetain(result);
    }
    
    return result;
}

CF_EXPORT CFURLRef CFBundleCopyResourceURLInDirectory(CFURLRef bundleURL, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName) {
    CFURLRef result = NULL;
    char buff[CFMaxPathSize];
    CFURLRef newURL = NULL;

    if (!CFURLGetFileSystemRepresentation(bundleURL, true, buff, CFMaxPathSize)) return NULL;

    newURL = CFURLCreateFromFileSystemRepresentation(NULL, buff, strlen(buff), true);
    if (NULL == newURL) {
        newURL = CFRetain(bundleURL);
    }
    if (_CFBundleCouldBeBundle(newURL)) {
        uint8_t version = 0;
        CFArrayRef languages = _CFBundleCopyLanguageSearchListInDirectory(NULL, newURL, &version);
        CFMutableArrayRef types = NULL;
        CFArrayRef array;

        if (resourceType) {
            types = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
            CFArrayAppendValue(types, resourceType);
        }
        
        array = _CFFindBundleResources(NULL, newURL, subDirName, languages, resourceName, types, 1, version);

        if (types) {
            CFRelease(types);
        }
        
        CFRelease(languages);

        if (array) {
            if (CFArrayGetCount(array) > 0) {
                result = CFRetain(CFArrayGetValueAtIndex(array, 0));
            }
            CFRelease(array);
        }
    }
    if (newURL) CFRelease(newURL);
    return result;
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfTypeInDirectory(CFURLRef bundleURL, CFStringRef resourceType, CFStringRef subDirName) {
    CFArrayRef array = NULL;
    char buff[CFMaxPathSize];
    CFURLRef newURL = NULL;

    if (!CFURLGetFileSystemRepresentation(bundleURL, true, buff, CFMaxPathSize)) return NULL;

    newURL = CFURLCreateFromFileSystemRepresentation(NULL, buff, strlen(buff), true);
    if (NULL == newURL) {
        newURL = CFRetain(bundleURL);
    }
    if (_CFBundleCouldBeBundle(newURL)) {
        uint8_t version = 0;
        CFArrayRef languages = _CFBundleCopyLanguageSearchListInDirectory(NULL, newURL, &version);
        CFMutableArrayRef types = NULL;

        if (resourceType) {
            types = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
            CFArrayAppendValue(types, resourceType);
        }

        // MF:!!! Better "limit" than 1,000,000?
        array = _CFFindBundleResources(NULL, newURL, subDirName, languages, NULL, types, 1000000, version);

        if (types) {
            CFRelease(types);
        }
        
        CFRelease(languages);
    }
    if (newURL) CFRelease(newURL);
    return array;
}

// string, with groups of 6 characters being 1 element in the array of locale abbreviations
const char * __CFBundleLocaleAbbreviationsArray =
    "en_US\0"      "fr_FR\0"      "en_GB\0"      "de_DE\0"      "it_IT\0"      "nl_NL\0"      "nl_BE\0"      "sv_SE\0"
    "es_ES\0"      "da_DK\0"      "pt_PT\0"      "fr_CA\0"      "no_NO\0"      "he_IL\0"      "ja_JP\0"      "en_AU\0"
    "ar\0\0\0\0"   "fi_FI\0"      "fr_CH\0"      "de_CH\0"      "el_GR\0"      "is_IS\0"      "mt_MT\0"      "\0\0\0\0\0\0"
    "tr_TR\0"      "hr_HR\0"      "nl_NL\0"      "nl_BE\0"      "en_CA\0"      "en_CA\0"      "pt_PT\0"      "no_NO\0"
    "da_DK\0"      "hi_IN\0"      "ur_PK\0"      "tr_TR\0"      "it_CH\0"      "en\0\0\0\0"   "\0\0\0\0\0\0" "ro_RO\0"
    "el_GR\0"      "lt_LT\0"      "pl_PL\0"      "hu_HU\0"      "et_EE\0"      "lv_LV\0"      "se\0\0\0\0"   "fo_FO\0"
    "fa_IR\0"      "ru_RU\0"      "ga_IE\0"      "ko_KR\0"      "zh_CN\0"      "zh_TW\0"      "th_TH\0"      "\0\0\0\0\0\0"
    "cs_CZ\0"      "sk_SK\0"      "\0\0\0\0\0\0" "hu_HU\0"      "bn\0\0\0\0"   "be_BY\0"      "uk_UA\0"      "\0\0\0\0\0\0"
    "el_GR\0"      "sr_YU\0"      "sl_SI\0"      "mk_MK\0"      "hr_HR\0"      "\0\0\0\0\0\0" "de_DE\0"      "pt_BR\0"
    "bg_BG\0"      "ca_ES\0"      "\0\0\0\0\0\0" "gd\0\0\0\0"   "gv\0\0\0\0"   "br\0\0\0\0"   "iu_CA\0"      "cy\0\0\0\0"
    "en_CA\0"      "ga_IE\0"      "en_CA\0"      "dz_BT\0"      "hy_AM\0"      "ka_GE\0"      "es\0\0\0\0"   "es_ES\0"
    "to_TO\0"      "pl_PL\0"      "ca_ES\0"      "fr\0\0\0\0"   "de_AT\0"      "es\0\0\0\0"   "gu_IN\0"      "pa\0\0\0\0"
    "ur_IN\0"      "vi_VN\0"      "fr_BE\0"      "uz_UZ\0"      "\0\0\0\0\0\0" "\0\0\0\0\0\0" "af_ZA\0"      "eo\0\0\0\0"
    "mr_IN\0"      "bo\0\0\0\0"   "ne_NP\0"      "kl\0\0\0";

#define NUM_LOCALE_ABBREVIATIONS	108
#define LOCALE_ABBREVIATION_LENGTH	6

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
    "Scottish",     "Manx",         "Irish",        "Tongan",       "Greek",        "Greenlandic",  "Azerbaijani"
};

#define NUM_LANGUAGE_NAMES	151
#define LANGUAGE_NAME_LENGTH	13

// string, with groups of 3 characters being 1 element in the array of abbreviations
const char * __CFBundleLanguageAbbreviationsArray =
    "en\0"   "fr\0"   "de\0"   "it\0"   "nl\0"   "sv\0"   "es\0"   "da\0"
    "pt\0"   "no\0"   "he\0"   "ja\0"   "ar\0"   "fi\0"   "el\0"   "is\0"
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
    "gd\0"   "gv\0"   "ga\0"   "to\0"   "el\0"   "kl\0"   "az\0";

#define NUM_LANGUAGE_ABBREVIATIONS	151
#define LANGUAGE_ABBREVIATION_LENGTH	3

static const SInt32 __CFBundleScriptCodesArray[] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  1,  4,  0,  0,  0,
     0,  0,  0,  2,  4,  9, 21,  3, 29, 29, 29, 29, 29,  0,  0,  4,
     7, 25,  0,  0,  0,  0, 29, 29,  0,  5,  7,  7,  7,  7,  7,  7,
     7,  7,  4, 24, 23,  7,  7,  7,  7, 27,  7,  4,  4,  4,  4, 26,
     9,  9,  9, 13, 13, 11, 10, 12, 17, 16, 14, 15, 18, 19, 20, 22,
    30,  0,  0,  0,  4, 28, 28, 28,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  7,  4, 26,  0,  0,  0,  0,  0, 28,
     0,  0,  0,  0,  6,  0,  0
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
    39, 39, 40,  0,  6,  0,  0
};

static SInt32 _CFBundleGetLanguageCodeForLocalization(CFStringRef localizationName) {
    SInt32 result = -1, i;
    char buff[256];
    CFIndex length = CFStringGetLength(localizationName);
    if ((length >= LANGUAGE_ABBREVIATION_LENGTH - 1) && (length <= 255) && CFStringGetCString(localizationName, buff, 255, kCFStringEncodingASCII)) {
        buff[255] = '\0';
        for (i = 0; -1 == result && i < NUM_LANGUAGE_NAMES; i++) {
            if (0 == strcmp(buff, __CFBundleLanguageNamesArray[i])) result = i;
        }
        if (0 == strcmp(buff, "zh_CN")) result = 33;	// hack for mixed-up Chinese language codes
        buff[LANGUAGE_ABBREVIATION_LENGTH - 1] = '\0';
        for (i = 0; -1 == result && i < NUM_LANGUAGE_ABBREVIATIONS * LANGUAGE_ABBREVIATION_LENGTH; i += LANGUAGE_ABBREVIATION_LENGTH) {
            if (buff[0] == *(__CFBundleLanguageAbbreviationsArray + i + 0) && buff[1] == *(__CFBundleLanguageAbbreviationsArray + i + 1)) result = i / LANGUAGE_ABBREVIATION_LENGTH;
        }
    }
    return result;
}

static CFStringRef _CFBundleCopyLanguageAbbreviationForLanguageCode(SInt32 languageCode) {
    CFStringRef result = NULL;
    if (0 <= languageCode && languageCode < NUM_LANGUAGE_ABBREVIATIONS) {
        const char *languageAbbreviation = __CFBundleLanguageAbbreviationsArray + languageCode * LANGUAGE_ABBREVIATION_LENGTH;
        if (languageAbbreviation != NULL && *languageAbbreviation != '\0') {
            result = CFStringCreateWithCStringNoCopy(NULL, languageAbbreviation, kCFStringEncodingASCII, kCFAllocatorNull);
        }
    }
    return result;
}

static CFStringRef _CFBundleCopyLanguageNameForLanguageCode(SInt32 languageCode) {
    CFStringRef result = NULL;
    if (0 <= languageCode && languageCode < NUM_LANGUAGE_NAMES) {
        const char *languageName = __CFBundleLanguageNamesArray[languageCode];
        if (languageName != NULL && *languageName != '\0') {
            result = CFStringCreateWithCStringNoCopy(NULL, languageName, kCFStringEncodingASCII, kCFAllocatorNull);
        }
    }
    return result;
}

static CFStringRef _CFBundleCopyLanguageAbbreviationForLocalization(CFStringRef localizationName) {
    CFStringRef result = NULL;
    SInt32 languageCode = _CFBundleGetLanguageCodeForLocalization(localizationName);
    if (languageCode >= 0) {
        result = _CFBundleCopyLanguageAbbreviationForLanguageCode(languageCode);
    } else {
        CFIndex length = CFStringGetLength(localizationName);
        if (length == LANGUAGE_ABBREVIATION_LENGTH - 1 || (length > LANGUAGE_ABBREVIATION_LENGTH - 1 && CFStringGetCharacterAtIndex(localizationName, LANGUAGE_ABBREVIATION_LENGTH - 1) == '_')) {
            result = CFStringCreateWithSubstring(NULL, localizationName, CFRangeMake(0, LANGUAGE_ABBREVIATION_LENGTH - 1));
        }
    }
    return result;
}

static CFStringRef _CFBundleCopyLanguageNameForLocalization(CFStringRef localizationName) {
    CFStringRef result = NULL;
    SInt32 languageCode = _CFBundleGetLanguageCodeForLocalization(localizationName);
    if (languageCode >= 0) {
        result = _CFBundleCopyLanguageNameForLanguageCode(languageCode);
    } else {
        result = CFStringCreateCopy(NULL, localizationName);
    }
    return result;
}

static SInt32 _CFBundleGetLanguageCodeForRegionCode(SInt32 regionCode) {
    SInt32 result = -1, i;
    if (52 == regionCode) {	// hack for mixed-up Chinese language codes
        result = 33;
    } else if (0 <= regionCode && regionCode < NUM_LOCALE_ABBREVIATIONS) {
        const char *localeAbbreviation = __CFBundleLocaleAbbreviationsArray + regionCode * LOCALE_ABBREVIATION_LENGTH;
        if (localeAbbreviation != NULL && *localeAbbreviation != '\0') {
            for (i = 0; -1 == result && i < NUM_LANGUAGE_ABBREVIATIONS * LANGUAGE_ABBREVIATION_LENGTH; i += LANGUAGE_ABBREVIATION_LENGTH) {
                if (localeAbbreviation[0] == *(__CFBundleLanguageAbbreviationsArray + i + 0) && localeAbbreviation[1] == *(__CFBundleLanguageAbbreviationsArray + i + 1)) result = i / LANGUAGE_ABBREVIATION_LENGTH;
            }
        }
    }
    return result;
}

static SInt32 _CFBundleGetRegionCodeForLanguageCode(SInt32 languageCode) {
    SInt32 result = -1, i;
    if (19 == languageCode) {	// hack for mixed-up Chinese language codes
        result = 53;
    } else if (0 <= languageCode && languageCode < NUM_LANGUAGE_ABBREVIATIONS) {
        const char *languageAbbreviation = __CFBundleLanguageAbbreviationsArray + languageCode * LANGUAGE_ABBREVIATION_LENGTH;
        if (languageAbbreviation != NULL && *languageAbbreviation != '\0') {
            for (i = 0; -1 == result && i < NUM_LOCALE_ABBREVIATIONS * LOCALE_ABBREVIATION_LENGTH; i += LOCALE_ABBREVIATION_LENGTH) {
                if (*(__CFBundleLocaleAbbreviationsArray + i + 0) == languageAbbreviation[0] && *(__CFBundleLocaleAbbreviationsArray + i + 1) == languageAbbreviation[1]) result = i / LOCALE_ABBREVIATION_LENGTH;
            }
        }
    }
    return result;
}

static SInt32 _CFBundleGetRegionCodeForLocalization(CFStringRef localizationName) {
    SInt32 result = -1, i;
    char buff[LOCALE_ABBREVIATION_LENGTH];
    CFIndex length = CFStringGetLength(localizationName);
    if ((length >= LANGUAGE_ABBREVIATION_LENGTH - 1) && (length <= LOCALE_ABBREVIATION_LENGTH - 1) && CFStringGetCString(localizationName, buff, LOCALE_ABBREVIATION_LENGTH, kCFStringEncodingASCII)) {
        buff[LOCALE_ABBREVIATION_LENGTH - 1] = '\0';
        for (i = 0; -1 == result && i < NUM_LOCALE_ABBREVIATIONS * LOCALE_ABBREVIATION_LENGTH; i += LOCALE_ABBREVIATION_LENGTH) {
            if (0 == strcmp(buff, __CFBundleLocaleAbbreviationsArray + i)) result = i / LOCALE_ABBREVIATION_LENGTH;
        }
    }
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
        if (localeAbbreviation != NULL && *localeAbbreviation != '\0') {
            result = CFStringCreateWithCStringNoCopy(NULL, localeAbbreviation, kCFStringEncodingASCII, kCFAllocatorNull);
        }
    }
    return result;
}

Boolean CFBundleGetLocalizationInfoForLocalization(CFStringRef localizationName, SInt32 *languageCode, SInt32 *regionCode, SInt32 *scriptCode, CFStringEncoding *stringEncoding) {
    SInt32 language = -1, region = -1, script = 0;
    CFStringEncoding encoding = kCFStringEncodingMacRoman;
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
    if (languageCode) *languageCode = language;
    if (regionCode) *regionCode = region;
    if (scriptCode) *scriptCode = script;
    if (stringEncoding) *stringEncoding = encoding;
    return (language != -1 || region != -1);
}

CFStringRef CFBundleCopyLocalizationForLocalizationInfo(SInt32 languageCode, SInt32 regionCode, SInt32 scriptCode, CFStringEncoding stringEncoding) {
    CFStringRef localizationName = NULL;
    if (!localizationName) {
        localizationName = _CFBundleCopyLocaleAbbreviationForRegionCode(regionCode);
    }
    if (!localizationName) {
        localizationName = _CFBundleCopyLanguageAbbreviationForLanguageCode(languageCode);
    }
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
    CFArrayRef result = NULL;
    static CFArrayRef userLanguages = NULL;
    static Boolean didit = false;
    CFArrayRef preferencesArray = NULL;
    // This is a temporary solution, until the argument domain is moved down into CFPreferences
    __CFSpinLock(&CFBundleResourceGlobalDataLock);
    if (!didit) {
        if (__CFAppleLanguages) {
            CFDataRef data;
            CFIndex length = strlen(__CFAppleLanguages);
            if (length > 0) {
                data = CFDataCreateWithBytesNoCopy(NULL, __CFAppleLanguages, length, kCFAllocatorNull);
                if (data) {
__CFSetNastyFile(CFSTR("<plist command-line argument>"));
                    userLanguages = CFPropertyListCreateFromXMLData(NULL, data, kCFPropertyListImmutable, NULL);
                    CFRelease(data);
                }
            }
        }
        if (!userLanguages && preferencesArray) userLanguages = CFRetain(preferencesArray);
        { // could perhaps read out of LANG environment variable
        CFStringRef english = CFSTR("English");
        if (!userLanguages) userLanguages = CFArrayCreate(kCFAllocatorDefault, (const void **)&english, 1, &kCFTypeArrayCallBacks);
        }
        if (userLanguages && CFGetTypeID(userLanguages) != CFArrayGetTypeID()) {
            CFRelease(userLanguages);
            userLanguages = NULL;
        }
        didit = true;
    }
    __CFSpinUnlock(&CFBundleResourceGlobalDataLock);
    if (preferencesArray) CFRelease(preferencesArray);
    if (!result && userLanguages) result = CFRetain(userLanguages);
    return result;
}

CF_EXPORT void _CFBundleGetLanguageAndRegionCodes(SInt32 *languageCode, SInt32 *regionCode) {
    // an attempt to answer the question, "what language are we running in?"
    // note that the question cannot be answered fully since it may depend on the bundle
    SInt32 language = -1, region = -1;
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFArrayRef languages = NULL;
    CFStringRef localizationName = NULL;
    if (mainBundle) {
        languages = _CFBundleGetLanguageSearchList(mainBundle);
        if (languages) CFRetain(languages);
    }
    if (!languages) languages = _CFBundleCopyUserLanguages(false);
    if (languages && (CFArrayGetCount(languages) > 0)) {
        localizationName = CFArrayGetValueAtIndex(languages, 0);
        language = _CFBundleGetLanguageCodeForLocalization(localizationName);
        region = _CFBundleGetRegionCodeForLocalization(localizationName);
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

static Boolean _CFBundleTryOnePreferredLprojNameInDirectory(CFAllocatorRef alloc, UniChar *pathUniChars, CFIndex pathLen, uint8_t version, CFDictionaryRef infoDict, CFStringRef curLangStr, CFMutableArrayRef lprojNames) {
    CFIndex curLangLen = CFStringGetLength(curLangStr);
    UniChar curLangUniChars[255];
    CFIndex savedPathLen;
    CFStringRef languageAbbreviation = _CFBundleCopyLanguageAbbreviationForLocalization(curLangStr), languageName = _CFBundleCopyLanguageNameForLocalization(curLangStr);
    Boolean foundOne = false;
    CFArrayRef predefinedLocalizations = NULL;
    CFRange predefinedLocalizationsRange;
    CFMutableStringRef cheapStr, tmpString;
#if USE_GETDIRENTRIES
    CFArrayRef directoryContents;
    CFRange directoryContentsRange;
#else
    Boolean isDir = false;
#endif

    // both of these used for temp string operations, for slightly
    // different purposes, where each type is appropriate
    cheapStr = CFStringCreateMutable(alloc, 0);
    _CFStrSetDesiredCapacity(cheapStr, CFMaxPathSize);
    tmpString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, NULL, 0, 0, kCFAllocatorNull);

#if USE_GETDIRENTRIES
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    directoryContents = _CFBundleCopyDirectoryContentsAtPath(cheapStr, true);
    directoryContentsRange = CFRangeMake(0, CFArrayGetCount(directoryContents));
#endif
    
    if (infoDict) {
        predefinedLocalizations = CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
        if (predefinedLocalizations != NULL && CFGetTypeID(predefinedLocalizations) != CFArrayGetTypeID()) {
            predefinedLocalizations = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleLocalizationsKey);
        }
    }
    predefinedLocalizationsRange = CFRangeMake(0, predefinedLocalizations ? CFArrayGetCount(predefinedLocalizations) : 0);
    
    CFStringGetCharacters(curLangStr, CFRangeMake(0, curLangLen), curLangUniChars);
    savedPathLen = pathLen;
    _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen);
    _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen);
#if USE_GETDIRENTRIES
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
    CFStringReplaceAll(cheapStr, tmpString);
    if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, curLangStr)) || (version != 4 && CFArrayContainsValue(directoryContents, directoryContentsRange, cheapStr))) {
#else
    CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
    CFStringReplaceAll(cheapStr, tmpString);
    if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, curLangStr)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif
        // We found one.
        CFArrayAppendValue(lprojNames, curLangStr);
        foundOne = true;
    }
    // Now, if the curLangStr was a region name, and we can map it to a language name, try that too.
    if (languageAbbreviation && !CFEqual(curLangStr, languageAbbreviation)) {
        curLangLen = CFStringGetLength(languageAbbreviation);
        CFStringGetCharacters(languageAbbreviation, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen);
        _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen);
#if USE_GETDIRENTRIES
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageAbbreviation)) || (version != 4 && CFArrayContainsValue(directoryContents, directoryContentsRange, cheapStr))) {
#else
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageAbbreviation)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif
            // We found one.
            CFArrayAppendValue(lprojNames, languageAbbreviation);
            foundOne = true;
        }
    }
    if (languageName && !CFEqual(curLangStr, languageName)) {
        curLangLen = CFStringGetLength(languageName);
        CFStringGetCharacters(languageName, CFRangeMake(0, curLangLen), curLangUniChars);
        pathLen = savedPathLen;
        _CFAppendPathComponent(pathUniChars, &pathLen, CFMaxPathSize, curLangUniChars, curLangLen);
        _CFAppendPathExtension(pathUniChars, &pathLen, CFMaxPathSize, _LprojUniChars, _LprojLen);
#if USE_GETDIRENTRIES
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars + savedPathLen + 1, pathLen - savedPathLen - 1, pathLen - savedPathLen - 1);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageName)) || (version != 4 && CFArrayContainsValue(directoryContents, directoryContentsRange, cheapStr))) {
#else
        CFStringSetExternalCharactersNoCopy(tmpString, pathUniChars, pathLen, pathLen);
        CFStringReplaceAll(cheapStr, tmpString);
        if ((predefinedLocalizations && CFArrayContainsValue(predefinedLocalizations, predefinedLocalizationsRange, languageName)) || (version != 4 && _CFIsResourceAtPath(cheapStr, &isDir) && isDir)) {
#endif
            // We found one.
            CFArrayAppendValue(lprojNames, languageName);
            foundOne = true;
        }
    }

    CFRelease(cheapStr);
    CFRelease(tmpString);
    if (languageAbbreviation) CFRelease(languageAbbreviation);
    if (languageName) CFRelease(languageName);
#if USE_GETDIRENTRIES
    CFRelease(directoryContents);
#endif

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

__private_extern__ void _CFBundleAddPreferredLprojNamesInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, uint8_t version, CFDictionaryRef infoDict, CFMutableArrayRef lprojNames, CFStringRef devLang) {
    // This function will add zero, one or two elements to the lprojNames array.
    // It examines the users preferred language list and the lproj directories inside the bundle directory.  It picks the lproj directory that is highest on the users list.
    // The users list can contain region names (like "en_US" for US English).  In this case, if the region lproj exists, it will be added, and, if the region's associated language lproj exists that will be added.
    CFURLRef resourcesURL = _CFBundleCopyResourcesDirectoryURLInDirectory(alloc, bundleURL, version);
    CFURLRef absoluteURL;
    CFIndex idx;
    CFIndex count;
    CFStringRef resourcesPath;
    UniChar pathUniChars[CFMaxPathSize];
    CFIndex pathLen;
    CFStringRef curLangStr;
    Boolean foundOne = false;

    CFArrayRef userLanguages;
    
    // Init the one-time-only unichar buffers.
    _CFEnsureStaticBuffersInited();

    // Get the path to the resources and extract into a buffer.
    absoluteURL = CFURLCopyAbsoluteURL(resourcesURL);
    resourcesPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFRelease(absoluteURL);
    pathLen = CFStringGetLength(resourcesPath);
    CFStringGetCharacters(resourcesPath, CFRangeMake(0, pathLen), pathUniChars);
    CFRelease(resourcesURL);
    CFRelease(resourcesPath);

    // First check the main bundle.
    if (!CFBundleAllowMixedLocalizations()) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            CFURLRef mainBundleURL = CFBundleCopyBundleURL(mainBundle);
            if (!CFEqual(bundleURL, mainBundleURL)) {
                // If there is a main bundle, and it isn't this one, try to use the language it prefers.
                CFArrayRef mainBundleLangs = _CFBundleGetLanguageSearchList(mainBundle);
                if (mainBundleLangs && (CFArrayGetCount(mainBundleLangs) > 0)) {
                    curLangStr = CFArrayGetValueAtIndex(mainBundleLangs, 0);
                    foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(alloc, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames);
                }
            }
            CFRelease(mainBundleURL);
        }
    }

    if (!foundOne) {
        // If we didn't find the main bundle's preferred language, look at the users' prefs again and find the best one.
        userLanguages = _CFBundleCopyUserLanguages(true);
        count = (userLanguages ? CFArrayGetCount(userLanguages) : 0);
        for (idx = 0; !foundOne && idx < count; idx++) {
            curLangStr = CFArrayGetValueAtIndex(userLanguages, idx);
            foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(alloc, pathUniChars, pathLen, version, infoDict, curLangStr, lprojNames);
        }
        // use development region and U.S. English as backstops
        if (!foundOne && devLang != NULL) {
            foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(alloc, pathUniChars, pathLen, version, infoDict, devLang, lprojNames);
        }
        if (!foundOne) {
            foundOne = _CFBundleTryOnePreferredLprojNameInDirectory(alloc, pathUniChars, pathLen, version, infoDict, CFSTR("en_US"), lprojNames);
        }
        if (userLanguages != NULL) {
            CFRelease(userLanguages);
        }
    }
}

static Boolean _CFBundleTryOnePreferredLprojNameInArray(CFArrayRef array, CFStringRef curLangStr, CFMutableArrayRef lprojNames) {
    Boolean foundOne = false;
    CFRange range = CFRangeMake(0, CFArrayGetCount(array));
    CFStringRef languageAbbreviation = _CFBundleCopyLanguageAbbreviationForLocalization(curLangStr), languageName = _CFBundleCopyLanguageNameForLocalization(curLangStr);

    if (CFArrayContainsValue(array, range, curLangStr)) {
        // We found one.
        CFArrayAppendValue(lprojNames, curLangStr);
        foundOne = true;
    }
    if (languageAbbreviation && !CFEqual(curLangStr, languageAbbreviation)) {
        if (CFArrayContainsValue(array, range, languageAbbreviation)) {
            // We found one.
            CFArrayAppendValue(lprojNames, languageAbbreviation);
            foundOne = true;
        }
    }
    if (languageName && !CFEqual(curLangStr, languageName)) {
        if (CFArrayContainsValue(array, range, languageName)) {
            // We found one.
            CFArrayAppendValue(lprojNames, languageName);
            foundOne = true;
        }
    }

    if (languageAbbreviation) CFRelease(languageAbbreviation);
    if (languageName) CFRelease(languageName);

    return foundOne;
}

static CFArrayRef _CFBundleCopyLocalizationsForPreferences(CFArrayRef locArray, CFArrayRef prefArray, Boolean considerMain) {
    CFMutableArrayRef lprojNames = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    Boolean foundOne = false, releasePrefArray = false;
    CFIndex idx, count;
    
    if (considerMain && !CFBundleAllowMixedLocalizations()) {
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            // If there is a main bundle, try to use the language it prefers.
            CFArrayRef mainBundleLangs = _CFBundleGetLanguageSearchList(mainBundle);
            if (mainBundleLangs && (CFArrayGetCount(mainBundleLangs) > 0)) {
                foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, CFArrayGetValueAtIndex(mainBundleLangs, 0), lprojNames);
            }
        }
    }
    if (!foundOne) {
        if (!prefArray) {
            prefArray = _CFBundleCopyUserLanguages(true);
            if (prefArray) releasePrefArray = true;
        }
        count = (prefArray ? CFArrayGetCount(prefArray) : 0);
        for (idx = 0; !foundOne && idx < count; idx++) {
            foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, CFArrayGetValueAtIndex(prefArray, idx), lprojNames);
        }
        // use U.S. English as backstop
        if (!foundOne) {
            foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, CFSTR("en_US"), lprojNames);
        }
        // use random entry as backstop
        if (!foundOne && CFArrayGetCount(lprojNames) > 0) {
            foundOne = _CFBundleTryOnePreferredLprojNameInArray(locArray, CFArrayGetValueAtIndex(locArray, 0), lprojNames);
        }
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

CF_EXPORT CFArrayRef CFBundleCopyLocalizationsForPreferences(CFArrayRef locArray, CFArrayRef prefArray) {return _CFBundleCopyLocalizationsForPreferences(locArray, prefArray, false);}

CF_EXPORT CFArrayRef CFBundleCopyPreferredLocalizationsFromArray(CFArrayRef locArray) {return _CFBundleCopyLocalizationsForPreferences(locArray, NULL, true);}

__private_extern__ CFArrayRef _CFBundleCopyLanguageSearchListInDirectory(CFAllocatorRef alloc, CFURLRef url, uint8_t *version) {
    CFMutableArrayRef langs = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    uint8_t localVersion = 0;
    CFDictionaryRef infoDict = _CFBundleCopyInfoDictionaryInDirectory(alloc, url, &localVersion);
    CFStringRef devLang = NULL;
    if (infoDict != NULL) {
        devLang = CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
    }
    if (devLang != NULL && (CFGetTypeID(devLang) != CFStringGetTypeID() || CFStringGetLength(devLang) == 0)) devLang = NULL;

    _CFBundleAddPreferredLprojNamesInDirectory(alloc, url, localVersion, infoDict, langs, devLang);
    
    if (devLang != NULL && CFArrayGetFirstIndexOfValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), devLang) < 0) {
        CFArrayAppendValue(langs, devLang);
    }
    if (CFArrayGetCount(langs) == 0) {
        // Total backstop behavior to avoid having an empty array. 
        CFArrayAppendValue(langs, CFSTR("en"));
    }
    if (infoDict != NULL) {
        CFRelease(infoDict);
    }
    if (version) {
        *version = localVersion;
    }
    return langs;
}

CF_EXPORT Boolean _CFBundleURLLooksLikeBundle(CFURLRef url) {
    Boolean result = false;
    CFBundleRef bundle = _CFBundleCreateIfLooksLikeBundle(NULL, url);
    if (bundle) {
        result = true;
        CFRelease(bundle);
    }
    return result;
}

// Note that subDirName is expected to be the string for a URL
CF_INLINE Boolean _CFBundleURLHasSubDir(CFURLRef url, CFStringRef subDirName) {
    CFURLRef dirURL;
    Boolean isDir, result = false;

    dirURL = CFURLCreateWithString(NULL, subDirName, url);
    if (dirURL != NULL) {
        if (_CFIsResourceAtURL(dirURL, &isDir) && isDir) {
            result = true;
        }
        CFRelease(dirURL);
    }
    return result;
}

__private_extern__ Boolean _CFBundleURLLooksLikeBundleVersion(CFURLRef url, uint8_t *version) {
    Boolean result = false;
    uint8_t localVersion = 0;

    // check for existence of "Resources" or "Contents" or "Support Files"
    // but check for the most likely one first
    // version 0:  old-style "Resources" bundles
    // version 1:  obsolete "Support Files" bundles
    // version 2:  modern "Contents" bundles
    // version 3:  none of the above (see below)
    // version 4:  not a bundle (for main bundle only)
    if (CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework/"))) {
        if (_CFBundleURLHasSubDir(url, _CFBundleResourcesURLFromBase0)) {
            result = true;
            localVersion = 0;
        } else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase2)) {
            result = true;
            localVersion = 2;
        } else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase1)) {
            result = true;
            localVersion = 1;
        }
    } else {
        if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase2)) {
            result = true;
            localVersion = 2;
        } else if (_CFBundleURLHasSubDir(url, _CFBundleResourcesURLFromBase0)) {
            result = true;
            localVersion = 0;
        } else if (_CFBundleURLHasSubDir(url, _CFBundleSupportFilesURLFromBase1)) {
            result = true;
            localVersion = 1;
        }
    }
    if (result && version) *version = localVersion;
    return result;
}

__private_extern__ CFDictionaryRef _CFBundleCopyInfoDictionaryInDirectory(CFAllocatorRef alloc, CFURLRef url, uint8_t *version) {
    CFDictionaryRef dict = NULL;
    char buff[CFMaxPathSize];
    uint8_t localVersion = 0;
    
    if (CFURLGetFileSystemRepresentation(url, true, buff, CFMaxPathSize)) {
        CFURLRef newURL = CFURLCreateFromFileSystemRepresentation(alloc, buff, strlen(buff), true);
        if (NULL == newURL) newURL = CFRetain(url);

        if (!_CFBundleURLLooksLikeBundleVersion(newURL, &localVersion)) {
            // version 3 is for flattened pseudo-bundles with no Contents, Support Files, or Resources directories
            localVersion = 3;
        }
        dict = _CFBundleCopyInfoDictionaryInDirectoryWithVersion(alloc, newURL, localVersion);
        CFRelease(newURL);
    }
    if (version) *version = localVersion;
    return dict;
}

__private_extern__ CFDictionaryRef _CFBundleCopyInfoDictionaryInDirectoryWithVersion(CFAllocatorRef alloc, CFURLRef url, uint8_t version) {
    CFDictionaryRef result = NULL;
    if (url != NULL) {
        CFURLRef infoURL = NULL;
        CFDataRef infoData = NULL;
        UniChar buff[CFMaxPathSize];
        CFIndex len;
        CFMutableStringRef cheapStr;
        CFStringRef infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension0, infoURLFromBase = _CFBundleInfoURLFromBase0;
        Boolean tryPlatformSpecific = true, tryGlobal = true;
#if USE_GETDIRENTRIES
        CFArrayRef directoryContents = NULL;
        CFRange directoryContentsRange = CFRangeMake(0, 0);
#endif    

        _CFEnsureStaticBuffersInited();

        if (0 == version) {
#if USE_GETDIRENTRIES
            // we want to read the Resources directory anyway, so we might as well do it now
            CFURLRef resourcesURL = _CFBundleCopyResourcesDirectoryURLInDirectory(alloc, url, version);
            CFURLRef absoluteURL = CFURLCopyAbsoluteURL(resourcesURL);
            CFStringRef resourcesPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
            directoryContents = _CFBundleCopyDirectoryContentsAtPath(resourcesPath, false);
            directoryContentsRange = CFRangeMake(0, CFArrayGetCount(directoryContents));
            CFRelease(resourcesPath);
            CFRelease(absoluteURL);
            CFRelease(resourcesURL);
#endif
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension0;
            infoURLFromBase = _CFBundleInfoURLFromBase0;
        } else if (1 == version) {
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension1;
            infoURLFromBase = _CFBundleInfoURLFromBase1;
        } else if (2 == version) {
            infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension2;
            infoURLFromBase = _CFBundleInfoURLFromBase2;
        } else if (3 == version) {
            CFStringRef posixPath = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
            // this test is necessary to exclude the case where a bundle is spuriously created from the innards of another bundle
            if (posixPath) {
                if (!(CFStringHasSuffix(posixPath, _CFBundleSupportFilesDirectoryName1) || CFStringHasSuffix(posixPath, _CFBundleSupportFilesDirectoryName2) || CFStringHasSuffix(posixPath, _CFBundleResourcesDirectoryName))) {
                    infoURLFromBaseNoExtension = _CFBundleInfoURLFromBaseNoExtension3;
                    infoURLFromBase = _CFBundleInfoURLFromBase3;
                }
                CFRelease(posixPath);
            }
        }
        
        len = CFStringGetLength(infoURLFromBaseNoExtension);
        CFStringGetCharacters(infoURLFromBaseNoExtension, CFRangeMake(0, len), buff);
        buff[len++] = (UniChar)'-';
        memmove(buff + len, _PlatformUniChars, _PlatformLen * sizeof(UniChar));
        len += _PlatformLen;
        _CFAppendPathExtension(buff, &len, CFMaxPathSize, _InfoExtensionUniChars, _InfoExtensionLen);
        cheapStr = CFStringCreateMutable(alloc, 0);
        CFStringAppendCharacters(cheapStr, buff, len);
        infoURL = CFURLCreateWithString(alloc, cheapStr, url);
#if USE_GETDIRENTRIES
        if (directoryContents) {
            CFIndex resourcesLen = CFStringGetLength(_CFBundleResourcesURLFromBase0);
            CFStringDelete(cheapStr, CFRangeMake(0, CFStringGetLength(cheapStr)));
            CFStringAppendCharacters(cheapStr, buff + resourcesLen, len - resourcesLen);
            tryPlatformSpecific = CFArrayContainsValue(directoryContents, directoryContentsRange, cheapStr);
        }
#endif
        if (tryPlatformSpecific) CFURLCreateDataAndPropertiesFromResource(alloc, infoURL, &infoData, NULL, NULL, NULL);
        //fprintf(stderr, "looking for ");CFShow(infoURL);if (infoData) fprintf(stderr, "found it\n");
        CFRelease(cheapStr);
        if (!infoData) {
            // Check for global Info.plist
            CFRelease(infoURL);
            infoURL = CFURLCreateWithString(alloc, infoURLFromBase, url);
#if USE_GETDIRENTRIES
            if (directoryContents) tryGlobal = CFArrayContainsValue(directoryContents, directoryContentsRange, _CFBundleInfoFileName);
#endif
            if (tryGlobal) CFURLCreateDataAndPropertiesFromResource(alloc, infoURL, &infoData, NULL, NULL, NULL);
            //fprintf(stderr, "looking for ");CFShow(infoURL);if (infoData) fprintf(stderr, "found it\n");
        }
        
        if (infoData) {
            result = CFPropertyListCreateFromXMLData(alloc, infoData, kCFPropertyListMutableContainers, NULL);
            if (result) {
                if (CFDictionaryGetTypeID() == CFGetTypeID(result)) {
                    CFDictionarySetValue((CFMutableDictionaryRef)result, _kCFBundleInfoPlistURLKey, infoURL);
                } else {
                    CFRelease(result);
                    result = NULL;
                }
            }
            CFRelease(infoData);
        }
        if (!result) {
            result = CFDictionaryCreateMutable(alloc, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }

        CFRelease(infoURL);
#if USE_GETDIRENTRIES
        if (directoryContents) CFRelease(directoryContents);
#endif
    }
    return result;
}

static Boolean _CFBundleGetPackageInfoInDirectoryWithInfoDictionary(CFAllocatorRef alloc, CFURLRef url, CFDictionaryRef infoDict, UInt32 *packageType, UInt32 *packageCreator) {
    Boolean retVal = false, hasType = false, hasCreator = false, releaseInfoDict = false;
    CFURLRef tempURL;
    CFDataRef pkgInfoData = NULL;

    // Check for a "real" new bundle
    tempURL = CFURLCreateWithString(alloc, _CFBundlePkgInfoURLFromBase2, url);
    CFURLCreateDataAndPropertiesFromResource(alloc, tempURL, &pkgInfoData, NULL, NULL, NULL);
    CFRelease(tempURL);
    if (pkgInfoData == NULL) {
        tempURL = CFURLCreateWithString(alloc, _CFBundlePkgInfoURLFromBase1, url);
        CFURLCreateDataAndPropertiesFromResource(alloc, tempURL, &pkgInfoData, NULL, NULL, NULL);
        CFRelease(tempURL);
    }
    if (pkgInfoData == NULL) {
        // Check for a "pseudo" new bundle
        tempURL = CFURLCreateWithString(alloc, _CFBundlePseudoPkgInfoURLFromBase, url);
        CFURLCreateDataAndPropertiesFromResource(alloc, tempURL, &pkgInfoData, NULL, NULL, NULL);
        CFRelease(tempURL);
    }

    // Now, either we have a pkgInfoData or not.  If not, then is it because this is a new bundle without one (do we allow this?), or is it dbecause it is an old bundle.
    // If we allow new bundles to not have a PkgInfo (because they already have the same data in the Info.plist), then we have to go read the info plist which makes failure expensive.
    // drd: So we assume that a new bundle _must_ have a PkgInfo if they have this data at all, otherwise we manufacture it from the extension.
    
    if ((pkgInfoData != NULL) && (CFDataGetLength(pkgInfoData) >= (int)(sizeof(UInt32) * 2))) {
        UInt32 *pkgInfo = (UInt32 *)CFDataGetBytePtr(pkgInfoData);

        if (packageType != NULL) {
            *packageType = CFSwapInt32BigToHost(pkgInfo[0]);
        }
        if (packageCreator != NULL) {
            *packageCreator = CFSwapInt32BigToHost(pkgInfo[1]);
        }
        retVal = hasType = hasCreator = true;
    }
    if (pkgInfoData != NULL) CFRelease(pkgInfoData);
    if (!retVal) {
        if (!infoDict) {
            infoDict = _CFBundleCopyInfoDictionaryInDirectory(alloc, url, NULL);
            releaseInfoDict = true;
        }
        if (infoDict) {
            CFStringRef typeString = CFDictionaryGetValue(infoDict, _kCFBundlePackageTypeKey), creatorString = CFDictionaryGetValue(infoDict, _kCFBundleSignatureKey);
            UInt32 tmp;
            CFIndex usedBufLen = 0;
            if (typeString && CFGetTypeID(typeString) == CFStringGetTypeID() && CFStringGetLength(typeString) == 4 && 4 == CFStringGetBytes(typeString, CFRangeMake(0, 4), kCFStringEncodingMacRoman, 0, false, (UInt8 *)&tmp, 4, &usedBufLen) && 4 == usedBufLen) {
                if (packageType != NULL) {
                    *packageType = CFSwapInt32BigToHost(tmp);
                }
                retVal = hasType = true;
            }
            if (creatorString && CFGetTypeID(creatorString) == CFStringGetTypeID() && CFStringGetLength(creatorString) == 4 && 4 == CFStringGetBytes(creatorString, CFRangeMake(0, 4), kCFStringEncodingMacRoman, 0, false, (UInt8 *)&tmp, 4, &usedBufLen) && 4 == usedBufLen) {
                if (packageCreator != NULL) {
                    *packageCreator = CFSwapInt32BigToHost(tmp);
                }
                retVal = hasCreator = true;
            }
            if (releaseInfoDict) CFRelease(infoDict);
        }
    }
    if (!hasType || !hasCreator) {
        // If this looks like a bundle then manufacture the type and creator.
        if (retVal || _CFBundleURLLooksLikeBundle(url)) {
            if (packageCreator != NULL && !hasCreator) {
                *packageCreator = 0x3f3f3f3f;  // '????'
            }
            if (packageType != NULL && !hasType) {
                CFStringRef urlStr;
                UniChar buff[CFMaxPathSize];
                CFIndex strLen, startOfExtension;
                CFURLRef absoluteURL;
                
                // Detect "app", "debug", "profile", or "framework" extensions
                absoluteURL = CFURLCopyAbsoluteURL(url);
                urlStr = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
                CFRelease(absoluteURL);
                strLen = CFStringGetLength(urlStr);
                CFStringGetCharacters(urlStr, CFRangeMake(0, strLen), buff);
                CFRelease(urlStr);
                startOfExtension = _CFStartOfPathExtension(buff, strLen);
                if (((strLen - startOfExtension == 4) || (strLen - startOfExtension == 5)) && (buff[startOfExtension] == (UniChar)'.') && (buff[startOfExtension+1] == (UniChar)'a') && (buff[startOfExtension+2] == (UniChar)'p') && (buff[startOfExtension+3] == (UniChar)'p') && ((strLen - startOfExtension == 4) || (buff[startOfExtension+4] == (UniChar)'/'))) {
                    // This is an app
                    *packageType = CFSwapInt32BigToHost(0x4150504c);  // 'APPL'
                } else if (((strLen - startOfExtension == 6) || (strLen - startOfExtension == 7)) && (buff[startOfExtension] == (UniChar)'.') && (buff[startOfExtension+1] == (UniChar)'d') && (buff[startOfExtension+2] == (UniChar)'e') && (buff[startOfExtension+3] == (UniChar)'b') && (buff[startOfExtension+4] == (UniChar)'u') && (buff[startOfExtension+5] == (UniChar)'g') && ((strLen - startOfExtension == 6) || (buff[startOfExtension+6] == (UniChar)'/'))) {
                    // This is an app (debug version)
                    *packageType = CFSwapInt32BigToHost(0x4150504c);  // 'APPL'
                } else if (((strLen - startOfExtension == 8) || (strLen - startOfExtension == 9)) && (buff[startOfExtension] == (UniChar)'.') && (buff[startOfExtension+1] == (UniChar)'p') && (buff[startOfExtension+2] == (UniChar)'r') && (buff[startOfExtension+3] == (UniChar)'o') && (buff[startOfExtension+4] == (UniChar)'f') && (buff[startOfExtension+5] == (UniChar)'i') && (buff[startOfExtension+6] == (UniChar)'l') && (buff[startOfExtension+7] == (UniChar)'e') && ((strLen - startOfExtension == 8) || (buff[startOfExtension+8] == (UniChar)'/'))) {
                    // This is an app (profile version)
                    *packageType = CFSwapInt32BigToHost(0x4150504c);  // 'APPL'
                } else if (((strLen - startOfExtension == 8) || (strLen - startOfExtension == 9)) && (buff[startOfExtension] == (UniChar)'.') && (buff[startOfExtension+1] == (UniChar)'s') && (buff[startOfExtension+2] == (UniChar)'e') && (buff[startOfExtension+3] == (UniChar)'r') && (buff[startOfExtension+4] == (UniChar)'v') && (buff[startOfExtension+5] == (UniChar)'i') && (buff[startOfExtension+6] == (UniChar)'c') && (buff[startOfExtension+7] == (UniChar)'e') && ((strLen - startOfExtension == 8) || (buff[startOfExtension+8] == (UniChar)'/'))) {
                    // This is a service
                    *packageType = CFSwapInt32BigToHost(0x4150504c);  // 'APPL'
                } else if (((strLen - startOfExtension == 10) || (strLen - startOfExtension == 11)) && (buff[startOfExtension] == (UniChar)'.') && (buff[startOfExtension+1] == (UniChar)'f') && (buff[startOfExtension+2] == (UniChar)'r') && (buff[startOfExtension+3] == (UniChar)'a') && (buff[startOfExtension+4] == (UniChar)'m') && (buff[startOfExtension+5] == (UniChar)'e') && (buff[startOfExtension+6] == (UniChar)'w') && (buff[startOfExtension+7] == (UniChar)'o') && (buff[startOfExtension+8] == (UniChar)'r') && (buff[startOfExtension+9] == (UniChar)'k') && ((strLen - startOfExtension == 10) || (buff[startOfExtension+10] == (UniChar)'/'))) {
                    // This is a framework
                    *packageType = CFSwapInt32BigToHost(0x464d574b);  // 'FMWK'
                } else {
                    // Default to BNDL for generic bundle
                    *packageType = CFSwapInt32BigToHost(0x424e444C);  // 'BNDL'
                }
            }
            retVal = true;
        }
    }
    return retVal;
}

CF_EXPORT Boolean _CFBundleGetPackageInfoInDirectory(CFAllocatorRef alloc, CFURLRef url, UInt32 *packageType, UInt32 *packageCreator) {return _CFBundleGetPackageInfoInDirectoryWithInfoDictionary(alloc, url, NULL, packageType, packageCreator);}

CF_EXPORT void CFBundleGetPackageInfo(CFBundleRef bundle, UInt32 *packageType, UInt32 *packageCreator) {
    CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
    if (!_CFBundleGetPackageInfoInDirectoryWithInfoDictionary(CFGetAllocator(bundle), bundleURL, CFBundleGetInfoDictionary(bundle), packageType, packageCreator)) {
        if (packageType != NULL) {
            *packageType = CFSwapInt32BigToHost(0x424e444C);  // 'BNDL'
        }
        if (packageCreator != NULL) {
            *packageCreator = 0x3f3f3f3f;  // '????'
        }
    }
    if (bundleURL) CFRelease(bundleURL);
}

CF_EXPORT Boolean CFBundleGetPackageInfoInDirectory(CFURLRef url, UInt32 *packageType, UInt32 *packageCreator) {return _CFBundleGetPackageInfoInDirectory(NULL, url, packageType, packageCreator);}

__private_extern__ CFStringRef _CFBundleGetPlatformExecutablesSubdirectoryName(void) {
#if defined(__MACOS8__)
    return CFSTR("MacOSClassic");
#elif defined(__WIN32__)
    return CFSTR("Windows");
#elif defined(__MACH__)
    return CFSTR("MacOS");
#elif defined(__hpux__)
    return CFSTR("HPUX");
#elif defined(__svr4__)
    return CFSTR("Solaris");
#elif defined(__LINUX__)
    return CFSTR("Linux");
#elif defined(__FREEBSD__)
    return CFSTR("FreeBSD");
#else
#warning CFBundle:  Unknown architecture
    return CFSTR("Other");
#endif
}

__private_extern__ CFStringRef _CFBundleGetAlternatePlatformExecutablesSubdirectoryName(void) {
#if defined(__MACOS8__)
    return CFSTR("Mac OS 8");
#elif defined(__WIN32__)
    return CFSTR("WinNT");
#elif defined(__MACH__)
    return CFSTR("Mac OS X");
#elif defined(__hpux__)
    return CFSTR("HP-UX");
#elif defined(__svr4__)
    return CFSTR("Solaris");
#elif defined(__LINUX__)
    return CFSTR("Linux");
#elif defined(__FREEBSD__)
    return CFSTR("FreeBSD");
#else
#warning CFBundle:  Unknown architecture
    return CFSTR("Other");
#endif
}

__private_extern__ CFStringRef _CFBundleGetOtherPlatformExecutablesSubdirectoryName(void) {
#if defined(__MACOS8__)
    return CFSTR("MacOS");
#elif defined(__WIN32__)
    return CFSTR("Other");
#elif defined(__MACH__)
    return CFSTR("MacOSClassic");
#elif defined(__hpux__)
    return CFSTR("Other");
#elif defined(__svr4__)
    return CFSTR("Other");
#elif defined(__LINUX__)
    return CFSTR("Other");
#elif defined(__FREEBSD__)
    return CFSTR("Other");
#else
#warning CFBundle:  Unknown architecture
    return CFSTR("Other");
#endif
}

__private_extern__ CFStringRef _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName(void) {
#if defined(__MACOS8__)
    return CFSTR("Mac OS X");
#elif defined(__WIN32__)
    return CFSTR("Other");
#elif defined(__MACH__)
    return CFSTR("Mac OS 8");
#elif defined(__hpux__)
    return CFSTR("Other");
#elif defined(__svr4__)
    return CFSTR("Other");
#elif defined(__LINUX__)
    return CFSTR("Other");
#elif defined(__FREEBSD__)
    return CFSTR("Other");
#else
#warning CFBundle:  Unknown architecture
    return CFSTR("Other");
#endif
}

__private_extern__ CFArrayRef _CFBundleCopyBundleRegionsArray(CFBundleRef bundle) {return CFBundleCopyBundleLocalizations(bundle);}

CF_EXPORT CFArrayRef CFBundleCopyBundleLocalizations(CFBundleRef bundle) {
    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
    uint8_t version = _CFBundleLayoutVersion(bundle);
    CFArrayRef urls = ((version != 4) ? _CFContentsOfDirectory(CFGetAllocator(bundle), NULL, NULL, resourcesURL, CFSTR("lproj")) : NULL);
    CFArrayRef predefinedLocalizations = NULL;
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFMutableArrayRef result = NULL;

    if (infoDict) {
        predefinedLocalizations = CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
        if (predefinedLocalizations != NULL && CFGetTypeID(predefinedLocalizations) != CFArrayGetTypeID()) {
            predefinedLocalizations = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleLocalizationsKey);
        }
        if (predefinedLocalizations != NULL) {
            CFIndex i, c = CFArrayGetCount(predefinedLocalizations);
            if (c > 0 && !result) {
                result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
            }
            for (i=0; i<c; i++) {
                CFArrayAppendValue(result, CFArrayGetValueAtIndex(predefinedLocalizations, i));
            }
        }
    }

    if (urls) {
        CFIndex i, c;
        CFURLRef curURL, curAbsoluteURL;
        CFStringRef curStr, regionStr;
        UniChar buff[CFMaxPathSize];
        CFIndex strLen, startOfLastPathComponent, regionLen;

        c = CFArrayGetCount(urls);
        if (c > 0 && !result) {
            result = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &kCFTypeArrayCallBacks);
        }
        for (i = 0; i < c; i++) {
            curURL = CFArrayGetValueAtIndex(urls, i);
            curAbsoluteURL = CFURLCopyAbsoluteURL(curURL);
            curStr = CFURLCopyFileSystemPath(curAbsoluteURL, PLATFORM_PATH_STYLE);
            CFRelease(curAbsoluteURL);
            strLen = CFStringGetLength(curStr);
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
    Boolean isDir;
    if (_CFIsResourceAtURL(url, &isDir)) {
        if (isDir) {
            result = _CFBundleCopyInfoDictionaryInDirectory(NULL, url, NULL);
        } else {
            result = _CFBundleCopyInfoDictionaryInExecutable(url);
        }
    }
    return result;
}

CFArrayRef CFBundleCopyLocalizationsForURL(CFURLRef url) {
    CFArrayRef result = NULL;
    CFBundleRef bundle = CFBundleCreate(NULL, url);
    CFStringRef devLang = NULL;
    if (bundle) {
        result = CFBundleCopyBundleLocalizations(bundle);
        CFRelease(bundle);
    } else {
        CFDictionaryRef infoDict = _CFBundleCopyInfoDictionaryInExecutable(url);
        if (infoDict) {
            CFArrayRef predefinedLocalizations = CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
            if (predefinedLocalizations != NULL && CFGetTypeID(predefinedLocalizations) == CFArrayGetTypeID()) {
                result = CFRetain(predefinedLocalizations);
            }
            if (!result) {
                devLang = CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
                if (devLang != NULL && (CFGetTypeID(devLang) == CFStringGetTypeID() && CFStringGetLength(devLang) > 0)) {
                    result = CFArrayCreate(NULL, (const void **)&devLang, 1, &kCFTypeArrayCallBacks);
                }
            }
            CFRelease(infoDict);
        }
    }
    return result;
}
