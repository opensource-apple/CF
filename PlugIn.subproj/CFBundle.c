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
/*	CFBundle.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Doug Davidson
*/

#include "CFBundle_Internal.h"
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFURLAccess.h>
#include <string.h>
#include "CFInternal.h"
#include "CFPriv.h"
#include <CoreFoundation/CFByteOrder.h>
#include "CFBundle_BinaryTypes.h"

#if defined(BINARY_SUPPORT_DYLD)
// Import the mach-o headers that define the macho magic numbers
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/arch.h>
#include <mach-o/swap.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>

#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>

#endif /* BINARY_SUPPORT_DYLD */

#if defined(__MACOS8__)
/* MacOS8 Headers */
#include <stat.h>
#include <Processes.h>
#else
/* Unixy & Windows Headers */
#include <sys/stat.h>
#include <stdlib.h>
#endif
#if defined(__LINUX__)
#include <fcntl.h>
#endif

#if defined(__WIN32__)
#undef __STDC__
#include <fcntl.h>
#include <io.h>
#endif

// Public CFBundle Info plist keys
CONST_STRING_DECL(kCFBundleInfoDictionaryVersionKey, "CFBundleInfoDictionaryVersion")
CONST_STRING_DECL(kCFBundleExecutableKey, "CFBundleExecutable")
CONST_STRING_DECL(kCFBundleIdentifierKey, "CFBundleIdentifier")
CONST_STRING_DECL(kCFBundleVersionKey, "CFBundleVersion")
CONST_STRING_DECL(kCFBundleDevelopmentRegionKey, "CFBundleDevelopmentRegion")
CONST_STRING_DECL(kCFBundleLocalizationsKey, "CFBundleLocalizations")

// Finder stuff
CONST_STRING_DECL(_kCFBundlePackageTypeKey, "CFBundlePackageType")
CONST_STRING_DECL(_kCFBundleSignatureKey, "CFBundleSignature")
CONST_STRING_DECL(_kCFBundleIconFileKey, "CFBundleIconFile")
CONST_STRING_DECL(_kCFBundleDocumentTypesKey, "CFBundleDocumentTypes")
CONST_STRING_DECL(_kCFBundleURLTypesKey, "CFBundleURLTypes")

// Keys that are usually localized in InfoPlist.strings
CONST_STRING_DECL(kCFBundleNameKey, "CFBundleName")
CONST_STRING_DECL(_kCFBundleDisplayNameKey, "CFBundleDisplayName")
CONST_STRING_DECL(_kCFBundleShortVersionStringKey, "CFBundleShortVersionString")
CONST_STRING_DECL(_kCFBundleGetInfoStringKey, "CFBundleGetInfoString")
CONST_STRING_DECL(_kCFBundleGetInfoHTMLKey, "CFBundleGetInfoHTML")

// Sub-keys for CFBundleDocumentTypes dictionaries
CONST_STRING_DECL(_kCFBundleTypeNameKey, "CFBundleTypeName")
CONST_STRING_DECL(_kCFBundleTypeRoleKey, "CFBundleTypeRole")
CONST_STRING_DECL(_kCFBundleTypeIconFileKey, "CFBundleTypeIconFile")
CONST_STRING_DECL(_kCFBundleTypeOSTypesKey, "CFBundleTypeOSTypes")
CONST_STRING_DECL(_kCFBundleTypeExtensionsKey, "CFBundleTypeExtensions")
CONST_STRING_DECL(_kCFBundleTypeMIMETypesKey, "CFBundleTypeMIMETypes")

// Sub-keys for CFBundleURLTypes dictionaries
CONST_STRING_DECL(_kCFBundleURLNameKey, "CFBundleURLName")
CONST_STRING_DECL(_kCFBundleURLIconFileKey, "CFBundleURLIconFile")
CONST_STRING_DECL(_kCFBundleURLSchemesKey, "CFBundleURLSchemes")

// Compatibility key names
CONST_STRING_DECL(_kCFBundleOldExecutableKey, "NSExecutable")
CONST_STRING_DECL(_kCFBundleOldInfoDictionaryVersionKey, "NSInfoPlistVersion")
CONST_STRING_DECL(_kCFBundleOldNameKey, "NSHumanReadableName")
CONST_STRING_DECL(_kCFBundleOldIconFileKey, "NSIcon")
CONST_STRING_DECL(_kCFBundleOldDocumentTypesKey, "NSTypes")
CONST_STRING_DECL(_kCFBundleOldShortVersionStringKey, "NSAppVersion")

// Compatibility CFBundleDocumentTypes key names
CONST_STRING_DECL(_kCFBundleOldTypeNameKey, "NSName")
CONST_STRING_DECL(_kCFBundleOldTypeRoleKey, "NSRole")
CONST_STRING_DECL(_kCFBundleOldTypeIconFileKey, "NSIcon")
CONST_STRING_DECL(_kCFBundleOldTypeExtensions1Key, "NSUnixExtensions")
CONST_STRING_DECL(_kCFBundleOldTypeExtensions2Key, "NSDOSExtensions")
CONST_STRING_DECL(_kCFBundleOldTypeOSTypesKey, "NSMacOSType")

// Internally used keys for loaded Info plists.
CONST_STRING_DECL(_kCFBundleInfoPlistURLKey, "CFBundleInfoPlistURL")
CONST_STRING_DECL(_kCFBundleNumericVersionKey, "CFBundleNumericVersion")
CONST_STRING_DECL(_kCFBundleExecutablePathKey, "CFBundleExecutablePath")
CONST_STRING_DECL(_kCFBundleResourcesFileMappedKey, "CSResourcesFileMapped")
CONST_STRING_DECL(_kCFBundleCFMLoadAsBundleKey, "CFBundleCFMLoadAsBundle")
CONST_STRING_DECL(_kCFBundleAllowMixedLocalizationsKey, "CFBundleAllowMixedLocalizations")

static CFTypeID __kCFBundleTypeID = _kCFRuntimeNotATypeID;

struct __CFBundle {
    CFRuntimeBase _base;

    CFURLRef _url;
    CFDateRef _modDate;

    CFDictionaryRef _infoDict;
    CFDictionaryRef _localInfoDict;
    CFArrayRef _searchLanguages;

    __CFPBinaryType _binaryType;
    Boolean _isLoaded;
    uint8_t _version;
    Boolean _sharesStringsFiles;
    char _padding[1];

    /* CFM goop */
    void *_connectionCookie;

    /* DYLD goop */
    void *_imageCookie;
    void *_moduleCookie;

    /* CFM<->DYLD glue */
    CFMutableDictionaryRef _glueDict;
    
    /* Resource fork goop */
    _CFResourceData _resourceData;

    _CFPlugInData _plugInData;

#if defined(BINARY_SUPPORT_DLL)
    HMODULE _hModule;
#endif

};

static CFSpinLock_t CFBundleGlobalDataLock = 0;

static CFMutableDictionaryRef _bundlesByURL = NULL;
static CFMutableDictionaryRef _bundlesByIdentifier = NULL;

// For scheduled lazy unloading.  Used by CFPlugIn.
static CFMutableSetRef _bundlesToUnload = NULL;
static Boolean _scheduledBundlesAreUnloading = false;

// Various lists of all bundles.
static CFMutableArrayRef _allBundles = NULL;

static Boolean _initedMainBundle = false;
static CFBundleRef _mainBundle = NULL;
static CFStringRef _defaultLocalization = NULL;

// Forward declares functions.
static CFBundleRef _CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL, Boolean alreadyLocked);
static CFStringRef _CFBundleCopyExecutableName(CFAllocatorRef alloc, CFBundleRef bundle, CFURLRef url, CFDictionaryRef infoDict);
static CFURLRef _CFBundleCopyExecutableURLIgnoringCache(CFBundleRef bundle);
static void _CFBundleEnsureBundleExistsForImagePath(CFStringRef imagePath);
static void _CFBundleEnsureBundlesExistForImagePaths(CFArrayRef imagePaths);
static void _CFBundleEnsureBundlesUpToDateWithHintAlreadyLocked(CFStringRef hint);
static void _CFBundleEnsureAllBundlesUpToDateAlreadyLocked(void);
static void _CFBundleCheckWorkarounds(CFBundleRef bundle);
#if defined(BINARY_SUPPORT_DYLD)
static CFDictionaryRef _CFBundleGrokInfoDictFromMainExecutable(void);
static CFStringRef _CFBundleDYLDCopyLoadedImagePathForPointer(void *p);
static void *_CFBundleDYLDGetSymbolByNameWithSearch(CFBundleRef bundle, CFStringRef symbolName, Boolean globalSearch);
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DYLD) && defined(BINARY_SUPPORT_CFM) && defined(__ppc__)
static void *_CFBundleFunctionPointerForTVector(CFAllocatorRef allocator, void *tvp);
static void *_CFBundleTVectorForFunctionPointer(CFAllocatorRef allocator, void *fp);
#endif /* BINARY_SUPPORT_DYLD && BINARY_SUPPORT_CFM && __ppc__ */

static void _CFBundleAddToTables(CFBundleRef bundle, Boolean alreadyLocked) {
    CFStringRef bundleID = CFBundleGetIdentifier(bundle);

    if (!alreadyLocked) {
        __CFSpinLock(&CFBundleGlobalDataLock);
    }
    
    // Add to the _allBundles list
    if (_allBundles == NULL) {
        // Create this from the default allocator
        CFArrayCallBacks nonRetainingArrayCallbacks = kCFTypeArrayCallBacks;
        nonRetainingArrayCallbacks.retain = NULL;
        nonRetainingArrayCallbacks.release = NULL;
        _allBundles = CFArrayCreateMutable(NULL, 0, &nonRetainingArrayCallbacks);
    }
    CFArrayAppendValue(_allBundles, bundle);
    
    // Add to the table that maps urls to bundles
    if (_bundlesByURL == NULL) {
        // Create this from the default allocator
        CFDictionaryValueCallBacks nonRetainingDictionaryValueCallbacks = kCFTypeDictionaryValueCallBacks;
        nonRetainingDictionaryValueCallbacks.retain = NULL;
        nonRetainingDictionaryValueCallbacks.release = NULL;
        _bundlesByURL = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &nonRetainingDictionaryValueCallbacks);
    }
    CFDictionarySetValue(_bundlesByURL, bundle->_url, bundle);

    // Add to the table that maps identifiers to bundles
    if (bundleID) {
        CFBundleRef existingBundle = NULL;
        Boolean addIt = true;
        if (_bundlesByIdentifier == NULL) {
            // Create this from the default allocator
            CFDictionaryValueCallBacks nonRetainingDictionaryValueCallbacks = kCFTypeDictionaryValueCallBacks;
            nonRetainingDictionaryValueCallbacks.retain = NULL;
            nonRetainingDictionaryValueCallbacks.release = NULL;
            _bundlesByIdentifier = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &nonRetainingDictionaryValueCallbacks);
        }
        existingBundle = (CFBundleRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
        if (existingBundle) {
            UInt32 existingVersion, newVersion;
            existingVersion = CFBundleGetVersionNumber(existingBundle);
            newVersion = CFBundleGetVersionNumber(bundle);
            if (newVersion < existingVersion) {
                // Less than to means that if you load two bundles with the same identifier and the same version, the last one wins.
                addIt = false;
            }
        }
        if (addIt) {
            CFDictionarySetValue(_bundlesByIdentifier, bundleID, bundle);
        }
    }
    if (!alreadyLocked) {
        __CFSpinUnlock(&CFBundleGlobalDataLock);
    }
}

static void _CFBundleRemoveFromTables(CFBundleRef bundle) {
    CFStringRef bundleID = CFBundleGetIdentifier(bundle);

    __CFSpinLock(&CFBundleGlobalDataLock);

    // Remove from the various lists
    if (_allBundles != NULL) {
        CFIndex i = CFArrayGetFirstIndexOfValue(_allBundles, CFRangeMake(0, CFArrayGetCount(_allBundles)), bundle);
        if (i>=0) {
            CFArrayRemoveValueAtIndex(_allBundles, i);
        }
    }

    // Remove from the table that maps urls to bundles
    if (_bundlesByURL != NULL) {
        CFDictionaryRemoveValue(_bundlesByURL, bundle->_url);
    }
    
    // Remove from the table that maps identifiers to bundles
    if ((bundleID != NULL) && (_bundlesByIdentifier != NULL)) {
        if (CFDictionaryGetValue(_bundlesByIdentifier, bundleID) == bundle) {
            CFDictionaryRemoveValue(_bundlesByIdentifier, bundleID);
        }
    }
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

__private_extern__ CFBundleRef _CFBundleFindByURL(CFURLRef url, Boolean alreadyLocked) {
    CFBundleRef result = NULL;
    if (!alreadyLocked) {
        __CFSpinLock(&CFBundleGlobalDataLock);
    }
    if (_bundlesByURL != NULL) {
        result = (CFBundleRef)CFDictionaryGetValue(_bundlesByURL, url);
    }
    if (!alreadyLocked) {
        __CFSpinUnlock(&CFBundleGlobalDataLock);
    }
    return result;
}

static CFURLRef _CFBundleCopyBundleURLForExecutablePath(CFStringRef str) {
    //!!! need to handle frameworks, NT; need to integrate with NSBundle - drd
    UniChar buff[CFMaxPathSize];
    CFIndex buffLen;
    CFURLRef url = NULL;
    CFStringRef outstr;
    
    buffLen = CFStringGetLength(str);
    CFStringGetCharacters(str, CFRangeMake(0, buffLen), buff);
    buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);  // Remove exe name

    if (buffLen > 0) {
        // See if this is a new bundle.  If it is, we have to remove more path components.
        CFIndex startOfLastDir = _CFStartOfLastPathComponent(buff, buffLen);
        if ((startOfLastDir > 0) && (startOfLastDir < buffLen)) {
            CFStringRef lastDirName = CFStringCreateWithCharacters(NULL, &(buff[startOfLastDir]), buffLen - startOfLastDir);

            if (CFEqual(lastDirName, _CFBundleGetPlatformExecutablesSubdirectoryName()) || CFEqual(lastDirName, _CFBundleGetAlternatePlatformExecutablesSubdirectoryName()) || CFEqual(lastDirName, _CFBundleGetOtherPlatformExecutablesSubdirectoryName()) || CFEqual(lastDirName, _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName())) {
                // This is a new bundle.  Back off a few more levels
                if (buffLen > 0) {
                    // Remove platform folder
                    buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
                }
                if (buffLen > 0) {
                    // Remove executables folder (if present)
                    CFIndex startOfNextDir = _CFStartOfLastPathComponent(buff, buffLen);
                    if ((startOfNextDir > 0) && (startOfNextDir < buffLen)) {
                        CFStringRef nextDirName = CFStringCreateWithCharacters(NULL, &(buff[startOfNextDir]), buffLen - startOfNextDir);
                        if (CFEqual(nextDirName, _CFBundleExecutablesDirectoryName)) {
                            buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
                        }
                        CFRelease(nextDirName);
                    }
                }
                if (buffLen > 0) {
                    // Remove support files folder
                    buffLen = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
                }
            }
            CFRelease(lastDirName);
        }
    }

    if (buffLen > 0) {
        outstr = CFStringCreateWithCharactersNoCopy(NULL, buff, buffLen, kCFAllocatorNull);
        url = CFURLCreateWithFileSystemPath(NULL, outstr, PLATFORM_PATH_STYLE, true);
        CFRelease(outstr);
    }
    return url;
}

