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
/*	CFPriv.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

/*
        APPLE SPI:  NOT TO BE USED OUTSIDE APPLE!
*/

#if !defined(__COREFOUNDATION_CFPRIV__)
#define __COREFOUNDATION_CFPRIV__ 1

#include <string.h>
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>
#if defined(__MACH__)
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFSocket.h>
#endif

#if defined(__cplusplus)
extern "C" {
#endif

CF_EXPORT intptr_t _CFDoOperation(intptr_t code, intptr_t subcode1, intptr_t subcode2);

CF_EXPORT void _CFRuntimeSetCFMPresent(int a);

CF_EXPORT const char *_CFProcessPath(void);


#if defined(__MACH__)
CF_EXPORT CFRunLoopRef CFRunLoopGetMain(void);
CF_EXPORT SInt32 CFRunLoopRunSpecific(CFRunLoopRef rl, CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled);

CF_EXPORT void _CFRunLoopSetCurrent(CFRunLoopRef rl);

CF_EXPORT Boolean _CFRunLoopModeContainsMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef candidateContainedName);
CF_EXPORT void _CFRunLoopAddModeToMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef toModeName);
CF_EXPORT void _CFRunLoopRemoveModeFromMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef fromModeName);
CF_EXPORT void _CFRunLoopStopMode(CFRunLoopRef rl, CFStringRef modeName);

CF_EXPORT CFIndex CFMachPortGetQueuedMessageCount(CFMachPortRef mp);

CF_EXPORT CFPropertyListRef _CFURLCopyPropertyListRepresentation(CFURLRef url);
CF_EXPORT CFURLRef _CFURLCreateFromPropertyListRepresentation(CFAllocatorRef alloc, CFPropertyListRef pListRepresentation);
#endif /* __MACH__ */


#if !defined(__WIN32__)
struct FSSpec;
CF_EXPORT
Boolean _CFGetFSSpecFromURL(CFAllocatorRef alloc, CFURLRef url, struct FSSpec *spec);

CF_EXPORT
CFURLRef _CFCreateURLFromFSSpec(CFAllocatorRef alloc, const struct FSSpec *voidspec, Boolean isDirectory);
#endif

#if MAC_OS_X_VERSION_10_2 <= MAC_OS_X_VERSION_MAX_ALLOWED
typedef enum {
	kCFURLComponentDecompositionNonHierarchical,
	kCFURLComponentDecompositionRFC1808, /* use this for RFC 1738 decompositions as well */
	kCFURLComponentDecompositionRFC2396
} CFURLComponentDecomposition;

typedef struct {
	CFStringRef scheme;
	CFStringRef schemeSpecific;
} CFURLComponentsNonHierarchical;

typedef struct {
	CFStringRef scheme;
	CFStringRef user;
	CFStringRef password;
	CFStringRef host;
	CFIndex port; /* kCFNotFound means ignore/omit */
	CFArrayRef pathComponents;
	CFStringRef parameterString;
	CFStringRef query;
	CFStringRef fragment;
	CFURLRef baseURL;
} CFURLComponentsRFC1808;

typedef struct {
	CFStringRef scheme;

	/* if the registered name form of the net location is used, userinfo is NULL, port is kCFNotFound, and host is the entire registered name. */
	CFStringRef userinfo;
	CFStringRef host;
	CFIndex port;

	CFArrayRef pathComponents;
	CFStringRef query;
	CFStringRef fragment;
	CFURLRef baseURL;
} CFURLComponentsRFC2396;

/* Fills components and returns TRUE if the URL can be decomposed according to decompositionType; FALSE (leaving components unchanged) otherwise.  components should be a pointer to the CFURLComponents struct defined above that matches decompositionStyle */
CF_EXPORT
Boolean _CFURLCopyComponents(CFURLRef url, CFURLComponentDecomposition decompositionType, void *components);

/* Creates and returns the URL described by components; components should point to the CFURLComponents struct defined above that matches decompositionType. */
CF_EXPORT
CFURLRef _CFURLCreateFromComponents(CFAllocatorRef alloc, CFURLComponentDecomposition decompositionType, const void *components);
#define CFURLCopyComponents _CFURLCopyComponents
#define CFURLCreateFromComponents _CFURLCreateFromComponents
#endif


CF_EXPORT Boolean _CFStringGetFileSystemRepresentation(CFStringRef string, UInt8 *buffer, CFIndex maxBufLen);

/* If this is publicized, we might need to create a GetBytesPtr type function as well. */
CF_EXPORT CFStringRef _CFStringCreateWithBytesNoCopy(CFAllocatorRef alloc, const UInt8 *bytes, CFIndex numBytes, CFStringEncoding encoding, Boolean externalFormat, CFAllocatorRef contentsDeallocator);

