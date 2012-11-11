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
/*	CFURLAccess.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Becky Willrich
*/

/*------
CFData read/write routines
-------*/

#include "CFInternal.h"
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFNumber.h>
#include <string.h>

#if defined(__WIN32__)
#include <winsock2.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#define timeval xxx_timeval
#define BOOLEAN xxx_BOOLEAN
#include <windows.h>
#undef BOOLEAN
#undef timeval
#else
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <fcntl.h>
#include <dlfcn.h>
#endif

#if defined(__MACH__)

DEFINE_WEAK_CFNETWORK_FUNC(Boolean, _CFURLCreateDataAndPropertiesFromResource, (CFAllocatorRef A, CFURLRef B, CFDataRef *C, CFDictionaryRef *D, CFArrayRef E, SInt32 *F), (A, B, C, D, E, F), false)
DEFINE_WEAK_CFNETWORK_FUNC(Boolean, _CFURLWriteDataAndPropertiesToResource, (CFURLRef A, CFDataRef B, CFDictionaryRef C, SInt32 *D), (A, B, C, D), false)
DEFINE_WEAK_CFNETWORK_FUNC(Boolean, _CFURLDestroyResource, (CFURLRef A, SInt32 *B), (A, B), false)

#endif
            

CONST_STRING_DECL(kCFURLFileExists, "kCFURLFileExists")
CONST_STRING_DECL(kCFURLFilePOSIXMode, "kCFURLFilePOSIXMode")
CONST_STRING_DECL(kCFURLFileDirectoryContents, "kCFURLFileDirectoryContents")
CONST_STRING_DECL(kCFURLFileLength, "kCFURLFileLength")
CONST_STRING_DECL(kCFURLFileLastModificationTime, "kCFURLFileLastModificationTime")
CONST_STRING_DECL(kCFURLFileOwnerID, "kCFURLFileOwnerID")
CONST_STRING_DECL(kCFURLHTTPStatusCode, "kCFURLHTTPStatusCode")
CONST_STRING_DECL(kCFURLHTTPStatusLine, "kCFURLHTTPStatusLine")

// Compatibility property strings -- we obsoleted these names pre-DP4. REW, 5/22/2000
CONST_STRING_DECL(kCFFileURLExists, "kCFURLFileExists")
CONST_STRING_DECL(kCFFileURLPOSIXMode, "kCFURLFilePOSIXMode")
CONST_STRING_DECL(kCFFileURLDirectoryContents, "kCFURLFileDirectoryContents")
CONST_STRING_DECL(kCFFileURLSize, "kCFURLFileLength")
CONST_STRING_DECL(kCFFileURLLastModificationTime, "kCFURLFileLastModificationTime")
CONST_STRING_DECL(kCFHTTPURLStatusCode, "kCFURLHTTPStatusCode")
CONST_STRING_DECL(kCFHTTPURLStatusLine, "kCFURLHTTPStatusLine")

// Copied pretty much verbatim from NSData; note that files are still special cased in this code.  Ultimately, we probably want to treat file URLs the same way as any other URL (go through the URL Access layer).  -- REW, 10/21/98

/*************************/
/* file: access routines */
/*************************/

//#warning CF:For the moment file access failures are ill defined and set the error code to kCFURLUnknownError