static CFURLRef _CFBundleCopyResolvedURLForExecutableURL(CFURLRef url) {
    // this is necessary so that we match any sanitization CFURL may perform on the result of _CFBundleCopyBundleURLForExecutableURL()
    CFURLRef absoluteURL, url1, url2, outURL = NULL;
    CFStringRef str, str1, str2;
    absoluteURL = CFURLCopyAbsoluteURL(url);
    str = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    if (str) {
        UniChar buff[CFMaxPathSize];
        CFIndex buffLen = CFStringGetLength(str), len1;
        CFStringGetCharacters(str, CFRangeMake(0, buffLen), buff);
        len1 = _CFLengthAfterDeletingLastPathComponent(buff, buffLen);
        if (len1 > 0 && len1 + 1 < buffLen) {
            str1 = CFStringCreateWithCharacters(NULL, buff, len1);
            str2 = CFStringCreateWithCharacters(NULL, buff + len1 + 1, buffLen - len1 - 1);
            if (str1 && str2) {
                url1 = CFURLCreateWithFileSystemPath(NULL, str1, PLATFORM_PATH_STYLE, true);
                if (url1) {
                    url2 = CFURLCreateWithFileSystemPathRelativeToBase(NULL, str2, PLATFORM_PATH_STYLE, false, url1);
                    if (url2) {
                        outURL = CFURLCopyAbsoluteURL(url2);
                        CFRelease(url2);
                    }
                    CFRelease(url1);
                }
            }
            if (str1) CFRelease(str1);
            if (str2) CFRelease(str2);
        }
        CFRelease(str);
    }
    if (!outURL) {
        outURL = absoluteURL;
    } else {
        CFRelease(absoluteURL);
    }
    return outURL;
}

CFURLRef _CFBundleCopyBundleURLForExecutableURL(CFURLRef url) {
    CFURLRef resolvedURL, outurl = NULL;
    CFStringRef str;
    resolvedURL = _CFBundleCopyResolvedURLForExecutableURL(url);
    str = CFURLCopyFileSystemPath(resolvedURL, PLATFORM_PATH_STYLE);
    if (str != NULL) {
        outurl = _CFBundleCopyBundleURLForExecutablePath(str);
        CFRelease(str);
    }
    CFRelease(resolvedURL);
    return outurl;
}

CFBundleRef _CFBundleCreateIfLooksLikeBundle(CFAllocatorRef allocator, CFURLRef url) {
    CFBundleRef bundle = CFBundleCreate(allocator, url);
    
    // exclude type 0 bundles with no binary (or CFM binary) and no Info.plist, since they give too many false positives
    if (bundle && 0 == bundle->_version) {
        CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
        if (!infoDict || 0 == CFDictionaryGetCount(infoDict)) {
#if defined(BINARY_SUPPORT_CFM) && defined(BINARY_SUPPORT_DYLD)
            CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
            if (executableURL) {
                if (bundle->_binaryType == __CFBundleUnknownBinary) {
                    bundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
                }
                if (bundle->_binaryType == __CFBundleCFMBinary || bundle->_binaryType == __CFBundleUnreadableBinary) {
                    bundle->_version = 4;
                } else {
                    bundle->_resourceData._executableLacksResourceFork = true;
                }
                CFRelease(executableURL);
            } else {
                bundle->_version = 4;
            }
#elif defined(BINARY_SUPPORT_CFM)
            bundle->_version = 4;
#else 
            CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
            if (executableURL) {
                CFRelease(executableURL);
            } else {
                bundle->_version = 4;
            }
#endif /* BINARY_SUPPORT_CFM && BINARY_SUPPORT_DYLD */
        }
    }
    if (bundle && (3 == bundle->_version || 4 == bundle->_version)) {
        CFRelease(bundle);
        bundle = NULL;
    }
    return bundle;
}

CFBundleRef _CFBundleGetMainBundleIfLooksLikeBundle(void) {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle && (3 == mainBundle->_version || 4 == mainBundle->_version)) {
        mainBundle = NULL;
    }
    return mainBundle;
}

CFBundleRef _CFBundleCreateWithExecutableURLIfLooksLikeBundle(CFAllocatorRef allocator, CFURLRef url) {
    CFBundleRef bundle = NULL;
    CFURLRef bundleURL = _CFBundleCopyBundleURLForExecutableURL(url), resolvedURL = _CFBundleCopyResolvedURLForExecutableURL(url);
    if (bundleURL && resolvedURL) {
        bundle = _CFBundleCreateIfLooksLikeBundle(allocator, bundleURL);
        if (bundle) {
            CFURLRef executableURL = _CFBundleCopyExecutableURLIgnoringCache(bundle);
            char buff1[CFMaxPathSize], buff2[CFMaxPathSize];
            if (!executableURL || !CFURLGetFileSystemRepresentation(resolvedURL, true, buff1, CFMaxPathSize) || !CFURLGetFileSystemRepresentation(executableURL, true, buff2, CFMaxPathSize) || 0 != strcmp(buff1, buff2)) {
                CFRelease(bundle);
                bundle = NULL;
            }
            if (executableURL) CFRelease(executableURL);
        }
    }
    if (bundleURL) CFRelease(bundleURL);
    if (resolvedURL) CFRelease(resolvedURL);
    return bundle;
}

static CFBundleRef _CFBundleGetMainBundleAlreadyLocked(void) {
    if (!_initedMainBundle) {
        const char *processPath;
        CFStringRef str = NULL;
        CFURLRef executableURL = NULL, bundleURL = NULL;
#if defined(BINARY_SUPPORT_CFM)
        Boolean versRegionOverrides = false;
#endif /* BINARY_SUPPORT_CFM */
#if defined(__MACOS8__)
        // do not use Posix-styled _CFProcessPath()
        ProcessSerialNumber gProcessID;
        ProcessInfoRec processInfo;
        FSSpec processAppSpec;

        processInfo.processInfoLength = sizeof(ProcessInfoRec);
        processInfo.processAppSpec = &processAppSpec;

        if ((GetCurrentProcess(&gProcessID) == noErr) && (GetProcessInformation(&gProcessID, &processInfo) == noErr)) {
            executableURL = _CFCreateURLFromFSSpec(NULL, (void *)(&processAppSpec), false);
        }
#endif
        _initedMainBundle = true;
        processPath = _CFProcessPath();
        if (processPath) {
            str = CFStringCreateWithCString(NULL, processPath, CFStringFileSystemEncoding());
            if (!executableURL) executableURL = CFURLCreateWithFileSystemPath(NULL, str, PLATFORM_PATH_STYLE, false);
        }
        if (executableURL) {
            bundleURL = _CFBundleCopyBundleURLForExecutableURL(executableURL);
        }
        if (bundleURL != NULL) {
            // make sure that main bundle has executable path
            //??? what if we are not the main executable in the bundle?
            _mainBundle = _CFBundleCreate(NULL, bundleURL, true);
            if (_mainBundle != NULL) {
                CFBundleGetInfoDictionary(_mainBundle);
                // make sure that the main bundle is listed as loaded, and mark it as executable
                _mainBundle->_isLoaded = true;
#if defined(BINARY_SUPPORT_DYLD)
                if (_mainBundle->_binaryType == __CFBundleUnknownBinary) {
                    if (!executableURL) {
                        _mainBundle->_binaryType = __CFBundleNoBinary;
                    } else {
                        _mainBundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
#if defined(BINARY_SUPPORT_CFM)
                        if (_mainBundle->_binaryType != __CFBundleCFMBinary && _mainBundle->_binaryType != __CFBundleUnreadableBinary) {
                            _mainBundle->_resourceData._executableLacksResourceFork = true;
                        }
#endif /* BINARY_SUPPORT_CFM */
                    }
                }                
#endif /* BINARY_SUPPORT_DYLD */
                if (_mainBundle->_infoDict == NULL || CFDictionaryGetCount(_mainBundle->_infoDict) == 0) {
                    // if type 3 bundle and no Info.plist, treat as unbundled, since this gives too many false positives
                    if (_mainBundle->_version == 3) _mainBundle->_version = 4;
                    if (_mainBundle->_version == 0) {
                        // if type 0 bundle and no Info.plist and not main executable for bundle, treat as unbundled, since this gives too many false positives
                        CFStringRef executableName = _CFBundleCopyExecutableName(NULL, _mainBundle, NULL, NULL);
                        if (!executableName || !CFStringHasSuffix(str, executableName)) _mainBundle->_version = 4;
                        if (executableName) CFRelease(executableName);
                    }
#if defined(BINARY_SUPPORT_DYLD)
                    if (_mainBundle->_binaryType == __CFBundleDYLDExecutableBinary) {
                        if (_mainBundle->_infoDict != NULL) CFRelease(_mainBundle->_infoDict);
                        _mainBundle->_infoDict = _CFBundleGrokInfoDictFromMainExecutable();
                    }
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_CFM)
                    if (_mainBundle->_binaryType == __CFBundleCFMBinary || _mainBundle->_binaryType == __CFBundleUnreadableBinary) {
                        // if type 0 bundle and CFM binary and no Info.plist, treat as unbundled, since this also gives too many false positives
                        if (_mainBundle->_version == 0) _mainBundle->_version = 4;
                        if (_mainBundle->_infoDict != NULL) CFRelease(_mainBundle->_infoDict);
                        _mainBundle->_infoDict = _CFBundleCopyInfoDictionaryInResourceForkWithAllocator(CFGetAllocator(_mainBundle), executableURL);
                        if (_mainBundle->_binaryType == __CFBundleUnreadableBinary && _mainBundle->_infoDict != NULL && CFDictionaryGetValue(_mainBundle->_infoDict, kCFBundleDevelopmentRegionKey) != NULL) versRegionOverrides = true;
                    }
#endif /* BINARY_SUPPORT_CFM */
                }
                if (_mainBundle->_infoDict == NULL) {
                    _mainBundle->_infoDict = CFDictionaryCreateMutable(CFGetAllocator(_mainBundle), 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                }
                if (NULL == CFDictionaryGetValue(_mainBundle->_infoDict, _kCFBundleExecutablePathKey)) {
                    CFDictionarySetValue((CFMutableDictionaryRef)(_mainBundle->_infoDict), _kCFBundleExecutablePathKey, str);
                }
#if defined(BINARY_SUPPORT_DYLD)
                // get cookie for already-loaded main bundle
                if (_mainBundle->_binaryType == __CFBundleDYLDExecutableBinary && !_mainBundle->_imageCookie) {
                    _mainBundle->_imageCookie = (void *)_dyld_get_image_header(0);
                }
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_CFM)
                if (versRegionOverrides) {
                    // This is a hack to preserve backward compatibility for certain broken applications (2761067)
                    CFStringRef devLang = _CFBundleCopyBundleDevelopmentRegionFromVersResource(_mainBundle);
                    if (devLang != NULL) {
                        CFDictionarySetValue((CFMutableDictionaryRef)(_mainBundle->_infoDict), kCFBundleDevelopmentRegionKey, devLang);
                        CFRelease(devLang);
                    }
                }
#endif /* BINARY_SUPPORT_CFM */
            }
        }
        if (bundleURL) CFRelease(bundleURL);
        if (str) CFRelease(str);
        if (executableURL) CFRelease(executableURL);
    }
    return _mainBundle;
}

CFBundleRef CFBundleGetMainBundle(void) {
    CFBundleRef mainBundle;
    __CFSpinLock(&CFBundleGlobalDataLock);
    mainBundle = _CFBundleGetMainBundleAlreadyLocked();
    __CFSpinUnlock(&CFBundleGlobalDataLock);
    return mainBundle;
}

#if defined(BINARY_SUPPORT_DYLD)

static void *_CFBundleReturnAddressFromFrameAddress(void *addr)
{
    void *ret;
#if defined(__ppc__)
    __asm__ volatile("lwz %0,0x0008(%1)" : "=r" (ret) : "b" (addr));
#elif defined(__i386__)
    __asm__ volatile("movl 0x4(%1),%0" : "=r" (ret) : "r" (addr));
#elif defined(hppa)
    __asm__ volatile("ldw 0x4(%1),%0" : "=r" (ret) : "r" (addr));
#elif defined(sparc)
    __asm__ volatile("ta 0x3");
    __asm__ volatile("ld [%1 + 60],%0" : "=r" (ret) : "r" (addr));
#else
#warning Do not know how to define _CFBundleReturnAddressFromFrameAddress on this architecture
    ret = NULL;
#endif
    return ret;
}

#endif