/* These return NULL on MacOS 8 */
CF_EXPORT
CFStringRef CFGetUserName(void);

CF_EXPORT
CFURLRef CFCopyHomeDirectoryURLForUser(CFStringRef uName);	/* Pass NULL for the current user's home directory */

/*
	CFCopySearchPathForDirectoriesInDomains returns the various
	standard system directories where apps, resources, etc get
	installed. Because queries can return multiple directories,
	you get back a CFArray (which you should free when done) of
	CFStrings. The directories are returned in search path order;
	that is, the first place to look is returned first. This API
	may return directories that do not exist yet. If NSUserDomain
	is included in a query, then the results will contain "~" to
	refer to the user's directory. Specify expandTilde to expand
	this to the current user's home. Some calls might return no
	directories!
	??? On MacOS 8 this function currently returns an empty array.
*/
typedef enum {
    kCFApplicationDirectory = 1,	/* supported applications (Applications) */
    kCFDemoApplicationDirectory,	/* unsupported applications, demonstration versions (Demos) */
    kCFDeveloperApplicationDirectory,	/* developer applications (Developer/Applications) */
    kCFAdminApplicationDirectory,	/* system and network administration applications (Administration) */
    kCFLibraryDirectory, 		/* various user-visible documentation, support, and configuration files, resources (Library) */
    kCFDeveloperDirectory,		/* developer resources (Developer) */
    kCFUserDirectory,			/* user home directories (Users) */
    kCFDocumentationDirectory,		/* documentation (Documentation) */
    kCFDocumentDirectory,		/* documents (Library/Documents) */
    kCFAllApplicationsDirectory = 100,	/* all directories where applications can occur (ie Applications, Demos, Administration, Developer/Applications) */
    kCFAllLibrariesDirectory = 101	/* all directories where resources can occur (Library, Developer) */
} CFSearchPathDirectory;

typedef enum {
    kCFUserDomainMask = 1,	/* user's home directory --- place to install user's personal items (~) */
    kCFLocalDomainMask = 2,	/* local to the current machine --- place to install items available to everyone on this machine (/Local) */
    kCFNetworkDomainMask = 4, 	/* publically available location in the local area network --- place to install items available on the network (/Network) */
    kCFSystemDomainMask = 8,	/* provided by Apple, unmodifiable (/System) */
    kCFAllDomainsMask = 0x0ffff	/* all domains: all of the above and more, future items */
} CFSearchPathDomainMask;

CF_EXPORT
CFArrayRef CFCopySearchPathForDirectoriesInDomains(CFSearchPathDirectory directory, CFSearchPathDomainMask domainMask, Boolean expandTilde);

/* Obsolete keys */
CF_EXPORT const CFStringRef kCFFileURLExists;
CF_EXPORT const CFStringRef kCFFileURLPOSIXMode;
CF_EXPORT const CFStringRef kCFFileURLSize;
CF_EXPORT const CFStringRef kCFFileURLDirectoryContents;
CF_EXPORT const CFStringRef kCFFileURLLastModificationTime;
CF_EXPORT const CFStringRef kCFHTTPURLStatusCode;
CF_EXPORT const CFStringRef kCFHTTPURLStatusLine;


/* System Version file access - the results of these calls are cached, and should be fast after the first call */
CF_EXPORT CFStringRef CFCopySystemVersionString(void);			// Human-readable string containing both marketing and build version, should be API'd
CF_EXPORT CFDictionaryRef _CFCopySystemVersionDictionary(void);
CF_EXPORT CFDictionaryRef _CFCopyServerVersionDictionary(void);
CF_EXPORT const CFStringRef _kCFSystemVersionProductNameKey;
CF_EXPORT const CFStringRef _kCFSystemVersionProductCopyrightKey;
CF_EXPORT const CFStringRef _kCFSystemVersionProductVersionKey;
CF_EXPORT const CFStringRef _kCFSystemVersionProductVersionExtraKey;
CF_EXPORT const CFStringRef _kCFSystemVersionProductUserVisibleVersionKey;	// For loginwindow; see 2987512
CF_EXPORT const CFStringRef _kCFSystemVersionBuildVersionKey;		
CF_EXPORT const CFStringRef _kCFSystemVersionProductVersionStringKey;	// Localized string for the string "Version"
CF_EXPORT const CFStringRef _kCFSystemVersionBuildStringKey;		// Localized string for the string "Build"

