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
/*	CFPriv.h
	Copyright (c) 1998-2007, Apple Inc. All rights reserved.
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
#include <CoreFoundation/CFBundlePriv.h>


#if defined(__MACH__)
#include <CoreFoundation/CFMachPort.h>
#endif

#if defined(__MACH__) || defined(__WIN32__)
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSocket.h>
#endif

#if defined(__MACH__)
#include <CoreFoundation/CFMachPort.h>
#endif

#if 0
#include <shlobj.h>
#endif

CF_EXTERN_C_BEGIN

CF_EXPORT intptr_t _CFDoOperation(intptr_t code, intptr_t subcode1, intptr_t subcode2);

CF_EXPORT void _CFRuntimeSetCFMPresent(void *a);

CF_EXPORT const char *_CFProcessPath(void);
CF_EXPORT const char **_CFGetProcessPath(void);
CF_EXPORT const char **_CFGetProgname(void);


#if defined(__MACH__)
CF_EXPORT CFRunLoopRef CFRunLoopGetMain(void);
CF_EXPORT SInt32 CFRunLoopRunSpecific(CFRunLoopRef rl, CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled);

CF_EXPORT void _CFRunLoopSetCurrent(CFRunLoopRef rl);

CF_EXPORT Boolean _CFRunLoopModeContainsMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef candidateContainedName);
CF_EXPORT void _CFRunLoopAddModeToMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef toModeName);
CF_EXPORT void _CFRunLoopRemoveModeFromMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef fromModeName);
CF_EXPORT void _CFRunLoopStopMode(CFRunLoopRef rl, CFStringRef modeName);

#if defined(__MACH__)
CF_EXPORT CFIndex CFMachPortGetQueuedMessageCount(CFMachPortRef mp);
#endif

CF_EXPORT CFPropertyListRef _CFURLCopyPropertyListRepresentation(CFURLRef url);
CF_EXPORT CFURLRef _CFURLCreateFromPropertyListRepresentation(CFAllocatorRef alloc, CFPropertyListRef pListRepresentation);
#endif
CF_EXPORT CFPropertyListRef _CFURLCopyPropertyListRepresentation(CFURLRef url);
CF_EXPORT CFURLRef _CFURLCreateFromPropertyListRepresentation(CFAllocatorRef alloc, CFPropertyListRef pListRepresentation);

CF_EXPORT void CFPreferencesFlushCaches(void);

#if !__LP64__
#if !defined(__WIN32__)
struct FSSpec;
CF_EXPORT
Boolean _CFGetFSSpecFromURL(CFAllocatorRef alloc, CFURLRef url, struct FSSpec *spec);

CF_EXPORT
CFURLRef _CFCreateURLFromFSSpec(CFAllocatorRef alloc, const struct FSSpec *voidspec, Boolean isDirectory);
#endif
#endif

#if MAC_OS_X_VERSION_10_2 <= MAC_OS_X_VERSION_MAX_ALLOWED
enum {
	kCFURLComponentDecompositionNonHierarchical,
	kCFURLComponentDecompositionRFC1808, /* use this for RFC 1738 decompositions as well */
	kCFURLComponentDecompositionRFC2396
};
typedef CFIndex CFURLComponentDecomposition;

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


/* Extra user notification key for iPhone */
CF_EXPORT
const CFStringRef kCFUserNotificationKeyboardTypesKey AVAILABLE_MAC_OS_X_VERSION_10_5_AND_LATER;


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
enum {
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
};
typedef CFIndex CFSearchPathDirectory;

enum {
    kCFUserDomainMask = 1,	/* user's home directory --- place to install user's personal items (~) */
    kCFLocalDomainMask = 2,	/* local to the current machine --- place to install items available to everyone on this machine (/Local) */
    kCFNetworkDomainMask = 4, 	/* publically available location in the local area network --- place to install items available on the network (/Network) */
    kCFSystemDomainMask = 8,	/* provided by Apple, unmodifiable (/System) */
    kCFAllDomainsMask = 0x0ffff	/* all domains: all of the above and more, future items */
};
typedef CFOptionFlags CFSearchPathDomainMask;

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


/* System Version file access */
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


CF_EXPORT void CFMergeSortArray(void *list, CFIndex count, CFIndex elementSize, CFComparatorFunction comparator, void *context);
CF_EXPORT void CFQSortArray(void *list, CFIndex count, CFIndex elementSize, CFComparatorFunction comparator, void *context);

/* _CFExecutableLinkedOnOrAfter(releaseVersionName) will return YES if the current executable seems to be linked on or after the specified release. Example: If you specify CFSystemVersionPuma (10.1), you will get back true for executables linked on Puma or Jaguar(10.2), but false for those linked on Cheetah (10.0) or any of its software updates (10.0.x). You will also get back false for any app whose version info could not be figured out.
    This function caches its results, so no need to cache at call sites.

  Note that for non-MACH this function always returns true.
*/
enum {
    CFSystemVersionCheetah = 0,         /* 10.0 */
    CFSystemVersionPuma = 1,            /* 10.1 */
    CFSystemVersionJaguar = 2,          /* 10.2 */
    CFSystemVersionPanther = 3,         /* 10.3 */
    CFSystemVersionPinot = 3,           /* Deprecated name for Panther */
    CFSystemVersionTiger = 4,           /* 10.4 */
    CFSystemVersionMerlot = 4,          /* Deprecated name for Tiger */
    CFSystemVersionLeopard = 5,         /* Post-Tiger */
    CFSystemVersionChablis = 5,         /* Deprecated name for Leopard */
    CFSystemVersionMax                  /* This should bump up when new entries are added */
};
typedef CFIndex CFSystemVersion;