CFBundleRef CFBundleGetBundleWithIdentifier(CFStringRef bundleID) {
    CFBundleRef result = NULL;
    if (bundleID) {
        __CFSpinLock(&CFBundleGlobalDataLock);
        (void)_CFBundleGetMainBundleAlreadyLocked();
        if (_bundlesByIdentifier != NULL) {
            result = (CFBundleRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
        }
#if defined(BINARY_SUPPORT_DYLD)
        if (result == NULL) {
            // Try to create the bundle for the caller and try again
            void *p = _CFBundleReturnAddressFromFrameAddress(__builtin_frame_address(1));
            CFStringRef imagePath = _CFBundleDYLDCopyLoadedImagePathForPointer(p);
            if (imagePath != NULL) {
                _CFBundleEnsureBundleExistsForImagePath(imagePath);
                CFRelease(imagePath);
            }
            if (_bundlesByIdentifier != NULL) {
                result = (CFBundleRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
            }
        }
#endif
        if (result == NULL) {
            // Try to guess the bundle from the identifier and try again
            _CFBundleEnsureBundlesUpToDateWithHintAlreadyLocked(bundleID);
            if (_bundlesByIdentifier != NULL) {
                result = (CFBundleRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
            }
        }
        if (result == NULL) {
            // Make sure all bundles have been created and try again.
            _CFBundleEnsureAllBundlesUpToDateAlreadyLocked();
            if (_bundlesByIdentifier != NULL) {
                result = (CFBundleRef)CFDictionaryGetValue(_bundlesByIdentifier, bundleID);
            }
        }
        __CFSpinUnlock(&CFBundleGlobalDataLock);
    }
    return result;
}

static CFStringRef __CFBundleCopyDescription(CFTypeRef cf) {
    char buff[CFMaxPathSize];
    CFStringRef path = NULL, binaryType = NULL, retval = NULL;
    if (((CFBundleRef)cf)->_url != NULL && CFURLGetFileSystemRepresentation(((CFBundleRef)cf)->_url, true, buff, CFMaxPathSize)) {
        path = CFStringCreateWithCString(NULL, buff, CFStringFileSystemEncoding());
    }
    switch (((CFBundleRef)cf)->_binaryType) {
        case __CFBundleCFMBinary:
            binaryType = CFSTR("");
            break;
        case __CFBundleDYLDExecutableBinary:
            binaryType = CFSTR("executable, ");
            break;
        case __CFBundleDYLDBundleBinary:
            binaryType = CFSTR("bundle, ");
            break;
        case __CFBundleDYLDFrameworkBinary:
            binaryType = CFSTR("framework, ");
            break;
        case __CFBundleDLLBinary:
            binaryType = CFSTR("DLL, ");
            break;
        case __CFBundleUnreadableBinary:
            binaryType = CFSTR("");
            break;
        default:
            binaryType = CFSTR("");
            break;
    }
    if (((CFBundleRef)cf)->_plugInData._isPlugIn) {
        retval = CFStringCreateWithFormat(NULL, NULL, CFSTR("CFBundle/CFPlugIn 0x%x <%@> (%@%sloaded)"), cf, path, binaryType, ((CFBundleRef)cf)->_isLoaded ? "" : "not ");
    } else {
        retval = CFStringCreateWithFormat(NULL, NULL, CFSTR("CFBundle 0x%x <%@> (%@%sloaded)"), cf, path, binaryType, ((CFBundleRef)cf)->_isLoaded ? "" : "not ");
    }
    if (path) CFRelease(path);
    return retval;
}

static void _CFBundleDeallocateGlue(const void *key, const void *value, void *context) {
    CFAllocatorRef allocator = (CFAllocatorRef)context;
    if (value != NULL) {
        CFAllocatorDeallocate(allocator, (void *)value);
    }
}

static void __CFBundleDeallocate(CFTypeRef cf) {
    CFBundleRef bundle = (CFBundleRef)cf;
    CFAllocatorRef allocator;
    
    __CFGenericValidateType(cf, __kCFBundleTypeID);

    allocator = CFGetAllocator(bundle);

    /* Unload it */
    CFBundleUnloadExecutable(bundle);

    // Clean up plugIn stuff
    _CFBundleDeallocatePlugIn(bundle);
    
    _CFBundleRemoveFromTables(bundle);

    if (bundle->_url != NULL) {
        CFRelease(bundle->_url);
    }
    if (bundle->_infoDict != NULL) {
        CFRelease(bundle->_infoDict);
    }
    if (bundle->_modDate != NULL) {
        CFRelease(bundle->_modDate);
    }
    if (bundle->_localInfoDict != NULL) {
        CFRelease(bundle->_localInfoDict);
    }
    if (bundle->_searchLanguages != NULL) {
        CFRelease(bundle->_searchLanguages);
    }
    if (bundle->_glueDict != NULL) {
        CFDictionaryApplyFunction(bundle->_glueDict, _CFBundleDeallocateGlue, (void *)allocator);
        CFRelease(bundle->_glueDict);
    }
    if (bundle->_resourceData._stringTableCache != NULL) {
        CFRelease(bundle->_resourceData._stringTableCache);
    }
}

static const CFRuntimeClass __CFBundleClass = {
    0,
    "CFBundle",
    NULL,      // init
    NULL,      // copy
    __CFBundleDeallocate,
    NULL,      // equal
    NULL,      // hash
    NULL,      // 
    __CFBundleCopyDescription
};

__private_extern__ void __CFBundleInitialize(void) {
    __kCFBundleTypeID = _CFRuntimeRegisterClass(&__CFBundleClass);
}

CFTypeID CFBundleGetTypeID(void) {
    return __kCFBundleTypeID;
}

static CFBundleRef _CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL, Boolean alreadyLocked) {
    CFBundleRef bundle = NULL;
    char buff[CFMaxPathSize];
    CFDateRef modDate = NULL;
    Boolean exists = false;
    SInt32 mode = 0;
    CFURLRef newURL = NULL;
    uint8_t localVersion = 0;
    
    if (!CFURLGetFileSystemRepresentation(bundleURL, true, buff, CFMaxPathSize)) return NULL;

    newURL = CFURLCreateFromFileSystemRepresentation(allocator, buff, strlen(buff), true);
    if (NULL == newURL) {
        newURL = CFRetain(bundleURL);
    }
    bundle = _CFBundleFindByURL(newURL, alreadyLocked);
    if (bundle) {
        CFRetain(bundle);
        CFRelease(newURL);
        return bundle;
    }
    
    if (!_CFBundleURLLooksLikeBundleVersion(newURL, &localVersion)) {
        localVersion = 3;
        if (_CFGetFileProperties(allocator, newURL, &exists, &mode, NULL, &modDate, NULL, NULL) == 0) {
            if (!exists || ((mode & S_IFMT) != S_IFDIR)) {
                if (NULL != modDate) CFRelease(modDate);
                CFRelease(newURL);
                return NULL;
            }
        } else {
            CFRelease(newURL);
            return NULL;
        }
    }

    bundle = (CFBundleRef)_CFRuntimeCreateInstance(allocator, __kCFBundleTypeID, sizeof(struct __CFBundle) - sizeof(CFRuntimeBase), NULL);
    if (NULL == bundle) {
        CFRelease(newURL);
        return NULL;
    }

    bundle->_url = newURL;

    bundle->_modDate = modDate;
    bundle->_version = localVersion;
    bundle->_infoDict = NULL;
    bundle->_localInfoDict = NULL;
    bundle->_searchLanguages = NULL;
    
#if defined(BINARY_SUPPORT_DYLD)
    /* We'll have to figure it out later */
    bundle->_binaryType = __CFBundleUnknownBinary;
#elif defined(BINARY_SUPPORT_CFM)
    /* We support CFM only */
    bundle->_binaryType = __CFBundleCFMBinary;
#elif defined(BINARY_SUPPORT_DLL)
    /* We support DLL only */
    bundle->_binaryType = __CFBundleDLLBinary;
    bundle->_hModule = NULL;
#else
    /* We'll have to figure it out later */
    bundle->_binaryType = __CFBundleUnknownBinary;
#endif

    bundle->_isLoaded = false;
    bundle->_sharesStringsFiles = false;
    
    /* ??? For testing purposes? Or for good? */
    if (!getenv("CFBundleDisableStringsSharing") && 
        (strncmp(buff, "/System/Library/Frameworks", 26) == 0) && 
        (strncmp(buff + strlen(buff) - 10, ".framework", 10) == 0)) bundle->_sharesStringsFiles = true;

    bundle->_connectionCookie = NULL;
    bundle->_imageCookie = NULL;
    bundle->_moduleCookie = NULL;

    bundle->_glueDict = NULL;
    
#if defined(BINARY_SUPPORT_CFM)
    bundle->_resourceData._executableLacksResourceFork = false;
#else
    bundle->_resourceData._executableLacksResourceFork = true;
#endif
  
    bundle->_resourceData._stringTableCache = NULL;

    bundle->_plugInData._isPlugIn = false;
    bundle->_plugInData._loadOnDemand = false;
    bundle->_plugInData._isDoingDynamicRegistration = false;
    bundle->_plugInData._instanceCount = 0;
    bundle->_plugInData._factories = NULL;

    CFBundleGetInfoDictionary(bundle);
    
    _CFBundleAddToTables(bundle, alreadyLocked);

    _CFBundleInitPlugIn(bundle);

    _CFBundleCheckWorkarounds(bundle);
    
    return bundle;
}

CFBundleRef CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL) {return _CFBundleCreate(allocator, bundleURL, false);}

CFArrayRef CFBundleCreateBundlesFromDirectory(CFAllocatorRef alloc, CFURLRef directoryURL, CFStringRef bundleType) {
    CFMutableArrayRef bundles = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    CFArrayRef URLs = _CFContentsOfDirectory(alloc, NULL, NULL, directoryURL, bundleType);
    if (URLs != NULL) {
        CFIndex i, c = CFArrayGetCount(URLs);
        CFURLRef curURL;
        CFBundleRef curBundle;

        for (i=0; i<c; i++) {
            curURL = CFArrayGetValueAtIndex(URLs, i);
            curBundle = CFBundleCreate(alloc, curURL);
            if (curBundle != NULL) {
                CFArrayAppendValue(bundles, curBundle);
            }
        }
        CFRelease(URLs);
    }

    return bundles;
}

CFURLRef CFBundleCopyBundleURL(CFBundleRef bundle) {
    if (bundle->_url) {
        CFRetain(bundle->_url);
    }
    return bundle->_url;
}

void _CFBundleSetDefaultLocalization(CFStringRef localizationName) {
    CFStringRef newLocalization = localizationName ? CFStringCreateCopy(NULL, localizationName) : NULL;
    if (_defaultLocalization) CFRelease(_defaultLocalization);
    _defaultLocalization = newLocalization;
}

__private_extern__ CFArrayRef _CFBundleGetLanguageSearchList(CFBundleRef bundle) {
    if (bundle->_searchLanguages == NULL) {
        CFMutableArrayRef langs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        CFStringRef devLang = CFBundleGetDevelopmentRegion(bundle);
        
        _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version, bundle->_infoDict, langs, devLang);

        if (CFArrayGetCount(langs) == 0) {
            // If the user does not prefer any of our languages, and devLang is not present, try English
            _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version, bundle->_infoDict, langs, CFSTR("en_US"));
        }
        if (CFArrayGetCount(langs) == 0) {
            // if none of the preferred localizations are present, fall back on a random localization that is present
            CFArrayRef localizations = CFBundleCopyBundleLocalizations(bundle);
            if (localizations) {
                if (CFArrayGetCount(localizations) > 0) {
                    _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version, bundle->_infoDict, langs, CFArrayGetValueAtIndex(localizations, 0));
                }
                CFRelease(localizations);
            }
        }
        
        if (devLang != NULL && !CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), devLang)) {
            // Make sure that devLang is on the list as a fallback for individual resources that are not present
            CFArrayAppendValue(langs, devLang);
        } else if (devLang == NULL) {
            // Or if there is no devLang, try some variation of English that is present
            CFArrayRef localizations = CFBundleCopyBundleLocalizations(bundle);
            if (localizations) {
                CFStringRef en_US = CFSTR("en_US"), en = CFSTR("en"), English = CFSTR("English");
                CFRange range = CFRangeMake(0, CFArrayGetCount(localizations));
                if (CFArrayContainsValue(localizations, range, en)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), en)) CFArrayAppendValue(langs, en);
                } else if (CFArrayContainsValue(localizations, range, English)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), English)) CFArrayAppendValue(langs, English);
                } else if (CFArrayContainsValue(localizations, range, en_US)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), en_US)) CFArrayAppendValue(langs, en_US);
                }
                CFRelease(localizations);
            }
        }
        if (CFArrayGetCount(langs) == 0) {
            // Total backstop behavior to avoid having an empty array.
            if (_defaultLocalization != NULL) {
                CFArrayAppendValue(langs, _defaultLocalization);
            } else {
                CFArrayAppendValue(langs, CFSTR("en"));
            }
        }
        bundle->_searchLanguages = langs;
    }
    return bundle->_searchLanguages;
}

CFDictionaryRef CFBundleCopyInfoDictionaryInDirectory(CFURLRef url) {return _CFBundleCopyInfoDictionaryInDirectory(NULL, url, NULL);}

CFDictionaryRef CFBundleGetInfoDictionary(CFBundleRef bundle) {
    if (bundle->_infoDict == NULL) {
        bundle->_infoDict = _CFBundleCopyInfoDictionaryInDirectoryWithVersion(CFGetAllocator(bundle), bundle->_url, bundle->_version);
    }
    return bundle->_infoDict;
}

CFDictionaryRef _CFBundleGetLocalInfoDictionary(CFBundleRef bundle) {return CFBundleGetLocalInfoDictionary(bundle);}

CFDictionaryRef CFBundleGetLocalInfoDictionary(CFBundleRef bundle) {
    if (bundle->_localInfoDict == NULL) {
        CFURLRef url = CFBundleCopyResourceURL(bundle, _CFBundleLocalInfoName, _CFBundleStringTableType, NULL);
        if (url) {
            CFDataRef data;
            SInt32 errCode;
            CFStringRef errStr = NULL;
            
            if (CFURLCreateDataAndPropertiesFromResource(CFGetAllocator(bundle), url, &data, NULL, NULL, &errCode)) {
                bundle->_localInfoDict = CFPropertyListCreateFromXMLData(CFGetAllocator(bundle), data, kCFPropertyListImmutable, &errStr);
                if (errStr) {
                    CFRelease(errStr);
                }
                CFRelease(data);
            }
            CFRelease(url);
        }
    }
    return bundle->_localInfoDict;
}

CFPropertyListRef _CFBundleGetValueForInfoKey(CFBundleRef bundle, CFStringRef key) {return (CFPropertyListRef)CFBundleGetValueForInfoDictionaryKey(bundle, key);}

CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFBundleRef bundle, CFStringRef key) {
    // Look in InfoPlist.strings first.  Then look in Info.plist
    CFTypeRef result = NULL;
    if ((bundle!= NULL) && (key != NULL)) {
        CFDictionaryRef dict = CFBundleGetLocalInfoDictionary(bundle);
        if (dict != NULL) {
            result = CFDictionaryGetValue(dict, key);
        }
        if (result == NULL) {
            dict = CFBundleGetInfoDictionary(bundle);
            if (dict != NULL) {
                result = CFDictionaryGetValue(dict, key);
            }
        }
    }
    return result;
}

CFStringRef CFBundleGetIdentifier(CFBundleRef bundle) {
    CFStringRef bundleID = NULL;
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    if (infoDict) {
        bundleID = CFDictionaryGetValue(infoDict, kCFBundleIdentifierKey);
    }
    return bundleID;
}

#define DEVELOPMENT_STAGE 0x20
#define ALPHA_STAGE 0x40
#define BETA_STAGE 0x60
#define RELEASE_STAGE 0x80

#define MAX_VERS_LEN 10

CF_INLINE Boolean _isDigit(UniChar aChar) {return (((aChar >= (UniChar)'0') && (aChar <= (UniChar)'9')) ? true : false);}

__private_extern__ CFStringRef _CFCreateStringFromVersionNumber(CFAllocatorRef alloc, UInt32 vers) {
    CFStringRef result = NULL;
    uint8_t major1, major2, minor1, minor2, stage, build;

    major1 = (vers & 0xF0000000) >> 28;
    major2 = (vers & 0x0F000000) >> 24;
    minor1 = (vers & 0x00F00000) >> 20;
    minor2 = (vers & 0x000F0000) >> 16;
    stage = (vers & 0x0000FF00) >> 8;
    build = (vers & 0x000000FF);

    if (stage == RELEASE_STAGE) {
        if (major1 > 0) {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d%d.%d.%d"), major1, major2, minor1, minor2);
        } else {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d.%d.%d"), major2, minor1, minor2);
        }
    } else {
        if (major1 > 0) {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d%d.%d.%d%s%d"), major1, major2, minor1, minor2, ((stage == DEVELOPMENT_STAGE) ? "d" : ((stage == ALPHA_STAGE) ? "a" : "b")), build);
        } else {
            result = CFStringCreateWithFormat(alloc, NULL, CFSTR("%d.%d.%d%s%d"), major2, minor1, minor2, ((stage == DEVELOPMENT_STAGE) ? "d" : ((stage == ALPHA_STAGE) ? "a" : "b")), build);
        }
    }
    return result;
}

__private_extern__ UInt32 _CFVersionNumberFromString(CFStringRef versStr) {
    // Parse version number from string.
    // String can begin with "." for major version number 0.  String can end at any point, but elements within the string cannot be skipped.
    UInt32 major1 = 0, major2 = 0, minor1 = 0, minor2 = 0, stage = RELEASE_STAGE, build = 0;
    UniChar versChars[MAX_VERS_LEN];
    UniChar *chars = NULL;
    CFIndex len;
    UInt32 theVers;
    Boolean digitsDone = false;

    if (!versStr) return 0;

    len = CFStringGetLength(versStr);

    if ((len == 0) || (len > MAX_VERS_LEN)) return 0;

    CFStringGetCharacters(versStr, CFRangeMake(0, len), versChars);
    chars = versChars;
    
    // Get major version number.
    major1 = major2 = 0;
    if (_isDigit(*chars)) {
        major2 = *chars - (UniChar)'0';
        chars++;
        len--;
        if (len > 0) {
            if (_isDigit(*chars)) {
                major1 = major2;
                major2 = *chars - (UniChar)'0';
                chars++;
                len--;
                if (len > 0) {
                    if (*chars == (UniChar)'.') {
                        chars++;
                        len--;
                    } else {
                        digitsDone = true;
                    }
                }
            } else if (*chars == (UniChar)'.') {
                chars++;
                len--;
            } else {
                digitsDone = true;
            }
        }
    } else if (*chars == (UniChar)'.') {
        chars++;
        len--;
    } else {
        digitsDone = true;
    }

    // Now major1 and major2 contain first and second digit of the major version number as ints.
    // Now either len is 0 or chars points at the first char beyond the first decimal point.

    // Get the first minor version number.  
    if (len > 0 && !digitsDone) {
        if (_isDigit(*chars)) {
            minor1 = *chars - (UniChar)'0';
            chars++;
            len--;
            if (len > 0) {
                if (*chars == (UniChar)'.') {
                    chars++;
                    len--;
                } else {
                    digitsDone = true;
                }
            }
        } else {
            digitsDone = true;
        }
    }

    // Now minor1 contains the first minor version number as an int.
    // Now either len is 0 or chars points at the first char beyond the second decimal point.

    // Get the second minor version number. 
    if (len > 0 && !digitsDone) {
        if (_isDigit(*chars)) {
            minor2 = *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            digitsDone = true;
        }
    }

    // Now minor2 contains the second minor version number as an int.
    // Now either len is 0 or chars points at the build stage letter.

    // Get the build stage letter.  We must find 'd', 'a', 'b', or 'f' next, if there is anything next.
    if (len > 0) {
        if (*chars == (UniChar)'d') {
            stage = DEVELOPMENT_STAGE;
        } else if (*chars == (UniChar)'a') {
            stage = ALPHA_STAGE;
        } else if (*chars == (UniChar)'b') {
            stage = BETA_STAGE;
        } else if (*chars == (UniChar)'f') {
            stage = RELEASE_STAGE;
        } else {
            return 0;
        }
        chars++;
        len--;
    }

    // Now stage contains the release stage.
    // Now either len is 0 or chars points at the build number.

    // Get the first digit of the build number.
    if (len > 0) {
        if (_isDigit(*chars)) {
            build = *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            return 0;
        }
    }
    // Get the second digit of the build number.
    if (len > 0) {
        if (_isDigit(*chars)) {
            build *= 10;
            build += *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            return 0;
        }
    }
    // Get the third digit of the build number.
    if (len > 0) {
        if (_isDigit(*chars)) {
            build *= 10;
            build += *chars - (UniChar)'0';
            chars++;
            len--;
        } else {
            return 0;
        }
    }

    // Range check the build number and make sure we exhausted the string.
    if ((build > 0xFF) || (len > 0)) return 0;

    // Build the number
    theVers = major1 << 28;
    theVers += major2 << 24;
    theVers += minor1 << 20;
    theVers += minor2 << 16;
    theVers += stage << 8;
    theVers += build;

    return theVers;
}