static CFDictionaryRef _CFFileURLCreatePropertiesFromResource(CFAllocatorRef alloc, CFURLRef url, CFArrayRef desiredProperties, SInt32 *errorCode) {
    // MF:!!! This could/should be changed to use _CFGetFileProperties() to do the actual figuring.
    static CFArrayRef _allProps = NULL;
    CFRange arrayRange;
    SInt32 idx;
    CFMutableDictionaryRef propertyDict = NULL;

    Boolean exists;
    SInt32 posixMode;
    int64_t size;
    CFDateRef modTime = NULL, *modTimePtr = NULL;
    CFArrayRef contents = NULL, *contentsPtr = NULL;
    SInt32 ownerID;

    if (errorCode) *errorCode = 0;
    if (!desiredProperties) {
        // Cheap and dirty hack to make this work for the moment; ultimately we need to do something more sophisticated.  This will result in an error return whenever a property key is defined which isn't applicable to all file URLs.  REW, 3/2/99
        if (!_allProps) {
            const void *values[9];
            values[0] = kCFURLFileExists;
            values[1] = kCFURLFilePOSIXMode;
            values[2] = kCFURLFileDirectoryContents;
            values[3] = kCFURLFileLength;
            values[4] = kCFURLFileLastModificationTime;
            values[5] = kCFURLFileOwnerID;
            _allProps = CFArrayCreate(NULL, values, 6, &kCFTypeArrayCallBacks);
        }
        desiredProperties = _allProps;
    }

    arrayRange.location = 0;
    arrayRange.length = CFArrayGetCount(desiredProperties);
    propertyDict = CFDictionaryCreateMutable(alloc, 0, & kCFTypeDictionaryKeyCallBacks, & kCFTypeDictionaryValueCallBacks);
    if (arrayRange.length == 0) return propertyDict;

    if (CFArrayContainsValue(desiredProperties, arrayRange, kCFURLFileDirectoryContents)) {
        contentsPtr = &contents;
    }
    if (CFArrayContainsValue(desiredProperties, arrayRange, kCFURLFileLastModificationTime)) {
        modTimePtr = &modTime;
    }

    if (_CFGetFileProperties(alloc, url, &exists, &posixMode, &size, modTimePtr, &ownerID, contentsPtr) != 0) {
        if (errorCode) {
            *errorCode = kCFURLUnknownError;
        }
        return propertyDict;
    }
    
    for (idx = 0; idx < arrayRange.length; idx ++) {
        CFStringRef key = (CFMutableStringRef )CFArrayGetValueAtIndex(desiredProperties, idx);
        if (key == kCFURLFilePOSIXMode || CFEqual(kCFURLFilePOSIXMode, key)) {
            if (exists) {
                CFNumberRef num = CFNumberCreate(alloc, kCFNumberSInt32Type, &posixMode);
                CFDictionarySetValue(propertyDict, kCFURLFilePOSIXMode, num);
                CFRelease(num);
            } else if (errorCode) {
                *errorCode = kCFURLUnknownError;
            }
        } else if (key == kCFURLFileDirectoryContents || CFEqual(kCFURLFileDirectoryContents, key)) {
            if (exists && (posixMode & S_IFMT) == S_IFDIR && contents) {
                CFDictionarySetValue(propertyDict, kCFURLFileDirectoryContents, contents);
            } else if (errorCode) {
                *errorCode = kCFURLUnknownError;
            }
        } else if (key == kCFURLFileLength || CFEqual(kCFURLFileLength, key)) {
            if (exists) {
                CFNumberRef num = CFNumberCreate(alloc, kCFNumberSInt64Type, &size);
                CFDictionarySetValue(propertyDict, kCFURLFileLength, num);
                CFRelease(num);
            } else if (errorCode) {
                *errorCode = kCFURLUnknownError;
            }
        } else if (key == kCFURLFileLastModificationTime || CFEqual(kCFURLFileLastModificationTime, key)) {
            if (exists && modTime) {
                CFDictionarySetValue(propertyDict, kCFURLFileLastModificationTime, modTime);
            } else if (errorCode) {
                *errorCode = kCFURLUnknownError;
            }
        } else if (key == kCFURLFileExists || CFEqual(kCFURLFileExists, key)) {
            if (exists) {
                CFDictionarySetValue(propertyDict, kCFURLFileExists, kCFBooleanTrue);
            } else {
                CFDictionarySetValue(propertyDict, kCFURLFileExists, kCFBooleanFalse);
            }
        } else if (key == kCFURLFileOwnerID || CFEqual(kCFURLFileOwnerID, key)) {
            if (exists) {
                CFNumberRef num  = CFNumberCreate(alloc, kCFNumberSInt32Type, &ownerID);
                CFDictionarySetValue(propertyDict, kCFURLFileOwnerID, num);
                CFRelease(num);
            } else if (errorCode) {
                *errorCode = kCFURLUnknownError;
            }
        // Add more properties here
        } else if (errorCode) {
            *errorCode = kCFURLUnknownPropertyKeyError;
        }
    }
    if (modTime) CFRelease(modTime);
    if (contents) CFRelease(contents);
    return propertyDict;
}

