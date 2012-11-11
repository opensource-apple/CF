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
/*	CFXMLPreferencesDomain.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Chris Parker
*/

#if !defined(__WIN32__)

#include <CoreFoundation/CFPreferences.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFDate.h>
#include "CFInternal.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_syscalls.h>

Boolean __CFPreferencesShouldWriteXML(void);

typedef struct {
    CFMutableDictionaryRef _domainDict; // Current value of the domain dictionary
    CFMutableArrayRef _dirtyKeys; // The array of keys which must be synchronized
    CFAbsoluteTime _lastReadTime; // The last time we synchronized with the disk
    CFSpinLock_t _lock; // Lock for accessing fields in the domain
    Boolean _isWorldReadable; // HACK - this is because we have no good way to propogate the kCFPreferencesAnyUser information from the upper level CFPreferences routines  REW, 1/13/00
    char _padding[3];
} _CFXMLPreferencesDomain;

static void *createXMLDomain(CFAllocatorRef allocator, CFTypeRef context);
static void freeXMLDomain(CFAllocatorRef allocator, CFTypeRef context, void *tDomain);
static CFTypeRef fetchXMLValue(CFTypeRef context, void *xmlDomain, CFStringRef key);
static void writeXMLValue(CFTypeRef context, void *xmlDomain, CFStringRef key, CFTypeRef value);
static Boolean synchronizeXMLDomain(CFTypeRef context, void *xmlDomain);
static void getXMLKeysAndValues(CFAllocatorRef alloc, CFTypeRef context, void *xmlDomain, void **buf[], CFIndex *numKeyValuePairs);
static CFDictionaryRef copyXMLDomainDictionary(CFTypeRef context, void *domain);
static void setXMLDomainIsWorldReadable(CFTypeRef context, void *domain, Boolean isWorldReadable);

__private_extern__ const _CFPreferencesDomainCallBacks __kCFXMLPropertyListDomainCallBacks = {createXMLDomain, freeXMLDomain, fetchXMLValue, writeXMLValue, synchronizeXMLDomain, getXMLKeysAndValues, copyXMLDomainDictionary, setXMLDomainIsWorldReadable};

// Directly ripped from Foundation....
static void __CFMilliSleep(uint32_t msecs) {
#if defined(__WIN32__)
    SleepEx(msecs, false);
#elif defined(__svr4__) || defined(__hpux__)
    sleep((msecs + 900) / 1000);
#elif defined(__MACH__)
    struct timespec input;
    input.tv_sec = msecs / 1000;
    input.tv_nsec = (msecs - input.tv_sec * 1000) * 1000000;
    nanosleep(&input, NULL);
#else
#error Dont know how to define sleep for this platform
#endif
}

static CFSpinLock_t _propDictLock = 0; // Annoying that we need this, but otherwise we have a multithreading risk