UInt32 CFBundleGetVersionNumber(CFBundleRef bundle) {
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFTypeRef unknownVersionValue = CFDictionaryGetValue(infoDict, _kCFBundleNumericVersionKey);
    CFNumberRef versNum;
    UInt32 vers = 0;

    if (unknownVersionValue == NULL) {
        unknownVersionValue = CFDictionaryGetValue(infoDict, kCFBundleVersionKey);
    }
    if (unknownVersionValue != NULL) {
        if (CFGetTypeID(unknownVersionValue) == CFStringGetTypeID()) {
            // Convert a string version number into a numeric one.
            vers = _CFVersionNumberFromString((CFStringRef)unknownVersionValue);

            versNum = CFNumberCreate(CFGetAllocator(bundle), kCFNumberSInt32Type, &vers);
            CFDictionarySetValue((CFMutableDictionaryRef)infoDict, _kCFBundleNumericVersionKey, versNum);
            CFRelease(versNum);
        } else if (CFGetTypeID(unknownVersionValue) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)unknownVersionValue, kCFNumberSInt32Type, &vers);
        } else {
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, _kCFBundleNumericVersionKey);
        }
    }
    return vers;
}

CFStringRef CFBundleGetDevelopmentRegion(CFBundleRef bundle) {
    CFStringRef devLang = NULL;
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    if (infoDict) {
        devLang = CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
        if (devLang != NULL && (CFGetTypeID(devLang) != CFStringGetTypeID() || CFStringGetLength(devLang) == 0)) {
            devLang = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleDevelopmentRegionKey);
        }
    }

    return devLang;
}

Boolean _CFBundleGetHasChanged(CFBundleRef bundle) {
    CFDateRef modDate;
    Boolean result = false;
    Boolean exists = false;
    SInt32 mode = 0;

    if (_CFGetFileProperties(CFGetAllocator(bundle), bundle->_url, &exists, &mode, NULL, &modDate, NULL, NULL) == 0) {
        // If the bundle no longer exists or is not a folder, it must have "changed"
        if (!exists || ((mode & S_IFMT) != S_IFDIR)) {
            result = true;
        }
    } else {
        // Something is wrong.  The stat failed.
        result = true;
    }
    if (bundle->_modDate && !CFEqual(bundle->_modDate, modDate)) {
        // mod date is different from when we created.
        result = true;
    }
    CFRelease(modDate);
    return result;
}

void _CFBundleSetStringsFilesShared(CFBundleRef bundle, Boolean flag) {
    bundle->_sharesStringsFiles = flag;
}

Boolean _CFBundleGetStringsFilesShared(CFBundleRef bundle) {
    return bundle->_sharesStringsFiles;
}

static Boolean _urlExists(CFAllocatorRef alloc, CFURLRef url) {
    Boolean exists;
    return url && (0 == _CFGetFileProperties(alloc, url, &exists, NULL, NULL, NULL, NULL, NULL)) && exists;
}

__private_extern__ CFURLRef _CFBundleCopySupportFilesDirectoryURLInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, uint8_t version) {
    CFURLRef result = NULL;
    if (bundleURL) {
        if (1 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleSupportFilesURLFromBase1, bundleURL);
        } else if (2 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleSupportFilesURLFromBase2, bundleURL);
        } else {
            result = CFRetain(bundleURL);
        }
    }
    return result;
}

CF_EXPORT CFURLRef CFBundleCopySupportFilesDirectoryURL(CFBundleRef bundle) {return _CFBundleCopySupportFilesDirectoryURLInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version);}

__private_extern__ CFURLRef _CFBundleCopyResourcesDirectoryURLInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, uint8_t version) {
    CFURLRef result = NULL;
    if (bundleURL) {
        if (0 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleResourcesURLFromBase0, bundleURL);
        } else if (1 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleResourcesURLFromBase1, bundleURL);
        } else if (2 == version) {
            result = CFURLCreateWithString(alloc, _CFBundleResourcesURLFromBase2, bundleURL);
        } else {
            result = CFRetain(bundleURL);
        }
    }
    return result;
}

CFURLRef CFBundleCopyResourcesDirectoryURL(CFBundleRef bundle) {return _CFBundleCopyResourcesDirectoryURLInDirectory(CFGetAllocator(bundle), bundle->_url, bundle->_version);}

static CFURLRef _CFBundleCopyExecutableURLRaw(CFAllocatorRef alloc, CFURLRef urlPath, CFStringRef exeName) {
    // Given an url to a folder and a name, this returns the url to the executable in that folder with that name, if it exists, and NULL otherwise.  This function deals with appending the ".exe" or ".dll" on Windows.
    CFURLRef executableURL = NULL;
#if defined(__MACH__)
    const uint8_t *image_suffix = getenv("DYLD_IMAGE_SUFFIX");
#endif /* __MACH__ */
    
    if (urlPath == NULL || exeName == NULL) return NULL;
    
#if defined(__MACH__)
    if (image_suffix != NULL) {
        CFStringRef newExeName, imageSuffix;
        imageSuffix = CFStringCreateWithCString(NULL, image_suffix, kCFStringEncodingUTF8);
        if (CFStringHasSuffix(exeName, CFSTR(".dylib"))) {
            CFStringRef bareExeName = CFStringCreateWithSubstring(alloc, exeName, CFRangeMake(0, CFStringGetLength(exeName)-6));
            newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@.dylib"), exeName, imageSuffix);
            CFRelease(bareExeName);
        } else {
            newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@"), exeName, imageSuffix);
        }
        executableURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, newExeName, kCFURLPOSIXPathStyle, false, urlPath);
        if (executableURL != NULL && !_urlExists(alloc, executableURL)) {
            CFRelease(executableURL);
            executableURL = NULL;
        }
        CFRelease(newExeName);
        CFRelease(imageSuffix);
    }
#endif /* __MACH__ */
    if (executableURL == NULL) {
        executableURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, exeName, kCFURLPOSIXPathStyle, false, urlPath);
        if (executableURL != NULL && !_urlExists(alloc, executableURL)) {
            CFRelease(executableURL);
            executableURL = NULL;
        }
    }
#if defined(__WIN32__)
    if (executableURL == NULL) {
        if (!CFStringHasSuffix(exeName, CFSTR(".dll"))) {
            CFStringRef newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@"), exeName, CFSTR(".dll"));
            executableURL = CFURLCreateWithString(alloc, newExeName, urlPath);
            if (executableURL != NULL && !_urlExists(alloc, executableURL)) {
                CFRelease(executableURL);
                executableURL = NULL;
            }
            CFRelease(newExeName);
        }
    }
    if (executableURL == NULL) {
        if (!CFStringHasSuffix(exeName, CFSTR(".exe"))) {
            CFStringRef newExeName = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%@"), exeName, CFSTR(".exe"));
            executableURL = CFURLCreateWithString(alloc, newExeName, urlPath);
            if (executableURL != NULL && !_urlExists(alloc, executableURL)) {
                CFRelease(executableURL);
                executableURL = NULL;
            }
            CFRelease(newExeName);
        }
    }
#endif
    return executableURL;
}

static CFStringRef _CFBundleCopyExecutableName(CFAllocatorRef alloc, CFBundleRef bundle, CFURLRef url, CFDictionaryRef infoDict) {
    CFStringRef executableName = NULL;
    
    if (alloc == NULL && bundle != NULL) {
        alloc = CFGetAllocator(bundle);
    }
    if (infoDict == NULL && bundle != NULL) {
        infoDict = CFBundleGetInfoDictionary(bundle);
    }
    if (url == NULL && bundle != NULL) {
        url = bundle->_url;
    }
    
    if (infoDict != NULL) {
        // Figure out the name of the executable.
        // First try for the new key in the plist.
        executableName = CFDictionaryGetValue(infoDict, kCFBundleExecutableKey);
        if (executableName == NULL) {
            // Second try for the old key in the plist.
            executableName = CFDictionaryGetValue(infoDict, _kCFBundleOldExecutableKey);
        }
        if (executableName != NULL && CFGetTypeID(executableName) == CFStringGetTypeID() && CFStringGetLength(executableName) > 0) {
            CFRetain(executableName);
        } else {
            executableName = NULL;
        }
    }
    if (executableName == NULL && url != NULL) {
        // Third, take the name of the bundle itself (with path extension stripped)
        CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
        CFStringRef bundlePath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
        UniChar buff[CFMaxPathSize];
        CFIndex len = CFStringGetLength(bundlePath);
        CFIndex startOfBundleName, endOfBundleName;

        CFRelease(absoluteURL);
        CFStringGetCharacters(bundlePath, CFRangeMake(0, len), buff);
        startOfBundleName = _CFStartOfLastPathComponent(buff, len);
        endOfBundleName = _CFLengthAfterDeletingPathExtension(buff, len);

        if ((startOfBundleName <= len) && (endOfBundleName <= len) && (startOfBundleName < endOfBundleName)) {
            executableName = CFStringCreateWithCharacters(alloc, &(buff[startOfBundleName]), (endOfBundleName - startOfBundleName));
        }
        CFRelease(bundlePath);
    }
    
    return executableName;
}

__private_extern__ CFURLRef _CFBundleCopyResourceForkURLMayBeLocal(CFBundleRef bundle, Boolean mayBeLocal) {
    CFStringRef executableName = _CFBundleCopyExecutableName(NULL, bundle, NULL, NULL);
    CFURLRef resourceForkURL = NULL;
    if (executableName != NULL) {
        if (mayBeLocal) {
            resourceForkURL = CFBundleCopyResourceURL(bundle, executableName, CFSTR("rsrc"), NULL);
        } else {
            resourceForkURL = CFBundleCopyResourceURLForLocalization(bundle, executableName, CFSTR("rsrc"), NULL, NULL);
        }
        CFRelease(executableName);
    }
    
    return resourceForkURL;
}

CFURLRef _CFBundleCopyResourceForkURL(CFBundleRef bundle) {return _CFBundleCopyResourceForkURLMayBeLocal(bundle, true);}

static CFURLRef _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFAllocatorRef alloc, CFBundleRef bundle, CFURLRef url, CFStringRef executableName, Boolean ignoreCache, Boolean useOtherPlatform) {
    uint8_t version = 0;
    CFDictionaryRef infoDict = NULL;
    CFStringRef executablePath = NULL;
    CFURLRef executableURL = NULL;
    Boolean isDir = false;
    Boolean foundIt = false;
    Boolean lookupMainExe = ((executableName == NULL) ? true : false);
    
    if (bundle != NULL) {
        infoDict = CFBundleGetInfoDictionary(bundle);
        version = bundle->_version;
    } else {
        infoDict = _CFBundleCopyInfoDictionaryInDirectory(alloc, url, &version);
    }

    // If we have a bundle instance and an info dict, see if we have already cached the path
    if (lookupMainExe && !ignoreCache && !useOtherPlatform && (bundle != NULL) && (infoDict != NULL)) {
        executablePath = CFDictionaryGetValue(infoDict, _kCFBundleExecutablePathKey);
        if (executablePath != NULL) {
            executableURL = CFURLCreateWithFileSystemPath(alloc, executablePath, kCFURLPOSIXPathStyle, false);
            if (executableURL != NULL) foundIt = true;
            if (!foundIt) {
                executablePath = NULL;
                CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, _kCFBundleExecutablePathKey);
            }
        }
    }

    if (!foundIt) {
        if (lookupMainExe) {
            executableName = _CFBundleCopyExecutableName(alloc, bundle, url, infoDict);
        }
        if (executableName != NULL) {
            // Now, look for the executable inside the bundle.
            if (0 != version) {
                CFURLRef exeDirURL;
                CFURLRef exeSubdirURL;

                if (1 == version) {
                    exeDirURL = CFURLCreateWithString(alloc, _CFBundleExecutablesURLFromBase1, url);
                } else if (2 == version) {
                    exeDirURL = CFURLCreateWithString(alloc, _CFBundleExecutablesURLFromBase2, url);
                } else {
                    exeDirURL = CFRetain(url);
                }
                exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, useOtherPlatform ? _CFBundleGetOtherPlatformExecutablesSubdirectoryName() : _CFBundleGetPlatformExecutablesSubdirectoryName(), kCFURLPOSIXPathStyle, true, exeDirURL);
                executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                if (executableURL == NULL) {
                    CFRelease(exeSubdirURL);
                    exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, useOtherPlatform ? _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName() : _CFBundleGetAlternatePlatformExecutablesSubdirectoryName(), kCFURLPOSIXPathStyle, true, exeDirURL);
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                }
                if (executableURL == NULL) {
                    CFRelease(exeSubdirURL);
                    exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, useOtherPlatform ? _CFBundleGetPlatformExecutablesSubdirectoryName() : _CFBundleGetOtherPlatformExecutablesSubdirectoryName(), kCFURLPOSIXPathStyle, true, exeDirURL);
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                }
                if (executableURL == NULL) {
                    CFRelease(exeSubdirURL);
                    exeSubdirURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, useOtherPlatform ? _CFBundleGetAlternatePlatformExecutablesSubdirectoryName() : _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName(), kCFURLPOSIXPathStyle, true, exeDirURL);
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeSubdirURL, executableName);
                }
                if (executableURL == NULL) {
                    executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeDirURL, executableName);
                }

                CFRelease(exeDirURL);
                CFRelease(exeSubdirURL);
            }

            // If this was an old bundle, or we did not find the executable in the Excutables subdirectory, look directly in the bundle wrapper.
            if (executableURL == NULL) {
                executableURL = _CFBundleCopyExecutableURLRaw(alloc, url, executableName);
            }

#if defined(__WIN32__)
            // Windows only: If we still haven't found the exe, look in the Executables folder.
            // But only for the main bundle exe
            if (lookupMainExe && (executableURL == NULL)) {
                CFURLRef exeDirURL;

                exeDirURL = CFURLCreateWithString(alloc, CFSTR("../../Executables"), url);

                executableURL = _CFBundleCopyExecutableURLRaw(alloc, exeDirURL, executableName);

                CFRelease(exeDirURL);
            }
#endif

            if (lookupMainExe && !ignoreCache && !useOtherPlatform && (bundle != NULL) && (infoDict != NULL) && (executableURL != NULL)) {
                // We found it.  Cache the path.
                CFURLRef absURL = CFURLCopyAbsoluteURL(executableURL);
                executablePath = CFURLCopyFileSystemPath(absURL, kCFURLPOSIXPathStyle);
                CFRelease(absURL);
                CFDictionarySetValue((CFMutableDictionaryRef)infoDict, _kCFBundleExecutablePathKey, executablePath);
                CFRelease(executablePath);
            }
            if (lookupMainExe && !useOtherPlatform && (bundle != NULL) && (executableURL == NULL)) {
                bundle->_binaryType = __CFBundleNoBinary;
            }
            if (lookupMainExe) {
                CFRelease(executableName);
            }
        }
    }

    if ((bundle == NULL) && (infoDict != NULL)) {
        CFRelease(infoDict);
    }

    return executableURL;
}

