/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
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

/*	CFURLPriv.h
	Copyright (c) 2008-2011, Apple Inc. All rights reserved.
 */

#if !defined(__COREFOUNDATION_CFURLPRIV__)
#define __COREFOUNDATION_CFURLPRIV__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFError.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>
#if TARGET_OS_MAC
#include <sys/param.h>
#include <sys/mount.h>
#include <CoreFoundation/CFFileSecurity.h>
#include <CoreFoundation/CFURLEnumerator.h>
#include <CoreFoundation/CFDate.h>
#endif

CF_EXTERN_C_BEGIN

#if TARGET_OS_MAC

#if MAC_OS_X_VERSION_10_5 <= MAC_OS_X_VERSION_MAX_ALLOWED
enum {
    // Resource I/O related errors, with kCFErrorURLKey containing URL
    kCFURLNoSuchResourceError = 4,			   // Attempt to do a file system operation on a non-existent file
    kCFURLResourceLockingError = 255,			   // Couldn't get a lock on file
    kCFURLReadUnknownError = 256,                          // Read error (reason unknown)
    kCFURLReadNoPermissionError = 257,                     // Read error (permission problem)
    kCFURLReadInvalidResourceNameError = 258,              // Read error (invalid file name)
    kCFURLReadCorruptResourceError = 259,                  // Read error (file corrupt, bad format, etc)
    kCFURLReadNoSuchResourceError = 260,                   // Read error (no such file)
    kCFURLReadInapplicableStringEncodingError = 261,       // Read error (string encoding not applicable) also kCFStringEncodingErrorKey
    kCFURLReadUnsupportedSchemeError = 262,		   // Read error (unsupported URL scheme)
    kCFURLReadTooLargeError = 263,			   // Read error (file too large)
    kCFURLReadUnknownStringEncodingError = 264,		   // Read error (string encoding of file contents could not be determined)
    kCFURLWriteUnknownError = 512,			   // Write error (reason unknown)
    kCFURLWriteNoPermissionError = 513,                    // Write error (permission problem)
    kCFURLWriteInvalidResourceNameError = 514,             // Write error (invalid file name)
    kCFURLWriteInapplicableStringEncodingError = 517,      // Write error (string encoding not applicable) also kCFStringEncodingErrorKey
    kCFURLWriteUnsupportedSchemeError = 518,		   // Write error (unsupported URL scheme)
    kCFURLWriteOutOfSpaceError = 640,                      // Write error (out of storage space)
    kCFURLWriteVolumeReadOnlyError = 642,		   // Write error (readonly volume)
};
#endif


enum {
    kCFURLFileReferencePathStyle = 14,
};


/*
    Error user dictionary keys
*/
CF_EXPORT
const CFStringRef kCFURLKeyArrayErrorKey CF_AVAILABLE(10_6, 4_0);


/*
    Private File System Property Keys
*/
CF_EXPORT const CFStringRef _kCFURLPathKey CF_AVAILABLE(10_6, 4_0);
    /* File system path (CFString) */

CF_EXPORT const CFStringRef _kCFURLVolumeIDKey CF_AVAILABLE(10_6, 4_0);
    /* Volume ID (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLInodeNumberKey CF_AVAILABLE(10_6, 4_0);
    /* 64-bit inode number (the inode number from the file system) (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLFileIDKey CF_AVAILABLE(10_6, 4_0);
    /* 64-bit file ID (for tracking a file by ID. This may or may not be the inode number) (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLParentDirectoryIDKey CF_AVAILABLE(10_6, 4_0);
    /* 64-bit file ID (for tracking a parent directory by ID. This may or may not be the inode number) (CFNumber) */

/* OBSOLETE */CF_EXPORT const CFStringRef _kCFURLFilePathKey CF_DEPRECATED(10_0, 10_6, 2_0, 4_0);
    /* _kCFURLFilePathKey was never implemented. Use _kCFURLPathKey instead. */

CF_EXPORT const CFStringRef _kCFURLDistinctLocalizedNameKey CF_AVAILABLE(10_6, 4_0);
    /* The localized name, if it is disnct from the real name. Otherwise, NULL (CFString) */