CF_INLINE CFDictionaryRef URLPropertyDictForPOSIXMode(SInt32 mode) {
    static CFMutableDictionaryRef _propertyDict = NULL;
    CFNumberRef num = CFNumberCreate(__CFPreferencesAllocator(), kCFNumberSInt32Type, &mode);
    __CFSpinLock(&_propDictLock);
    if (!_propertyDict) {
        _propertyDict = CFDictionaryCreateMutable(__CFPreferencesAllocator(), 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    CFDictionarySetValue(_propertyDict, kCFURLFilePOSIXMode, num);
    CFRelease(num);
    return _propertyDict;
}

CF_INLINE void URLPropertyDictRelease(void) {
    __CFSpinUnlock(&_propDictLock);
}

// Asssumes caller already knows the directory doesn't exist.
static Boolean _createDirectory(CFURLRef dirURL, Boolean worldReadable) {
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    CFURLRef parentURL = CFURLCreateCopyDeletingLastPathComponent(alloc, dirURL);
    CFBooleanRef val = CFURLCreatePropertyFromResource(alloc, parentURL, kCFURLFileExists, NULL);
    Boolean parentExists = (val && CFBooleanGetValue(val));
    SInt32 mode;
    Boolean result;
    if (val) CFRelease(val);
    if (!parentExists) {
        CFStringRef path = CFURLCopyPath(parentURL);
        if (!CFEqual(path, CFSTR("/"))) {
            _createDirectory(parentURL, worldReadable);
            val = CFURLCreatePropertyFromResource(alloc, parentURL, kCFURLFileExists, NULL);
            parentExists = (val && CFBooleanGetValue(val));
            if (val) CFRelease(val);
        }
        CFRelease(path);
    }
    if (parentURL) CFRelease(parentURL);
    if (!parentExists) return false;

    mode = worldReadable ? S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH : S_IRWXU;

    result = CFURLWriteDataAndPropertiesToResource(dirURL, (CFDataRef)dirURL, URLPropertyDictForPOSIXMode(mode), NULL);
    URLPropertyDictRelease();
    return result;
}


/* XML - context is the CFURL where the property list is stored on disk; domain is an _CFXMLPreferencesDomain */
static void *createXMLDomain(CFAllocatorRef allocator, CFTypeRef context) {
    _CFXMLPreferencesDomain *domain = CFAllocatorAllocate(allocator, sizeof(_CFXMLPreferencesDomain), 0);
    domain->_lastReadTime = 0.0;
    domain->_domainDict = NULL;
    domain->_dirtyKeys = CFArrayCreateMutable(allocator, 0, & kCFTypeArrayCallBacks);
    domain->_lock = 0;
    domain->_isWorldReadable = false;
    return domain;
}

static void freeXMLDomain(CFAllocatorRef allocator, CFTypeRef context, void *tDomain) {
    _CFXMLPreferencesDomain *domain = (_CFXMLPreferencesDomain *)tDomain;
    if (domain->_domainDict) CFRelease(domain->_domainDict);
    if (domain->_dirtyKeys) CFRelease(domain->_dirtyKeys);
    CFAllocatorDeallocate(allocator, domain);
}

// Assumes the domain has already been locked
static void _loadXMLDomainIfStale(CFURLRef url, _CFXMLPreferencesDomain *domain) {
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    int idx;
    if (domain->_domainDict) {
        CFDateRef modDate;
        CFAbsoluteTime modTime;
    	CFURLRef testURL = url;

        if (CFDictionaryGetCount(domain->_domainDict) == 0) {
            // domain never existed; check the parent directory, not the child
            testURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, CFSTR(".."), kCFURLPOSIXPathStyle, true, url);
        }

        modDate = (CFDateRef )CFURLCreatePropertyFromResource(alloc, testURL, kCFURLFileLastModificationTime, NULL);
        modTime = modDate ? CFDateGetAbsoluteTime(modDate) : 0.0;

        // free before possible return. we can test non-NULL of modDate but don't depend on contents after this.
        if (testURL != url) CFRelease(testURL);
        if (modDate) CFRelease(modDate);
        
        if (modDate != NULL && modTime < domain->_lastReadTime) {            // We're up-to-date
            return;
        }
    }

	
    // We're out-of-date; destroy domainDict and reload
    if (domain->_domainDict) {
        CFRelease(domain->_domainDict);
        domain->_domainDict = NULL;
    }

    // We no longer lock on read; instead, we assume parse failures are because someone else is writing the file, and just try to parse again.  If we fail 3 times in a row, we assume the file is corrupted.  REW, 7/13/99

    for (idx = 0; idx < 3; idx ++) {
        CFDataRef data;
        if (!CFURLCreateDataAndPropertiesFromResource(alloc, url, &data, NULL, NULL, NULL) || !data) {
            // Either a file system error (so we can't read the file), or an empty (or perhaps non-existant) file
            domain->_domainDict = CFDictionaryCreateMutable(alloc, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            break;
        } else {
            CFTypeRef pList = CFPropertyListCreateFromXMLData(alloc, data, kCFPropertyListImmutable, NULL);
            CFRelease(data);
            if (pList && CFGetTypeID(pList) == CFDictionaryGetTypeID()) {
                domain->_domainDict = CFDictionaryCreateMutableCopy(alloc, 0, (CFDictionaryRef)pList);
                CFRelease(pList);
                break;
            } else if (pList) {
                CFRelease(pList);
            }
            // Assume the file is being written; sleep for a short time (to allow the write to complete) then re-read
            __CFMilliSleep(150);
        }
    }
    if (!domain->_domainDict) {
        // Failed to ever load
        domain->_domainDict = CFDictionaryCreateMutable(alloc, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    domain->_lastReadTime = CFAbsoluteTimeGetCurrent();
}

static CFTypeRef fetchXMLValue(CFTypeRef context, void *xmlDomain, CFStringRef key) {
    _CFXMLPreferencesDomain *domain = (_CFXMLPreferencesDomain *)xmlDomain;
    CFTypeRef result;
 
    // Never reload if we've looked at the file system within the last 5 seconds.
    __CFSpinLock(&domain->_lock);
    if (domain->_domainDict == NULL) _loadXMLDomainIfStale((CFURLRef )context, domain);
    result = CFDictionaryGetValue(domain->_domainDict, key);
    if (result) CFRetain(result); 
    __CFSpinUnlock(&domain->_lock);

    return result;
}


#if defined(__MACH__)
#include <sys/fcntl.h>
#if 0
// appends a unique 8.3 path name to the directory name specified in fn,
// atomically determining uniqueness and opening the file.  The file
// descriptor is returned by reference in the second parameter.  0 is
// returned on success, -1 on failure.
// We don't currently handle the case where the directory name is very
// long and adding an 8.3 name makes the path too long.
static int __CFmkstemp83(char *fn, char *prefix, int mode, int *fd) {
    static CFSpinLock_t counter_lock = 0;
    static unsigned int extension_counter = 0;
    int origlen = strlen(fn);
    char idbuf[6], extbuf[6], prebuf[5];
    uint16_t pid, origpid, ext, origext;

    __CFSpinLock(&counter_lock);
    ext = extension_counter++;
    if (0xFFF < extension_counter) extension_counter = 0;
    __CFSpinUnlock(&counter_lock);
    origext = ext;
    do {
	char *s1 = prebuf;
	const char *s2 = prefix;
	int n = 0;
	for (; (*s1 = *s2) && (n < 4); s1++, s2++, n++);
    } while (0);
    prebuf[4] = '\0';
    if (0 < origlen && fn[origlen - 1] != '/')
	fn[origlen++] = '/';
    pid = getpid() & 0xFFFF;
    origpid = pid;
    snprintf(idbuf, 6, "%04x", pid);
    snprintf(extbuf, 6, ".%03x", ext);
    fn[origlen] = '\0';
    strcat(fn, prebuf);
    strcat(fn, idbuf);
    strcat(fn, extbuf);
    for (;;) {
    	*fd = open(fn, O_CREAT|O_EXCL|O_RDWR, mode);
	if (0 <= *fd)
	    return 0;
	if (EEXIST != thread_errno())
	    return -1;
	ext = (ext + 1) & 0xFFF;
	if (origext == ext) {
	    // bump the number and start over with extension
	    pid = (pid + 1) & 0xFFFF;
	    if (pid == origpid)
	    	return -1;	// 2^28 file names tried! errno == EEXIST
            snprintf(idbuf, 6, "%04x", pid);
	}
        snprintf(extbuf, 6, ".%03x", ext);
	fn[origlen] = '\0';
	strcat(fn, prebuf);
	strcat(fn, idbuf);
	strcat(fn, extbuf);
    }
    return -1;
}
#endif

/* __CFWriteBytesToFileWithAtomicity is a "safe save" facility. Write the bytes using the specified mode on the file to the provided URL. If the atomic flag is true, try to do it in a fashion that will enable a safe save.
 */
static Boolean __CFWriteBytesToFileWithAtomicity(CFURLRef url, const void *bytes, int length, SInt32 mode, Boolean atomic) {
    int fd = -1;
    char auxPath[CFMaxPathSize + 16];
    char cpath[CFMaxPathSize];
    int fsyncErr = 0;

    if (!CFURLGetFileSystemRepresentation(url, true, cpath, CFMaxPathSize)) {
        return false;
    }
    if (-1 == mode) {
	struct stat statBuf;
	mode = (0 == stat(cpath, &statBuf)) ? statBuf.st_mode : 0600;
    }
    if (atomic) {
        CFURLRef dir = CFURLCreateCopyDeletingLastPathComponent(NULL, url);
        CFURLRef tempFile = CFURLCreateCopyAppendingPathComponent(NULL, dir, CFSTR("cf#XXXXX"), false);
        CFRelease(dir);
        if (!CFURLGetFileSystemRepresentation(tempFile, true, auxPath, CFMaxPathSize)) {
            CFRelease(tempFile);
            return false;
        }
        CFRelease(tempFile);
        fd = mkstemp(auxPath);
    } else {
        fd = open(cpath, O_WRONLY|O_CREAT|O_TRUNC, mode);
    }
    if (fd < 0) return false;
    if (length && (write(fd, bytes, length) != length || fsync(fd) < 0)) {
        int saveerr = thread_errno();
        close(fd);
        if (atomic)
            unlink(auxPath);
        thread_set_errno(saveerr);
        return false;
    }
    close(fd);
    if (atomic) {
        // preserve the mode as passed in originally
        chmod(auxPath, mode);
        
        if (0 != rename(auxPath, cpath)) {
            unlink(auxPath);
            return false;
        }
    }
    return true;
}
#endif

// domain should already be locked.
static Boolean _writeXMLFile(CFURLRef url, CFMutableDictionaryRef dict, Boolean isWorldReadable, Boolean *tryAgain) {
    Boolean success = false;
    CFAllocatorRef alloc = __CFPreferencesAllocator();
    *tryAgain = false;
    if (CFDictionaryGetCount(dict) == 0) {
        // Destroy the file
        CFBooleanRef val = CFURLCreatePropertyFromResource(alloc, url, kCFURLFileExists, NULL);
        if (val && CFBooleanGetValue(val)) {
            success = CFURLDestroyResource(url, NULL);
        } else {
            success = true;
        }
        if (val) CFRelease(val);
    } else {
        CFPropertyListFormat desiredFormat = __CFPreferencesShouldWriteXML() ? kCFPropertyListXMLFormat_v1_0 : kCFPropertyListBinaryFormat_v1_0;
        CFWriteStreamRef binStream = CFWriteStreamCreateWithAllocatedBuffers(alloc, alloc);
        CFWriteStreamOpen(binStream);
        CFPropertyListWriteToStream(dict, binStream, desiredFormat, NULL);
        CFWriteStreamClose(binStream);
        CFDataRef data = CFWriteStreamCopyProperty(binStream, kCFStreamPropertyDataWritten);
        CFRelease(binStream);
        if (data) {
            SInt32 mode;
            mode = isWorldReadable ? S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH : S_IRUSR|S_IWUSR;
#if 1 && defined(__MACH__)
	    {	// Try quick atomic way first, then fallback to slower ways and error cases
	    CFStringRef scheme = CFURLCopyScheme(url);
	    if (!scheme) {
		*tryAgain = false;
		CFRelease(data);
		return false;
	    } else if (CFStringCompare(scheme, CFSTR("file"), 0) == kCFCompareEqualTo) {
                SInt32 length = CFDataGetLength(data);
                const void *bytes = (0 == length) ? (const void *)"" : CFDataGetBytePtr(data);
		Boolean atomicWriteSuccess = __CFWriteBytesToFileWithAtomicity(url, bytes, length, mode, true);
		if (atomicWriteSuccess) {
		    CFRelease(scheme);
		    *tryAgain = false;
			CFRelease(data);
		    return true;
		}
		if (!atomicWriteSuccess && thread_errno() == ENOSPC) {
			CFRelease(scheme);
		    *tryAgain = false;
			CFRelease(data);
		    return false;
		}
	    }
	    CFRelease(scheme);
	    }
#endif
            success = CFURLWriteDataAndPropertiesToResource(url, data, URLPropertyDictForPOSIXMode(mode), NULL);
            URLPropertyDictRelease();
            if (success) {
                CFDataRef readData;
                if (!CFURLCreateDataAndPropertiesFromResource(alloc, url, &readData, NULL, NULL, NULL) || !CFEqual(readData, data)) {
                    success = false;
                    *tryAgain = true;
                }
                if (readData) CFRelease(readData);
            } else {
                CFBooleanRef val = CFURLCreatePropertyFromResource(alloc, url, kCFURLFileExists, NULL);
                if (!val || !CFBooleanGetValue(val)) {
                    CFURLRef tmpURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, CFSTR("."), kCFURLPOSIXPathStyle, true, url); // Just "." because url is not a directory URL
                    CFURLRef parentURL = tmpURL ? CFURLCopyAbsoluteURL(tmpURL) : NULL;
                    if (tmpURL) CFRelease(tmpURL);
                    if (val) CFRelease(val);
                    val = CFURLCreatePropertyFromResource(alloc, parentURL, kCFURLFileExists, NULL);
                    if ((!val || !CFBooleanGetValue(val)) && _createDirectory(parentURL, isWorldReadable)) {
                        // parent directory didn't exist; now it does; try again to write
                        success = CFURLWriteDataAndPropertiesToResource(url, data, URLPropertyDictForPOSIXMode(mode), NULL);
                        URLPropertyDictRelease();
                        if (success) {
                            CFDataRef rdData;
                            if (!CFURLCreateDataAndPropertiesFromResource(alloc, url, &rdData, NULL, NULL, NULL) || !CFEqual(rdData, data)) {
                                success = false;
                                *tryAgain = true;
                            }
                            if (rdData) CFRelease(rdData);
                        }
                        
                    }
                    if (parentURL) CFRelease(parentURL);
                }
                if (val) CFRelease(val);
            }
            CFRelease(data);
        } else {
            // ???  This should never happen
            CFLog(__kCFLogAssertion, CFSTR("Could not generate XML data for property list"));
            success = false;
        }
    }
    return success;
}

static void writeXMLValue(CFTypeRef context, void *xmlDomain, CFStringRef key, CFTypeRef value) {
    _CFXMLPreferencesDomain *domain = (_CFXMLPreferencesDomain *)xmlDomain;
    const void *existing = NULL;

    __CFSpinLock(&domain->_lock);
    if (domain->_domainDict == NULL) {
        _loadXMLDomainIfStale((CFURLRef )context, domain);
    }

	// check to see if the value is the same
	// if (1) the key is present AND value is !NULL and equal to existing, do nothing, or
	// if (2) the key is not present AND value is NULL, do nothing
	// these things are no-ops, and should not dirty the domain
    if (CFDictionaryGetValueIfPresent(domain->_domainDict, key, &existing)) {
	if (NULL != value && (existing == value || CFEqual(existing, value))) {
	    __CFSpinUnlock(&domain->_lock);
	    return;
	}
    } else {
	if (NULL == value) {
	    __CFSpinUnlock(&domain->_lock);
	    return;
	}
    }

	// We must append first so key gets another retain (in case we're
	// about to remove it from the dictionary, and that's the sole reference)
    // This should be a set not an array.
    if (!CFArrayContainsValue(domain->_dirtyKeys, CFRangeMake(0, CFArrayGetCount(domain->_dirtyKeys)), key)) {
	CFArrayAppendValue(domain->_dirtyKeys, key);
    }
    if (value) {
        // Must copy for two reasons - we don't want mutable objects in the cache, and we don't want objects allocated from a different allocator in the cache.
        CFTypeRef newValue = CFPropertyListCreateDeepCopy(__CFPreferencesAllocator(), value, kCFPropertyListImmutable);
        CFDictionarySetValue(domain->_domainDict, key, newValue);
        CFRelease(newValue);
    } else {
        CFDictionaryRemoveValue(domain->_domainDict, key);
    }
    __CFSpinUnlock(&domain->_lock);
}

static void getXMLKeysAndValues(CFAllocatorRef alloc, CFTypeRef context, void *xmlDomain, void **buf[], CFIndex *numKeyValuePairs) {
    _CFXMLPreferencesDomain *domain = (_CFXMLPreferencesDomain *)xmlDomain;
    CFIndex count;
    __CFSpinLock(&domain->_lock);
    if (!domain->_domainDict) {
        _loadXMLDomainIfStale((CFURLRef )context, domain);
    }
    count = CFDictionaryGetCount(domain->_domainDict);
    if (buf) {
        void **values;
        if (count <= *numKeyValuePairs) {
            values = *buf + count;
            CFDictionaryGetKeysAndValues(domain->_domainDict, (const void **)*buf, (const void **)values);
        } else if (alloc != kCFAllocatorNull) {
	    *buf = CFAllocatorReallocate(alloc, (*buf ? *buf : NULL), count * 2 * sizeof(void *), 0);
            if (*buf) {
                values = *buf + count;
                CFDictionaryGetKeysAndValues(domain->_domainDict, (const void **)*buf, (const void **)values);
            }
        }
    }
    *numKeyValuePairs = count;
    __CFSpinUnlock(&domain->_lock);
}

static CFDictionaryRef copyXMLDomainDictionary(CFTypeRef context, void *xmlDomain) {
    _CFXMLPreferencesDomain *domain = (_CFXMLPreferencesDomain *)xmlDomain;
    CFDictionaryRef result;
    
    __CFSpinLock(&domain->_lock);
    if(!domain->_domainDict) {
        _loadXMLDomainIfStale((CFURLRef)context, domain);
    }
    
    result = (CFDictionaryRef)CFPropertyListCreateDeepCopy(__CFPreferencesAllocator(), domain->_domainDict, kCFPropertyListImmutable);
    
    __CFSpinUnlock(&domain->_lock);
    return result;
}


static void setXMLDomainIsWorldReadable(CFTypeRef context, void *domain, Boolean isWorldReadable) {
    ((_CFXMLPreferencesDomain *)domain)->_isWorldReadable = isWorldReadable;
}

static Boolean synchronizeXMLDomain(CFTypeRef context, void *xmlDomain) {
    _CFXMLPreferencesDomain *domain = (_CFXMLPreferencesDomain *)xmlDomain;
    CFMutableDictionaryRef cachedDict;
    CFMutableArrayRef changedKeys;
    SInt32 idx,  count;
    Boolean success, tryAgain;
    
    __CFSpinLock(&domain->_lock);
    cachedDict = domain->_domainDict;
    changedKeys = domain->_dirtyKeys;
    count = CFArrayGetCount(changedKeys);
    
    if (count == 0) {
        // no changes were made to this domain; just remove it from the cache to guarantee it will be taken from disk next access
        if (cachedDict) {
            CFRelease(cachedDict);
            domain->_domainDict = NULL;
        }
        __CFSpinUnlock(&domain->_lock);
        return true;
    }

    domain->_domainDict = NULL; // This forces a reload.  Note that we now have a retain on cachedDict
    do {
        _loadXMLDomainIfStale((CFURLRef )context, domain);
        // now cachedDict holds our changes; domain->_domainDict has the latest version from the disk
        for (idx = 0; idx < count; idx ++) {
            CFStringRef key = CFArrayGetValueAtIndex(changedKeys, idx);
            CFTypeRef value = CFDictionaryGetValue(cachedDict, key);
            if (value)
                CFDictionarySetValue(domain->_domainDict, key, value);
            else
                CFDictionaryRemoveValue(domain->_domainDict, key);
        }
        success = _writeXMLFile((CFURLRef )context, domain->_domainDict, domain->_isWorldReadable, &tryAgain);
        if (tryAgain) {
            __CFMilliSleep(((__CFReadTSR() & 0xf) + 1) * 50);
        }
    } while (tryAgain);
    CFRelease(cachedDict);
    if (success) {
	CFArrayRemoveAllValues(domain->_dirtyKeys);
    }
    domain->_lastReadTime = CFAbsoluteTimeGetCurrent();
    __CFSpinUnlock(&domain->_lock);
    return success;
}

#endif /* !defined(__WIN32__) */