CFURLRef _CFBundleCopyExecutableURLInDirectory(CFURLRef url) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(NULL, NULL, url, NULL, true, false);}

CFURLRef _CFBundleCopyOtherExecutableURLInDirectory(CFURLRef url) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(NULL, NULL, url, NULL, true, true);}

CFURLRef CFBundleCopyExecutableURL(CFBundleRef bundle) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFGetAllocator(bundle), bundle, bundle->_url, NULL, false, false);}

static CFURLRef _CFBundleCopyExecutableURLIgnoringCache(CFBundleRef bundle) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFGetAllocator(bundle), bundle, bundle->_url, NULL, true, false);}

CFURLRef CFBundleCopyAuxiliaryExecutableURL(CFBundleRef bundle, CFStringRef executableName) {return _CFBundleCopyExecutableURLInDirectoryWithAllocator(CFGetAllocator(bundle), bundle, bundle->_url, executableName, true, false);}

Boolean CFBundleIsExecutableLoaded(CFBundleRef bundle) {return bundle->_isLoaded;}

#define UNKNOWN_FILETYPE 0x0
#define PEF_FILETYPE 0x1000
#define XLS_FILETYPE 0x10001
#define DOC_FILETYPE 0x10002
#define PPT_FILETYPE 0x10003
#define XLS_NAME "Workbook"
#define DOC_NAME "WordDocument"
#define PPT_NAME "PowerPoint Document"
#define PEF_MAGIC 0x4a6f7921
#define PEF_CIGAM 0x21796f4a
#define PLIST_SEGMENT "__TEXT"
#define PLIST_SECTION "__info_plist"
#define LIB_X11 "/usr/X11R6/lib/libX"

static const uint32_t __CFBundleMagicNumbersArray[] = {
    0xcafebabe, 0xbebafeca, 0xfeedface, 0xcefaedfe, 0x4a6f7921, 0x21796f4a, 0xffd8ffe0, 0x4d4d002a, 
    0x49492a00, 0x47494638, 0x89504e47, 0x69636e73, 0x00000100, 0x7b5c7274, 0x25504446, 0x2e7261fd, 
    0x2e736e64, 0x2e736400, 0x464f524d, 0x52494646, 0x38425053, 0x000001b3, 0x000001ba, 0x4d546864, 
    0x504b0304, 0x53495421, 0x53495432, 0x53495435, 0x53495444, 0x53747566, 0x3c212d2d, 0x25215053, 
    0xd0cf11e0, 0x62656769, 0x6b6f6c79, 0x3026b275, 0x0000000c
};

// string, with groups of 5 characters being 1 element in the array
static const char * __CFBundleExtensionsArray =
    "mach\0"  "mach\0"  "mach\0"  "mach\0"  "pef\0\0" "pef\0\0" "jpeg\0"  "tiff\0"
    "tiff\0"  "gif\0\0" "png\0\0" "icns\0"  "ico\0\0" "rtf\0\0" "pdf\0\0" "ra\0\0\0"
    "au\0\0\0""au\0\0\0""iff\0\0" "riff\0"  "psd\0\0" "mpeg\0"  "mpeg\0"  "mid\0\0"
    "zip\0\0" "sit\0\0" "sit\0\0" "sit\0\0" "sit\0\0" "sit\0\0" "html\0"  "ps\0\0\0"
    "ole\0\0" "uu\0\0\0""dmg\0\0" "wmv\0\0" "jp2\0\0";

#define NUM_EXTENSIONS		37
#define EXTENSION_LENGTH	5
#define MAGIC_BYTES_TO_READ	512

#if defined(BINARY_SUPPORT_DYLD)

CF_INLINE uint32_t _CFBundleSwapInt32Conditional(uint32_t arg, Boolean swap) {return swap ? CFSwapInt32(arg) : arg;}

static CFDictionaryRef _CFBundleGrokInfoDictFromData(char *bytes, unsigned long length) {
    CFMutableDictionaryRef result = NULL;
    CFDataRef infoData = NULL;
    if (NULL != bytes && 0 < length) {
        infoData = CFDataCreateWithBytesNoCopy(NULL, bytes, length, kCFAllocatorNull);
        if (infoData) {

__CFSetNastyFile(CFSTR("<plist section in main executable>"));

            result = (CFMutableDictionaryRef)CFPropertyListCreateFromXMLData(NULL, infoData, kCFPropertyListMutableContainers, NULL);
            if (result && CFDictionaryGetTypeID() != CFGetTypeID(result)) {
                CFRelease(result);
                result = NULL;
            }
            CFRelease(infoData);
        }
        if (!result) {
            result = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
    }
    return result;
}

static CFDictionaryRef _CFBundleGrokInfoDictFromMainExecutable() {
    unsigned long length = 0;
    char *bytes = getsectdata(PLIST_SEGMENT, PLIST_SECTION, &length);
    return _CFBundleGrokInfoDictFromData(bytes, length);
}

static CFDictionaryRef _CFBundleGrokInfoDictFromFile(int fd, unsigned long offset, Boolean swapped) {
    struct stat statBuf;
    char *maploc;
    unsigned i, j;
    CFDictionaryRef result = NULL;
    Boolean foundit = false;
    if (fstat(fd, &statBuf) == 0 && (maploc = mmap(0, statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) != (void *)-1) {
        unsigned long ncmds = _CFBundleSwapInt32Conditional(((struct mach_header *)(maploc + offset))->ncmds, swapped);
        unsigned long sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header *)(maploc + offset))->sizeofcmds, swapped);
        char *startofcmds = maploc + offset + sizeof(struct mach_header);
        char *endofcmds = startofcmds + sizeofcmds;
        struct segment_command *sgp = (struct segment_command *)startofcmds;
        if (endofcmds > maploc + statBuf.st_size) endofcmds = maploc + statBuf.st_size;
        for (i = 0; !foundit && i < ncmds && startofcmds <= (char *)sgp && (char *)sgp < endofcmds; i++) {
            if (LC_SEGMENT == _CFBundleSwapInt32Conditional(sgp->cmd, swapped)) {
                struct section *sp = (struct section *)((char *)sgp + sizeof(struct segment_command));
                unsigned long nsects = _CFBundleSwapInt32Conditional(sgp->nsects, swapped);
                for (j = 0; !foundit && j < nsects && startofcmds <= (char *)sp && (char *)sp < endofcmds; j++) {
                    if (0 == strncmp(sp->sectname, PLIST_SECTION, sizeof(sp->sectname)) && 0 == strncmp(sp->segname, PLIST_SEGMENT, sizeof(sp->segname))) {
                        unsigned long length = _CFBundleSwapInt32Conditional(sp->size, swapped);
                        unsigned long sectoffset = _CFBundleSwapInt32Conditional(sp->offset, swapped);
                        char *bytes = maploc + offset + sectoffset;
                        if (maploc <= bytes && bytes + length <= maploc + statBuf.st_size) result = _CFBundleGrokInfoDictFromData(bytes, length);
                        foundit = true;
                    }
                    sp = (struct section *)((char *)sp + sizeof(struct section));
                }
            }
            sgp = (struct segment_command *)((char *)sgp + _CFBundleSwapInt32Conditional(sgp->cmdsize, swapped));
        }
        munmap(maploc, statBuf.st_size);
    }
    return result;
}

static Boolean _CFBundleGrokX11(int fd, unsigned long offset, Boolean swapped) {
    static const char libX11name[] = LIB_X11;
    struct stat statBuf;
    char *maploc;
    unsigned i;
    Boolean result = false;
    if (fstat(fd, &statBuf) == 0 && (maploc = mmap(0, statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) != (void *)-1) {
        unsigned long ncmds = _CFBundleSwapInt32Conditional(((struct mach_header *)(maploc + offset))->ncmds, swapped);
        unsigned long sizeofcmds = _CFBundleSwapInt32Conditional(((struct mach_header *)(maploc + offset))->sizeofcmds, swapped);
        char *startofcmds = maploc + offset + sizeof(struct mach_header);
        char *endofcmds = startofcmds + sizeofcmds;
        struct dylib_command *dlp = (struct dylib_command *)startofcmds;
        if (endofcmds > maploc + statBuf.st_size) endofcmds = maploc + statBuf.st_size;
        for (i = 0; !result && i < ncmds && startofcmds <= (char *)dlp && (char *)dlp < endofcmds; i++) {
            if (LC_LOAD_DYLIB == _CFBundleSwapInt32Conditional(dlp->cmd, swapped)) {
                unsigned long nameoffset = _CFBundleSwapInt32Conditional(dlp->dylib.name.offset, swapped);
                if (0 == strncmp((char *)dlp + nameoffset, libX11name, sizeof(libX11name) - 1)) result = true;
            }
            dlp = (struct dylib_command *)((char *)dlp + _CFBundleSwapInt32Conditional(dlp->cmdsize, swapped));
        }
        munmap(maploc, statBuf.st_size);
    }
    return result;
}
    
    
static UInt32 _CFBundleGrokMachTypeForFatFile(int fd, void *bytes, CFIndex length, Boolean *isX11, CFDictionaryRef *infodict) {
    UInt32 machtype = UNKNOWN_FILETYPE, magic, numFatHeaders = ((struct fat_header *)bytes)->nfat_arch, maxFatHeaders = (length - sizeof(struct fat_header)) / sizeof(struct fat_arch);
    unsigned char moreBytes[sizeof(struct mach_header)];
    const NXArchInfo *archInfo = NXGetLocalArchInfo();
    struct fat_arch *fat = NULL;

    if (isX11) *isX11 = false;
    if (infodict) *infodict = NULL;
    if (numFatHeaders > maxFatHeaders) numFatHeaders = maxFatHeaders;
    if (numFatHeaders > 0) {
        fat = NXFindBestFatArch(archInfo->cputype, archInfo->cpusubtype, (struct fat_arch *)(bytes + sizeof(struct fat_header)), numFatHeaders);
        if (!fat) fat = (struct fat_arch *)(bytes + sizeof(struct fat_header));
    } 
    if (fat) {
        if (lseek(fd, fat->offset, SEEK_SET) == (off_t)fat->offset && read(fd, moreBytes, sizeof(struct mach_header)) >= (int)sizeof(struct mach_header)) {
            magic = *((UInt32 *)moreBytes);
            if (MH_MAGIC == magic) {
                machtype = ((struct mach_header *)moreBytes)->filetype;
                if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, fat->offset, false);
                if (isX11) *isX11 = _CFBundleGrokX11(fd, fat->offset, false);
            } else if (MH_CIGAM == magic) {
                machtype = CFSwapInt32(((struct mach_header *)moreBytes)->filetype);
                if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, fat->offset, true);
                if (isX11) *isX11 = _CFBundleGrokX11(fd, fat->offset, true);
            }
        }
    }
    return machtype;
}

static UInt32 _CFBundleGrokMachType(int fd, void *bytes, CFIndex length, Boolean *isX11, CFDictionaryRef *infodict) {
    unsigned int magic = *((UInt32 *)bytes), machtype = UNKNOWN_FILETYPE;
    CFIndex i;

    if (isX11) *isX11 = false;
    if (infodict) *infodict = NULL;
    if (MH_MAGIC == magic) {
        machtype = ((struct mach_header *)bytes)->filetype;
        if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, 0, false);
        if (isX11) *isX11 = _CFBundleGrokX11(fd, 0, false);
    } else if (MH_CIGAM == magic) {
        for (i = 0; i < length; i += 4) *(UInt32 *)(bytes + i) = CFSwapInt32(*(UInt32 *)(bytes + i));
        machtype = ((struct mach_header *)bytes)->filetype;
        if (infodict) *infodict = _CFBundleGrokInfoDictFromFile(fd, 0, true);
        if (isX11) *isX11 = _CFBundleGrokX11(fd, 0, true);
    } else if (FAT_MAGIC == magic) {
        machtype = _CFBundleGrokMachTypeForFatFile(fd, bytes, length, isX11, infodict);
    } else if (FAT_CIGAM == magic) {
        for (i = 0; i < length; i += 4) *(UInt32 *)(bytes + i) = CFSwapInt32(*(UInt32 *)(bytes + i));
        machtype = _CFBundleGrokMachTypeForFatFile(fd, bytes, length, isX11, infodict);
    } else if (PEF_MAGIC == magic || PEF_CIGAM == magic) {
        machtype = PEF_FILETYPE;
    }
    return machtype;
}

#endif /* BINARY_SUPPORT_DYLD */

static UInt32 _CFBundleGrokFileTypeForOLEFile(int fd, unsigned long offset) {
    UInt32 filetype = UNKNOWN_FILETYPE;
    static const unsigned char xlsname[] = XLS_NAME, docname[] = DOC_NAME, pptname[] = PPT_NAME;
    unsigned char moreBytes[512];
    
    if (lseek(fd, offset, SEEK_SET) == (off_t)offset && read(fd, moreBytes, sizeof(moreBytes)) >= (int)sizeof(moreBytes)) {
        CFIndex i, j;
        Boolean foundit = false;
        for (i = 0; !foundit && i < 4; i++) {
            char namelength = moreBytes[128 * i + 64] / 2;
            if (sizeof(xlsname) == namelength) {
                for (j = 0, foundit = true; j + 1 < namelength; j++) if (moreBytes[128 * i + 2 * j] != xlsname[j]) foundit = false;
                if (foundit) filetype = XLS_FILETYPE;
            } else if (sizeof(docname) == namelength) {
                for (j = 0, foundit = true; j + 1 < namelength; j++) if (moreBytes[128 * i + 2 * j] != docname[j]) foundit = false;
                if (foundit) filetype = DOC_FILETYPE;
            } else if (sizeof(pptname) == namelength) {
                for (j = 0, foundit = true; j + 1 < namelength; j++) if (moreBytes[128 * i + 2 * j] != pptname[j]) foundit = false;
                if (foundit) filetype = PPT_FILETYPE;
            }
        }
    }
    return filetype;
}