CF_EXPORT const CFStringRef _kCFURLNameExtensionKey CF_AVAILABLE(10_6, 4_0);
    /* The name extenison (CFString) */

CF_EXPORT const CFStringRef _kCFURLFinderInfoKey CF_AVAILABLE(10_6, 4_0);
    /* A 16-byte Finder Info structure immediately followed by a 16-byte Extended Finder Info structure (CFData) */

CF_EXPORT const CFStringRef _kCFURLIsCompressedKey CF_AVAILABLE(10_6, 4_0);
    /* True if resource's data is transparently compressed by the system on its storage device (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLIsApplicationKey CF_AVAILABLE(10_6, 4_0);
    /* True if resource is an application (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLCanSetHiddenExtensionKey CF_AVAILABLE(10_6, 4_0);
    /* True if the filename extension can be hidden or unhidden (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLIsReadableKey CF_AVAILABLE(10_6, 4_0);
/* OBSOLETE */CF_EXPORT const CFStringRef _kCFURLUserCanReadKey CF_DEPRECATED(10_0, 10_6, 2_0, 4_0);
    /* True if current user can read the resource (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLIsWriteableKey CF_AVAILABLE(10_6, 4_0);
/* OBSOLETE */CF_EXPORT const CFStringRef _kCFURLUserCanWriteKey CF_DEPRECATED(10_0, 10_6, 2_0, 4_0);
    /* True if current user can write to the resource (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLIsExecutableKey CF_AVAILABLE(10_6, 4_0);
/* OBSOLETE */CF_EXPORT const CFStringRef _kCFURLUserCanExecuteKey CF_DEPRECATED(10_0, 10_6, 2_0, 4_0);
    /* True if current user can execute a file resource or search a directory resource (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLParentDirectoryIsVolumeRootKey CF_AVAILABLE(10_6, 4_0);
    /* True if the parent directory is the root of a volume (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLFileSecurityKey CF_AVAILABLE(10_6, 4_0); 
    /* The file's security settings (FSFileSecurity, CarbonCore/Files.h) */

CF_EXPORT const CFStringRef _kCFURLFileSizeOfResourceForkKey CF_AVAILABLE(10_6, 4_0);
    /* Size in bytes of the resource fork (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLFileAllocatedSizeOfResourceForkKey CF_AVAILABLE(10_6, 4_0);
    /* Size in bytes of the blocks allocated for the resource fork (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLEffectiveIconImageDataKey CF_AVAILABLE(10_6, 4_0);
    /* Icon image data, i.e. raw pixel data (CFData) */

CF_EXPORT const CFStringRef _kCFURLCustomIconImageDataKey CF_AVAILABLE(10_6, 4_0);
    /* Icon image data of the item's custom icon, if any (CFData) */

CF_EXPORT const CFStringRef _kCFURLEffectiveIconFlattenedReferenceDataKey CF_AVAILABLE(10_6, 4_0);
    /* Icon flattened reference, suitable for cheaply sharing the effective icon reference across processess (CFData) */

CF_EXPORT const CFStringRef _kCFURLBundleIdentifierKey CF_AVAILABLE(10_6, 4_0);
    /* If resource is a bundle, the bundle identifier (CFString) */

CF_EXPORT const CFStringRef _kCFURLVersionKey CF_AVAILABLE(10_6, 4_0);
    /* If resource is a bundle, the bundle version (CFBundleVersion) as a string (CFString) */

CF_EXPORT const CFStringRef _kCFURLShortVersionStringKey CF_AVAILABLE(10_6, 4_0);
    /* If resource is a bundle, the bundle short version (CFBundleShortVersionString) as a string (CFString) */

CF_EXPORT const CFStringRef _kCFURLOwnerIDKey CF_AVAILABLE(10_6, 4_0);
    /* 32-bit owner ID (uid_t) (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLGroupIDKey CF_AVAILABLE(10_6, 4_0);
    /* 32-bit group ID (gid_t) (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLStatModeKey CF_AVAILABLE(10_6, 4_0);
    /* 32-bit group ID (mode_t) (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLLocalizedNameDictionaryKey CF_AVAILABLE(10_7, NA);
    /* For items with localized display names, the dictionary of all available localizations. The keys are the cannonical locale strings for the available localizations. (CFDictionary) */