static Boolean _CFFileURLWritePropertiesToResource(CFURLRef url, CFDictionaryRef propertyDict, SInt32 *errorCode) {
#if defined(__MACOS8__)
    return false; // No properties are writable on OS 8
#else
    CFTypeRef buffer[16];
    CFTypeRef *keys;
    CFTypeRef *values;
    Boolean result = true;
    SInt32 idx, count;
    char cPath[CFMaxPathSize];

    if (!CFURLGetFileSystemRepresentation(url, true, cPath, CFMaxPathSize)) {
        if (errorCode) *errorCode = kCFURLImproperArgumentsError;
        return false;
    }

    count = CFDictionaryGetCount(propertyDict);
    if (count < 8) {
        (CFTypeRef)keys = buffer;
        (CFTypeRef)values = buffer+8;
    } else {
        keys = CFAllocatorAllocate(CFGetAllocator(url), sizeof(void *) * count * 2, 0);
        values = keys + count;
    }
    CFDictionaryGetKeysAndValues(propertyDict, keys, values);

    for (idx = 0; idx < count; idx ++) {
        CFStringRef key = keys[idx];
        CFTypeRef value = values[idx];
        if (key == kCFURLFilePOSIXMode || CFEqual(kCFURLFilePOSIXMode, key)) {
            SInt32 mode;
            int err;
            if (CFNumberGetTypeID() == CFGetTypeID(value)) {
                CFNumberRef modeNum = (CFNumberRef)value;
                CFNumberGetValue(modeNum, kCFNumberSInt32Type, &mode);
            } else {
#if defined(__WIN32__)
                const uint16_t *modePtr = (const uint16_t *)CFDataGetBytePtr((CFDataRef)value);
#else
                const mode_t *modePtr = (const mode_t *)CFDataGetBytePtr((CFDataRef)value);
#endif
                mode = *modePtr;
            }
            err = chmod(cPath, mode);
            if (err != 0) result = false;
        } else {
            result = false;
        }
    }

    if ((CFTypeRef)keys != buffer) CFAllocatorDeallocate(CFGetAllocator(url), keys);

    if (errorCode) *errorCode = result ? 0 : kCFURLUnknownError;
    return result;
#endif
}

static Boolean _CFFileURLCreateDataAndPropertiesFromResource(CFAllocatorRef alloc, CFURLRef url, CFDataRef *fetchedData, CFArrayRef desiredProperties, CFDictionaryRef *fetchedProperties, SInt32 *errorCode) {
    Boolean success = true;

    if (errorCode) *errorCode = 0;
    if (fetchedData) {
        void *bytes;
        CFIndex length;
        Boolean releaseAlloc = false;
        
        if (alloc == NULL) {
            // We need a real allocator to pass to _CFReadBytesFromFile so that the CFDataRef we create with
			//	CFDataCreateWithBytesNoCopy() can free up the object _CFReadBytesFromFile() returns.
            alloc = CFRetain(__CFGetDefaultAllocator());
            releaseAlloc = true;
        }
        if (!_CFReadBytesFromFile(alloc, url, &bytes, &length, 0)) {
            if (errorCode) *errorCode = kCFURLUnknownError;
            *fetchedData = NULL;
            success = false;
        } else {
            *fetchedData = CFDataCreateWithBytesNoCopy(alloc, bytes, length, alloc);
        }
        if (releaseAlloc) {
            // Now the CFData should be hanging on to it.
            CFRelease(alloc);
        }
    }

    if (fetchedProperties) {
        *fetchedProperties = _CFFileURLCreatePropertiesFromResource(alloc, url, desiredProperties, errorCode);
        if (!*fetchedProperties) success = false;
    }

    return success;
}
/*************************/
/* Public routines       */
/*************************/

Boolean CFURLCreateDataAndPropertiesFromResource(CFAllocatorRef alloc, CFURLRef url, CFDataRef *fetchedData, CFDictionaryRef *fetchedProperties, CFArrayRef desiredProperties, SInt32 *errorCode) {
    CFStringRef scheme = CFURLCopyScheme(url);

    if (!scheme) {
        if (errorCode) *errorCode = kCFURLImproperArgumentsError;
        if (fetchedData) *fetchedData = NULL;
        if (fetchedProperties) *fetchedProperties = NULL;
        return false;
    } else {
        Boolean result;
        if (CFStringCompare(scheme, CFSTR("file"), 0) == kCFCompareEqualTo) {
            result = _CFFileURLCreateDataAndPropertiesFromResource(alloc, url, fetchedData, desiredProperties, fetchedProperties, errorCode);
        } else {
#if defined(__MACH__)
            result = __CFNetwork__CFURLCreateDataAndPropertiesFromResource(alloc, url, fetchedData, fetchedProperties, desiredProperties, errorCode);
	    if (!result) {
		if (fetchedData) *fetchedData = NULL;
		if (fetchedProperties) *fetchedProperties = NULL;
		if (errorCode) *errorCode = kCFURLUnknownSchemeError;
	    }
#else
            if (fetchedData) *fetchedData = NULL;
            if (fetchedProperties) *fetchedProperties = NULL;
            if (errorCode) *errorCode = kCFURLUnknownSchemeError;
            result = false;
#endif
        }
        CFRelease(scheme);
        return result;
    }
}