static Boolean _CFBundleGrokFileType(CFURLRef url, CFStringRef *extension, UInt32 *machtype, CFDictionaryRef *infodict) {
    struct stat statBuf;
    int fd = -1;
    char path[CFMaxPathSize];
    unsigned char bytes[MAGIC_BYTES_TO_READ];
    CFIndex i, length = 0;
    const char *ext = NULL;
    UInt32 mt = UNKNOWN_FILETYPE;
    Boolean isX11 = false, isPlain = true, isZero = true;
    // extensions returned:  o, tool, x11app, pef, core, dylib, bundle, jpeg, jp2, tiff, gif, png, pict, icns, ico, rtf, pdf, ra, au, aiff, aifc, wav, avi, wmv, psd, mpeg, mid, zip, jar, sit, html, ps, mov, qtif, bmp, hqx, bin, class, tar, txt, gz, Z, uu, sh, pl, py, rb, dvi, sgi, mp3, xml, plist, xls, doc, ppt, mp4, m4a, m4b, m4p, dmg
    if (url && CFURLGetFileSystemRepresentation(url, true, path, CFMaxPathSize) && (fd = open(path, O_RDONLY, 0777)) >= 0 && fstat(fd, &statBuf) == 0 && (statBuf.st_mode & S_IFMT) == S_IFREG) {
        if ((length = read(fd, bytes, MAGIC_BYTES_TO_READ)) >= 4) {
            UInt32 magic = CFSwapInt32HostToBig(*((UInt32 *)bytes));
            for (i = 0; !ext && i < NUM_EXTENSIONS; i++) {
                if (__CFBundleMagicNumbersArray[i] == magic) ext = __CFBundleExtensionsArray + i * EXTENSION_LENGTH;
            }
            if (ext) {
                if (0xcafebabe == magic && 8 <= length && 0 != *((UInt16 *)(bytes + 4))) {
                    ext = "class";
#if defined(BINARY_SUPPORT_DYLD)
                } else if ((int)sizeof(struct mach_header) <= length) {
                    mt = _CFBundleGrokMachType(fd, bytes, length, extension ? &isX11 : NULL, infodict);
#endif /* BINARY_SUPPORT_DYLD */
                }
#if defined(BINARY_SUPPORT_DYLD)
                if (MH_OBJECT == mt) {
                    ext = "o";
                } else if (MH_EXECUTE == mt) {
                    ext = isX11 ? "x11app" : "tool";
                } else if (PEF_FILETYPE == mt) {
                    ext = "pef";
                } else if (MH_CORE == mt) {
                    ext = "core";
                } else if (MH_DYLIB == mt) {
                    ext = "dylib";
                } else if (MH_BUNDLE == mt) {
                    ext = "bundle";
                } else 
#endif /* BINARY_SUPPORT_DYLD */
                if (0x7b5c7274 == magic && (6 > length || 'f' != bytes[4])) {
                    ext = NULL;
                } else if (0x47494638 == magic && (6 > length || (0x3761 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))) && 0x3961 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4))))))  {
                    ext = NULL;
                } else if (0x0000000c == magic && (6 > length || 0x6a50 != CFSwapInt16HostToBig(*((UInt16 *)(bytes + 4)))))  {
                    ext = NULL;
                } else if (0x89504e47 == magic && (8 > length || 0x0d0a1a0a != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) {
                    ext = NULL;
                } else if (0x53747566 == magic && (8 > length || 0x66497420 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) {
                    ext = NULL;
                } else if (0x3026b275 == magic && (8 > length || 0x8e66cf11 != CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4))))) {
                    // ??? we do not distinguish between different wm types, returning wmv for any of wmv, wma, or asf
                    ext = NULL;
                } else if (0x504b0304 == magic && 38 <= length && 0x4d455441 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 30))) && 0x2d494e46 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 34)))) {
                    ext = "jar";
                } else if (0x464f524d == magic) {
                    // IFF
                    ext = NULL;
                    if (12 <= length) {
                        UInt32 iffMagic = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)));
                        if (0x41494646 == iffMagic) {
                            ext = "aiff";
                        } else if (0x414946 == iffMagic) {
                            ext = "aifc";
                        }
                    }
                } else if (0x52494646 == magic) {
                    // RIFF
                    ext = NULL;
                    if (12 <= length) {
                        UInt32 riffMagic = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)));
                        if (0x57415645 == riffMagic) {
                            ext = "wav";
                        } else if (0x41564920 == riffMagic) {
                            ext = "avi";
                        }
                    }
                } else if (0xd0cf11e0 == magic) {
                    // OLE
                    ext = NULL;
                    if (52 <= length) {
                        UInt32 ft = _CFBundleGrokFileTypeForOLEFile(fd, 512 * (1 + CFSwapInt32HostToLittle(*((UInt32 *)(bytes + 48)))));
                        if (XLS_FILETYPE == ft) {
                            ext = "xls";
                        } else if (DOC_FILETYPE == ft) {
                            ext = "doc";
                        } else if (PPT_FILETYPE == ft) {
                            ext = "ppt";
                        }
                    }
                } else if (0x62656769 == magic) {
                    // uu
                    ext = NULL;
                    if (76 <= length && 'n' == bytes[4] && ' ' == bytes[5] && isdigit(bytes[6]) && isdigit(bytes[7]) && isdigit(bytes[8]) && ' ' == bytes[9]) {
                        CFIndex endOfLine = 0;
                        for (i = 10; 0 == endOfLine && i < length; i++) if ('\n' == bytes[i]) endOfLine = i;
                        if (10 <= endOfLine && endOfLine + 62 < length && 'M' == bytes[endOfLine + 1] && '\n' == bytes[endOfLine + 62]) {
                            ext = "uu";
                            for (i = endOfLine + 1; ext && i < endOfLine + 62; i++) if (!isprint(bytes[i])) ext = NULL;
                        }
                    }
                }
            }
            if (extension && !ext) {
                UInt16 shortMagic = CFSwapInt16HostToBig(*((UInt16 *)bytes));
                if (8 <= length && 0x6d6f6f76 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4)))) {
                    ext = "mov";
                } else if (8 <= length && 0x69647363 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4)))) {
                    ext = "qtif";
                } else if (12 <= length && 0x66747970 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 4)))) {
                    // ??? list of ftyp values needs to be checked
                    if (0x6d703432 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) {
                        ext = "mp4";
                    } else if (0x4d344120 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) {
                        ext = "m4a";
                    } else if (0x4d344220 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) {
                        ext = "m4b";
                    } else if (0x4d345020 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 8)))) {
                        ext = "m4p";
                    }
                } else if (0x424d == shortMagic && 18 <= length && 40 == CFSwapInt32HostToLittle(*((UInt32 *)(bytes + 14)))) {
                    ext = "bmp";
                } else if (40 <= length && 0x42696e48 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 34))) && 0x6578 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 38)))) {
                    ext = "hqx";
                } else if (128 <= length && 0x6d42494e == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 102)))) {
                    ext = "bin";
                } else if (128 <= length && 0 == bytes[0] && 0 < bytes[1] && bytes[1] < 64 && 0 == bytes[74] && 0 == bytes[82] && 0 == (statBuf.st_size % 128)) {
                    unsigned df = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 83))), rf = CFSwapInt32HostToBig(*((UInt32 *)(bytes + 87))), blocks = 1 + (df + 127) / 128 + (rf + 127) / 128;
                    if (df < 0x00800000 && rf < 0x00800000 && 1 < blocks && (off_t)(128 * blocks) == statBuf.st_size) {
                        ext = "bin";
                    }
                } else if (265 <= length && 0x75737461 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 257))) && (0x72202000 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 261))) || 0x7200 == CFSwapInt16HostToBig(*((UInt16 *)(bytes + 261))))) {
                    ext = "tar";
                } else if (0xfeff == shortMagic || 0xfffe == shortMagic) {
                    ext = "txt";
                } else if (0x1f9d == shortMagic) {
                    ext = "Z";
                } else if (0x1f8b == shortMagic) {
                    ext = "gz";
                } else if (0xf702 == shortMagic) {
                    ext = "dvi";
                } else if (0x01da == shortMagic && (0 == bytes[2] || 1 == bytes[2]) && (0 < bytes[3] && 16 > bytes[3])) {
                    ext = "sgi";
                } else if (0x2321 == shortMagic) {
                    CFIndex endOfLine = 0, lastSlash = 0;
                    for (i = 2; 0 == endOfLine && i < length; i++) if ('\n' == bytes[i]) endOfLine = i;
                    if (endOfLine > 3) {
                        for (i = endOfLine - 1; 0 == lastSlash && i > 1; i--) if ('/' == bytes[i]) lastSlash = i;
                        if (lastSlash > 0) {
                            if (0 == strncmp(bytes + lastSlash + 1, "perl", 4)) {
                                ext = "pl";
                            } else if (0 == strncmp(bytes + lastSlash + 1, "python", 6)) {
                                ext = "py";
                            } else if (0 == strncmp(bytes + lastSlash + 1, "ruby", 4)) {
                                ext = "rb";
                            } else {
                                ext = "sh";
                            }
                        }
                    } 
                } else if (0xffd8 == shortMagic && 0xff == bytes[2]) {
                    ext = "jpeg";
                } else if (0x4944 == shortMagic && '3' == bytes[2] && 0x20 > bytes[3]) {
                    ext = "mp3";
                } else if ('<' == bytes[0] && 14 <= length) {
                    if (0 == strncasecmp(bytes + 1, "!doctype html", 13) || 0 == strncasecmp(bytes + 1, "head", 4) || 0 == strncasecmp(bytes + 1, "title", 5) || 0 == strncasecmp(bytes + 1, "html", 4)) {
                        ext = "html";
                    } else if (0 == strncasecmp(bytes + 1, "?xml", 4)) {
                        if (116 <= length && 0 == strncasecmp(bytes + 100, "PropertyList.dtd", 16)) {
                            ext = "plist";
                        } else {
                            ext = "xml";
                        }
                    }
                }
            }
        }
        if (extension && !ext) {
            //??? what about MacOSRoman?
            for (i = 0; (isPlain || isZero) && i < length && i < 512; i++) {
                char c = bytes[i];
                if (0x7f <= c || (0x20 > c && !isspace(c))) isPlain = false;
                if (0 != c) isZero = false;
            }
            if (isPlain) {
                ext = "txt";
            } else if (isZero && length >= MAGIC_BYTES_TO_READ && statBuf.st_size >= 526) {
                if (lseek(fd, 512, SEEK_SET) == 512 && read(fd, bytes, 512) >= 14) {
                    if (0x001102ff == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 10)))) {
                        ext = "pict";
                    }
                }
            }
        }
        if (extension && !ext && !isZero && length >= MAGIC_BYTES_TO_READ && statBuf.st_size >= 1024) {
            off_t offset = statBuf.st_size - 512;
            if (lseek(fd, offset, SEEK_SET) == offset && read(fd, bytes, 512) >= 512) {
                if (0x6b6f6c79 == CFSwapInt32HostToBig(*((UInt32 *)bytes)) || (0x63647361 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 504))) && 0x656e6372 == CFSwapInt32HostToBig(*((UInt32 *)(bytes + 508))))) {
                    ext = "dmg";
                }
            }
        }
    }
    if (extension) *extension = ext ? CFStringCreateWithCStringNoCopy(NULL, ext, kCFStringEncodingASCII, kCFAllocatorNull) : NULL;
    if (machtype) *machtype = mt;
    close(fd);
    return (ext != NULL);
}

CFStringRef _CFBundleCopyFileTypeForFileURL(CFURLRef url) {
    CFStringRef extension = NULL;
    (void)_CFBundleGrokFileType(url, &extension, NULL, NULL);
    return extension;
}

__private_extern__ CFDictionaryRef _CFBundleCopyInfoDictionaryInExecutable(CFURLRef url) {
    CFDictionaryRef result = NULL;
    (void)_CFBundleGrokFileType(url, NULL, NULL, &result);
    return result;
}

#if defined(BINARY_SUPPORT_DYLD)

__private_extern__ __CFPBinaryType _CFBundleGrokBinaryType(CFURLRef executableURL) {
    // Attempt to grok the type of the binary by looking for DYLD magic numbers.  If one of the DYLD magic numbers is found, find out what type of Mach-o file it is.  Otherwise, look for the PEF magic numbers to see if it is CFM (if we understand CFM).
    __CFPBinaryType result = executableURL ? __CFBundleUnreadableBinary : __CFBundleNoBinary;
    UInt32 machtype = UNKNOWN_FILETYPE;
    if (_CFBundleGrokFileType(executableURL, NULL, &machtype, NULL)) {
        switch (machtype) {
            case MH_EXECUTE:
                result = __CFBundleDYLDExecutableBinary;
                break;
            case MH_BUNDLE:
                result = __CFBundleDYLDBundleBinary;
                break;
            case MH_DYLIB:
                result = __CFBundleDYLDFrameworkBinary;
                break;
#if defined(BINARY_SUPPORT_CFM)
            case PEF_FILETYPE:
                result = __CFBundleCFMBinary;
                break;
#endif /* BINARY_SUPPORT_CFM */
        }
    }
    return result;
}

#endif /* BINARY_SUPPORT_DYLD */

void _CFBundleSetCFMConnectionID(CFBundleRef bundle, void *connectionID) {
#if defined(BINARY_SUPPORT_CFM)
    if (bundle->_binaryType == __CFBundleUnknownBinary || bundle->_binaryType == __CFBundleUnreadableBinary) {
        bundle->_binaryType = __CFBundleCFMBinary;
    }
#endif /* BINARY_SUPPORT_CFM */
    bundle->_connectionCookie = connectionID;
    bundle->_isLoaded = true;
}

Boolean CFBundleLoadExecutable(CFBundleRef bundle) {
    Boolean result = false;
    CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

    if (!executableURL) {
        bundle->_binaryType = __CFBundleNoBinary;
    }
#if defined(BINARY_SUPPORT_DYLD)
    // make sure we know whether bundle is already loaded or not
    if (!bundle->_isLoaded) {
        _CFBundleDYLDCheckLoaded(bundle);
    }
    // We might need to figure out what it is
    if (bundle->_binaryType == __CFBundleUnknownBinary) {
        bundle->_binaryType = _CFBundleGrokBinaryType(executableURL);
#if defined(BINARY_SUPPORT_CFM)
        if (bundle->_binaryType != __CFBundleCFMBinary && bundle->_binaryType != __CFBundleUnreadableBinary) {
            bundle->_resourceData._executableLacksResourceFork = true;
        }
#endif /* BINARY_SUPPORT_CFM */
    }
#endif /* BINARY_SUPPORT_DYLD */
    if (executableURL) CFRelease(executableURL);
    
    if (bundle->_isLoaded) {
        // Remove from the scheduled unload set if we are there.
        __CFSpinLock(&CFBundleGlobalDataLock);
        if (_bundlesToUnload) {
            CFSetRemoveValue(_bundlesToUnload, bundle);
        }
        __CFSpinUnlock(&CFBundleGlobalDataLock);
        return true;
    }

    // Unload bundles scheduled for unloading
    if (!_scheduledBundlesAreUnloading) {
        _CFBundleUnloadScheduledBundles();
    }

    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
        case __CFBundleUnreadableBinary:
            result = _CFBundleCFMLoad(bundle);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
            result = _CFBundleDYLDLoadBundle(bundle);
            break;
        case __CFBundleDYLDFrameworkBinary:
            result = _CFBundleDYLDLoadFramework(bundle);
            break;
        case __CFBundleDYLDExecutableBinary:
            CFLog(__kCFLogBundle, CFSTR("Attempt to load executable of a type that cannot be dynamically loaded for %@"), bundle);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            result = _CFBundleDLLLoad(bundle);
            break;
#endif /* BINARY_SUPPORT_DLL */
        case __CFBundleNoBinary:
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for %@"), bundle);
            break;     
        default:
            CFLog(__kCFLogBundle, CFSTR("Cannot recognize type of executable for %@"), bundle);
            break;
    }
    
    return result;
}

void CFBundleUnloadExecutable(CFBundleRef bundle) {

    if (!_scheduledBundlesAreUnloading) {
        // First unload bundles scheduled for unloading (if that's not what we are already doing.)
        _CFBundleUnloadScheduledBundles();
    }
    
    if (!bundle->_isLoaded) return;
    
    // Remove from the scheduled unload set if we are there.
    if (!_scheduledBundlesAreUnloading) {
        __CFSpinLock(&CFBundleGlobalDataLock);
    }
    if (_bundlesToUnload) {
        CFSetRemoveValue(_bundlesToUnload, bundle);
    }
    if (!_scheduledBundlesAreUnloading) {
        __CFSpinUnlock(&CFBundleGlobalDataLock);
    }
    
    // Give the plugIn code a chance to realize this...
    _CFPlugInWillUnload(bundle);

    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
             _CFBundleCFMUnload(bundle);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
            _CFBundleDYLDUnloadBundle(bundle);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            _CFBundleDLLUnload(bundle);
            break;
#endif /* BINARY_SUPPORT_DLL */
        default:
            break;
    }
    if (!bundle->_isLoaded && bundle->_glueDict != NULL) {
        CFDictionaryApplyFunction(bundle->_glueDict, _CFBundleDeallocateGlue, (void *)CFGetAllocator(bundle));
        CFRelease(bundle->_glueDict);
        bundle->_glueDict = NULL;
    }
}