CF_EXPORT const CFStringRef _kCFURLLocalizedTypeDescriptionDictionaryKey CF_AVAILABLE(10_7, NA);
    /* The dictionary of all available localizations of the item kind string. The keys are the cannonical locale strings for the available localizations. (CFDictionary) */

CF_EXPORT const CFStringRef _kCFURLApplicationCategoriesKey CF_AVAILABLE(10_7, NA);
    /* The array of category UTI strings associated with the url. (CFArray) */

/* Additional volume properties */

CF_EXPORT const CFStringRef _kCFURLVolumeRefNumKey CF_AVAILABLE(10_6, 4_0);
    /* The Carbon File Manager's FSVolumeRefNum for the resource volume (CFNumber) */

CF_EXPORT const CFStringRef _kCFURLVolumeUUIDStringKey CF_AVAILABLE(10_6, 4_0);
    /* The volume's persistent unique ID as a string, or NULL if not available (CFString) */

CF_EXPORT const CFStringRef _kCFURLVolumeCreationDateKey CF_AVAILABLE(10_6, 4_0);
    /* Value type CFDate */

CF_EXPORT const CFStringRef _kCFURLVolumeIsLocalKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is on a locally connected device -- not a network volume (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsAutomountKey CF_AVAILABLE(10_6, 4_0);
    /* Volume was mounted by the automounter (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeDontBrowseKey CF_AVAILABLE(10_6, 4_0);
    /* Don't browse: generally, not shown to users (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsReadOnlyKey CF_AVAILABLE(10_6, 4_0);
    /* Mounted read-only (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsQuarantinedKey CF_AVAILABLE(10_6, 4_0);
    /* Mounted quarantined (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsEjectableKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is ejectable media (CD, DVD, etc) (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsRemovableKey CF_AVAILABLE(10_6, 4_0);
    /* Volume can be disconnected (USB, FireWire) (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsInternalKey CF_AVAILABLE(10_6, 4_0);
    /* Device is on an internal bus/channel (Note: this has different behavior than the public VolumeIsInternal key) (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsExternalKey CF_AVAILABLE(10_6, 4_0);
    /* Device is on an external bus/channel (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsDiskImageKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is a mounted disk image (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLDiskImageBackingURLKey CF_AVAILABLE(10_6, 4_0);
    /* If volume is a mounted disk image, the URL of the backing disk image (CFURL) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsFileVaultKey CF_AVAILABLE(10_6, 4_0);
    /* Volume uses File Vault encryption (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsiDiskKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is an iDisk (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeiDiskUserNameKey CF_AVAILABLE(10_6, 4_0);
    /* If volume is an iDisk, the base member name of the iDisk owner (CFString) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsLocaliDiskMirrorKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is a local mirror of an iDisk (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsiPodKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is on an iPod (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsCDKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is a CD (audio or CD-ROM). (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsDVDKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is a DVD (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsDeviceFileSystemKey CF_AVAILABLE(10_7, 5_0);
    /* Volume is devfs (CFBoolean) */

CF_EXPORT const CFStringRef _kCFURLVolumeIsHFSStandardKey CF_AVAILABLE(10_6, 4_0);
    /* Volume is HFS standard (which includes AFP volumes). Directory IDs, but not file IDs, can be looked up. (CFBoolean) */

/*  Keys specific to bookmarks at this time */
CF_EXPORT const CFStringRef _kCFURLPropertyKeyFullPathString CF_AVAILABLE(10_6, 4_0);
    /*	the full filesystem path of the item the bookmark was create against */

CF_EXPORT const CFStringRef _kCFURLURLString CF_AVAILABLE(10_6, 4_0);
    /*	the url string ( possibly relative to a base url ) of the url this bookmark was created against. */

CF_EXPORT const CFStringRef _kCFURLFileIDData CF_AVAILABLE(10_6, 4_0);			    
    /*	the url string ( possibly relative to a base url ) of the url this bookmark was created against. */

CF_EXPORT const CFStringRef _kCFURLResolvedFromBookmarkDataKey CF_AVAILABLE(10_6, 4_0);			    
    /*	the bookmark data that was resolved to produce the URL, if any (CFData) */

CF_EXPORT const CFStringRef _kCFURLVolumeMountPointStringKey CF_AVAILABLE(10_6, 4_0);			    
    /*	the volume mountpoint string (Read-only, value type CFString) */

CF_EXPORT const CFStringRef _kCFURLCompleteMountURLKey CF_AVAILABLE(10_6, 4_0);			    
    /* the URL to mount the volume on which the resource is stored, if any (Read-only, value type CFURL) */



/*
    Some common boolean properties can be accessed as a bitfield
    for better performance -- see _CFURLGetResourcePropertyFlags() and
    _CFURLCopyResourcePropertyValuesAndFlags(), below.
 */
enum {
    kCFURLResourceIsRegularFile         = 0x00000001,
    kCFURLResourceIsDirectory           = 0x00000002,
    kCFURLResourceIsSymbolicLink        = 0x00000004,
    kCFURLResourceIsVolume              = 0x00000008,
    kCFURLResourceIsPackage             = 0x00000010,
    kCFURLResourceIsSystemImmutable     = 0x00000020,
    kCFURLResourceIsUserImmutable       = 0x00000040,
    kCFURLResourceIsHidden              = 0x00000080,
    kCFURLResourceHasHiddenExtension    = 0x00000100,
    kCFURLResourceIsApplication         = 0x00000200,
    kCFURLResourceIsCompressed          = 0x00000400,
    /* OBSOLETE */ kCFURLResourceIsSystemCompressed	= 0x00000400,  /* -> kCFURLResourceIsCompressed */
    kCFURLCanSetHiddenExtension         = 0x00000800,
    kCFURLResourceIsReadable		= 0x00001000,
    kCFURLResourceIsWriteable		= 0x00002000,
    kCFURLResourceIsExecutable		= 0x00004000,   /* execute files or search directories */
    kCFURLIsAliasFile                   = 0x00008000,
    kCFURLIsMountTrigger		= 0x00010000,
};
typedef unsigned long long CFURLResourcePropertyFlags;


/*
    _CFURLGetResourceFlags - Returns a bit array of resource flags in the "flags"
    output parameter. Only flags whose corresponding bits are set in the "mask" parameter
    are valid in the output bit array. Returns true on success, false if an error occurs.
    Optional output error: the error is set to a valid CFErrorRef if and only if the function 
    returns false. A valid output error must be released by the caller.
 */
CF_EXPORT
Boolean _CFURLGetResourcePropertyFlags(CFURLRef url, CFURLResourcePropertyFlags mask, CFURLResourcePropertyFlags *flags, CFErrorRef *error) CF_AVAILABLE(10_6, 4_0);


/*
    File resource properties which can be obtained with _CFURLCopyFilePropertyValuesAndFlags().
 */
enum {
    kCFURLName				    = 0x0000000000000001,
    kCFURLLinkCount			    = 0x0000000000000002,
    kCFURLVolumeIdentifier		    = 0x0000000000000004,
    kCFURLObjectIdentifier		    = 0x0000000000000008,
    kCFURLCreationDate			    = 0x0000000000000010,
    kCFURLContentModificationDate	    = 0x0000000000000020,
    kCFURLAttributeModificationDate	    = 0x0000000000000040,
    kCFURLFileSize			    = 0x0000000000000080,
    kCFURLFileAllocatedSize		    = 0x0000000000000100,
    kCFURLFileSizeOfResourceFork	    = 0x0000000000000200,
    kCFURLFileAllocatedSizeOfResourceFork   = 0x0000000000000400,
    kCFURLFinderInfo			    = 0x0000000000000800,
    kCFURLFileSecurity			    = 0x0000000000001000,
};
typedef unsigned long long CFURLFilePropertyBitmap;

/*
    The structure where _CFURLCopyFilePropertyValuesAndFlags() returns file resource properties.
 */
struct _CFURLFilePropertyValues {
    CFStringRef		name;		/* you are responsible for releasing this if you ask for it and get it */
    uint32_t		linkCount;
    uint64_t		volumeIdentifier;
    uint64_t		objectIdentifier;
    CFAbsoluteTime	creationDate;
    CFAbsoluteTime	contentModificationDate;
    CFAbsoluteTime	attributeModificationDate;
    uint64_t		fileSize;
    uint64_t		fileAllocatedSize;
    uint64_t		fileSizeOfResourceFork;
    uint64_t		fileAllocatedSizeOfResourceFork;
    uint8_t		finderInfo[32];
    CFFileSecurityRef	fileSecurity;	/* you are responsible for releasing this if you ask for it and get it */
};
typedef struct _CFURLFilePropertyValues _CFURLFilePropertyValues;

/*
    _CFURLCopyResourcePropertyValuesAndFlags - Returns property values as simple types
    whenever possible. Returns a bit array of resource flags in the "flags"
    output parameter. Only flags whose corresponding bits are set in the "mask" parameter
    are valid in the output bit array. Returns true on success, false if an error occurs.
    Optional output error: the error is set to a valid CFErrorRef if and only if the function 
    returns false. A valid output error must be released by the caller.
 */
CF_EXPORT
Boolean _CFURLCopyResourcePropertyValuesAndFlags( CFURLRef url, CFURLFilePropertyBitmap requestProperties, CFURLFilePropertyBitmap *actualProperties, struct _CFURLFilePropertyValues *properties, CFURLResourcePropertyFlags propertyFlagsMask, CFURLResourcePropertyFlags *propertyFlags, CFErrorRef *error) CF_AVAILABLE(10_7, 4_0);

/*
    Volume property flags
 */
enum {
	kCFURLVolumeIsLocal				=                0x1LL,	// Local device (vs. network device)
	kCFURLVolumeIsAutomount				=                0x2LL,	// Mounted by the automounter
	kCFURLVolumeDontBrowse				=                0x4LL,	// Hidden from user browsing
	kCFURLVolumeIsReadOnly				=                0x8LL,	// Mounted read-only
	kCFURLVolumeIsQuarantined		        =               0x10LL,	// Mounted with quarantine bit
	kCFURLVolumeIsEjectable				=               0x20LL,
	kCFURLVolumeIsRemovable				=               0x40LL,
	kCFURLVolumeIsInternal				=               0x80LL,
	kCFURLVolumeIsExternal				=              0x100LL,
	kCFURLVolumeIsDiskImage				=              0x200LL,
	kCFURLVolumeIsFileVault				=              0x400LL,
	kCFURLVolumeIsLocaliDiskMirror			=              0x800LL,
	kCFURLVolumeIsiPod				=             0x1000LL,
	kCFURLVolumeIsiDisk                             =             0x2000LL,
	kCFURLVolumeIsCD				=             0x4000LL,
	kCFURLVolumeIsDVD				=             0x8000LL,
	kCFURLVolumeIsDeviceFileSystem			=	     0x10000LL,
// IMPORTANT: The values of the following flags must stay in sync with the
// VolumeCapabilities flags in CarbonCore (FileIDTreeStorage.h)
	kCFURLVolumeSupportsPersistentIDs		=        0x100000000LL,
	kCFURLVolumeSupportsSearchFS			=        0x200000000LL,
	kCFURLVolumeSupportsExchange			=        0x400000000LL,
	// reserved						 0x800000000LL,
	kCFURLVolumeSupportsSymbolicLinks		=       0x1000000000LL,
	kCFURLVolumeSupportsDenyModes			=       0x2000000000LL,
	kCFURLVolumeSupportsCopyFile			=       0x4000000000LL,
	kCFURLVolumeSupportsReadDirAttr			=       0x8000000000LL,
	kCFURLVolumeSupportsJournaling			=      0x10000000000LL,
	kCFURLVolumeSupportsRename			=      0x20000000000LL,
	kCFURLVolumeSupportsFastStatFS			=      0x40000000000LL,
	kCFURLVolumeSupportsCaseSensitiveNames		=      0x80000000000LL,
	kCFURLVolumeSupportsCasePreservedNames		=     0x100000000000LL,
	kCFURLVolumeSupportsFLock			=     0x200000000000LL,
	kCFURLVolumeHasNoRootDirectoryTimes		=     0x400000000000LL,
	kCFURLVolumeSupportsExtendedSecurity		=     0x800000000000LL,
	kCFURLVolumeSupports2TBFileSize			=    0x1000000000000LL,
	kCFURLVolumeSupportsHardLinks			=    0x2000000000000LL,
	kCFURLVolumeSupportsMandatoryByteRangeLocks	=    0x4000000000000LL,
	kCFURLVolumeSupportsPathFromID			=    0x8000000000000LL,
	// reserved					    0x10000000000000LL,
	kCFURLVolumeIsJournaling			=   0x20000000000000LL,
	kCFURLVolumeSupportsSparseFiles			=   0x40000000000000LL,
	kCFURLVolumeSupportsZeroRuns			=   0x80000000000000LL,
	kCFURLVolumeSupportsVolumeSizes			=  0x100000000000000LL,
	kCFURLVolumeSupportsRemoteEvents		=  0x200000000000000LL,
	kCFURLVolumeSupportsHiddenFiles			=  0x400000000000000LL,
	kCFURLVolumeSupportsDecmpFSCompression		=  0x800000000000000LL,
	kCFURLVolumeHas64BitObjectIDs			= 0x1000000000000000LL,
	kCFURLVolumePropertyFlagsAll			= 0xffffffffffffffffLL
};
typedef unsigned long long CFURLVolumePropertyFlags;


/*
    _CFURLGetVolumePropertyFlags - Returns a bit array of volume properties.
    Only flags whose corresponding bits are set in the "mask" parameter are valid 
    in the output bit array. Returns true on success, false if an error occurs.
    Optional output error: the error is set to a valid CFErrorRef if and only if the function 
    returns false. A valid output error must be released by the caller.
 */
CF_EXPORT
Boolean _CFURLGetVolumePropertyFlags(CFURLRef url, CFURLVolumePropertyFlags mask, CFURLVolumePropertyFlags *flags, CFErrorRef *error) CF_AVAILABLE(10_6, 4_0);

/*
 _CFURLSetResourcePropertyForKeyAndUpdateFileCache - Works mostly like CFURLSetResourcePropertyForKey
 except that file system properties are updated in the URL's file cache (if it has a valid cache)
 and dependant properties are not flushed. This means that values in the cache may not match what
 is on the file system (see <rdar://problem/8371295> for details).
 
 Only for use by DesktopServices!
 */
CF_EXPORT
Boolean _CFURLSetResourcePropertyForKeyAndUpdateFileCache(CFURLRef url, CFStringRef key, CFTypeRef propertyValue, CFErrorRef *error) CF_AVAILABLE(10_7, NA);

/*
    _CFURLCreateDisplayPathComponentsArray()

    Summary:
	_FileURLCreateDisplayPathComponentsArray creates a CFArray of
	CFURLs for each component in the path leading up to the target
	URL. This routine is suitable for clients who wish to show the
	path leading up to a file system item. NOTE: This routine can be
	I/O intensive, so use it sparingly, and cache the results if
	possible.

    Discussion:
	The CFURLs in the result CFArray are ordered from the target URL
	to the root of the display path. For example, if the target URL
	is file://localhost/System/Library/ the CFURLs in the array will
	be ordered: file://localhost/System/Library/,
	file://localhost/System/, and then file://localhost/

    Parameters:
      
	targetURL:
	    The target URL.

	error:
	    A pointer to a CFErrorRef, or NULL. If error is non-NULL and
	    the function result is NULL, this will be filled in with a
	    CFErrorRef representing the error that occurred.

    Result:
	A CFArray or NULL if an error occurred.
 */
CF_EXPORT
CFArrayRef _CFURLCreateDisplayPathComponentsArray(CFURLRef url, CFErrorRef *error) CF_AVAILABLE(10_7, 4_0);

#if 0
/* Not implemented */
CF_EXPORT
CFURLRef _CFURLCreateFileIDURLFromFSRef(CFAllocatorRef allocator, const struct FSRef *fsRef);
#endif

CF_EXPORT
CFURLRef _CFURLCreateAbsoluteURLWithDirectoryURLAndName(CFAllocatorRef allocator, CFURLRef directoryURL, CFStringRef name) CF_AVAILABLE(10_6, 4_0);

/* Returns true for URLs that locate file system resources. */
CF_EXPORT
Boolean _CFURLIsFileURL(CFURLRef url) CF_AVAILABLE(10_6, 4_0);

/* Returns true for file URLs that use the file reference form */
CF_EXPORT
Boolean _CFURLIsFileReferenceURL(CFURLRef url) CF_AVAILABLE(10_6, 4_0);

/* For use by Core Services */
CF_EXPORT 
void *__CFURLResourceInfoPtr(CFURLRef url) CF_AVAILABLE(10_6, 4_0);

CF_EXPORT 
void __CFURLSetResourceInfoPtr(CFURLRef url, void *ptr) CF_AVAILABLE(10_6, 4_0);

#if TARGET_OS_MAC

CF_EXPORT
CFURLRef _CFURLCreateFileReferenceURLFromIDs( CFAllocatorRef allocator, fsid_t fsid, UInt64 inodeNumber ) CF_AVAILABLE(10_7, NA);

/* _CFURLVolumeIdentifierGetVolumeRefNum is used by LaunchServices */
CF_EXPORT
SInt16 _CFURLVolumeIdentifierGetVolumeRefNum(UInt64 volumeIdentifier) CF_AVAILABLE(10_7, NA);

CF_EXPORT
CFURLRef _CFURLCreateFileReferenceURLFromFSRef(CFAllocatorRef allocator, const struct FSRef *ref) CF_AVAILABLE(10_7, NA);

/* _CFURLGetFSRef is a very lightweight version of CFURLGetFSRef that assumes the CFURL is a file URL and the properties needed to create a FSRef are available in the file URL's cache. This is for use only by CarbonCore's FileOperations SPI and API. */
CF_EXPORT
Boolean _CFURLGetFSRef(CFURLRef url, struct FSRef *fsRef) CF_AVAILABLE(10_7, NA);

/* _CFURLGetObjectInformationNoIO is used by LaunchServices */
CF_EXPORT
Boolean _CFURLGetObjectInformationNoIO(CFURLRef url, UInt64 * volumeIdentifier, UInt64 * objectIdentifier, UInt32 * statMode) CF_AVAILABLE(10_7, NA);

#endif

struct FSCatalogInfo;
struct HFSUniStr255;

/* _CFURLGetCatalogInfo is used by LaunchServices */
CF_EXPORT
SInt32 _CFURLGetCatalogInfo(CFURLRef url, UInt32 whichInfo, struct FSCatalogInfo *catalogInfo, struct HFSUniStr255 *name) CF_AVAILABLE(10_7, 5_0);

/* _CFURLReplaceObject SPI */

/* options for _CFURLReplaceObject */
enum {
//  _CFURLItemReplacementUsingOriginalMetadataOnly  = 1,    // not used
    _CFURLItemReplacementUsingNewMetadataOnly       = 2,
//  _CFURLItemReplacementByMergingMetadata          = 3,    // not used
    _CFURLItemReplacementWithoutDeletingBackupItem  = 1 << 4
};

CF_EXPORT 
Boolean _CFURLReplaceObject( CFAllocatorRef allocator, CFURLRef originalItemURL, CFURLRef newItemURL, CFStringRef newName, CFStringRef backupItemName, CFOptionFlags options, CFURLRef *resultingURL, CFErrorRef *error ) CF_AVAILABLE(10_7, 5_0);


#if (TARGET_OS_MAC) || CF_BUILDING_CF || NSBUILDINGFOUNDATION
CF_EXPORT
CFURLEnumeratorResult _CFURLEnumeratorGetURLsBulk(CFURLEnumeratorRef enumerator, CFIndex maximumURLs, CFIndex *actualURLs, CFURLRef *urls, CFErrorRef *error) CF_AVAILABLE(10_6, 4_0);
#endif

#if TARGET_OS_MAC

enum {
    kCFBookmarkFileCreationWithoutOverwritingExistingFile   = ( 1UL << 8 ), // if destination file already exists don't overwrite it and return an error
    kCFBookmarkFileCreationWithoutAppendingAliasExtension   = ( 1UL << 9 ), // don't add / change whatever extension is on the created alias file
    kCFBookmarkFileCreationWithoutCreatingResourceFork      = ( 1UL << 10 ), // don't create the resource-fork half of the alias file

    kCFURLBookmarkCreationAllowCreationIfResourceDoesNotExistMask = ( 1 << 28 ),	// allow creation of a bookmark to a file: scheme with a CFURLRef of item which may not exist.  If the filesystem item does not exist, the created bookmark contains essentially no properties beyond the url string.

    kCFURLBookmarkCreationDoNotIncludeSandboxExtensionsMask = ( 1 << 29 ),	// If set, sandbox extensions are not included in created bookmarks.  Ordinarily, bookmarks ( except those created suitable for putting into a bookmark file ) will have a sandbox extension added for the item
};

enum {
    kCFBookmarkResolutionPerformRelativeResolutionFirstMask = ( 1 << 10 ), // perform relative resolution before absolute resolution.  If this bit is set, for this to be useful a relative URL must also have been passed in and the bookmark when created must have been created relative to another url.
};

enum {
    kCFURLBookmarkComparisonUnableToCompare = 0x00000000,   /* the two bookmarks could not be compared for some reason */
    kCFURLBookmarkComparisonNoMatch         = 0x00001000,   /* Bookmarks do not refer to the same item */
    kCFURLBookmarkComparisonUnlikelyToMatch = 0x00002000,   /* it is unlikely that the two items refer to the same filesystem item */
    kCFURLBookmarkComparisonLikelyToMatch   = 0x00004000,   /* it is likely that the two items refer to the same filesystem item ( but, they may not ) */
    kCFURLBookmarkComparisonMatch           = 0x00008000,   /* the two items refer to the same item, but other information in the bookmarks may not match */
    kCFURLBookmarkComparisonExactMatch      = 0x0000f000    /* the two bookmarks are identical */
};
typedef CFIndex CFURLBookmarkMatchResult;

/*  Keys specific to bookmarks at this time */
CF_EXPORT const CFStringRef _kCFURLPropertyKeyFullPathString;	    //	the full filesystem path of the item the bookmark was create against
CF_EXPORT const CFStringRef _kCFURLURLString;			    //	the url string ( possibly relative to a base url ) of the url this bookmark was created against.
CF_EXPORT const CFStringRef _kCFURLFileIDData;			    //	the url string ( possibly relative to a base url ) of the url this bookmark was created against.

CFURLBookmarkMatchResult _CFURLCompareBookmarkData( CFDataRef bookmark1Ref, CFDataRef bookmark2Ref, CFURLRef relativeToURL, CFArrayRef* matchingPropertyKeys ) CF_AVAILABLE(10_7, NA);

CF_EXPORT
CFURLBookmarkMatchResult _CFURLBookmarkDataCompare(CFDataRef bookmark1Ref, CFDataRef bookmark2Ref, CFURLRef relativeToURL, CFArrayRef* matchingPropertyKeys) CF_AVAILABLE(10_7, NA); // Use _CFURLCompareBookmarkData() instead

CF_EXPORT
OSStatus _CFURLBookmarkDataToAliasHandle(CFDataRef bookmarkRef, void* aliasHandleP) CF_AVAILABLE(10_7, NA);

#endif

/*
 The following are properties that can be asked of bookmark data objects in addition to the resource properties
 from CFURL itself.
 */

extern const CFStringRef kCFURLBookmarkOriginalPathKey CF_AVAILABLE(10_7, 5_0);
extern const CFStringRef kCFURLBookmarkOriginalRelativePathKey CF_AVAILABLE(10_7, 5_0);
extern const CFStringRef kCFURLBookmarkOriginalRelativePathComponentsArrayKey CF_AVAILABLE(10_7, 5_0);
extern const CFStringRef kCFURLBookmarkOriginalVolumeNameKey CF_AVAILABLE(10_7, 5_0);
extern const CFStringRef kCFURLBookmarkOriginalVolumeCreationDateKey CF_AVAILABLE(10_7, 5_0);

#endif /* TARGET_OS_MAC */

CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFURLPRIV__ */