CFTypeRef CFURLCreatePropertyFromResource(CFAllocatorRef alloc, CFURLRef url, CFStringRef property, SInt32 *errorCode) {
    CFArrayRef array = CFArrayCreate(alloc, (const void **)&property, 1, &kCFTypeArrayCallBacks);
    CFDictionaryRef dict;

    if (CFURLCreateDataAndPropertiesFromResource(alloc, url, NULL, &dict, array, errorCode)) {
        CFTypeRef result = CFDictionaryGetValue(dict, property);
        if (result) CFRetain(result);
        CFRelease(array);
        CFRelease(dict);
        return result;
    } else {
        if (dict) CFRelease(dict);
        CFRelease(array);
        return NULL;
    }
}

Boolean CFURLWriteDataAndPropertiesToResource(CFURLRef url, CFDataRef data, CFDictionaryRef propertyDict, SInt32 *errorCode) {
    CFStringRef scheme = CFURLCopyScheme(url);
    if (!scheme) {
        if (errorCode) *errorCode = kCFURLImproperArgumentsError;
        return false;
    } else if (CFStringCompare(scheme, CFSTR("file"), 0) == kCFCompareEqualTo) {
        Boolean success = true;
        CFRelease(scheme);
        if (errorCode) *errorCode = 0;
        if (data) {
            if (CFURLHasDirectoryPath(url)) {
                // Create a directory
                char cPath[CFMaxPathSize];
                if (!CFURLGetFileSystemRepresentation(url, true, cPath, CFMaxPathSize)) {
                    if (errorCode) *errorCode = kCFURLImproperArgumentsError;
                    success = false;
                } else {
                    success = _CFCreateDirectory(cPath);
                    if (!success && errorCode) *errorCode = kCFURLUnknownError;
                }
            } else {
               // Write data
                SInt32 length = CFDataGetLength(data);
                const void *bytes = (0 == length) ? (const void *)"" : CFDataGetBytePtr(data);
                success = _CFWriteBytesToFile(url, bytes, length);
                if (!success && errorCode) *errorCode = kCFURLUnknownError;
            }
        }
        if (propertyDict) {
            if (!_CFFileURLWritePropertiesToResource(url, propertyDict, errorCode))
                success = false;
        }
        return success;
    } else {
        CFRelease(scheme);
#if defined(__MACH__)
        Boolean result = __CFNetwork__CFURLWriteDataAndPropertiesToResource(url, data, propertyDict, errorCode);
	if (!result) {
	    if (errorCode) *errorCode = kCFURLUnknownSchemeError;
	}
	return result;
#else
        if (errorCode) *errorCode = kCFURLUnknownSchemeError;
        return false;
#endif
    }
}

Boolean CFURLDestroyResource(CFURLRef url, SInt32 *errorCode) {
    CFStringRef scheme = CFURLCopyScheme(url);
    char cPath[CFMaxPathSize];

    if (!scheme) {
        if (errorCode) *errorCode = kCFURLImproperArgumentsError;
        return false;
    } else if (CFStringCompare(scheme, CFSTR("file"), 0) == kCFCompareEqualTo) {
        CFRelease(scheme);
        if (!CFURLGetFileSystemRepresentation(url, true, cPath, CFMaxPathSize)) {
            if (errorCode) *errorCode = kCFURLImproperArgumentsError;
            return false;
        }

        if (CFURLHasDirectoryPath(url)) {
            if (_CFRemoveDirectory(cPath)) {
                if (errorCode) *errorCode = 0;
                return true;
            } else {
                if (errorCode) *errorCode = kCFURLUnknownError;
                return false;
            }
        } else {
            if (_CFDeleteFile(cPath)) {
                if (errorCode) *errorCode = 0;
                return true;
            } else {
                if (errorCode) *errorCode = kCFURLUnknownError;
                return false;
            }
        }
    } else {
        CFRelease(scheme);
#if defined(__MACH__)
        Boolean result = __CFNetwork__CFURLDestroyResource(url, errorCode);
	if (!result) {
	    if (errorCode) *errorCode = kCFURLUnknownSchemeError;
	}
	return result;
#else
        if (errorCode) *errorCode = kCFURLUnknownSchemeError;
        return false;
#endif
    }
}