__private_extern__ void _CFBundleScheduleForUnloading(CFBundleRef bundle) {
    __CFSpinLock(&CFBundleGlobalDataLock);
    if (!_bundlesToUnload) {
        // Create this from the default allocator
        CFSetCallBacks nonRetainingCallbacks = kCFTypeSetCallBacks;
        nonRetainingCallbacks.retain = NULL;
        nonRetainingCallbacks.release = NULL;
        _bundlesToUnload = CFSetCreateMutable(NULL, 0, &nonRetainingCallbacks);
    }
    CFSetAddValue(_bundlesToUnload, bundle);
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

__private_extern__ void _CFBundleUnscheduleForUnloading(CFBundleRef bundle) {
    __CFSpinLock(&CFBundleGlobalDataLock);
    if (_bundlesToUnload) {
        CFSetRemoveValue(_bundlesToUnload, bundle);
    }
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

__private_extern__ void _CFBundleUnloadScheduledBundles(void) {
    __CFSpinLock(&CFBundleGlobalDataLock);
    if (_bundlesToUnload) {
        CFIndex c = CFSetGetCount(_bundlesToUnload);
        if (c > 0) {
            CFIndex i;
            CFBundleRef *unloadThese = CFAllocatorAllocate(NULL, sizeof(CFBundleRef) * c, 0);
            CFSetGetValues(_bundlesToUnload, (const void **)unloadThese);
            _scheduledBundlesAreUnloading = true;
            for (i=0; i<c; i++) {
                // This will cause them to be removed from the set.  (Which is why we copied all the values out of the set up front.)
                CFBundleUnloadExecutable(unloadThese[i]);
            }
            _scheduledBundlesAreUnloading = false;
            CFAllocatorDeallocate(NULL, unloadThese);
        }
    }
    __CFSpinUnlock(&CFBundleGlobalDataLock);
}

void *CFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef funcName) {
    void *tvp = NULL;
    // Load if necessary
    if (!bundle->_isLoaded) {
        if (!CFBundleLoadExecutable(bundle)) return NULL;
    }
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
            tvp = _CFBundleCFMGetSymbolByName(bundle, funcName, kTVectorCFragSymbol);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
        case __CFBundleDYLDFrameworkBinary:
        case __CFBundleDYLDExecutableBinary:
            return _CFBundleDYLDGetSymbolByName(bundle, funcName);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            tvp = _CFBundleDLLGetSymbolByName(bundle, funcName);
            break;
#endif /* BINARY_SUPPORT_DLL */
        default:
            break;
    }
#if defined(BINARY_SUPPORT_DYLD) && defined(BINARY_SUPPORT_CFM) && defined(__ppc__)
    if (tvp != NULL) {
        if (bundle->_glueDict == NULL) {
            bundle->_glueDict = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, NULL, NULL);
        }
        void *fp = (void *)CFDictionaryGetValue(bundle->_glueDict, tvp);
        if (fp == NULL) {
            fp = _CFBundleFunctionPointerForTVector(CFGetAllocator(bundle), tvp);
            CFDictionarySetValue(bundle->_glueDict, tvp, fp);
        }
        return fp;
    }
#endif /* BINARY_SUPPORT_DYLD && BINARY_SUPPORT_CFM && __ppc__ */
    return tvp;
}

void *_CFBundleGetCFMFunctionPointerForName(CFBundleRef bundle, CFStringRef funcName) {
    void *fp = NULL;
    // Load if necessary
    if (!bundle->_isLoaded) {
        if (!CFBundleLoadExecutable(bundle)) return NULL;
    }
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
            return _CFBundleCFMGetSymbolByName(bundle, funcName, kTVectorCFragSymbol);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
        case __CFBundleDYLDFrameworkBinary:
        case __CFBundleDYLDExecutableBinary:
            fp = _CFBundleDYLDGetSymbolByNameWithSearch(bundle, funcName, true);
            break;
#endif /* BINARY_SUPPORT_DYLD */
        default:
            break;
    }
#if defined(BINARY_SUPPORT_DYLD) && defined(BINARY_SUPPORT_CFM) && defined(__ppc__)
    if (fp != NULL) {
        if (bundle->_glueDict == NULL) {
            bundle->_glueDict = CFDictionaryCreateMutable(CFGetAllocator(bundle), 0, NULL, NULL);
        }
        void *tvp = (void *)CFDictionaryGetValue(bundle->_glueDict, fp);
        if (tvp == NULL) {
            tvp = _CFBundleTVectorForFunctionPointer(CFGetAllocator(bundle), fp);
            CFDictionarySetValue(bundle->_glueDict, fp, tvp);
        }
        return tvp;
    }
#endif /* BINARY_SUPPORT_DYLD && BINARY_SUPPORT_CFM && __ppc__ */
    return fp;
}

void CFBundleGetFunctionPointersForNames(CFBundleRef bundle, CFArrayRef functionNames, void *ftbl[]) {
    SInt32 i, c;

    if (!ftbl) return;

    c = CFArrayGetCount(functionNames);
    for (i = 0; i < c; i++) {
        ftbl[i] = CFBundleGetFunctionPointerForName(bundle, CFArrayGetValueAtIndex(functionNames, i));
    }
}

void _CFBundleGetCFMFunctionPointersForNames(CFBundleRef bundle, CFArrayRef functionNames, void *ftbl[]) {
    SInt32 i, c;

    if (!ftbl) return;

    c = CFArrayGetCount(functionNames);
    for (i = 0; i < c; i++) {
        ftbl[i] = _CFBundleGetCFMFunctionPointerForName(bundle, CFArrayGetValueAtIndex(functionNames, i));
    }
}

void *CFBundleGetDataPointerForName(CFBundleRef bundle, CFStringRef symbolName) {
    void *dp = NULL;
    // Load if necessary
    if (!bundle->_isLoaded) {
        if (!CFBundleLoadExecutable(bundle)) return NULL;
    }
    
    switch (bundle->_binaryType) {
#if defined(BINARY_SUPPORT_CFM)
        case __CFBundleCFMBinary:
            dp = _CFBundleCFMGetSymbolByName(bundle, symbolName, kDataCFragSymbol);
            break;
#endif /* BINARY_SUPPORT_CFM */
#if defined(BINARY_SUPPORT_DYLD)
        case __CFBundleDYLDBundleBinary:
        case __CFBundleDYLDFrameworkBinary:
        case __CFBundleDYLDExecutableBinary:
            dp = _CFBundleDYLDGetSymbolByName(bundle, symbolName);
            break;
#endif /* BINARY_SUPPORT_DYLD */
#if defined(BINARY_SUPPORT_DLL)
        case __CFBundleDLLBinary:
            /* MF:!!! Handle this someday */
            break;
#endif /* BINARY_SUPPORT_DLL */
        default:
            break;
    }
    return dp;
}

void CFBundleGetDataPointersForNames(CFBundleRef bundle, CFArrayRef symbolNames, void *stbl[]) {
    SInt32 i, c;

    if (!stbl) return;

    c = CFArrayGetCount(symbolNames);
    for (i = 0; i < c; i++) {
        stbl[i] = CFBundleGetDataPointerForName(bundle, CFArrayGetValueAtIndex(symbolNames, i));
    }
}

__private_extern__ _CFResourceData *__CFBundleGetResourceData(CFBundleRef bundle) {
    return &(bundle->_resourceData);
}

CFPlugInRef CFBundleGetPlugIn(CFBundleRef bundle) {
    if (bundle->_plugInData._isPlugIn) {
        return (CFPlugInRef)bundle;
    } else {
        return NULL;
    }
}

__private_extern__ _CFPlugInData *__CFBundleGetPlugInData(CFBundleRef bundle) {
    return &(bundle->_plugInData);
}

__private_extern__ Boolean _CFBundleCouldBeBundle(CFURLRef url) {
    Boolean result = false;
    Boolean exists;
    SInt32 mode;

    if (_CFGetFileProperties(NULL, url, &exists, &mode, NULL, NULL, NULL, NULL) == 0) {
        result = (exists && ((mode & S_IFMT) == S_IFDIR));
#if !defined(__MACOS8__)
        result = (result && ((mode & 0444) != 0));
#endif
    }
    return result;
}

__private_extern__ CFURLRef _CFBundleCopyFrameworkURLForExecutablePath(CFAllocatorRef alloc, CFStringRef executablePath) {
    // MF:!!! Implement me.  We need to be able to find the bundle from the exe, dealing with old vs. new as well as the Executables dir business on Windows.
#if defined(__WIN32__)
    UniChar executablesToFrameworksPathBuff[] = {'.', '.', '\\', 'F', 'r', 'a', 'm', 'e', 'w', 'o', 'r', 'k', 's'};  // length 16
    UniChar executablesToPrivateFrameworksPathBuff[] = {'.', '.', '\\', 'P', 'r', 'i', 'v', 'a', 't', 'e', 'F', 'r', 'a', 'm', 'e', 'w', 'o', 'r', 'k', 's'};  // length 23
    UniChar frameworksExtension[] = {'f', 'r', 'a', 'm', 'e', 'w', 'o', 'r', 'k'};  // length 9
#endif
    
    UniChar pathBuff[CFMaxPathSize];
    UniChar nameBuff[CFMaxPathSize];
    CFIndex length, nameStart, nameLength, savedLength;
    CFMutableStringRef cheapStr = CFStringCreateMutableWithExternalCharactersNoCopy(alloc, NULL, 0, 0, NULL);
    CFURLRef bundleURL = NULL;

    length = CFStringGetLength(executablePath);
    CFStringGetCharacters(executablePath, CFRangeMake(0, length), pathBuff);

    // Save the name in nameBuff
    length = _CFLengthAfterDeletingPathExtension(pathBuff, length);
    nameStart = _CFStartOfLastPathComponent(pathBuff, length);
    nameLength = length - nameStart;
    memmove(nameBuff, &(pathBuff[nameStart]), nameLength * sizeof(UniChar));

    // Strip the name from pathBuff
    length = _CFLengthAfterDeletingLastPathComponent(pathBuff, length);
    savedLength = length;

#if defined(__WIN32__)
    // * (Windows-only) First check the "Executables" directory parallel to the "Frameworks" directory case.
    _CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, executablesToFrameworksPathBuff, 16);
    _CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, nameBuff, nameLength);
    _CFAppendPathExtension(pathBuff, &length, CFMaxPathSize, frameworksExtension, 9);

    CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
    bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
    if (!_CFBundleCouldBeBundle(bundleURL)) {
        CFRelease(bundleURL);
        bundleURL = NULL;
    }
    // * (Windows-only) Next check the "Executables" directory parallel to the "PrivateFrameworks" directory case.
    if (bundleURL == NULL) {
        length = savedLength;
        _CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, executablesToPrivateFrameworksPathBuff, 23);
        _CFAppendPathComponent(pathBuff, &length, CFMaxPathSize, nameBuff, nameLength);
        _CFAppendPathExtension(pathBuff, &length, CFMaxPathSize, frameworksExtension, 9);

        CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
        bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
        if (!_CFBundleCouldBeBundle(bundleURL)) {
            CFRelease(bundleURL);
            bundleURL = NULL;
        }
    }
#endif
    // * Finally check the executable inside the framework case.
    if (bundleURL == NULL) {
        // MF:!!! This should ensure the framework name is the same as the library name!
        CFIndex curStart;
        
        length = savedLength;
        // To catch all the cases, we just peel off level looking for one ending in .framework or one called "Supporting Files".

        while (length > 0) {
            curStart = _CFStartOfLastPathComponent(pathBuff, length);
            if (curStart >= length) {
                break;
            }
            CFStringSetExternalCharactersNoCopy(cheapStr, &(pathBuff[curStart]), length - curStart, CFMaxPathSize - curStart);
            if (CFEqual(cheapStr, _CFBundleSupportFilesDirectoryName1) || CFEqual(cheapStr, _CFBundleSupportFilesDirectoryName2)) {
                length = _CFLengthAfterDeletingLastPathComponent(pathBuff, length);
                CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
                bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
                if (!_CFBundleCouldBeBundle(bundleURL)) {
                    CFRelease(bundleURL);
                    bundleURL = NULL;
                }
                break;
            } else if (CFStringHasSuffix(cheapStr, CFSTR(".framework"))) {
                CFStringSetExternalCharactersNoCopy(cheapStr, pathBuff, length, CFMaxPathSize);
                bundleURL = CFURLCreateWithFileSystemPath(alloc, cheapStr, PLATFORM_PATH_STYLE, true);
                if (!_CFBundleCouldBeBundle(bundleURL)) {
                    CFRelease(bundleURL);
                    bundleURL = NULL;
                }
                break;
            }
            length = _CFLengthAfterDeletingLastPathComponent(pathBuff, length);
        }
    }

    CFStringSetExternalCharactersNoCopy(cheapStr, NULL, 0, 0);
    CFRelease(cheapStr);

    return bundleURL;
}

static void _CFBundleEnsureBundleExistsForImagePath(CFStringRef imagePath) {
    // This finds the bundle for the given path.
    // If an image path corresponds to a bundle, we see if there is already a bundle instance.  If there is and it is NOT in the _dynamicBundles array, it is added to the staticBundles.  Do not add the main bundle to the list here.
    CFBundleRef bundle;
    CFURLRef curURL = _CFBundleCopyFrameworkURLForExecutablePath(NULL, imagePath);

    if (curURL != NULL) {
        bundle = _CFBundleFindByURL(curURL, true);
        if (bundle == NULL) {
            bundle = _CFBundleCreate(NULL, curURL, true);
        }
        if (bundle != NULL && !bundle->_isLoaded) {
            // make sure that these bundles listed as loaded, and mark them frameworks (we probably can't see anything else here, and we cannot unload them)
#if defined(BINARY_SUPPORT_DYLD)
            if (bundle->_binaryType == __CFBundleUnknownBinary) {
                bundle->_binaryType = __CFBundleDYLDFrameworkBinary;
            }
            if (bundle->_binaryType != __CFBundleCFMBinary && bundle->_binaryType != __CFBundleUnreadableBinary) {
                bundle->_resourceData._executableLacksResourceFork = true;
            }
            if (!bundle->_imageCookie) _CFBundleDYLDCheckLoaded(bundle);
#endif /* BINARY_SUPPORT_DYLD */
            bundle->_isLoaded = true;
        }
        CFRelease(curURL);
    }
}

static void _CFBundleEnsureBundlesExistForImagePaths(CFArrayRef imagePaths) {
    // This finds the bundles for the given paths.
    // If an image path corresponds to a bundle, we see if there is already a bundle instance.  If there is and it is NOT in the _dynamicBundles array, it is added to the staticBundles.  Do not add the main bundle to the list here (even if it appears in imagePaths).
    CFIndex i, imagePathCount = CFArrayGetCount(imagePaths);

    for (i=0; i<imagePathCount; i++) {
        _CFBundleEnsureBundleExistsForImagePath(CFArrayGetValueAtIndex(imagePaths, i));
    }
}

static void _CFBundleEnsureBundlesUpToDateWithHintAlreadyLocked(CFStringRef hint) {
    CFArrayRef imagePaths;
    // Tickle the main bundle into existence
    (void)_CFBundleGetMainBundleAlreadyLocked();
#if defined(BINARY_SUPPORT_DYLD)
    imagePaths = _CFBundleDYLDCopyLoadedImagePathsForHint(hint);
    if (imagePaths != NULL) {
        _CFBundleEnsureBundlesExistForImagePaths(imagePaths);
        CFRelease(imagePaths);
    }
#endif
}

static void _CFBundleEnsureAllBundlesUpToDateAlreadyLocked(void) {
    // This method returns all the statically linked bundles.  This includes the main bundle as well as any frameworks that the process was linked against at launch time.  It does not include frameworks or opther bundles that were loaded dynamically.
    
    CFArrayRef imagePaths;

    // Tickle the main bundle into existence
    (void)_CFBundleGetMainBundleAlreadyLocked();

#if defined(BINARY_SUPPORT_DLL)
#warning (MF) Dont know how to find static bundles for DLLs
#endif

#if defined(BINARY_SUPPORT_CFM)
// CFM bundles are supplied to us by CFM, so we do not need to figure them out ourselves
#endif

#if defined(BINARY_SUPPORT_DYLD)
    imagePaths = _CFBundleDYLDCopyLoadedImagePathsIfChanged();
    if (imagePaths != NULL) {
        _CFBundleEnsureBundlesExistForImagePaths(imagePaths);
        CFRelease(imagePaths);
    }
#endif
}

CFArrayRef CFBundleGetAllBundles(void) {
    // To answer this properly, we have to have created the static bundles!
    __CFSpinLock(&CFBundleGlobalDataLock);
    _CFBundleEnsureAllBundlesUpToDateAlreadyLocked();
    __CFSpinUnlock(&CFBundleGlobalDataLock);
    return _allBundles;
}