CF_EXPORT Boolean _CFExecutableLinkedOnOrAfter(CFSystemVersion version);


enum {
    kCFStringGraphemeCluster = 1, /* Unicode Grapheme Cluster */
    kCFStringComposedCharacterCluster = 2, /* Compose all non-base (including spacing marks) */
    kCFStringCursorMovementCluster = 3, /* Cluster suitable for cursor movements */
    kCFStringBackwardDeletionCluster = 4 /* Cluster suitable for backward deletion */
};
typedef CFIndex CFStringCharacterClusterType;

CF_EXPORT CFRange CFStringGetRangeOfCharacterClusterAtIndex(CFStringRef string, CFIndex charIndex, CFStringCharacterClusterType type);

// Compatibility kCFCompare flags. Use the new public kCFCompareDiacriticInsensitive
enum {
    kCFCompareDiacriticsInsensitive = 128, /* kCFCompareDiacriticInsensitive */
    kCFCompareDiacriticsInsensitiveCompatibilityMask = ((1 << 28)|kCFCompareDiacriticsInsensitive),
};

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
            CFIndex bufLen = desiredRange.length;

            if (bufLen > (buf->bufferedRangeEnd - desiredRange.location)) bufLen = (buf->bufferedRangeEnd - desiredRange.location);

            memmove(outBuf, buf->buffer + (desiredRange.location - buf->bufferedRangeStart), bufLen * sizeof(UniChar));
            outBuf += bufLen; desiredRange.location += bufLen; desiredRange.length -= bufLen;
        } else {
            CFIndex desiredRangeMax = (desiredRange.location + desiredRange.length);

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

/*
 CFCharacterSetInlineBuffer related declarations
 */
/*!
@typedef CFCharacterSetInlineBuffer
 @field cset The character set this inline buffer is initialized with.
 The object is not retained by the structure.
 @field flags The field is a bit mask that carries various settings.
 @field rangeStart The beginning of the character range that contains all members.
 It is guaranteed that there is no member below this value.
 @field rangeLimit The end of the character range that contains all members.
 It is guaranteed that there is no member above and equal to this value.
 @field bitmap The bitmap data representing the membership of the Basic Multilingual Plane characters.
 If NULL, all BMP characters inside the range are members of the character set.
 */
typedef struct {
    CFCharacterSetRef cset;
    uint32_t flags;
    uint32_t rangeStart;
    uint32_t rangeLimit;
    const uint8_t *bitmap;
} CFCharacterSetInlineBuffer;

// Bits for flags field
enum {
    kCFCharacterSetIsCompactBitmap = (1 << 0),
    kCFCharacterSetNoBitmapAvailable = (1 << 1),
    kCFCharacterSetIsInverted = (1 << 2)
};

/*!
@function CFCharacterSetInitInlineBuffer
 Initializes buffer with cset.
 @param cset The character set used to initialized the buffer.
 If this parameter is not a valid CFCharacterSet, the behavior is undefined.
 @param buffer The reference to the inline buffer to be initialized.
 */
CF_EXPORT
void CFCharacterSetInitInlineBuffer(CFCharacterSetRef cset, CFCharacterSetInlineBuffer *buffer);

/*!
@function CFCharacterSetInlineBufferIsLongCharacterMember
 Reports whether or not the UTF-32 character is in the character set.
	@param buffer The reference to the inline buffer to be searched.
	@param character The UTF-32 character for which to test against the
 character set.
 @result true, if the value is in the character set, otherwise false.
 */
#if defined(CF_INLINE)
CF_INLINE bool CFCharacterSetInlineBufferIsLongCharacterMember(CFCharacterSetInlineBuffer *buffer, UTF32Char character) {
    bool isInverted = ((0 == (buffer->flags & kCFCharacterSetIsInverted)) ? false : true);

    if ((character >= buffer->rangeStart) && (character < buffer->rangeLimit)) {
        if ((character > 0xFFFF) || (0 != (buffer->flags & kCFCharacterSetNoBitmapAvailable))) return (CFCharacterSetIsLongCharacterMember(buffer->cset, character) != 0);
        if (NULL == buffer->bitmap) {
            if (0 == (buffer->flags & kCFCharacterSetIsCompactBitmap)) isInverted = !isInverted;
        } else if (0 == (buffer->flags & kCFCharacterSetIsCompactBitmap)) {
            if (buffer->bitmap[character >> 3] & (1 << (character & 7))) isInverted = !isInverted;
        } else {
            uint8_t value = buffer->bitmap[character >> 8];
            
            if (value == 0xFF) {
                isInverted = !isInverted;
            } else if (value > 0) {
                const uint8_t *segment = buffer->bitmap + (256 + (32 * (value - 1)));
                character &= 0xFF;
                if (segment[character >> 3] & (1 << (character % 8))) isInverted = !isInverted;
            }
        }
    }
    return isInverted;
}
#else /* CF_INLINE */
#define CFCharacterSetInlineBufferIsLongCharacterMember(buffer, character) (CFCharacterSetIsLongCharacterMember(buffer->cset, character))
#endif /* CF_INLINE */


#if defined(__MACH__)
#include <CoreFoundation/CFMessagePort.h>

CFMessagePortRef CFMessagePortCreatePerProcessLocal(CFAllocatorRef allocator, CFStringRef name, CFMessagePortCallBack callout, CFMessagePortContext *context, Boolean *shouldFreeInfo);
CFMessagePortRef CFMessagePortCreatePerProcessRemote(CFAllocatorRef allocator, CFStringRef name, CFIndex pid);
#endif


CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFPRIV__ */