typedef enum {
    kCFStringGramphemeCluster = 1, /* Unicode Grapheme Cluster (not different from kCFStringComposedCharacterCluster right now) */
    kCFStringComposedCharacterCluster = 2, /* Compose all non-base (including spacing marks) */
    kCFStringCursorMovementCluster = 3, /* Cluster suitable for cursor movements */
    kCFStringBackwardDeletionCluster = 4 /* Cluster suitable for backward deletion */
} CFStringCharacterClusterType;

CF_EXPORT CFRange CFStringGetRangeOfCharacterClusterAtIndex(CFStringRef string, CFIndex charIndex, CFStringCharacterClusterType type);


/* CFStringEncoding SPI */
/* When set, CF encoding conversion engine keeps ASCII compatibility. (i.e. ASCII backslash <-> Unicode backslash in MacJapanese */
CF_EXPORT void _CFStringEncodingSetForceASCIICompatibility(Boolean flag);

#if defined(CF_INLINE)
CF_INLINE const UniChar *CFStringGetCharactersPtrFromInlineBuffer(CFStringInlineBuffer *buf, CFRange desiredRange) {
    if ((desiredRange.location < 0) || ((desiredRange.location + desiredRange.length) > buf->rangeToBuffer.length)) return NULL;

    if (buf->directBuffer) {
        return buf->directBuffer + buf->rangeToBuffer.location + desiredRange.location;
    } else {
        if (desiredRange.length > __kCFStringInlineBufferLength) return NULL;

        if (((desiredRange.location + desiredRange.length) > buf->bufferedRangeEnd) || (desiredRange.location < buf->bufferedRangeStart)) {
            buf->bufferedRangeStart = desiredRange.location;
            buf->bufferedRangeEnd = buf->bufferedRangeStart + __kCFStringInlineBufferLength;
            if (buf->bufferedRangeEnd > buf->rangeToBuffer.length) buf->bufferedRangeEnd = buf->rangeToBuffer.length;
            CFStringGetCharacters(buf->theString, CFRangeMake(buf->rangeToBuffer.location + buf->bufferedRangeStart, buf->bufferedRangeEnd - buf->bufferedRangeStart), buf->buffer);
        }

        return buf->buffer + (desiredRange.location - buf->bufferedRangeStart);
    }
}

CF_INLINE void CFStringGetCharactersFromInlineBuffer(CFStringInlineBuffer *buf, CFRange desiredRange, UniChar *outBuf) {
    if (buf->directBuffer) {
        memmove(outBuf, buf->directBuffer + buf->rangeToBuffer.location + desiredRange.location, desiredRange.length * sizeof(UniChar));
    } else {
        if ((desiredRange.location >= buf->bufferedRangeStart) && (desiredRange.location < buf->bufferedRangeEnd)) {
            int bufLen = desiredRange.length;

            if (bufLen > (buf->bufferedRangeEnd - desiredRange.location)) bufLen = (buf->bufferedRangeEnd - desiredRange.location);

            memmove(outBuf, buf->buffer + (desiredRange.location - buf->bufferedRangeStart), bufLen * sizeof(UniChar));
            outBuf += bufLen; desiredRange.location += bufLen; desiredRange.length -= bufLen;
        } else {
            int desiredRangeMax = (desiredRange.location + desiredRange.length);

            if ((desiredRangeMax > buf->bufferedRangeStart) && (desiredRangeMax < buf->bufferedRangeEnd)) {
                desiredRange.length = (buf->bufferedRangeStart - desiredRange.location);
                memmove(outBuf + desiredRange.length, buf->buffer, (desiredRangeMax - buf->bufferedRangeStart) * sizeof(UniChar));
            }
        }

        if (desiredRange.length > 0) CFStringGetCharacters(buf->theString, CFRangeMake(buf->rangeToBuffer.location + desiredRange.location, desiredRange.length), outBuf);
    }
}

#else
#define CFStringGetCharactersPtrFromInlineBuffer(buf, desiredRange) ((buf)->directBuffer ? (buf)->directBuffer + (buf)->rangeToBuffer.location + desiredRange.location : NULL)

#define CFStringGetCharactersFromInlineBuffer(buf, desiredRange, outBuf) \
    if (buf->directBuffer) memmove(outBuf, (buf)->directBuffer + (buf)->rangeToBuffer.location + desiredRange.location, desiredRange.length * sizeof(UniChar)); \
    else CFStringGetCharacters((buf)->theString, CFRangeMake((buf)->rangeToBuffer.location + desiredRange.location, desiredRange.length), outBuf);

#endif /* CF_INLINE */

#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_CFPRIV__ */