uint8_t _CFBundleLayoutVersion(CFBundleRef bundle) {return bundle->_version;}

CF_EXPORT CFURLRef _CFBundleCopyInfoPlistURL(CFBundleRef bundle) {
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    CFStringRef path = CFDictionaryGetValue(infoDict, _kCFBundleInfoPlistURLKey);
    return (path ? CFRetain(path) : NULL);
}

CF_EXPORT CFURLRef _CFBundleCopyPrivateFrameworksURL(CFBundleRef bundle) {return CFBundleCopyPrivateFrameworksURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopyPrivateFrameworksURL(CFBundleRef bundle) {
    CFURLRef result = NULL;

    if (1 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundlePrivateFrameworksURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundlePrivateFrameworksURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundlePrivateFrameworksURLFromBase0, bundle->_url);
    }
    return result;
}

CF_EXPORT CFURLRef _CFBundleCopySharedFrameworksURL(CFBundleRef bundle) {return CFBundleCopySharedFrameworksURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopySharedFrameworksURL(CFBundleRef bundle) {
    CFURLRef result = NULL;

    if (1 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedFrameworksURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedFrameworksURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedFrameworksURLFromBase0, bundle->_url);
    }
    return result;
}

CF_EXPORT CFURLRef _CFBundleCopySharedSupportURL(CFBundleRef bundle) {return CFBundleCopySharedSupportURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopySharedSupportURL(CFBundleRef bundle) {
    CFURLRef result = NULL;

    if (1 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedSupportURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedSupportURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(CFGetAllocator(bundle), _CFBundleSharedSupportURLFromBase0, bundle->_url);
    }
    return result;
}

__private_extern__ CFURLRef _CFBundleCopyBuiltInPlugInsURL(CFBundleRef bundle) {return CFBundleCopyBuiltInPlugInsURL(bundle);}

CF_EXPORT CFURLRef CFBundleCopyBuiltInPlugInsURL(CFBundleRef bundle) {
    CFURLRef result = NULL, alternateResult = NULL;

    CFAllocatorRef alloc = CFGetAllocator(bundle);
    if (1 == bundle->_version) {
        result = CFURLCreateWithString(alloc, _CFBundleBuiltInPlugInsURLFromBase1, bundle->_url);
    } else if (2 == bundle->_version) {
        result = CFURLCreateWithString(alloc, _CFBundleBuiltInPlugInsURLFromBase2, bundle->_url);
    } else {
        result = CFURLCreateWithString(alloc, _CFBundleBuiltInPlugInsURLFromBase0, bundle->_url);
    }
    if (!result || !_urlExists(alloc, result)) {
        if (1 == bundle->_version) {
            alternateResult = CFURLCreateWithString(alloc, _CFBundleAlternateBuiltInPlugInsURLFromBase1, bundle->_url);
        } else if (2 == bundle->_version) {
            alternateResult = CFURLCreateWithString(alloc, _CFBundleAlternateBuiltInPlugInsURLFromBase2, bundle->_url);
        } else {
            alternateResult = CFURLCreateWithString(alloc, _CFBundleAlternateBuiltInPlugInsURLFromBase0, bundle->_url);
        }
        if (alternateResult && _urlExists(alloc, alternateResult)) {
            if (result) CFRelease(result);
            result = alternateResult;
        } else {
            if (alternateResult) CFRelease(alternateResult);
        }
    }
    return result;
}



#if defined(BINARY_SUPPORT_DYLD)

static void *__CFBundleDYLDFindImage(char *buff) {
    void *header = NULL;
    unsigned long i, numImages = _dyld_image_count(), numMatches = 0;
    char *curName, *p, *q;

    for (i = 0; !header && i < numImages; i++) {
        curName = _dyld_get_image_name(i);
        if (curName && 0 == strncmp(curName, buff, CFMaxPathSize)) {
            header = _dyld_get_image_header(i);
            numMatches = 1;
        }
    }
    if (!header) {
        for (i = 0; i < numImages; i++) {
            curName = _dyld_get_image_name(i);
            if (curName) {
                for (p = buff, q = curName; *p && *q && (q - curName < CFMaxPathSize); p++, q++) {
                    if (*p != *q && (q - curName > 11) && 0 == strncmp(q - 11, ".framework/Versions/", 20) && *(q + 9) && '/' == *(q + 10)) q += 11;
                    else if (*p != *q && (q - curName > 12) && 0 == strncmp(q - 12, ".framework/Versions/", 20) && *(q + 8) && '/' == *(q + 9)) q += 10;
                    if (*p != *q) break;
                }
                if (*p == *q) {
                    header = _dyld_get_image_header(i);
                    numMatches++;
                }
            }
        }
    }
    return (numMatches == 1) ? header : NULL;
}

__private_extern__ Boolean _CFBundleDYLDCheckLoaded(CFBundleRef bundle) {
    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

        if (executableURL != NULL) {
            char buff[CFMaxPathSize];

            if (CFURLGetFileSystemRepresentation(executableURL, true, buff, CFMaxPathSize)) {
                void *header = __CFBundleDYLDFindImage(buff);
                if (header) {
                    if (bundle->_binaryType == __CFBundleUnknownBinary) {
                        bundle->_binaryType = __CFBundleDYLDFrameworkBinary;
                    }
                    if (!bundle->_imageCookie) bundle->_imageCookie = header;
                    bundle->_isLoaded = true;
                }
            }
            CFRelease(executableURL);
        }
    }
    return bundle->_isLoaded;
}

__private_extern__ Boolean _CFBundleDYLDLoadBundle(CFBundleRef bundle) {
    NSLinkEditErrors c = NSLinkEditUndefinedError;
    int errorNumber = 0;
    const char *fileName = NULL;
    const char *errorString = NULL;

    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

        if (executableURL) {
            char buff[CFMaxPathSize];

            if (CFURLGetFileSystemRepresentation(executableURL, true, buff, CFMaxPathSize)) {
                NSObjectFileImage image;
                NSObjectFileImageReturnCode retCode = NSCreateObjectFileImageFromFile(buff, &image);
                if (retCode == NSObjectFileImageSuccess) {
                    NSModule module = NSLinkModule(image, buff, (NSLINKMODULE_OPTION_BINDNOW | NSLINKMODULE_OPTION_PRIVATE | NSLINKMODULE_OPTION_RETURN_ON_ERROR));
                    if (module) {
                        bundle->_imageCookie = image;
                        bundle->_moduleCookie = module;
                        bundle->_isLoaded = true;
                    } else {
                        NSLinkEditError(&c, &errorNumber, &fileName, &errorString);
                        CFLog(__kCFLogBundle, CFSTR("Error loading %s:  error code %d, error number %d (%s)"), fileName, c, errorNumber, errorString);
                        if (!NSDestroyObjectFileImage(image)) {
                            /* MF:!!! Error destroying object file image */
                        }
                    }
                } else {
                    CFLog(__kCFLogBundle, CFSTR("dyld returns %d when trying to load %@"), retCode, executableURL);
                }
            }
            CFRelease(executableURL);
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
        }
    }
    return bundle->_isLoaded;
}

__private_extern__ Boolean _CFBundleDYLDLoadFramework(CFBundleRef bundle) {
    // !!! Framework loading should be better.  Can't unload frameworks.
    NSLinkEditErrors c = NSLinkEditUndefinedError;
    int errorNumber = 0;
    const char *fileName = NULL;
    const char *errorString = NULL;

    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

        if (executableURL) {
            char buff[CFMaxPathSize];

            if (CFURLGetFileSystemRepresentation(executableURL, true, buff, CFMaxPathSize)) {
                void *image = (void *)NSAddImage(buff, NSADDIMAGE_OPTION_RETURN_ON_ERROR);
                if (image) {
                    bundle->_imageCookie = image;
                    bundle->_isLoaded = true;
                } else {
                    NSLinkEditError(&c, &errorNumber, &fileName, &errorString);
                    CFLog(__kCFLogBundle, CFSTR("Error loading %s:  error code %d, error number %d (%s)"), fileName, c, errorNumber, errorString);
                }
            }
            CFRelease(executableURL);
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
        }
    }
    return bundle->_isLoaded;
}

__private_extern__ void _CFBundleDYLDUnloadBundle(CFBundleRef bundle) {
    if (bundle->_isLoaded) {
        if (bundle->_moduleCookie && !NSUnLinkModule(bundle->_moduleCookie, NSUNLINKMODULE_OPTION_NONE)) {
            CFLog(__kCFLogBundle, CFSTR("Internal error unloading bundle %@"), bundle);
        } else {
            if (bundle->_moduleCookie && bundle->_imageCookie && !NSDestroyObjectFileImage(bundle->_imageCookie)) {
                /* MF:!!! Error destroying object file image */
            }
            bundle->_connectionCookie = bundle->_imageCookie = bundle->_moduleCookie = NULL;
            bundle->_isLoaded = false;
        }
    }
}

__private_extern__ void *_CFBundleDYLDGetSymbolByName(CFBundleRef bundle, CFStringRef symbolName) {return _CFBundleDYLDGetSymbolByNameWithSearch(bundle, symbolName, false);}

static void *_CFBundleDYLDGetSymbolByNameWithSearch(CFBundleRef bundle, CFStringRef symbolName, Boolean globalSearch) {
    void *result = NULL;
    char buff[1026];
    NSSymbol symbol = NULL;
    
    // MF:!!! What if the factory was in C++ code (and is therefore mangled differently)?  Huh, answer me that!
    // MF: The current answer to the mangling question is that you cannot look up C++ functions with this API.  Thus things like plugin factories must be extern "C" in plugin code.
    buff[0] = '_';
    /* MF:??? ASCII appropriate here? */
    if (CFStringGetCString(symbolName, &(buff[1]), 1024, kCFStringEncodingASCII)) {
        if (bundle->_moduleCookie) {
            symbol = NSLookupSymbolInModule(bundle->_moduleCookie, buff);
        } else if (bundle->_imageCookie) {
            symbol = NSLookupSymbolInImage(bundle->_imageCookie, buff, NSLOOKUPSYMBOLINIMAGE_OPTION_BIND|NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR);
        } 
        if (NULL == symbol && NULL == bundle->_moduleCookie && (NULL == bundle->_imageCookie || globalSearch)) {
            char hintBuff[1026];
            CFStringRef executableName = _CFBundleCopyExecutableName(NULL, bundle, NULL, NULL);
            hintBuff[0] = '\0';
            if (executableName) {
                if (!CFStringGetCString(executableName, hintBuff, 1024, kCFStringEncodingUTF8)) hintBuff[0] = '\0';
                CFRelease(executableName);
            }
            if (NSIsSymbolNameDefinedWithHint(buff, hintBuff)) {
                symbol = NSLookupAndBindSymbolWithHint(buff, hintBuff);
            }
        }
        if (symbol) {
            result = NSAddressOfSymbol(symbol);
        } else {
#if defined(DEBUG)
            CFLog(__kCFLogBundle, CFSTR("dyld cannot find symbol %s in %@"), buff, bundle);
#endif
        }
    }
    return result;
}

static CFStringRef _CFBundleDYLDCopyLoadedImagePathForPointer(void *p) {
    unsigned long i, j, n = _dyld_image_count();
    Boolean foundit = false;
    char *name;
    CFStringRef result = NULL;
    for (i = 0; !foundit && i < n; i++) {
        struct mach_header *mh = _dyld_get_image_header(i);
        unsigned long addr = (unsigned long)p - _dyld_get_image_vmaddr_slide(i);
        if (mh) {
            struct load_command *lc = (struct load_command *)((char *)mh + sizeof(struct mach_header));
            for (j = 0; !foundit && j < mh->ncmds; j++, lc = (struct load_command *)((char *)lc + lc->cmdsize)) {
                if (LC_SEGMENT == lc->cmd && addr >= ((struct segment_command *)lc)->vmaddr && addr < ((struct segment_command *)lc)->vmaddr + ((struct segment_command *)lc)->vmsize) {
                    foundit = true;
                    name = _dyld_get_image_name(i);
                    if (name != NULL) {
                        result = CFStringCreateWithCString(NULL, name, CFStringFileSystemEncoding());
                    }
                }
            }
        }
    }
    return result;
}

__private_extern__ CFArrayRef _CFBundleDYLDCopyLoadedImagePathsForHint(CFStringRef hint) {
    unsigned long i, numImages = _dyld_image_count();
    CFMutableArrayRef result = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFRange range = CFRangeMake(0, CFStringGetLength(hint));
    
    for (i=0; i<numImages; i++) {
        char *curName = _dyld_get_image_name(i), *lastComponent = NULL;
        if (curName != NULL) lastComponent = strrchr(curName, '/');
        if (lastComponent != NULL) {
            CFStringRef str = CFStringCreateWithCString(NULL, lastComponent + 1, CFStringFileSystemEncoding());
            if (str) {
                if (CFStringFindWithOptions(hint, str, range, kCFCompareAnchored|kCFCompareBackwards|kCFCompareCaseInsensitive, NULL)) {
                    CFStringRef curStr = CFStringCreateWithCString(NULL, curName, CFStringFileSystemEncoding());
                    if (curStr != NULL) {
                        CFArrayAppendValue(result, curStr);
                        CFRelease(curStr);
                    }
                }
                CFRelease(str);
            }
        }
    }
    return result;
}

__private_extern__ CFArrayRef _CFBundleDYLDCopyLoadedImagePathsIfChanged(void) {
    // This returns an array of the paths of all the dyld images in the process.  These paths may not be absolute, they may point at things that are not bundles, they may be staticly linked bundles or dynamically loaded bundles, they may be NULL.
    static unsigned long _cachedDYLDImageCount = -1;

    unsigned long i, numImages = _dyld_image_count();
    CFMutableArrayRef result = NULL;

    if (numImages != _cachedDYLDImageCount) {
        char *curName;
        CFStringRef curStr;

        result = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

        for (i=0; i<numImages; i++) {
            curName = _dyld_get_image_name(i);
            if (curName != NULL) {
                curStr = CFStringCreateWithCString(NULL, curName, CFStringFileSystemEncoding());
                if (curStr != NULL) {
                    CFArrayAppendValue(result, curStr);
                    CFRelease(curStr);
                }
            }
        }
        _cachedDYLDImageCount = numImages;
    }
    return result;
}

#endif /* BINARY_SUPPORT_DYLD */


#if defined(BINARY_SUPPORT_DLL)

__private_extern__ Boolean _CFBundleDLLLoad(CFBundleRef bundle) {

    if (!bundle->_isLoaded) {
        CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);

        if (executableURL) {
            char buff[CFMaxPathSize];

            if (CFURLGetFileSystemRepresentation(executableURL, true, buff, CFMaxPathSize)) {
                bundle->_hModule = LoadLibrary(buff);
                if (bundle->_hModule == NULL) {
                } else {
                   bundle->_isLoaded = true;
                }
            }
            CFRelease(executableURL);
        } else {
            CFLog(__kCFLogBundle, CFSTR("Cannot find executable for bundle %@"), bundle);
        }
    }

    return bundle->_isLoaded;
}

__private_extern__ void _CFBundleDLLUnload(CFBundleRef bundle) {
    if (bundle->_isLoaded) {
       FreeLibrary(bundle->_hModule);
       bundle->_hModule = NULL;
       bundle->_isLoaded = false;
    }
}

__private_extern__ void *_CFBundleDLLGetSymbolByName(CFBundleRef bundle, CFStringRef symbolName) {
    void *result = NULL;
    char buff[1024];
    
    if (CFStringGetCString(symbolName, buff, 1024, kCFStringEncodingWindowsLatin1)) {
        result = GetProcAddress(bundle->_hModule, buff);
    }
    return result;
}

#endif /* BINARY_SUPPORT_DLL */


/* Workarounds to be applied in the presence of certain bundles can go here. This is called on every bundle creation.
*/
extern void _CFStringSetCompatibility(CFOptionFlags);

static void _CFBundleCheckWorkarounds(CFBundleRef bundle) {
}

