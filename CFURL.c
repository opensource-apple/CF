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

/*	CFURL.c
	Copyright (c) 1998-2012, Apple Inc. All rights reserved.
	Responsibility: John Iarocci
*/

#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFPriv.h>
#include <CoreFoundation/CFCharacterSetPriv.h>
#include <CoreFoundation/CFNumber.h>
#include "CFInternal.h"
#include <CoreFoundation/CFStringEncodingConverter.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX
#if DEPLOYMENT_TARGET_MACOSX
#include <CoreFoundation/CFNumberFormatter.h>
#endif
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <CoreFoundation/CFURLPriv.h>
#endif


#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
static CFArrayRef HFSPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFStringRef HFSPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
#endif
static CFArrayRef WindowsPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFStringRef WindowsPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFStringRef POSIXPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDirectory);
CFStringRef CFURLCreateStringWithFileSystemPath(CFAllocatorRef allocator, CFURLRef anURL, CFURLPathStyle fsType, Boolean resolveAgainstBase);
CF_EXPORT CFURLRef _CFURLCreateCurrentDirectoryURL(CFAllocatorRef allocator);
#if DEPLOYMENT_TARGET_MACOSX
static Boolean _CFURLHasFileURLScheme(CFURLRef url, Boolean *hasScheme);
#endif




#ifndef DEBUG_URL_MEMORY_USAGE
#define DEBUG_URL_MEMORY_USAGE 0
#endif

#if DEBUG_URL_MEMORY_USAGE
static UInt32 numURLs = 0;
static UInt32 numDealloced = 0;
static UInt32 numFileURLsCreated = 0;
static UInt32 numExtraDataAllocated = 0;
static UInt32 numURLsWithBaseURL = 0;
static UInt32 numNonUTF8EncodedURLs = 0;
#endif

/* The bit flags in myURL->_flags */
// component bits
#define HAS_SCHEME                      (0x00000001)
#define HAS_USER                        (0x00000002)
#define HAS_PASSWORD                    (0x00000004)
#define HAS_HOST                        (0x00000008)
#define HAS_PORT                        (0x00000010)
#define HAS_PATH                        (0x00000020)
#define HAS_PARAMETERS                  (0x00000040)
#define HAS_QUERY                       (0x00000080)
#define HAS_FRAGMENT                    (0x00000100)
// various boolean flags
#define IS_IPV6_ENCODED                 (0x00000400)
#define IS_DIRECTORY                    (0x00000800)
#define IS_CANONICAL_FILE_URL           (0x00001000)
#define PATH_HAS_FILE_ID                (0x00002000)
#define IS_ABSOLUTE                     (0x00004000)
#define IS_DECOMPOSABLE                 (0x00008000)
#define POSIX_AND_URL_PATHS_MATCH       (0x00010000) // POSIX_AND_URL_PATHS_MATCH will only be true if the URL path and the POSIX path are identical, character for character, except for the presence/absence of a trailing slash on directories
#define ORIGINAL_AND_URL_STRINGS_MATCH  (0x00020000)
#define USES_EIGHTBITSTRINGENCODING     (0x00040000)
// scheme bits and amount to shift it to translate to the kXXXXScheme enums
#define SCHEME_TYPE_MASK                (0xE0000000)
#define SCHEME_SHIFT                    29
enum {
    kHasUncommonScheme  = 0,
    kHasHttpScheme      = 1,
    kHasHttpsScheme     = 2,
    kHasFileScheme      = 3,
    kHasDataScheme      = 4,
    kHasFtpScheme       = 5,
    kMaxScheme
};
// accessors for the scheme bits in _flags
CF_INLINE UInt32 _getSchemeTypeFromFlags(UInt32 flags);
CF_INLINE void _setSchemeTypeInFlags(UInt32 *flags, UInt32 schemeType);

// Other useful defines
#define NET_LOCATION_MASK (HAS_HOST | HAS_USER | HAS_PASSWORD | HAS_PORT)
#define RESOURCE_SPECIFIER_MASK  (HAS_PARAMETERS | HAS_QUERY | HAS_FRAGMENT)
// These flags can be compared for equality since these are all set once when the CFURL is created.
// IS_CANONICAL_FILE_URL cannot be compared since we don't create the URL string.
// POSIX_AND_URL_PATHS_MATCH cannot be compared because it may not be set
// ORIGINAL_AND_URL_STRINGS_MATCH cannot be compared because it gets set on demand later.
#define EQUAL_FLAGS_MASK (HAS_SCHEME | HAS_USER | HAS_PASSWORD | HAS_HOST | HAS_PORT | HAS_PATH | HAS_PARAMETERS | HAS_QUERY | HAS_FRAGMENT | IS_IPV6_ENCODED | IS_DIRECTORY | PATH_HAS_FILE_ID | IS_ABSOLUTE | IS_DECOMPOSABLE | SCHEME_TYPE_MASK )

// The value of FULL_URL_REPRESENTATION must not be in the CFURLPathStyle enums. Also, its value is exposed via _CFURLCopyPropertyListRepresentation to the Finder so don't change it.  
#define FULL_URL_REPRESENTATION (0xF)

/* The bit flags in _CFURLAdditionalData->_additionalDataFlags */
/* If ORIGINAL_AND_URL_STRINGS_MATCH in myURL->_flags is false, these bits determine where they differ. XXXX_DIFFERS must match the HAS_XXXX */
#define SCHEME_DIFFERS                  HAS_SCHEME      // Scheme can actually never differ because if there were escaped characters prior to the colon, we'd interpret the string as a relative path
#define USER_DIFFERS                    HAS_USER
#define PASSWORD_DIFFERS                HAS_PASSWORD
#define HOST_DIFFERS                    HAS_HOST
#define PORT_DIFFERS                    HAS_PORT        // Port can actually never differ because if there were a non-digit following a colon in the net location, we'd interpret the whole net location as the host 
#define PATH_DIFFERS                    HAS_PATH        // unused
#define PARAMETERS_DIFFER               HAS_PARAMETERS  // unused
#define QUERY_DIFFER                    HAS_QUERY       // unused
#define FRAGMENT_DIFFER                 HAS_FRAGMENT    // unused

#define FILE_ID_PREFIX ".file"
#define FILE_ID_KEY "id"
#define FILE_ID_PREAMBLE "/.file/id="
#define FILE_ID_PREAMBLE_LENGTH 10

#define FILE_PREFIX "file://"
#define FILE_PREFIX_WITH_AUTHORITY "file://localhost"
static const UInt8 fileURLPrefixWithAuthority[] = FILE_PREFIX_WITH_AUTHORITY;

//	In order to reduce the sizeof ( __CFURL ), move these items into a seperate structure which is
//	only allocated when necessary.  In my tests, it's almost never needed -- very rarely does a CFURL have
//	either a sanitized string or a reserved pointer for URLHandle.
struct _CFURLAdditionalData {
    void *_reserved; // Reserved for URLHandle's use.
    CFStringRef _sanitizedString; // The fully compliant RFC string.  This is only non-NULL if ORIGINAL_AND_URL_STRINGS_MATCH is false.
    UInt32 _additionalDataFlags; // these flags only apply to things we need to keep state for in _CFURLAdditionalData (like the XXXX_DIFFERS flags)
};

struct __CFURL {
    CFRuntimeBase _cfBase;
    UInt32 _flags;
    CFStringEncoding _encoding; // The encoding to use when asked to remove percent escapes
    CFStringRef _string; // Never NULL
    CFURLRef _base;
    CFRange *_ranges;
    struct _CFURLAdditionalData* _extra;
    void *_resourceInfo;    // For use by CarbonCore to cache property values. Retained and released by CFURL.
};


CF_INLINE void* _getReserved ( const struct __CFURL* url )
{
    if ( url && url->_extra ) {
        return ( url->_extra->_reserved );
    }
    else {
        return ( NULL );
    }
}

CF_INLINE CFStringRef _getSanitizedString(const struct __CFURL* url)
{
    if ( url && url->_extra ) {
        return ( url->_extra->_sanitizedString );
    }
    else {
	return ( NULL );
    }
}

CF_INLINE UInt32 _getAdditionalDataFlags(const struct __CFURL* url)
{
    if ( url && url->_extra ) {
        return ( url->_extra->_additionalDataFlags );
    }
    else {
	return ( 0 );
    }
}

CF_INLINE void* _getResourceInfo ( const struct __CFURL* url )
{
    if ( url ) {
        return url->_resourceInfo;
    }
    else {
        return NULL;
    }
}

static void _CFURLAllocateExtraDataspace( struct __CFURL* url )
{	
    if ( url && ! url->_extra )
    {	struct _CFURLAdditionalData* extra = (struct _CFURLAdditionalData*) CFAllocatorAllocate( CFGetAllocator( url), sizeof( struct _CFURLAdditionalData ), __kCFAllocatorGCScannedMemory);
	
	extra->_reserved = _getReserved( url );
        extra->_additionalDataFlags = _getAdditionalDataFlags(url);
	extra->_sanitizedString = _getSanitizedString(url);
	
	url->_extra = extra;
	
	#if DEBUG_URL_MEMORY_USAGE
	numExtraDataAllocated ++;
	#endif
    }
}

CF_INLINE void _setReserved ( struct __CFURL* url, void* reserved )
{
    if ( url )
    {
        // Don't allocate extra space if we're just going to be storing NULL
        if ( !url->_extra && reserved )
            _CFURLAllocateExtraDataspace( url );
        
        if ( url->_extra )
            __CFAssignWithWriteBarrier((void **)&url->_extra->_reserved, reserved);
    }
}

CF_INLINE void _setSanitizedString( struct __CFURL* url, CFMutableStringRef sanitizedString )
{
    if ( url )
    {
        // Don't allocate extra space if we're just going to be storing NULL
        if ( !url->_extra && sanitizedString ) {
            _CFURLAllocateExtraDataspace( url );
        }
        
        if ( url->_extra ) {
            if ( url->_extra->_sanitizedString ) {
                CFRelease(url->_extra->_sanitizedString);
            }
            url->_extra->_sanitizedString = CFStringCreateCopy(CFGetAllocator(url), sanitizedString);

        }
    }
}

CF_INLINE void _setAdditionalDataFlags(struct __CFURL* url, UInt32 additionalDataFlags)
{
    if ( url )
    {
        // Don't allocate extra space if we're just going to be storing 0
        if ( !url->_extra && (additionalDataFlags != 0) ) {
            _CFURLAllocateExtraDataspace( url );
        }
        
        if ( url->_extra ) {
            url->_extra->_additionalDataFlags = additionalDataFlags;
        }
    }
}

CF_INLINE void _setResourceInfo ( struct __CFURL* url, void* resourceInfo )
{
    // Must be atomic
    // Never a GC object
    if ( url && OSAtomicCompareAndSwapPtrBarrier( NULL, resourceInfo, &url->_resourceInfo )) {
	CFRetain( resourceInfo );
    }
}

CF_INLINE UInt32 _getSchemeTypeFromFlags(UInt32 flags)
{
    return ( (flags & SCHEME_TYPE_MASK) >> SCHEME_SHIFT );
}

CF_INLINE void _setSchemeTypeInFlags(UInt32 *flags, UInt32 schemeType)
{
    CFAssert2((schemeType >= kHasUncommonScheme) &&  (schemeType < kMaxScheme), __kCFLogAssertion, "%s(): Received bad schemeType %d", __PRETTY_FUNCTION__, schemeType);
    *flags = (*flags & ~SCHEME_TYPE_MASK) + (schemeType << SCHEME_SHIFT);
}

/* Returns whether the provided bytes can be stored in ASCII
 */
static Boolean __CFBytesInASCII(const uint8_t *bytes, CFIndex len) {
#if __LP64__
    /* A bit of unrolling; go by 32s, 16s, and 8s first */
    while (len >= 32) {
        uint64_t val = *(const uint64_t *)bytes;
        uint64_t hiBits = (val & 0x8080808080808080ULL);    // More efficient to collect this rather than do a conditional at every step
        bytes += 8;
        val = *(const uint64_t *)bytes;
        hiBits |= (val & 0x8080808080808080ULL);
        bytes += 8;
        val = *(const uint64_t *)bytes;
        hiBits |= (val & 0x8080808080808080ULL);
        bytes += 8;
        val = *(const uint64_t *)bytes;
        if (hiBits | (val & 0x8080808080808080ULL)) return false;
        bytes += 8;
        len -= 32;
    }
    
    while (len >= 16) {
        uint64_t val = *(const uint64_t *)bytes;
        uint64_t hiBits = (val & 0x8080808080808080ULL);
        bytes += 8;
        val = *(const uint64_t *)bytes;
        if (hiBits | (val & 0x8080808080808080ULL)) return false;
        bytes += 8;
        len -= 16;
    }
    
    while (len >= 8) {
        uint64_t val = *(const uint64_t *)bytes;
        if (val & 0x8080808080808080ULL) return false;
        bytes += 8;
        len -= 8;
    }
#endif
    /* Go by 4s */
    while (len >= 4) {
        uint32_t val = *(const uint32_t *)bytes;
        if (val & 0x80808080U) return false;
        bytes += 4;
        len -= 4;
    }
    /* Handle the rest one byte at a time */
    while (len--) {
        if (*bytes++ & 0x80) return false;
    }
    
    return true;
}

static Boolean _pathHasFileIDPrefix(CFStringRef path);
static void _convertToURLRepresentation(struct __CFURL *url, UInt32 fsType);
static CFStringRef _resolveFileSystemPaths(CFStringRef relativePath, CFStringRef basePath, Boolean baseIsDir, CFURLPathStyle fsType, CFAllocatorRef alloc);
static void _parseComponents(CFAllocatorRef alloc, CFStringRef string, CFURLRef base, UInt32 *flags, CFRange **range);
static CFRange _rangeForComponent(UInt32 flags, CFRange *ranges, UInt32 compFlag);
static CFRange _netLocationRange(UInt32 flags, CFRange *ranges);
static UInt32 _firstResourceSpecifierFlag(UInt32 flags);
static void computeSanitizedString(CFURLRef url);
static CFStringRef correctedComponent(CFStringRef component, UInt32 compFlag, CFStringEncoding enc);
static CFMutableStringRef resolveAbsoluteURLString(CFAllocatorRef alloc, CFStringRef relString, UInt32 relFlags, CFRange *relRanges, CFStringRef baseString, UInt32 baseFlags, CFRange *baseRanges);
static CFStringRef _resolvedPath(UniChar *pathStr, UniChar *end, UniChar pathDelimiter, Boolean stripLeadingDotDots, Boolean stripTrailingDelimiter, CFAllocatorRef alloc);


CF_INLINE void _parseComponentsOfURL(CFURLRef url) {
    _parseComponents(CFGetAllocator(url), url->_string, url->_base, &(((struct __CFURL *)url)->_flags), &(((struct __CFURL *)url)->_ranges));
}

enum {
	VALID = 1,
	UNRESERVED = 2,
	PATHVALID = 4,
	SCHEME = 8,
	HEXDIGIT = 16
};

static const unsigned char sURLValidCharacters[128] = {
    /* nul   0 */   0,
    /* soh   1 */   0,
    /* stx   2 */   0,
    /* etx   3 */   0,
    /* eot   4 */   0,
    /* enq   5 */   0,
    /* ack   6 */   0,
    /* bel   7 */   0,
    /* bs    8 */   0,
    /* ht    9 */   0,
    /* nl   10 */   0,
    /* vt   11 */   0,
    /* np   12 */   0,
    /* cr   13 */   0,
    /* so   14 */   0,
    /* si   15 */   0,
    /* dle  16 */   0,
    /* dc1  17 */   0,
    /* dc2  18 */   0,
    /* dc3  19 */   0,
    /* dc4  20 */   0,
    /* nak  21 */   0,
    /* syn  22 */   0,
    /* etb  23 */   0,
    /* can  24 */   0,
    /* em   25 */   0,
    /* sub  26 */   0,
    /* esc  27 */   0,
    /* fs   28 */   0,
    /* gs   29 */   0,
    /* rs   30 */   0,
    /* us   31 */   0,
    /* sp   32 */   0,
    /* '!'  33 */   VALID | UNRESERVED | PATHVALID ,
    /* '"'  34 */   0,
    /* '#'  35 */   0,
    /* '$'  36 */   VALID | PATHVALID ,
    /* '%'  37 */   0,
    /* '&'  38 */   VALID | PATHVALID ,
    /* '''  39 */   VALID | UNRESERVED | PATHVALID ,
    /* '('  40 */   VALID | UNRESERVED | PATHVALID ,
    /* ')'  41 */   VALID | UNRESERVED | PATHVALID ,
    /* '*'  42 */   VALID | UNRESERVED | PATHVALID ,
    /* '+'  43 */   VALID | SCHEME | PATHVALID ,
    /* ','  44 */   VALID | PATHVALID ,
    /* '-'  45 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* '.'  46 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* '/'  47 */   VALID | PATHVALID ,
    /* '0'  48 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '1'  49 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '2'  50 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '3'  51 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '4'  52 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '5'  53 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '6'  54 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '7'  55 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '8'  56 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* '9'  57 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* ':'  58 */   VALID ,
    /* ';'  59 */   VALID ,
    /* '<'  60 */   0,
    /* '='  61 */   VALID | PATHVALID ,
    /* '>'  62 */   0,
    /* '?'  63 */   VALID ,
    /* '@'  64 */   VALID ,
    /* 'A'  65 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'B'  66 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'C'  67 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'D'  68 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'E'  69 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'F'  70 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'G'  71 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'H'  72 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'I'  73 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'J'  74 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'K'  75 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'L'  76 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'M'  77 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'N'  78 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'O'  79 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'P'  80 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'Q'  81 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'R'  82 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'S'  83 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'T'  84 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'U'  85 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'V'  86 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'W'  87 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'X'  88 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'Y'  89 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'Z'  90 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* '['  91 */   0,
    /* '\'  92 */   0,
    /* ']'  93 */   0,
    /* '^'  94 */   0,
    /* '_'  95 */   VALID | UNRESERVED | PATHVALID ,
    /* '`'  96 */   0,
    /* 'a'  97 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'b'  98 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'c'  99 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'd' 100 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'e' 101 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'f' 102 */   VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT ,
    /* 'g' 103 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'h' 104 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'i' 105 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'j' 106 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'k' 107 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'l' 108 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'm' 109 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'n' 110 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'o' 111 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'p' 112 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'q' 113 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'r' 114 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 's' 115 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 't' 116 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'u' 117 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'v' 118 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'w' 119 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'x' 120 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'y' 121 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* 'z' 122 */   VALID | UNRESERVED | SCHEME | PATHVALID ,
    /* '{' 123 */   0,
    /* '|' 124 */   0,
    /* '}' 125 */   0,
    /* '~' 126 */   VALID | UNRESERVED | PATHVALID ,
    /* del 127 */   0,
};

CF_INLINE Boolean isURLLegalCharacter(UniChar ch) {
    return (ch <= 127) ? (sURLValidCharacters[ch] & VALID) : false;
}

CF_INLINE Boolean scheme_valid(UniChar ch) {
    return (ch <= 127) ? (sURLValidCharacters[ch] & SCHEME) : false;
}

// "Unreserved" as defined by RFC 2396
CF_INLINE Boolean isUnreservedCharacter(UniChar ch) {
    return (ch <= 127) ? (sURLValidCharacters[ch] & UNRESERVED) : false;
}

CF_INLINE Boolean isPathLegalCharacter(UniChar ch) {
    return (ch <= 127) ? (sURLValidCharacters[ch] & PATHVALID) : false;
}

CF_INLINE Boolean isHexDigit(UniChar ch) {
    return (ch <= 127) ? (sURLValidCharacters[ch] & HEXDIGIT) : false;
}

// Returns false if ch1 or ch2 isn't properly formatted
CF_INLINE Boolean _translateBytes(UniChar ch1, UniChar ch2, uint8_t *result) {
    *result = 0;
    if (ch1 >= '0' && ch1 <= '9') *result += (ch1 - '0');
    else if (ch1 >= 'a' && ch1 <= 'f') *result += 10 + ch1 - 'a';
    else if (ch1 >= 'A' && ch1 <= 'F') *result += 10 + ch1 - 'A';
    else return false;

    *result  = (*result) << 4;
    if (ch2 >= '0' && ch2 <= '9') *result += (ch2 - '0');
    else if (ch2 >= 'a' && ch2 <= 'f') *result += 10 + ch2 - 'a';
    else if (ch2 >= 'A' && ch2 <= 'F') *result += 10 + ch2 - 'A';
    else return false;

    return true;
}

CF_INLINE Boolean _haveTestedOriginalString(CFURLRef url) {
    return ((url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) != 0) || (_getSanitizedString(url) != NULL);
}

/*
 CreateStringFromFileSystemRepresentationByAddingPercentEscapes creates a CFString
 for the path-absolute form of a URI path component from the native file system representation.
 
 The rules for path-absolute from rfc3986 are:
 path-absolute = "/" [ segment-nz *( "/" segment ) ]
 segment       = *pchar
 segment-nz    = 1*pchar
 pchar         = unreserved / pct-encoded / sub-delims / ":" / "@"
 pct-encoded   = "%" HEXDIG HEXDIG
 unreserved    = ALPHA / DIGIT / "-" / "." / "_" / "~"
 sub-delims    = "!" / "$" / "&" / "'" / "(" / ")" / "*" / "+" / "," / ";" / "="
 */
static CFStringRef CreateStringFromFileSystemRepresentationByAddingPercentEscapes(CFAllocatorRef alloc, const UInt8 *bytes, CFIndex numBytes, Boolean windowsPath)
{
    static const UInt8 hexchars[] = "0123456789ABCDEF";
    STACK_BUFFER_DECL(UInt8, stackBuf, PATH_MAX * 3);   // worst case is every byte needs to be percent-escaped
    UInt8 *bufStartPtr;
    UInt8 *bufBytePtr;
    const UInt8 *bytePtr = bytes;
    CFIndex idx;
    CFStringRef result;
   
    // choose a buffer to percent-escape into.
    if ( numBytes <= PATH_MAX ) {
        bufStartPtr = &stackBuf[0];
    }
    else {
        // worst case is every byte needs to be percent-escaped (numBytes * 3)
        bufStartPtr = (UInt8 *)malloc(numBytes * 3);
    }
    
    if ( bufStartPtr != NULL ) {
        bufBytePtr = bufStartPtr;
        for ( idx = 0; (idx < numBytes) && (*bytePtr != 0); ++idx ) {
            switch ( *bytePtr ) {
                    // these are the visible 7-bit ascii characters that are not legal pchar octets
                case '"':
                case '#':
                case '%':
                case ';':	// we need to percent-escape ';' in file system paths so it won't be mistaken for the start of the obsolete param rule (rfc2396) that CFURL still supports
                case '<':
                case '>':
                case '?':	// we need to percent-escape '?' in file system paths so it won't be mistaken for the start of a query
                case '[':
                case '\\':
                case ']':
                case '^':
                case '`':
                case '{':
                case '|':
                case '}':
                    // percent-escape non-pchar octets spread throughout the visible 7-bit ascii range
                    *bufBytePtr++ = '%';
                    *bufBytePtr++ = hexchars[*bytePtr >> 4];
                    *bufBytePtr++ = hexchars[*bytePtr & 0x0f];
                    break;
                default:
                    if ( (*bytePtr <= ' ') ||	// percent-escape non-pchar octets that are space or less (control characters)
                        (*bytePtr >= 0x7f) ||	// percent-escape non-pchar octets that del and 8-bit ascii with the high bit set
                        (windowsPath && (*bytePtr == '/')) ) {	// percent-escape the forward slash if this is a windowsPath
                        *bufBytePtr++ = '%';
                        *bufBytePtr++ = hexchars[*bytePtr >> 4];
                        *bufBytePtr++ = hexchars[*bytePtr & 0x0f];
                    }
                    else {
                        // copy everything else
                        *bufBytePtr++ = *bytePtr;
                    }
                    break;
            }
            ++bytePtr;
        }
        
        // did we convert numBytes?
        if ( idx == numBytes ) {
            // create the result
            result = CFStringCreateWithBytes(alloc, bufStartPtr, (CFIndex)(bufBytePtr-bufStartPtr), kCFStringEncodingUTF8, FALSE);
        }
        else {
            // no, but it's OK if the remaining bytes are all nul (embedded nul bytes are not allowed)
            for ( /* start where we left off */; (idx < numBytes) && (bufStartPtr[idx] == 0); ++idx ) {
                // do nothing
            }
            if ( idx == numBytes ) {
                // create the result
                result = CFStringCreateWithBytes(alloc, bufStartPtr, (CFIndex)(bufBytePtr-bufStartPtr), kCFStringEncodingUTF8, FALSE);
            }
            else {
                // the remaining bytes were not all nul
                result = NULL;
            }
        }
        
        // free the buffer if we malloc'd it
        if ( bufStartPtr != &stackBuf[0] ) {
            free(bufStartPtr);
        }
    }
    else {
        result = NULL;
    }
    return ( result );
}

// Returns NULL if str cannot be converted for whatever reason, str if str contains no characters in need of escaping, or a newly-created string with the appropriate % escape codes in place.  Caller must always release the returned string.
CF_INLINE CFStringRef _replacePathIllegalCharacters(CFStringRef str, CFAllocatorRef alloc, Boolean preserveSlashes) {
    CFStringRef result = NULL;
    STACK_BUFFER_DECL(char, buffer, PATH_MAX);
    if ( CFStringGetCString(str, buffer, PATH_MAX, kCFStringEncodingUTF8) ) {
        result = CreateStringFromFileSystemRepresentationByAddingPercentEscapes(kCFAllocatorDefault, (const UInt8 *)buffer, strlen(buffer), !preserveSlashes);
    }
    return result;
}

// We have 2 UniChars of a surrogate; we must convert to the correct percent-encoded UTF8 string and append to str.  Added so that file system URLs can always be converted from POSIX to full URL representation.  -- REW, 8/20/2001
static Boolean _hackToConvertSurrogates(UniChar highChar, UniChar lowChar, CFMutableStringRef str) {
    UniChar surrogate[2];
    uint8_t bytes[6]; // Aki sez it should never take more than 6 bytes
    CFIndex len; 
    uint8_t *currByte;
    surrogate[0] = highChar;
    surrogate[1] = lowChar;
    if (CFStringEncodingUnicodeToBytes(kCFStringEncodingUTF8, 0, surrogate, 2, NULL, bytes, 6, &len) != kCFStringEncodingConversionSuccess) {
        return false;
    }
    for (currByte = bytes; currByte < bytes + len; currByte ++) {
        UniChar escapeSequence[3] = {'%', '\0', '\0'};
        unsigned char high, low;
        high = ((*currByte) & 0xf0) >> 4;
        low = (*currByte) & 0x0f;
        escapeSequence[1] = (high < 10) ? '0' + high : 'A' + high - 10;
        escapeSequence[2] = (low < 10) ? '0' + low : 'A' + low - 10;
        CFStringAppendCharacters(str, escapeSequence, 3);
    }
    return true;
}

static Boolean _appendPercentEscapesForCharacter(UniChar ch, CFStringEncoding encoding, CFMutableStringRef str) {
    uint8_t bytes[6]; // 6 bytes is the maximum a single character could require in UTF8 (most common case); other encodings could require more
    uint8_t *bytePtr = bytes, *currByte;
    CFIndex byteLength;
    CFAllocatorRef alloc = NULL;
    if (CFStringEncodingUnicodeToBytes(encoding, 0, &ch, 1, NULL, bytePtr, 6, &byteLength) != kCFStringEncodingConversionSuccess) {
        byteLength = CFStringEncodingByteLengthForCharacters(encoding, 0, &ch, 1);
        if (byteLength <= 6) {
            // The encoding cannot accomodate the character
            return false;
        }
        alloc = CFGetAllocator(str);
        bytePtr = (uint8_t *)CFAllocatorAllocate(alloc, byteLength, 0);
        if (!bytePtr || CFStringEncodingUnicodeToBytes(encoding, 0, &ch, 1, NULL, bytePtr, byteLength, &byteLength) != kCFStringEncodingConversionSuccess) {
            if (bytePtr) CFAllocatorDeallocate(alloc, bytePtr);
            return false;
        }
    }
    for (currByte = bytePtr; currByte < bytePtr + byteLength; currByte ++) {
        UniChar escapeSequence[3] = {'%', '\0', '\0'};
        unsigned char high, low;
        high = ((*currByte) & 0xf0) >> 4;
        low = (*currByte) & 0x0f;
        escapeSequence[1] = (high < 10) ? '0' + high : 'A' + high - 10;
        escapeSequence[2] = (low < 10) ? '0' + low : 'A' + low - 10;
        CFStringAppendCharacters(str, escapeSequence, 3);
    }
    if (bytePtr != bytes) {
        CFAllocatorDeallocate(alloc, bytePtr);
    }
    return true;
}

// Uses UTF-8 to translate all percent escape sequences; returns NULL if it encounters a format failure.  May return the original string.
CFStringRef  CFURLCreateStringByReplacingPercentEscapes(CFAllocatorRef alloc, CFStringRef  originalString, CFStringRef  charactersToLeaveEscaped) {
    CFMutableStringRef newStr = NULL;
    CFIndex length;
    CFIndex mark = 0;
    CFRange percentRange, searchRange;
    CFStringRef escapedStr = NULL;
    CFMutableStringRef strForEscapedChar = NULL;
    UniChar escapedChar;
    Boolean escapeAll = (charactersToLeaveEscaped && CFStringGetLength(charactersToLeaveEscaped) == 0);
    Boolean failed = false;
    
    if (!originalString) return NULL;

    if (charactersToLeaveEscaped == NULL) {
        return (CFStringRef)CFStringCreateCopy(alloc, originalString);
    }

    length = CFStringGetLength(originalString);
    searchRange = CFRangeMake(0, length);

    while (!failed && CFStringFindWithOptions(originalString, CFSTR("%"), searchRange, 0, &percentRange)) {
        uint8_t bytes[4]; // Single UTF-8 character could require up to 4 bytes.
        uint8_t numBytesExpected;
        UniChar ch1, ch2;

        escapedStr = NULL;
        // Make sure we have at least 2 more characters
        if (length - percentRange.location < 3) { failed = true; break; }

        // if we don't have at least 2 more characters, we can't interpret the percent escape code,
        // so we assume the percent character is legit, and let it pass into the string
        ch1 = CFStringGetCharacterAtIndex(originalString, percentRange.location+1);
        ch2 = CFStringGetCharacterAtIndex(originalString, percentRange.location+2);
        if (!_translateBytes(ch1, ch2, bytes)) { failed = true;  break; }
        if (!(bytes[0] & 0x80)) {
            numBytesExpected = 1;
        } else if (!(bytes[0] & 0x20)) {
            numBytesExpected = 2;
        } else if (!(bytes[0] & 0x10)) {
            numBytesExpected = 3;
        } else {
            numBytesExpected = 4;
        }
        if (numBytesExpected == 1) {
            // one byte sequence (most common case); handle this specially
            escapedChar = bytes[0];
            if (!strForEscapedChar) {
                strForEscapedChar = CFStringCreateMutableWithExternalCharactersNoCopy(alloc, &escapedChar, 1, 1, kCFAllocatorNull);
            }
            escapedStr = (CFStringRef)CFRetain(strForEscapedChar);
        } else {
            CFIndex j;
            // Make sure up front that we have enough characters
            if (length < percentRange.location + numBytesExpected * 3) { failed = true; break; }
            for (j = 1; j < numBytesExpected; j ++) {
                if (CFStringGetCharacterAtIndex(originalString, percentRange.location + 3*j) != '%') { failed = true; break; }
                ch1 = CFStringGetCharacterAtIndex(originalString, percentRange.location + 3*j + 1);
                ch2 = CFStringGetCharacterAtIndex(originalString, percentRange.location + 3*j + 2);
                if (!_translateBytes(ch1, ch2, bytes+j)) { failed = true; break; }
            }

            // !!! We should do the low-level bit-twiddling ourselves; this is expensive!  REW, 6/10/99
            escapedStr = CFStringCreateWithBytes(alloc, bytes, numBytesExpected, kCFStringEncodingUTF8, false);
            if (!escapedStr) {
                failed = true;
            } else if (CFStringGetLength(escapedStr) == 0 && numBytesExpected == 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
                // Somehow, the UCS-2 BOM got translated in to a UTF8 string
                escapedChar = 0xfeff;
                if (!strForEscapedChar) {
                    strForEscapedChar = CFStringCreateMutableWithExternalCharactersNoCopy(alloc, &escapedChar, 1, 1, kCFAllocatorNull);
                }
                CFRelease(escapedStr);
                escapedStr = (CFStringRef)CFRetain(strForEscapedChar);
            }
            if (failed) break;
        }

        // The new character is in escapedChar; the number of percent escapes it took is in numBytesExpected.
        searchRange.location = percentRange.location + 3 * numBytesExpected;
        searchRange.length = length - searchRange.location;
        
        if (!escapeAll) {
            if (CFStringFind(charactersToLeaveEscaped, escapedStr, 0).location != kCFNotFound) {
                if (escapedStr) {
                    CFRelease(escapedStr);
                    escapedStr = NULL;
                }
                continue;
            } 
        }
        
        if (!newStr) {
            newStr = CFStringCreateMutable(alloc, length);
        }
        if (percentRange.location - mark > 0) {
            // The creation of this temporary string is unfortunate. 
            CFStringRef substring = CFStringCreateWithSubstring(alloc, originalString, CFRangeMake(mark, percentRange.location - mark));
            CFStringAppend(newStr, substring);
            CFRelease(substring);
        }
        CFStringAppend(newStr, escapedStr);
        if (escapedStr) {
            CFRelease(escapedStr);
            escapedStr = NULL;
        }
        mark = searchRange.location;// We need mark to be the index of the first character beyond the escape sequence
    }

    if (escapedStr) CFRelease(escapedStr);
    if (strForEscapedChar) CFRelease(strForEscapedChar);
    if (failed) {
        if (newStr) CFRelease(newStr);
        return NULL;
    } else if (newStr) {
        if (mark < length) {
            // Need to cat on the remainder of the string
            CFStringRef substring = CFStringCreateWithSubstring(alloc, originalString, CFRangeMake(mark, length - mark));
            CFStringAppend(newStr, substring);
            CFRelease(substring);
        }
        return newStr;
    } else {
	return (CFStringRef)CFStringCreateCopy(alloc, originalString);
    }
}

CF_EXPORT
CFStringRef CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFAllocatorRef alloc, CFStringRef  originalString, CFStringRef  charactersToLeaveEscaped, CFStringEncoding enc) {
    if (enc == kCFStringEncodingUTF8) {
        return CFURLCreateStringByReplacingPercentEscapes(alloc, originalString, charactersToLeaveEscaped);
    } else {
        CFMutableStringRef newStr = NULL;
        CFMutableStringRef escapedStr = NULL;
        CFIndex length;
        CFIndex mark = 0;
        CFRange percentRange, searchRange;
        Boolean escapeAll = (charactersToLeaveEscaped && CFStringGetLength(charactersToLeaveEscaped) == 0);
        Boolean failed = false;
        uint8_t byteBuffer[8];
        uint8_t *bytes = byteBuffer;
        int capacityOfBytes = 8;
        
        if (!originalString) return NULL;
    
        if (charactersToLeaveEscaped == NULL) {
            return (CFStringRef)CFStringCreateCopy(alloc, originalString);
        }
    
        length = CFStringGetLength(originalString);
        searchRange = CFRangeMake(0, length);
    
        while (!failed && CFStringFindWithOptions(originalString, CFSTR("%"), searchRange, 0, &percentRange)) {
            UniChar ch1, ch2;
            CFIndex percentLoc = percentRange.location;
            CFStringRef convertedString;
            int numBytesUsed = 0;
            do {
                // Make sure we have at least 2 more characters
                if (length - percentLoc < 3) { failed = true; break; }
    
                if (numBytesUsed == capacityOfBytes) {
                    if (bytes == byteBuffer) {
                        bytes = (uint8_t *)CFAllocatorAllocate(alloc, 16 * sizeof(uint8_t), 0);
                        memmove(bytes, byteBuffer, capacityOfBytes);
                        capacityOfBytes = 16;
                    } else {
			void *oldbytes = bytes;
			int oldcap = capacityOfBytes;
                        capacityOfBytes = 2*capacityOfBytes;
                        bytes = (uint8_t *)CFAllocatorAllocate(alloc, capacityOfBytes * sizeof(uint8_t), 0);
			memmove(bytes, oldbytes, oldcap);
                        CFAllocatorDeallocate(alloc, oldbytes);
                    }
                }
                percentLoc ++;
                ch1 = CFStringGetCharacterAtIndex(originalString, percentLoc);
                percentLoc ++;
                ch2 = CFStringGetCharacterAtIndex(originalString, percentLoc);
                percentLoc ++;
                if (!_translateBytes(ch1, ch2, bytes + numBytesUsed)) { failed = true;  break; }
                numBytesUsed ++;
            } while (CFStringGetCharacterAtIndex(originalString, percentLoc) == '%');
            searchRange.location = percentLoc;
            searchRange.length = length - searchRange.location;

            if (failed) break;
            convertedString = CFStringCreateWithBytes(alloc, bytes, numBytesUsed, enc, false);
            if (!convertedString) {
                failed = true;
                break;
            }
    
            if (!newStr) {
                newStr = CFStringCreateMutable(alloc, length);
            }
            if (percentRange.location - mark > 0) {
                // The creation of this temporary string is unfortunate. 
                CFStringRef substring = CFStringCreateWithSubstring(alloc, originalString, CFRangeMake(mark, percentRange.location - mark));
                CFStringAppend(newStr, substring);
                CFRelease(substring);
            }

            if (escapeAll) {
                CFStringAppend(newStr, convertedString);
            } else {
                CFIndex i, c = CFStringGetLength(convertedString);
                if (!escapedStr) {
                    escapedStr = CFStringCreateMutableWithExternalCharactersNoCopy(alloc, &ch1, 1, 1, kCFAllocatorNull);
                }
                for (i = 0; i < c; i ++) {
                    ch1 = CFStringGetCharacterAtIndex(convertedString, i);
                    if (CFStringFind(charactersToLeaveEscaped, escapedStr, 0).location == kCFNotFound) {
                        CFStringAppendCharacters(newStr, &ch1, 1);
                    } else {
                        // Must regenerate the escape sequence for this character; because we started with percent escapes, we know this call cannot fail
                        _appendPercentEscapesForCharacter(ch1, enc, newStr);
                    }
                }
            }
	    CFRelease(convertedString);
            mark = searchRange.location;// We need mark to be the index of the first character beyond the escape sequence
        }
    
        if (escapedStr) CFRelease(escapedStr);
        if (bytes != byteBuffer) CFAllocatorDeallocate(alloc, bytes);
        if (failed) {
            if (newStr) CFRelease(newStr);
            return NULL;
        } else if (newStr) {
            if (mark < length) {
                // Need to cat on the remainder of the string
                CFStringRef substring = CFStringCreateWithSubstring(alloc, originalString, CFRangeMake(mark, length - mark));
                CFStringAppend(newStr, substring);
                CFRelease(substring);
            }
            return newStr;
        } else {
            return (CFStringRef)CFStringCreateCopy(alloc, originalString);
        }
    }
}
    

static CFStringRef _addPercentEscapesToString(CFAllocatorRef allocator, CFStringRef originalString, Boolean (*shouldReplaceChar)(UniChar, void*), CFIndex (*handlePercentChar)(CFIndex, CFStringRef, CFStringRef *, void *), CFStringEncoding encoding, void *context) {
    CFMutableStringRef newString = NULL;
    CFIndex idx, length;
    CFStringInlineBuffer buf;

    if (!originalString) return NULL;
    length = CFStringGetLength(originalString);
    if (length == 0) return (CFStringRef)CFStringCreateCopy(allocator, originalString);
    CFStringInitInlineBuffer(originalString, &buf, CFRangeMake(0, length));

    for (idx = 0; idx < length; idx ++) {
        UniChar ch = CFStringGetCharacterFromInlineBuffer(&buf, idx);
        Boolean shouldReplace = shouldReplaceChar(ch, context);
        if (shouldReplace) {
            // Perform the replacement
            if (!newString) {
                newString = CFStringCreateMutableCopy(CFGetAllocator(originalString), 0, originalString);
                CFStringDelete(newString, CFRangeMake(idx, length-idx));
            }
            if (!_appendPercentEscapesForCharacter(ch, encoding, newString)) {
//#warning FIXME - once CFString supports finding glyph boundaries walk by glyph boundaries instead of by unichars
                if (encoding == kCFStringEncodingUTF8 && CFCharacterSetIsSurrogateHighCharacter(ch) && idx + 1 < length && CFCharacterSetIsSurrogateLowCharacter(CFStringGetCharacterFromInlineBuffer(&buf, idx+1))) {
                    // Hack to guarantee we always safely convert file URLs between POSIX & full URL representation
                    if (_hackToConvertSurrogates(ch, CFStringGetCharacterFromInlineBuffer(&buf, idx+1), newString)) {
                        idx ++; // We consumed 2 characters, not 1
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
        } else if (ch == '%' && handlePercentChar) {
            CFStringRef replacementString = NULL;
            CFIndex newIndex = handlePercentChar(idx, originalString, &replacementString, context);
            if (newIndex < 0) {
                break;
            } else if (replacementString) {
                if (!newString) {
                    newString = CFStringCreateMutableCopy(CFGetAllocator(originalString), 0, originalString);
                    CFStringDelete(newString, CFRangeMake(idx, length-idx));
                }
                CFStringAppend(newString, replacementString);
                CFRelease(replacementString);
            }
            if (newIndex == idx) {
                if (newString) {
                    CFStringAppendCharacters(newString, &ch, 1);
                }
            } else {
                if (!replacementString && newString) {
                    CFIndex tmpIndex;
                    for (tmpIndex = idx; tmpIndex < newIndex; tmpIndex ++) {
                        ch = CFStringGetCharacterAtIndex(originalString, idx);
                        CFStringAppendCharacters(newString, &ch, 1);
                    }                        
                }
                idx = newIndex - 1;
            }
        } else if (newString) {
            CFStringAppendCharacters(newString, &ch, 1);
        }
    }
    if (idx < length) {
        // Ran in to an encoding failure
        if (newString) CFRelease(newString);
        return NULL;
    } else if (newString) {
        return newString;
    } else {
        return (CFStringRef)CFStringCreateCopy(CFGetAllocator(originalString), originalString);
    }
}


static Boolean _stringContainsCharacter(CFStringRef string, UniChar ch) {
    CFIndex i, c = CFStringGetLength(string);
    CFStringInlineBuffer buf;
    CFStringInitInlineBuffer(string, &buf, CFRangeMake(0, c));
    for (i = 0; i < c; i ++) if (__CFStringGetCharacterFromInlineBufferQuick(&buf, i) == ch) return true;
    return false;
}

static Boolean _shouldPercentReplaceChar(UniChar ch, void *context) {
    CFStringRef unescape = ((CFStringRef *)context)[0];
    CFStringRef escape = ((CFStringRef *)context)[1];
    Boolean shouldReplace = (isURLLegalCharacter(ch) == false);
    if (shouldReplace) {
        if (unescape && _stringContainsCharacter(unescape, ch)) {
            shouldReplace = false;
        }
    } else if (escape && _stringContainsCharacter(escape, ch)) {
        shouldReplace = true;
    }
    return shouldReplace;
}

CF_EXPORT CFStringRef CFURLCreateStringByAddingPercentEscapes(CFAllocatorRef allocator, CFStringRef originalString, CFStringRef charactersToLeaveUnescaped, CFStringRef legalURLCharactersToBeEscaped, CFStringEncoding encoding) {
    CFStringRef strings[2];
    strings[0] = charactersToLeaveUnescaped;
    strings[1] = legalURLCharactersToBeEscaped;
    return _addPercentEscapesToString(allocator, originalString, _shouldPercentReplaceChar, NULL, encoding, strings);
}

static Boolean __CFURLEqual(CFTypeRef  cf1, CFTypeRef  cf2) {
    Boolean result;
    CFURLRef  url1 = (CFURLRef)cf1;
    CFURLRef  url2 = (CFURLRef)cf2;
    
    __CFGenericValidateType(cf1, CFURLGetTypeID());
    __CFGenericValidateType(cf2, CFURLGetTypeID());

    if ( url1 == url2 ) {
        result = true;
    }
    else {
        if ( (url1->_flags & EQUAL_FLAGS_MASK) != (url2->_flags & EQUAL_FLAGS_MASK) ) {
            result = false;
        }
        else {
            if ( (url1->_base && !url2->_base) ||
                (!url1->_base && url2->_base) ||
                (url1->_base && url2->_base && !CFEqual(url1->_base, url2->_base)) ) {
                result = false;
            }
            else {
                // no base urls, so compare the URL strings
                // Do not compare the original strings; compare the sanatized strings.
                result = CFEqual(CFURLGetString(url1), CFURLGetString(url2));
            }
        }
    }
    return ( result ) ;
}

static CFHashCode __CFURLHash(CFTypeRef cf)
{
    CFHashCode result;
    
    if ( cf ) {
        // use the CFHashCode of the URL
        result = CFHash(CFURLGetString((CFURLRef)cf));
    }
    else {
        // no object, no hashcode
        result = 0;
    }
    
    return ( result );
}

static CFStringRef  __CFURLCopyFormattingDescription(CFTypeRef  cf, CFDictionaryRef formatOptions) {
    CFURLRef  url = (CFURLRef)cf;
    __CFGenericValidateType(cf, CFURLGetTypeID());
    if (! url->_base) {
        CFRetain(url->_string);
        return url->_string;
    } else {
        // Do not dereference url->_base; it may be an ObjC object
        return CFStringCreateWithFormat(CFGetAllocator(url), NULL, CFSTR("%@ -- %@"), url->_string, url->_base);
    }
}


static CFStringRef __CFURLCopyDescription(CFTypeRef cf) {
    CFURLRef url = (CFURLRef)cf;
    CFStringRef result;
    CFAllocatorRef alloc = CFGetAllocator(url);
    if ( url->_base) {
        CFStringRef baseString = CFCopyDescription(url->_base);
        result = CFStringCreateWithFormat(alloc, NULL, CFSTR("<CFURL %p [%p]>{string = %@, encoding = %d\n\tbase = %@}"), cf, alloc, url->_string, url->_encoding, baseString);
        CFRelease(baseString);
    } else {
        result = CFStringCreateWithFormat(alloc, NULL, CFSTR("<CFURL %p [%p]>{string = %@, encoding = %d, base = (null)}"), cf, alloc, url->_string, url->_encoding);
    }
    return result;
}

#if DEBUG_URL_MEMORY_USAGE

extern __attribute((used)) void __CFURLDumpMemRecord(void) {
    CFStringRef str = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%d URLs created; %d destroyed\n%d file URLs created; %d urls had 'extra' data allocated, %d had base urls, %d were not UTF8 encoded\n"), numURLs, numDealloced, numFileURLsCreated, numExtraDataAllocated, numURLsWithBaseURL, numNonUTF8EncodedURLs );
    CFShow(str);
    CFRelease(str);
}
#endif

static void __CFURLDeallocate(CFTypeRef  cf) {
    CFURLRef  url = (CFURLRef)cf;
    CFAllocatorRef alloc;
    __CFGenericValidateType(cf, CFURLGetTypeID());
    alloc = CFGetAllocator(url);
#if DEBUG_URL_MEMORY_USAGE
    numDealloced ++;
#endif
    if (url->_string) CFRelease(url->_string); // GC: 3879914
    if (url->_base) CFRelease(url->_base);
    if (url->_ranges) CFAllocatorDeallocate(alloc, url->_ranges);
    CFStringRef sanitizedString = _getSanitizedString(url);
    if (sanitizedString) CFRelease(sanitizedString);
    if ( url->_extra != NULL ) CFAllocatorDeallocate( alloc, url->_extra );
    if (_getResourceInfo(url)) CFRelease(_getResourceInfo(url));
}

static CFTypeID __kCFURLTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFURLClass = {
    0,                                  // version
    "CFURL",                            // className
    NULL,                               // init
    NULL,                               // copy
    __CFURLDeallocate,                  // finalize
    __CFURLEqual,                       // equal
    __CFURLHash,                        // hash
    __CFURLCopyFormattingDescription,   // copyFormattingDesc
    __CFURLCopyDescription,             // copyDebugDesc
    NULL,                               // reclaim
    NULL,                               // refcount
};

// When __CONSTANT_CFSTRINGS__ is not defined, we have separate macros for static and exported constant strings, but
// when it is defined, we must prefix with static to prevent the string from being exported 
#ifdef __CONSTANT_CFSTRINGS__
static CONST_STRING_DECL(kCFURLHTTPScheme, "http")
static CONST_STRING_DECL(kCFURLHTTPSScheme, "https")
static CONST_STRING_DECL(kCFURLFileScheme, "file")
static CONST_STRING_DECL(kCFURLDataScheme, "data")
static CONST_STRING_DECL(kCFURLFTPScheme, "ftp")
static CONST_STRING_DECL(kCFURLLocalhost, "localhost")
#else
CONST_STRING_DECL(kCFURLHTTPScheme, "http")
CONST_STRING_DECL(kCFURLHTTPSScheme, "https")
CONST_STRING_DECL(kCFURLFileScheme, "file")
CONST_STRING_DECL(kCFURLDataScheme, "data")
CONST_STRING_DECL(kCFURLFTPScheme, "ftp")
CONST_STRING_DECL(kCFURLLocalhost, "localhost")
#endif
__private_extern__ void __CFURLInitialize(void) {
    __kCFURLTypeID = _CFRuntimeRegisterClass(&__CFURLClass);
}

/* Toll-free bridging support; get the true CFURL from an NSURL */
CF_INLINE CFURLRef _CFURLFromNSURL(CFURLRef url) {
    CF_OBJC_FUNCDISPATCHV(__kCFURLTypeID, CFURLRef, (NSURL *)url, _cfurl);
    return url;
}

CFTypeID CFURLGetTypeID(void) {
    return __kCFURLTypeID;
}

__private_extern__ void CFShowURL(CFURLRef url) {
    if (!url) {
        fprintf(stdout, "(null)\n");
        return;
    }
    fprintf(stdout, "<CFURL %p>{", (const void*)url);
    if (CF_IS_OBJC(__kCFURLTypeID, url)) {
        fprintf(stdout, "ObjC bridged object}\n");
        return;
    }
    fprintf(stdout, "\n\tRelative string: ");
    CFShow(url->_string);
    fprintf(stdout, "\tBase URL: ");
    if (url->_base) {
        fprintf(stdout, "<%p> ", (const void*)url->_base);
        CFShow(url->_base);
    } else {
        fprintf(stdout, "(null)\n");
    }
    fprintf(stdout, "\tFlags: 0x%x\n}\n", (unsigned int)url->_flags);
}


/***************************************************/
/* URL creation and String/Data creation from URLS */
/***************************************************/
static void constructBuffers(CFAllocatorRef alloc, CFStringRef string, Boolean useEightBitStringEncoding, UInt8 *inBuffer, CFIndex inBufferSize, const char **cstring, const UniChar **ustring, Boolean *useCString, Boolean *freeCharacters) {
    CFIndex neededLength;
    CFIndex length;
    CFRange rg;

    *cstring = CFStringGetCStringPtr(string, (useEightBitStringEncoding ? __CFStringGetEightBitStringEncoding() : kCFStringEncodingISOLatin1));
    if (*cstring) {
        *ustring = NULL;
        *useCString = true;
        *freeCharacters = false;
        return;
    } 

    *ustring = CFStringGetCharactersPtr(string);
    if (*ustring) {
        *useCString = false;
        *freeCharacters = false;
        return;
    } 

    length = CFStringGetLength(string);
    rg = CFRangeMake(0, length);
    CFStringGetBytes(string, rg, kCFStringEncodingISOLatin1, 0, false, NULL, INT_MAX, &neededLength);
    if (neededLength == length) {
        char *buf;
        if ( (inBuffer != NULL) && (length <= inBufferSize) ) {
            buf = (char *)inBuffer;
            *freeCharacters = false;
        }
        else {
            buf = (char *)CFAllocatorAllocate(alloc, length, 0);
            *freeCharacters = true;
        }
        CFStringGetBytes(string, rg, kCFStringEncodingISOLatin1, 0, false, (uint8_t *)buf, length, NULL);
        *cstring = buf;
        *useCString = true;
    } else {
        UniChar *buf;
        if ( (inBuffer != NULL) && ((length * sizeof(UniChar)) <= inBufferSize) ) {
            buf = (UniChar *)inBuffer;
            *freeCharacters = false;
        }
        else {
            buf = (UniChar *)CFAllocatorAllocate(alloc, length * sizeof(UniChar), 0);
            *freeCharacters = true;
        }
        CFStringGetCharacters(string, rg, buf);
        *ustring = buf;
        *useCString = false;
    }
}

#define STRING_CHAR(x) (useCString ? cstring[(x)] : ustring[(x)])
static void _parseComponents(CFAllocatorRef alloc, CFStringRef string, CFURLRef baseURL, UInt32 *theFlags, CFRange **range) {
    CFRange ranges[9];
    /* index gives the URL part involved; to calculate the correct range index, use the number of the bit of the equivalent flag (i.e. the host flag is HAS_HOST, which is 0x8.  so the range index for the host is 3.)  Note that this is true in this function ONLY, since the ranges stored in (*range) are actually packed, skipping those URL components that don't exist.  This is why the indices are hard-coded in this function. */

    CFIndex idx, base_idx = 0;
    CFIndex string_length;
    UInt32 flags = *theFlags;
    Boolean useEightBitStringEncoding = (flags & USES_EIGHTBITSTRINGENCODING) != 0;
    Boolean useCString, freeCharacters, isCompliant;
    uint8_t numRanges = 0;
    const char *cstring = NULL;
    const UniChar *ustring = NULL;
    CFIndex stackBufferSize = 4096;
    STACK_BUFFER_DECL(UInt8, stackBuffer, stackBufferSize);
    
    string_length = CFStringGetLength(string);
    constructBuffers(alloc, string, useEightBitStringEncoding, stackBuffer, stackBufferSize, &cstring, &ustring, &useCString, &freeCharacters);
    
    // Algorithm is as described in RFC 1808
    // 1: parse the fragment; remainder after left-most "#" is fragment
    for (idx = base_idx; idx < string_length; idx++) {
        if ('#' == STRING_CHAR(idx)) {
            flags |= HAS_FRAGMENT;
            ranges[8].location = idx + 1;
            ranges[8].length = string_length - (idx + 1);
            numRanges ++;
            string_length = idx;	// remove fragment from parse string
            break;
        }
    }
    // 2: parse the scheme
    for (idx = base_idx; idx < string_length; idx++) {
        UniChar ch = STRING_CHAR(idx);
        if (':' == ch) {
            flags |= HAS_SCHEME;
            flags |= IS_ABSOLUTE;
            ranges[0].location = base_idx;
            ranges[0].length = idx;
            numRanges ++;
            base_idx = idx + 1;
            // optimization for ftp urls
            if (idx == 3 && STRING_CHAR(0) == 'f' && STRING_CHAR(1) == 't' && STRING_CHAR(2) == 'p') {
                _setSchemeTypeInFlags(&flags, kHasFtpScheme);
            }
            else if (idx == 4) {
                // optimization for http urls
                if (STRING_CHAR(0) == 'h' && STRING_CHAR(1) == 't' && STRING_CHAR(2) == 't' && STRING_CHAR(3) == 'p') {
                    _setSchemeTypeInFlags(&flags, kHasHttpScheme);
                }
                // optimization for file urls
                if (STRING_CHAR(0) == 'f' && STRING_CHAR(1) == 'i' && STRING_CHAR(2) == 'l' && STRING_CHAR(3) == 'e') {
                    _setSchemeTypeInFlags(&flags, kHasFileScheme);
                }
                // optimization for data urls
                if (STRING_CHAR(0) == 'd' && STRING_CHAR(1) == 'a' && STRING_CHAR(2) == 't' && STRING_CHAR(3) == 'a') {
                    _setSchemeTypeInFlags(&flags, kHasDataScheme);
                }
            }
            // optimization for https urls
            else if (idx == 5 && STRING_CHAR(0) == 'h' && STRING_CHAR(1) == 't' && STRING_CHAR(2) == 't' && STRING_CHAR(3) == 'p' && STRING_CHAR(3) == 's') {
                _setSchemeTypeInFlags(&flags, kHasHttpsScheme);
            }
            break;
        } else if (!scheme_valid(ch)) {
            break;	// invalid scheme character -- no scheme
        }
    }

    // Make sure we have an RFC-1808 compliant URL - that's either something without a scheme, or scheme:/(stuff) or scheme://(stuff) 
    // Strictly speaking, RFC 1808 & 2396 bar "scheme:" (with nothing following the colon); however, common usage
    // expects this to be treated identically to "scheme://" - REW, 12/08/03
    if (!(flags & HAS_SCHEME)) {
        isCompliant = true;
    } else if (base_idx == string_length) {
        isCompliant = false;
    } else if (STRING_CHAR(base_idx) != '/') {
        isCompliant = false;
    } else {
        isCompliant = true;
    }
    
    if (!isCompliant) {
        // Clear the fragment flag if it's been set
        if (flags & HAS_FRAGMENT) {
            flags &= (~HAS_FRAGMENT);
            string_length = CFStringGetLength(string);
        }
        (*theFlags) = flags;
        (*range) = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
        (*range)->location = ranges[0].location;
        (*range)->length = ranges[0].length;

        if (freeCharacters) {
            CFAllocatorDeallocate(alloc, useCString ? (void *)cstring : (void *)ustring);
        }
        return;
    }
    // URL is 1808-compliant
    flags |= IS_DECOMPOSABLE;
    
    // 3: parse the network location and login
    if (2 <= (string_length - base_idx) && '/' == STRING_CHAR(base_idx) && '/' == STRING_CHAR(base_idx+1)) {
        CFIndex base = 2 + base_idx, extent;
        for (idx = base; idx < string_length; idx++) {
            if ('/' == STRING_CHAR(idx) || '?' == STRING_CHAR(idx)) break;
        }
        extent = idx;
        
        // net_loc parts extend from base to extent (but not including), which might be to end of string
        // net location is "<user>:<password>@<host>:<port>"
        if (extent != base) {
            for (idx = base; idx < extent; idx++) {
                if ('@' == STRING_CHAR(idx)) {   // there is a user
                    CFIndex idx2;
                    flags |= HAS_USER;
                    numRanges ++;
                    ranges[1].location = base;  // base of the user
                    for (idx2 = base; idx2 < idx; idx2++) {
                        if (':' == STRING_CHAR(idx2)) {	// found a password separator
                            flags |= HAS_PASSWORD;
                            numRanges ++;
                            ranges[2].location = idx2+1; // base of the password
                            ranges[2].length = idx-(idx2+1);  // password extent
                            ranges[1].length = idx2 - base; // user extent
                            break;
                        }
                    }
                    if (!(flags & HAS_PASSWORD)) {
                        // user extends to the '@'
                        ranges[1].length = idx - base; // user extent
                    }
                    base = idx + 1;
                    break;
                }
            }
            flags |= HAS_HOST;
            numRanges ++;
            ranges[3].location = base; // base of host

            // base has been advanced past the user and password if they existed
            for (idx = base; idx < extent; idx++) {
                // IPV6 support (RFC 2732) DCJ June/10/2002
                if ('[' == STRING_CHAR(idx)) {	// starting IPV6 explicit address
					//	Find the ']' terminator of the IPv6 address, leave idx pointing to ']' or end
					for ( ; idx < extent; ++ idx ) {
						if ( ']' == STRING_CHAR(idx)) {
							flags |= IS_IPV6_ENCODED;
							break;
						}
					}
				}
                // there is a port if we see a colon.  Only the last one is the port, though.
                else if ( ':' == STRING_CHAR(idx)) {	
                    flags |= HAS_PORT;
                    numRanges ++;
                    ranges[4].location = idx+1; // base of port
                    ranges[4].length = extent - (idx+1); // port extent
                    ranges[3].length = idx - base; // host extent
                    break;
                }
            }
            if (!(flags & HAS_PORT)) {
                ranges[3].length = extent - base;  // host extent
            }
        }
        base_idx = extent;
    }

    // 4: parse the query; remainder after left-most "?" is query
    for (idx = base_idx; idx < string_length; idx++) {
        if ('?' == STRING_CHAR(idx)) {
            flags |= HAS_QUERY;
            numRanges ++;
            ranges[7].location = idx + 1;
            ranges[7].length = string_length - (idx+1);
            string_length = idx;	// remove query from parse string
            break;
        }
    }
        
    // 5: parse the parameters; remainder after left-most ";" is parameters
    for (idx = base_idx; idx < string_length; idx++) {
        if (';' == STRING_CHAR(idx)) {
            flags |= HAS_PARAMETERS;
            numRanges ++;
            ranges[6].location = idx + 1;
            ranges[6].length = string_length - (idx+1);
            string_length = idx;	// remove parameters from parse string
            break;
        }
    }
        
    // 6: parse the path; it's whatever's left between string_length & base_idx
    if (string_length - base_idx != 0 || (flags & NET_LOCATION_MASK))
    {
        // If we have a net location, we are 1808-compliant, and an empty path substring implies a path of "/"
        UniChar ch;
        Boolean isDir;
        CFRange pathRg;
        flags |= HAS_PATH;
        numRanges ++;
        pathRg.location = base_idx;
        pathRg.length = string_length - base_idx;
        ranges[5] = pathRg;

        if (pathRg.length > 0) {
            Boolean sawPercent = FALSE;
            for (idx = pathRg.location; idx < string_length; idx++) {
                if ('%' == STRING_CHAR(idx)) {
                    sawPercent = TRUE;
                    break;
                }
            }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
	    if (pathRg.length > 6 && STRING_CHAR(pathRg.location) == '/' && STRING_CHAR(pathRg.location + 1) == '.' && STRING_CHAR(pathRg.location + 2) == 'f' && STRING_CHAR(pathRg.location + 3) == 'i' && STRING_CHAR(pathRg.location + 4) == 'l' && STRING_CHAR(pathRg.location + 5) == 'e' && STRING_CHAR(pathRg.location + 6) == '/') {
		flags |= PATH_HAS_FILE_ID;
	    } else if (!sawPercent) {
                flags |= POSIX_AND_URL_PATHS_MATCH;
            }
#elif DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_WINDOWS
            if (!sawPercent) {
                flags |= POSIX_AND_URL_PATHS_MATCH;
            }
#endif

            ch = STRING_CHAR(pathRg.location + pathRg.length - 1);
            if (ch == '/') {
                isDir = true;
            } else if (ch == '.') {
                if (pathRg.length == 1) {
                    isDir = true;
                } else {
                    ch = STRING_CHAR(pathRg.location + pathRg.length - 2);
                    if (ch == '/') {
                        isDir = true;
                    } else if (ch != '.') {
                        isDir = false;
                    } else if (pathRg.length == 2) {
                        isDir = true;
                    } else {
                        isDir = (STRING_CHAR(pathRg.location + pathRg.length - 3) == '/');
                    }
                }
            } else {
                isDir = false;
            }
        } else {
            isDir = (baseURL != NULL) ? CFURLHasDirectoryPath(baseURL) : false;
        }
        if (isDir) {
            flags |= IS_DIRECTORY;
        }
    }

    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void *)cstring : (void *)ustring);
    }
    (*theFlags) = flags;
    (*range) = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange)*numRanges, 0);
    numRanges = 0;
    for (idx = 0, flags = 1; flags != (1<<9); flags = (flags<<1), idx ++) {
        if ((*theFlags) & flags) {
            (*range)[numRanges] = ranges[idx];
            numRanges ++;
        }
    }
}

static Boolean scanCharacters(CFAllocatorRef alloc, CFMutableStringRef *escapedString, UInt32 *flags, const char *cstring, const UniChar *ustring, Boolean useCString, CFIndex base, CFIndex end, CFIndex *mark, UInt32 componentFlag, CFStringEncoding encoding) {
    CFIndex idx;
    Boolean sawIllegalChar = false;
    for (idx = base; idx < end; idx ++) {
        Boolean shouldEscape;
        UniChar ch = STRING_CHAR(idx);
        if (isURLLegalCharacter(ch)) {
            if ((componentFlag == HAS_USER || componentFlag == HAS_PASSWORD) && (ch == '/' || ch == '?' || ch == '@')) {
                shouldEscape = true;
            } else {
                shouldEscape = false;
            }
        } else if (ch == '%' && idx + 2 < end && isHexDigit(STRING_CHAR(idx + 1)) && isHexDigit(STRING_CHAR(idx+2))) {
            shouldEscape = false;
        } else if (componentFlag == HAS_HOST && ((idx == base && ch == '[') || (idx == end-1 && ch == ']'))) {
            shouldEscape = false;
        } else {
            shouldEscape = true;
        }
        if (shouldEscape) {
            sawIllegalChar = true;
            if (componentFlag && flags) {
                *flags |= componentFlag;
            }
            if (!*escapedString) {
                *escapedString = CFStringCreateMutable(alloc, 0);
            }
            if (useCString) {
                CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t *)&(cstring[*mark]), idx - *mark, kCFStringEncodingISOLatin1, false);
                CFStringAppend(*escapedString, tempString);
                CFRelease(tempString);
            } else {
                CFStringAppendCharacters(*escapedString, &(ustring[*mark]), idx - *mark);
            }
            *mark = idx + 1;
            _appendPercentEscapesForCharacter(ch, encoding, *escapedString); // This can never fail because anURL->_string was constructed from the encoding passed in
        }
    }
    return sawIllegalChar;
} 

static void computeSanitizedString(CFURLRef url) {
    CFAllocatorRef alloc = CFGetAllocator(url);
    CFIndex string_length = CFStringGetLength(url->_string);
    Boolean useCString, freeCharacters;
    const char *cstring = NULL;
    const UniChar *ustring = NULL;
    CFIndex base; // where to scan from
    CFIndex mark; // first character not-yet copied to sanitized string
    CFIndex stackBufferSize = 4096;
    STACK_BUFFER_DECL(UInt8, stackBuffer, stackBufferSize);
    CFMutableStringRef sanitizedString = NULL;
    UInt32 additionalDataFlags = 0;
    Boolean useEightBitStringEncoding = (url->_flags & USES_EIGHTBITSTRINGENCODING) != 0;

    constructBuffers(alloc, url->_string, useEightBitStringEncoding, stackBuffer, stackBufferSize, &cstring, &ustring, &useCString, &freeCharacters);
    if (!(url->_flags & IS_DECOMPOSABLE)) {
        // Impossible to have a problem character in the scheme
        base = _rangeForComponent(url->_flags, url->_ranges, HAS_SCHEME).length + 1;
        mark = 0;
        if (!scanCharacters(alloc, &sanitizedString, &additionalDataFlags, cstring, ustring, useCString, base, string_length, &mark, 0, url->_encoding)) {
            ((struct __CFURL *)url)->_flags |= ORIGINAL_AND_URL_STRINGS_MATCH;
        }
        if ( sanitizedString ) {
            _setAdditionalDataFlags((struct __CFURL*)url, additionalDataFlags);
        }
    } else {
        // Go component by component
        CFIndex currentComponent = HAS_USER;
        mark = 0;
        while (currentComponent < (HAS_FRAGMENT << 1)) {
            CFRange componentRange = _rangeForComponent(url->_flags, url->_ranges, currentComponent);
            if (componentRange.location != kCFNotFound) {
                scanCharacters(alloc, & sanitizedString, &additionalDataFlags, cstring, ustring, useCString, componentRange.location, componentRange.location + componentRange.length, &mark, currentComponent, url->_encoding);
            }
            currentComponent = currentComponent << 1;
        }
        if (sanitizedString) {
            _setAdditionalDataFlags((struct __CFURL*)url, additionalDataFlags);
        } else {
            ((struct __CFURL *)url)->_flags |= ORIGINAL_AND_URL_STRINGS_MATCH;
        }
    }
    if (sanitizedString && mark != string_length) {
        if (useCString) {
            CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t *)&(cstring[mark]), string_length - mark, kCFStringEncodingISOLatin1, false);
            CFStringAppend(sanitizedString, tempString);
            CFRelease(tempString);
        } else {
            CFStringAppendCharacters(sanitizedString, &(ustring[mark]), string_length - mark);
        }
    }
    if ( sanitizedString ) {
        _setSanitizedString((struct __CFURL*) url, sanitizedString);
        CFRelease(sanitizedString);
    }
    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void *)cstring : (void *)ustring);
    }
}


static CFStringRef correctedComponent(CFStringRef comp, UInt32 compFlag, CFStringEncoding enc) {
    CFAllocatorRef alloc = CFGetAllocator(comp);
    CFIndex string_length = CFStringGetLength(comp);
    Boolean useCString, freeCharacters;
    const char *cstring = NULL;
    const UniChar *ustring = NULL;
    CFIndex mark = 0; // first character not-yet copied to sanitized string
    CFMutableStringRef result = NULL;
    CFIndex stackBufferSize = 1024;
    STACK_BUFFER_DECL(UInt8, stackBuffer, stackBufferSize);

    constructBuffers(alloc, comp, false, stackBuffer, stackBufferSize, &cstring, &ustring, &useCString, &freeCharacters);
    scanCharacters(alloc, &result, NULL, cstring, ustring, useCString, 0, string_length, &mark, compFlag, enc);
    if (result) {
        if (mark < string_length) {
            if (useCString) {
                CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t *)&(cstring[mark]), string_length - mark, kCFStringEncodingISOLatin1, false);
                CFStringAppend(result, tempString);
                CFRelease(tempString);
            } else {
                CFStringAppendCharacters(result, &(ustring[mark]), string_length - mark);
            }
        }
    } else {
        // This should nevr happen
        CFRetain(comp);
        result = (CFMutableStringRef)comp;
    }
    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void *)cstring : (void *)ustring);
    }
    return result;
}

#undef STRING_CHAR
CF_EXPORT CFURLRef _CFURLAlloc(CFAllocatorRef allocator) {
    struct __CFURL *url;
#if DEBUG_URL_MEMORY_USAGE
    numURLs ++;
#endif
    url = (struct __CFURL *)_CFRuntimeCreateInstance(allocator, __kCFURLTypeID, sizeof(struct __CFURL) - sizeof(CFRuntimeBase), NULL);
    if (url) {
	url->_flags = 0;
	url->_encoding = kCFStringEncodingUTF8;
	url->_string = NULL;
	url->_base = NULL;
	url->_ranges = NULL;
	url->_extra = NULL;
	url->_resourceInfo = NULL;
    }
    return url;
}

// It is the caller's responsibility to guarantee that if URLString is absolute, base is NULL.  This is necessary to avoid duplicate processing for file system URLs, which had to decide whether to compute the cwd for the base; we don't want to duplicate that work.  This ALSO means it's the caller's responsibility to set the IS_ABSOLUTE bit, since we may have a degenerate URL whose string is relative, but lacks a base.
static void _CFURLInit(struct __CFURL *url, CFStringRef URLString, UInt32 fsType, CFURLRef base) {
    CFAssert2((fsType == FULL_URL_REPRESENTATION) || (fsType == kCFURLPOSIXPathStyle) || (fsType == kCFURLWindowsPathStyle) || (fsType == kCFURLHFSPathStyle), __kCFLogAssertion, "%s(): Received bad fsType %d", __PRETTY_FUNCTION__, fsType);
    
    // Coming in, the url has its allocator flag properly set, and its base initialized, and nothing else.    
    url->_string = CFStringCreateCopy(CFGetAllocator(url), URLString);    
    url->_base = base ? CFURLCopyAbsoluteURL(base) : NULL;
    
#if DEBUG_URL_MEMORY_USAGE
    if ( (fsType == kCFURLPOSIXPathStyle) || (fsType == kCFURLHFSPathStyle) || (fsType == kCFURLWindowsPathStyle) ) {
        numFileURLsCreated ++;
    }
    if ( url->_base ) {
        numURLsWithBaseURL ++;
    }
#endif
    if (fsType != FULL_URL_REPRESENTATION) {
        // _convertToURLRepresentation parses the URL
        _convertToURLRepresentation((struct __CFURL *)url, fsType);
    }
    else {
        _parseComponentsOfURL(url);
    }
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX
CF_EXPORT void _CFURLInitFSPath(CFURLRef url, CFStringRef path) {
    CFIndex len = CFStringGetLength(path);
    if (len && CFStringGetCharacterAtIndex(path, 0) == '/') {
        _CFURLInit((struct __CFURL *)url, path, kCFURLPOSIXPathStyle, NULL);
        ((struct __CFURL *)url)->_flags |= IS_ABSOLUTE;
    } else {
        CFURLRef cwdURL = _CFURLCreateCurrentDirectoryURL(CFGetAllocator(url));
        _CFURLInit((struct __CFURL *)url, path, kCFURLPOSIXPathStyle, cwdURL);
        if ( cwdURL )
            CFRelease(cwdURL);
    }
    if (!len || '/' == CFStringGetCharacterAtIndex(path, len - 1))
        ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
}
#elif DEPLOYMENT_TARGET_WINDOWS
CF_EXPORT void _CFURLInitFSPath(CFURLRef url, CFStringRef path) {
    CFIndex len = CFStringGetLength(path);
	// be sure to use the windows path separator when checking the path to see if it's a directory here
        if (!len || '\\' == CFStringGetCharacterAtIndex(path, len - 1))
            ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
    UniChar firstChar = 0 < len ? CFStringGetCharacterAtIndex(path, 0) : 0;
    UniChar secondChar = 1 < len ? CFStringGetCharacterAtIndex(path, 1) : 0;
    Boolean isDrive = ('A' <= firstChar && firstChar <= 'Z') || ('a' <= firstChar && firstChar <= 'z');
        if (!len || '/' == CFStringGetCharacterAtIndex(path, len - 1))
            ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
    isDrive = isDrive && (secondChar == ':' || secondChar == '|');
    if (isDrive || (firstChar == '\\' && secondChar == '\\')) {
        _CFURLInit((struct __CFURL *)url, path, kCFURLWindowsPathStyle, NULL);
        ((struct __CFURL *)url)->_flags |= IS_ABSOLUTE;
    } else if (firstChar == '/') {
        if (!len || '/' == CFStringGetCharacterAtIndex(path, len - 1))
            ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
        _CFURLInit((struct __CFURL *)url, path, kCFURLPOSIXPathStyle, NULL);
        ((struct __CFURL *)url)->_flags |= IS_ABSOLUTE;
    } else {
        CFURLRef cwdURL = _CFURLCreateCurrentDirectoryURL(CFGetAllocator(url));
        _CFURLInit((struct __CFURL *)url, path, kCFURLPOSIXPathStyle, cwdURL);
        if ( cwdURL )
            CFRelease(cwdURL);
    }
}
#endif

// Exported for Foundation's use
CF_EXPORT Boolean _CFStringIsLegalURLString(CFStringRef string) {
    // Check each character to make sure it is a legal URL char.  The valid characters are 'A'-'Z', 'a' - 'z', '0' - '9', plus the characters in "-_.!~*'()", and the set of reserved characters (these characters have special meanings in the URL syntax), which are ";/?:@&=+$,".  In addition, percent escape sequences '%' hex-digit hex-digit are permitted.
    // Plus the hash character '#' which denotes the beginning of a fragment, and can appear exactly once in the entire URL string. -- REW, 12/13/2000
    CFStringInlineBuffer stringBuffer;
    CFIndex idx = 0, length;
    Boolean sawHash = false;
    if (!string) {
        CFAssert(false, __kCFLogAssertion, "Cannot create an CFURL from a NULL string");
        return false;
    }
    length = CFStringGetLength(string);
    CFStringInitInlineBuffer(string, &stringBuffer, CFRangeMake(0, length));
    while (idx < length) {
        UniChar ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
        idx ++;
		
		//	Make sure that two valid hex digits follow a '%' character
		if ( ch == '%' ) {
			if ( idx + 2 > length )
			{
				//CFAssert1(false, __kCFLogAssertion, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-1);
				idx = -1;  // To guarantee index < length, and our failure case is triggered
				break;
			}
			
			ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
			idx ++;
			if (! isHexDigit(ch) ) {
				//CFAssert1(false, __kCFLogAssertion, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-2);
				idx = -1;
				break;
			}
			ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
			idx ++;
			if (! isHexDigit(ch) ) {
				//CFAssert1(false, __kCFLogAssertion, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-3);
				idx = -1;
				break;
			}

			continue;
        }
		if (ch == '[' || ch == ']') continue; // IPV6 support (RFC 2732) DCJ June/10/2002
        if (ch == '#') {
            if (sawHash) break;
            sawHash = true;
            continue;
        }
#if DEPLOYMENT_TARGET_WINDOWS
	// <rdar://problem/7134119> CF on Windows: CFURLCreateWithString should work with | in path on Windows
	if (isURLLegalCharacter(ch) || ch == '|')
#else
		if ( isURLLegalCharacter( ch ) )
#endif
			continue;
		break;
    }
    if (idx < length) {
        return false;
    }
    return true;
}

CF_EXPORT void _CFURLInitWithString(CFURLRef myURL, CFStringRef string, CFURLRef baseURL) {
    struct __CFURL *url = (struct __CFURL *)myURL; // Supress annoying compile warnings
    Boolean isAbsolute = false;
    CFRange colon = CFStringFind(string, CFSTR(":"), 0);
    if (colon.location != kCFNotFound) {
        isAbsolute = true;
        CFIndex i;
        for (i = 0; i < colon.location; i++) {
            char ch = (char)CFStringGetCharacterAtIndex(string, i);
            if (!scheme_valid(ch)) {
                isAbsolute = false;
                break;
            }
        }
    }
    _CFURLInit(url, string, FULL_URL_REPRESENTATION, isAbsolute ? NULL : baseURL);
    if (isAbsolute) {
        url->_flags |= IS_ABSOLUTE;
    }
}

struct __CFURLEncodingTranslationParameters {
    CFStringEncoding fromEnc;
    CFStringEncoding toEnc;
    const UniChar *addlChars;
    int count;
    Boolean escapeHighBit;
    Boolean escapePercents;
    Boolean agreesOverASCII;
    Boolean encodingsMatch;
} ;

// encoding will be used both to interpret the bytes of URLBytes, and to interpret any percent-escapes within the bytes.
CFURLRef CFURLCreateWithBytes(CFAllocatorRef allocator, const uint8_t *URLBytes, CFIndex length, CFStringEncoding encoding, CFURLRef baseURL) {
    CFStringRef  urlString;
    Boolean useEightBitStringEncoding = ( __CFStringEncodingIsSupersetOfASCII(encoding) && __CFBytesInASCII(URLBytes, length) );
    urlString = CFStringCreateWithBytes(allocator, URLBytes, length, useEightBitStringEncoding ? __CFStringGetEightBitStringEncoding() : encoding, false);
    CFURLRef  result;
    if (!urlString || CFStringGetLength(urlString) == 0) {
        if (urlString) CFRelease(urlString);
        return NULL;
    }
    result = _CFURLAlloc(allocator);
    if (result) {
        if ( useEightBitStringEncoding ) {
            ((struct __CFURL *)result)->_flags |= USES_EIGHTBITSTRINGENCODING;
        }
        _CFURLInitWithString(result, urlString, baseURL);
        if (encoding != kCFStringEncodingUTF8) {
            ((struct __CFURL *)result)->_encoding = encoding;
#if DEBUG_URL_MEMORY_USAGE
            numNonUTF8EncodedURLs++;
#endif
        }
    }
    CFRelease(urlString); // it's retained by result, now.
    return result;
}

CFDataRef CFURLCreateData(CFAllocatorRef allocator, CFURLRef  url, CFStringEncoding encoding, Boolean escapeWhitespace) {
    CFDataRef result = NULL;
    if ( url ) {
        CFStringRef myStr = CFURLGetString(url);
        if ( myStr ) {
            result = CFStringCreateExternalRepresentation(allocator, myStr, encoding, 0);
        }
    }
    return result;
}

// Any escape sequences in URLString will be interpreted via UTF-8.
CFURLRef CFURLCreateWithString(CFAllocatorRef allocator, CFStringRef  URLString, CFURLRef  baseURL) {
    CFURLRef url;
    if (!URLString || CFStringGetLength(URLString) == 0) return NULL;
    if (!_CFStringIsLegalURLString(URLString)) return NULL;
    url = _CFURLAlloc(allocator);
    if (url) {
        _CFURLInitWithString(url, URLString, baseURL);
    }
    return url;
}

static CFURLRef _CFURLCreateWithArbitraryString(CFAllocatorRef allocator, CFStringRef URLString, CFURLRef baseURL) {
    CFURLRef url;
    if (!URLString || CFStringGetLength(URLString) == 0) return NULL;
    url = _CFURLAlloc(allocator);
    if (url) {
        _CFURLInitWithString(url, URLString, baseURL);
    }
    return url;
}

CFURLRef CFURLCreateAbsoluteURLWithBytes(CFAllocatorRef alloc, const UInt8 *relativeURLBytes, CFIndex length, CFStringEncoding encoding, CFURLRef baseURL, Boolean useCompatibilityMode) {
    CFURLRef result = NULL;
    
    // if not useCompatibilityMode, use CFURLCreateWithBytes and then CFURLCopyAbsoluteURL if there's a baseURL
    if ( !useCompatibilityMode ) {
        CFURLRef url = CFURLCreateWithBytes(alloc, relativeURLBytes, length, encoding, baseURL);
        if ( url != NULL ) {
            if ( baseURL != NULL ) {
                result = CFURLCopyAbsoluteURL(url);
                CFRelease(url);
            } else {
                result = url;
            }
        }
    } else {
        UInt32 absFlags = 0;
        CFRange *absRanges;
        CFStringRef absString = NULL;
        Boolean absStringIsMutable = false;
        CFURLRef absURL;
        CFStringRef relativeString;
        Boolean useEightBitStringEncoding;
        
        useEightBitStringEncoding = ( __CFStringEncodingIsSupersetOfASCII(encoding) && __CFBytesInASCII(relativeURLBytes, length) );
        relativeString = CFStringCreateWithBytes(alloc, relativeURLBytes, length, useEightBitStringEncoding ? __CFStringGetEightBitStringEncoding() : encoding, false);
        if ( relativeString != NULL ) {
            if (!baseURL) {
                if ( useEightBitStringEncoding ) {
                    absFlags |= USES_EIGHTBITSTRINGENCODING;
                }
                absString = relativeString;
            } else {
                UniChar ch = CFStringGetCharacterAtIndex(relativeString, 0);
                if (ch == '?' || ch == ';' || ch == '#') {
                    // Nothing but parameter + query + fragment; append to the baseURL string
                    CFStringRef baseString;
                    if (CF_IS_OBJC(__kCFURLTypeID, baseURL)) {
                        baseString = CFURLGetString(baseURL);
                    } else {
                        baseString = baseURL->_string;
                    }
                    absString = CFStringCreateMutable(alloc, CFStringGetLength(baseString) + CFStringGetLength(relativeString));
                    CFStringAppend((CFMutableStringRef)absString, baseString);
                    CFStringAppend((CFMutableStringRef)absString, relativeString);
                    absStringIsMutable = true;
                } else {
                    UInt32 relFlags = 0;
                    CFRange *relRanges;
                    CFStringRef relString = NULL;
                    _parseComponents(alloc, relativeString, baseURL, &relFlags, &relRanges);
                    if (relFlags & HAS_SCHEME) {
                        CFStringRef baseScheme = CFURLCopyScheme(baseURL);
                        CFRange relSchemeRange = _rangeForComponent(relFlags, relRanges, HAS_SCHEME);
                        if (baseScheme && CFStringGetLength(baseScheme) == relSchemeRange.length && CFStringHasPrefix(relativeString, baseScheme)) {
                            relString = CFStringCreateWithSubstring(alloc, relativeString, CFRangeMake(relSchemeRange.length+1, CFStringGetLength(relativeString) - relSchemeRange.length - 1));
                            CFAllocatorDeallocate(alloc, relRanges);
                            relFlags = 0;
                            _parseComponents(alloc, relString, baseURL, &relFlags, &relRanges);
                        } else {
                            // Discard the base string; the relative string is absolute and we're not in the funky edge case where the schemes match
                            if ( useEightBitStringEncoding ) {
                                absFlags |= USES_EIGHTBITSTRINGENCODING;
                            }
                            CFRetain(relativeString);
                            absString = relativeString;
                        }
                        if (baseScheme) CFRelease(baseScheme);
                    } else {
                        CFRetain(relativeString);
                        relString = relativeString;
                    }
                    if (!absString) {
                        if (!CF_IS_OBJC(__kCFURLTypeID, baseURL)) {
                            absString = resolveAbsoluteURLString(alloc, relString, relFlags, relRanges, baseURL->_string, baseURL->_flags, baseURL->_ranges);
                        } else {
                            CFStringRef baseString;
                            UInt32 baseFlags = 0;
                            CFRange *baseRanges;
                            if (CF_IS_OBJC(__kCFURLTypeID, baseURL)) {
                                baseString = CFURLGetString(baseURL);
                            } else {
                                baseString = baseURL->_string;
                            }
                            _parseComponents(alloc, baseString, NULL, &baseFlags, &baseRanges);
                            absString = resolveAbsoluteURLString(alloc, relString, relFlags, relRanges, baseString, baseFlags, baseRanges);
                            CFAllocatorDeallocate(alloc, baseRanges);
                        }
                        absStringIsMutable = true;
                    }
                    if (relString) CFRelease(relString);
                    CFAllocatorDeallocate(alloc, relRanges);
                }
                CFRelease(relativeString);
            }            
        }
        if ( absString ) {
            _parseComponents(alloc, absString, NULL, &absFlags, &absRanges);
            if (absFlags & HAS_PATH) {
                CFRange pathRg = _rangeForComponent(absFlags, absRanges, HAS_PATH);
                // This is expensive, but it allows us to reuse _resolvedPath.  It should be cleaned up to get this allocation removed at some point. - REW
                UniChar *buf = (UniChar *)CFAllocatorAllocate(alloc, sizeof(UniChar) * (pathRg.length + 1), 0);
                CFStringRef newPath;
                CFStringGetCharacters(absString, pathRg, buf);
                buf[pathRg.length] = '\0';
                newPath = _resolvedPath(buf, buf + pathRg.length, '/', true, false, alloc);
                if (CFStringGetLength(newPath) != pathRg.length) {
                    if (!absStringIsMutable) {
                        CFStringRef tmp = CFStringCreateMutableCopy(alloc, CFStringGetLength(absString), absString);
                        CFRelease(absString);
                        absString = tmp;
                    }
                    CFStringReplace((CFMutableStringRef)absString, pathRg, newPath);
                }
                CFRelease(newPath);
                // Do not deallocate buf; newPath took ownership of it.
            }
            CFAllocatorDeallocate(alloc, absRanges);
            absURL = _CFURLCreateWithArbitraryString(alloc, absString, NULL);
            CFRelease(absString);
            if (absURL) {
                ((struct __CFURL *)absURL)->_encoding = encoding;
#if DEBUG_URL_MEMORY_USAGE
                if ( encoding != kCFStringEncodingUTF8 ) {
                    numNonUTF8EncodedURLs++;
                }
#endif
            }
            result = absURL;
        }
    }
    
    return ( result );
}

/* This function is this way because I pulled it out of _resolvedURLPath (so that _resolvedFileSystemPath could use it), and I didn't want to spend a bunch of energy reworking the code.  So instead of being a bit more intelligent about inputs, it just demands a slightly perverse set of parameters, to match the old _resolvedURLPath code.  -- REW, 6/14/99 */
static CFStringRef _resolvedPath(UniChar *pathStr, UniChar *end, UniChar pathDelimiter, Boolean stripLeadingDotDots, Boolean stripTrailingDelimiter, CFAllocatorRef alloc) {
    UniChar *idx = pathStr;
    while (idx < end) {
        if (*idx == '.') {
            if (idx+1 == end) {
                if (idx != pathStr) {
                    *idx = '\0';
                    end = idx;
                }
                break;
            } else if (*(idx+1) == pathDelimiter) {
                if (idx + 2 != end || idx != pathStr) {
                    memmove(idx, idx+2, (end-(idx+2)+1) * sizeof(UniChar));
                    end -= 2;
                    continue;
                } else {
                    // Do not delete the sole path component
                    break;
                }
            } else if (( end-idx >= 2 ) &&  *(idx+1) == '.' && (idx+2 == end || (( end-idx > 2 ) && *(idx+2) == pathDelimiter))) {
                if (idx - pathStr >= 2) {
                    // Need at least 2 characters between index and pathStr, because we know if index != newPath, then *(index-1) == pathDelimiter, and we need something before that to compact out.
                    UniChar *lastDelim = idx-2;
                    while (lastDelim >= pathStr && *lastDelim != pathDelimiter) lastDelim --;
                    lastDelim ++;
                    if (lastDelim != idx && (idx-lastDelim != 3 || *lastDelim != '.' || *(lastDelim +1) != '.')) {
                        // We have a genuine component to compact out
                        if (idx+2 != end) {
                            unsigned numCharsToMove = end - (idx+3) + 1; // +1 to move the '\0' as well
                            memmove(lastDelim, idx+3, numCharsToMove * sizeof(UniChar));
                            end -= (idx + 3 - lastDelim);
                            idx = lastDelim;
                            continue;
                        } else if (lastDelim != pathStr) {
                            *lastDelim = '\0';
                            end = lastDelim;
                            break;
                        } else {
                            // Don't allow the path string to devolve to the empty string.  Fall back to "." instead. - REW
                            pathStr[0] = '.';
                            pathStr[1] = '/';
                            pathStr[2] = '\0';
							end = & pathStr[3];
                            break;
                        }
                    }
                } else if (stripLeadingDotDots) {
                    if (idx + 3 != end) {
                        unsigned numCharsToMove = end - (idx + 3) + 1;
                        memmove(idx, idx+3, numCharsToMove * sizeof(UniChar));
                        end -= 3;
                        continue;
                    } else {
                        // Do not devolve the last path component
                        break;
                    }
                }
            }
        }
		while (idx < end && *idx != pathDelimiter) idx ++;
        idx ++;
    }
    if (stripTrailingDelimiter && end > pathStr && end-1 != pathStr && *(end-1) == pathDelimiter) {
        end --;
    }
    return CFStringCreateWithCharactersNoCopy(alloc, pathStr, end - pathStr, alloc);
}

static CFMutableStringRef resolveAbsoluteURLString(CFAllocatorRef alloc, CFStringRef relString, UInt32 relFlags, CFRange *relRanges, CFStringRef baseString, UInt32 baseFlags, CFRange *baseRanges) {
    CFMutableStringRef newString = CFStringCreateMutable(alloc, 0);
    CFIndex bufLen = CFStringGetLength(baseString) + CFStringGetLength(relString); // Overkill, but guarantees we never allocate again
    UniChar *buf = (UniChar *)CFAllocatorAllocate(alloc, bufLen * sizeof(UniChar), 0);
    CFRange rg;
    
    rg = _rangeForComponent(baseFlags, baseRanges, HAS_SCHEME);
    if (rg.location != kCFNotFound) {
        CFStringGetCharacters(baseString, rg, buf);
        CFStringAppendCharacters(newString, buf, rg.length);
        CFStringAppendCString(newString, ":", kCFStringEncodingASCII);
    }

    if (relFlags & NET_LOCATION_MASK) {
        CFStringAppend(newString, relString);
    } else {
        CFStringAppendCString(newString, "//", kCFStringEncodingASCII);
        rg = _netLocationRange(baseFlags, baseRanges);
        if (rg.location != kCFNotFound) {
            CFStringGetCharacters(baseString, rg, buf);
            CFStringAppendCharacters(newString, buf, rg.length);
        }

        if (relFlags & HAS_PATH) {
            CFRange relPathRg = _rangeForComponent(relFlags, relRanges, HAS_PATH);
            CFRange basePathRg = _rangeForComponent(baseFlags, baseRanges, HAS_PATH);
            CFStringRef newPath;
            Boolean useRelPath = false;
            Boolean useBasePath = false;
            if (basePathRg.location == kCFNotFound) {
                useRelPath = true;
            } else if (relPathRg.length == 0) {
                useBasePath = true;
            } else if (CFStringGetCharacterAtIndex(relString, relPathRg.location) == '/') {
                useRelPath = true;
            } else if (basePathRg.location == kCFNotFound || basePathRg.length == 0) {
                useRelPath = true;
            }
            if (useRelPath) {
                newPath = CFStringCreateWithSubstring(alloc, relString, relPathRg);
            } else if (useBasePath) {
                newPath = CFStringCreateWithSubstring(alloc, baseString, basePathRg);
            } else {
                // #warning FIXME - Get rid of this allocation
                UniChar *newPathBuf = (UniChar *)CFAllocatorAllocate(alloc, sizeof(UniChar) * (relPathRg.length + basePathRg.length + 1), 0);
                UniChar *idx, *end;
                CFStringGetCharacters(baseString, basePathRg, newPathBuf);
                idx = newPathBuf + basePathRg.length - 1;
                while (idx != newPathBuf && *idx != '/') idx --;
                if (*idx == '/') idx ++;
                CFStringGetCharacters(relString, relPathRg, idx);
                end = idx + relPathRg.length;
                *end = 0;
                newPath = _resolvedPath(newPathBuf, end, '/', false, false, alloc);
            }
            /* Under Win32 absolute path can begin with letter
             * so we have to add one '/' to the newString
             * (Sergey Zubarev)
             */
             // No - the input strings here are URL path strings, not Win32 paths.  
             // Absolute paths should have had a '/' prepended before this point. 
             // I have removed Sergey Zubarev's change and left his comment (and
             // this one) as a record. - REW, 1/5/2004
            
            // if the relative URL does not begin with a slash and
            // the base does not end with a slash, add a slash
            if ((basePathRg.location == kCFNotFound || basePathRg.length == 0) && CFStringGetCharacterAtIndex(newPath, 0) != '/') {
                CFStringAppendCString(newString, "/", kCFStringEncodingASCII);
            }
            
            CFStringAppend(newString, newPath);
            CFRelease(newPath);
            rg.location = relPathRg.location + relPathRg.length;
            rg.length = CFStringGetLength(relString);
            if (rg.length > rg.location) {
                rg.length -= rg.location; 
                CFStringGetCharacters(relString, rg, buf);
                CFStringAppendCharacters(newString, buf, rg.length);
            }
        } else {
            rg = _rangeForComponent(baseFlags, baseRanges, HAS_PATH);
            if (rg.location != kCFNotFound) {
                CFStringGetCharacters(baseString, rg, buf);
                CFStringAppendCharacters(newString, buf, rg.length);
            }

            if (!(relFlags & RESOURCE_SPECIFIER_MASK)) {
                // ???  Can this ever happen?
                UInt32 rsrcFlag = _firstResourceSpecifierFlag(baseFlags);
                if (rsrcFlag) {
                    rg.location = _rangeForComponent(baseFlags, baseRanges, rsrcFlag).location;
                    rg.length = CFStringGetLength(baseString) - rg.location;
                    rg.location --; // To pick up the separator
                    rg.length ++;
                    CFStringGetCharacters(baseString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                }
            } else if (relFlags & HAS_PARAMETERS) {
                rg = _rangeForComponent(relFlags, relRanges, HAS_PARAMETERS);
                rg.location --; // To get the semicolon that starts the parameters
                rg.length = CFStringGetLength(relString) - rg.location;
                CFStringGetCharacters(relString, rg, buf);
                CFStringAppendCharacters(newString, buf, rg.length);
            } else {
                // Sigh; we have to resolve these against one another
                rg = _rangeForComponent(baseFlags, baseRanges, HAS_PARAMETERS);
                if (rg.location != kCFNotFound) {
                    CFStringAppendCString(newString, ";", kCFStringEncodingASCII);
                    CFStringGetCharacters(baseString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                }
                rg = _rangeForComponent(relFlags, relRanges, HAS_QUERY);
                if (rg.location != kCFNotFound) {
                    CFStringAppendCString(newString, "?", kCFStringEncodingASCII);
                    CFStringGetCharacters(relString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                } else {
                    rg = _rangeForComponent(baseFlags, baseRanges, HAS_QUERY);
                    if (rg.location != kCFNotFound) {
                        CFStringAppendCString(newString, "?", kCFStringEncodingASCII);
                        CFStringGetCharacters(baseString, rg, buf);
                        CFStringAppendCharacters(newString, buf, rg.length);
                    }
                }
                // Only the relative portion of the URL can supply the fragment; otherwise, what would be in the relativeURL?
                rg = _rangeForComponent(relFlags, relRanges, HAS_FRAGMENT);
                if (rg.location != kCFNotFound) {
                    CFStringAppendCString(newString, "#", kCFStringEncodingASCII);
                    CFStringGetCharacters(relString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                }
            }
        }
    }
    CFAllocatorDeallocate(alloc, buf);
    return newString;
}

CFURLRef CFURLCopyAbsoluteURL(CFURLRef  relativeURL) {
    CFURLRef  anURL, base;
    CFAllocatorRef alloc = CFGetAllocator(relativeURL);
    CFStringRef baseString, newString;
    UInt32 baseFlags;
    CFRange *baseRanges;
    Boolean baseIsObjC;

    CFAssert1(relativeURL != NULL, __kCFLogAssertion, "%s(): Cannot create an absolute URL from a NULL relative URL", __PRETTY_FUNCTION__);
    if (CF_IS_OBJC(__kCFURLTypeID, relativeURL)) {
        anURL = (CFURLRef) CF_OBJC_CALLV((NSURL *)relativeURL, absoluteURL);
        if (anURL) CFRetain(anURL);
        return anURL;
    } 

    __CFGenericValidateType(relativeURL, __kCFURLTypeID);

    base = relativeURL->_base;
    if (!base) {
        return (CFURLRef)CFRetain(relativeURL);
    }
    baseIsObjC = CF_IS_OBJC(__kCFURLTypeID, base);

    if (!baseIsObjC) {
        baseString = base->_string;
        baseFlags = base->_flags;
        baseRanges = base->_ranges;
    } else {
        baseString = CFURLGetString(base);
        baseFlags = 0;
        baseRanges = NULL;
        _parseComponents(alloc, baseString, NULL, &baseFlags, &baseRanges);
    }
    
    newString = resolveAbsoluteURLString(alloc, relativeURL->_string, relativeURL->_flags, relativeURL->_ranges, baseString, baseFlags, baseRanges);
    if (baseIsObjC) {
        CFAllocatorDeallocate(alloc, baseRanges);
    }
    anURL = _CFURLCreateWithArbitraryString(alloc, newString, NULL);
    CFRelease(newString);
    ((struct __CFURL *)anURL)->_encoding = relativeURL->_encoding;
#if DEBUG_URL_MEMORY_USAGE
    if ( relativeURL->_encoding != kCFStringEncodingUTF8 ) {
	numNonUTF8EncodedURLs++;
    }
#endif
    return anURL;
}


/*******************/
/* Basic accessors */
/*******************/
CFStringEncoding _CFURLGetEncoding(CFURLRef url) {
    return url->_encoding;
}

Boolean CFURLCanBeDecomposed(CFURLRef  anURL) {
    anURL = _CFURLFromNSURL(anURL);
    return ((anURL->_flags & IS_DECOMPOSABLE) != 0);
}

CFStringRef  CFURLGetString(CFURLRef  url) {
    CF_OBJC_FUNCDISPATCHV(__kCFURLTypeID, CFStringRef, (NSURL *)url, relativeString);
    if (!_haveTestedOriginalString(url)) {
        computeSanitizedString(url);
    }
    if (url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) {
        return url->_string;
    } else {
        return _getSanitizedString( url );
    }
}

CFIndex CFURLGetBytes(CFURLRef url, UInt8 *buffer, CFIndex bufferLength) {
    CFIndex length, charsConverted, usedLength;
    CFStringRef string;
    CFStringEncoding enc;
    if (CF_IS_OBJC(__kCFURLTypeID, url)) {
        string = CFURLGetString(url);
        enc = kCFStringEncodingUTF8;
    } else {
        string = url->_string;
        enc = url->_encoding;
    }
    length = CFStringGetLength(string);
    charsConverted = CFStringGetBytes(string, CFRangeMake(0, length), enc, 0, false, buffer, bufferLength, &usedLength);
    if (charsConverted != length) {
        return -1;
    } else {
        return usedLength;
    }
}

CFURLRef  CFURLGetBaseURL(CFURLRef  anURL) {
    CF_OBJC_FUNCDISPATCHV(__kCFURLTypeID, CFURLRef, (NSURL *)anURL, baseURL);
    return anURL->_base;
}

// Assumes the URL is already parsed
static CFRange _rangeForComponent(UInt32 flags, CFRange *ranges, UInt32 compFlag) {
    UInt32 idx = 0;
    if (!(flags & compFlag)) return CFRangeMake(kCFNotFound, 0);
    while (!(compFlag & 1)) {
        compFlag = compFlag >> 1;
        if (flags & 1) {
            idx ++;
        }
        flags = flags >> 1;
    }
    return ranges[idx];
}
 
static CFStringRef _retainedComponentString(CFURLRef url, UInt32 compFlag, Boolean fromOriginalString, Boolean removePercentEscapes) {
    CFRange rg;
    CFStringRef comp;
    CFAllocatorRef alloc = CFGetAllocator(url);
    if (removePercentEscapes) {
        fromOriginalString = true;
    }
    rg = _rangeForComponent(url->_flags, url->_ranges, compFlag);
    if (rg.location == kCFNotFound) {
        comp = NULL;
    }
    else {
        if ( compFlag & HAS_SCHEME ) {
            switch ( _getSchemeTypeFromFlags(url->_flags) ) {
                case kHasHttpScheme:
                    comp = (CFStringRef)CFRetain(kCFURLHTTPScheme);
                    break;
                    
                case kHasHttpsScheme:
                    comp = (CFStringRef)CFRetain(kCFURLHTTPSScheme);
                    break;
                    
                case kHasFileScheme:
                    comp = (CFStringRef)CFRetain(kCFURLFileScheme);
                    break;
                    
                case kHasDataScheme:
                    comp = (CFStringRef)CFRetain(kCFURLDataScheme);
                    break;
                    
                case kHasFtpScheme:
                    comp = (CFStringRef)CFRetain(kCFURLFTPScheme);
                    break;
                    
                default:
                    comp = CFStringCreateWithSubstring(alloc, url->_string, rg);
                    break;
            }
        }
        else {
            comp = CFStringCreateWithSubstring(alloc, url->_string, rg);
        }
        
        if (!fromOriginalString) {
            if (!_haveTestedOriginalString(url)) {
                computeSanitizedString(url);
            }
            if (!(url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) && (_getAdditionalDataFlags(url) & compFlag)) {
                CFStringRef newComp = correctedComponent(comp, compFlag, url->_encoding);
                CFRelease(comp);
                comp = newComp;
            }
        }
        if (removePercentEscapes) {
            CFStringRef tmp;
            if (url->_encoding == kCFStringEncodingUTF8) {
                tmp = CFURLCreateStringByReplacingPercentEscapes(alloc, comp, CFSTR(""));
            } else {
                tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(alloc, comp, CFSTR(""), url->_encoding);
            }
            CFRelease(comp);
            comp = tmp;
        }
        
    }
    return comp;
}

CFStringRef  CFURLCopyScheme(CFURLRef  anURL) {
    CFStringRef scheme;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        scheme = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, scheme);
        if ( scheme ) {
            CFRetain(scheme);
        }
    }
    else {
        switch ( _getSchemeTypeFromFlags(anURL->_flags) ) {
            case kHasHttpScheme:
                scheme = (CFStringRef)CFRetain(kCFURLHTTPScheme);
                break;
                
            case kHasHttpsScheme:
                scheme = (CFStringRef)CFRetain(kCFURLHTTPSScheme);
                break;
                
            case kHasFileScheme:
                scheme = (CFStringRef)CFRetain(kCFURLFileScheme);
                break;
                
            case kHasDataScheme:
                scheme = (CFStringRef)CFRetain(kCFURLDataScheme);
                break;
                
            case kHasFtpScheme:
                scheme = (CFStringRef)CFRetain(kCFURLFTPScheme);
                break;
                
            default:
                scheme = _retainedComponentString(anURL, HAS_SCHEME, true, false);
                if ( !scheme ) {
                    if (anURL->_base) {
                        scheme = CFURLCopyScheme(anURL->_base);
                    } else {
                        scheme = NULL;
                    }
                }
                break;
        }
    }
    return ( scheme );
}

static CFRange _netLocationRange(UInt32 flags, CFRange *ranges) {
    CFRange netRgs[4];
    CFRange netRg = {kCFNotFound, 0};
    CFIndex i, c = 4;

    if ((flags & NET_LOCATION_MASK) == 0) return CFRangeMake(kCFNotFound, 0);

    netRgs[0] = _rangeForComponent(flags, ranges, HAS_USER);
    netRgs[1] = _rangeForComponent(flags, ranges, HAS_PASSWORD);
    netRgs[2] = _rangeForComponent(flags, ranges, HAS_HOST);
    netRgs[3] = _rangeForComponent(flags, ranges, HAS_PORT);
    for (i = 0; i < c; i ++) {
        if (netRgs[i].location == kCFNotFound) continue;
        if (netRg.location == kCFNotFound) {
            netRg = netRgs[i];
        } else {
            netRg.length = netRgs[i].location + netRgs[i].length - netRg.location;
        }
    }
    return netRg;
}

CFStringRef CFURLCopyNetLocation(CFURLRef  anURL) {
    anURL = _CFURLFromNSURL(anURL);
    if (anURL->_flags & NET_LOCATION_MASK) {
        // We provide the net location
        CFRange netRg = _netLocationRange(anURL->_flags, anURL->_ranges);
        CFStringRef netLoc;
        if (!_haveTestedOriginalString(anURL)) {
            computeSanitizedString(anURL);
        }
        if (!(anURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) && (_getAdditionalDataFlags(anURL) & (USER_DIFFERS | PASSWORD_DIFFERS | HOST_DIFFERS | PORT_DIFFERS))) {
            // Only thing that can come before the net location is the scheme.  It's impossible for the scheme to contain percent escapes.  Therefore, we can use the location of netRg in _sanatizedString, just not the length. 
            CFRange netLocEnd;
            CFStringRef sanitizedString = _getSanitizedString(anURL);
            netRg.length = CFStringGetLength(sanitizedString) - netRg.location;
            if (CFStringFindWithOptions(sanitizedString, CFSTR("/"), netRg, 0, &netLocEnd)) {
                netRg.length = netLocEnd.location - netRg.location;
            }
            netLoc = CFStringCreateWithSubstring(CFGetAllocator(anURL), sanitizedString, netRg);
        } else {
            netLoc = CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_string, netRg);
        }
        return netLoc;
    } else if (anURL->_base) {
        return CFURLCopyNetLocation(anURL->_base);
    } else {
        return NULL;
    }
}

// NOTE - if you want an absolute path, you must first get the absolute URL.  If you want a file system path, use the file system methods above.
CFStringRef  CFURLCopyPath(CFURLRef  anURL) {
    anURL = _CFURLFromNSURL(anURL);
    return _retainedComponentString(anURL, HAS_PATH, false, false);
}

/* NULL if CFURLCanBeDecomposed(anURL) is false; also does not resolve the URL against its base.  See also CFCreateAbsoluteURL().  Note that, strictly speaking, any leading '/' is not considered part of the URL's path, although its presence or absence determines whether the path is absolute.  CFURLCopyPath()'s return value includes any leading slash (giving the path the normal POSIX appearance); CFURLCopyStrictPath()'s return value omits any leading slash, and uses isAbsolute to report whether the URL's path is absolute.

  CFURLCopyFileSystemPath() returns the URL's path as a file system path for the given path style.  All percent escape sequences are replaced.  The URL is not resolved against its base before computing the path.
*/
CFStringRef CFURLCopyStrictPath(CFURLRef anURL, Boolean *isAbsolute) {
    CFStringRef path = CFURLCopyPath(anURL);
    if (!path || CFStringGetLength(path) == 0) {
        if (path) CFRelease(path);
        if (isAbsolute) *isAbsolute = false;
        return NULL;
    }
    if (CFStringGetCharacterAtIndex(path, 0) == '/') {
        CFStringRef tmp;
        if (isAbsolute) *isAbsolute = true;
        tmp = CFStringCreateWithSubstring(CFGetAllocator(path), path, CFRangeMake(1, CFStringGetLength(path)-1));
        CFRelease(path);
        path = tmp;
    } else {
        if (isAbsolute) *isAbsolute = false;
    }
    return path;
}

Boolean CFURLHasDirectoryPath(CFURLRef  anURL) {
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (!anURL->_base || (anURL->_flags & (HAS_PATH | NET_LOCATION_MASK))) {
        return ((anURL->_flags & IS_DIRECTORY) != 0);
    }
    else {
        return CFURLHasDirectoryPath(anURL->_base);
    }
}

static UInt32 _firstResourceSpecifierFlag(UInt32 flags) {
    UInt32 firstRsrcSpecFlag = 0;
    UInt32 flag = HAS_FRAGMENT;
    while (flag != HAS_PATH) {
        if (flags & flag) {
            firstRsrcSpecFlag = flag;
        }
        flag = flag >> 1;
    }
    return firstRsrcSpecFlag;
}

CFStringRef  CFURLCopyResourceSpecifier(CFURLRef  anURL) {
    anURL = _CFURLFromNSURL(anURL);
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (!(anURL->_flags & IS_DECOMPOSABLE)) {
        CFRange schemeRg = _rangeForComponent(anURL->_flags, anURL->_ranges, HAS_SCHEME);
        CFIndex base = schemeRg.location + schemeRg.length + 1;
        if (!_haveTestedOriginalString(anURL)) {
            computeSanitizedString(anURL);
        }
        
        CFStringRef sanitizedString = _getSanitizedString(anURL);
        
        if (sanitizedString) {
            // It is impossible to have a percent escape in the scheme (if there were one, we would have considered the URL a relativeURL with a  colon in the path instead), so this range computation is always safe.
            return CFStringCreateWithSubstring(CFGetAllocator(anURL), sanitizedString, CFRangeMake(base, CFStringGetLength(sanitizedString)-base));
        } else {
            return CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_string, CFRangeMake(base, CFStringGetLength(anURL->_string)-base));
        }
    } else {
        UInt32 firstRsrcSpecFlag = _firstResourceSpecifierFlag(anURL->_flags);
        UInt32 flag;
        if (firstRsrcSpecFlag) {
            Boolean canUseOriginalString = true;
            Boolean canUseSanitizedString = true;
            CFAllocatorRef alloc = CFGetAllocator(anURL);
            if (!_haveTestedOriginalString(anURL)) {
                computeSanitizedString(anURL);
            }
            
            UInt32 additionalDataFlags = _getAdditionalDataFlags(anURL);
            CFStringRef sanitizedString = _getSanitizedString(anURL);
            
            if (!(anURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH)) {
                // See if any pieces in the resource specifier differ between sanitized string and original string
                for (flag = firstRsrcSpecFlag; flag != (HAS_FRAGMENT << 1); flag = flag << 1) {
                    if (additionalDataFlags & flag) {
                        canUseOriginalString = false;
                        break;
                    }
                }
            }
            if (!canUseOriginalString) {
                // If none of the pieces prior to the first resource specifier flag differ, then we can use the offset from the original string as the offset in the sanitized string.
                for (flag = firstRsrcSpecFlag >> 1; flag != 0; flag = flag >> 1) {
                    if (additionalDataFlags & flag) {
                        canUseSanitizedString = false;
                        break;
                    }
                }
            }
            if (canUseOriginalString) {
                CFRange rg = _rangeForComponent(anURL->_flags, anURL->_ranges, firstRsrcSpecFlag);
                rg.location --; // Include the character that demarcates the component
                rg.length = CFStringGetLength(anURL->_string) - rg.location;
                return CFStringCreateWithSubstring(alloc, anURL->_string, rg);
            } else if (canUseSanitizedString) {
                CFRange rg = _rangeForComponent(anURL->_flags, anURL->_ranges, firstRsrcSpecFlag);
                rg.location --; // Include the character that demarcates the component
                rg.length = CFStringGetLength(sanitizedString) - rg.location;
                return CFStringCreateWithSubstring(alloc, sanitizedString, rg);
            } else {
                // Must compute the correct string to return; just reparse....
                UInt32 sanFlags = 0;
                CFRange *sanRanges = NULL;
                CFRange rg; 
                _parseComponents(alloc, sanitizedString, anURL->_base, &sanFlags, &sanRanges);
                rg = _rangeForComponent(sanFlags, sanRanges, firstRsrcSpecFlag);
                CFAllocatorDeallocate(alloc, sanRanges);
                rg.location --; // Include the character that demarcates the component
                rg.length = CFStringGetLength(sanitizedString) - rg.location;
                return CFStringCreateWithSubstring(CFGetAllocator(anURL), sanitizedString, rg);
            }
        } else {
            // The resource specifier cannot possibly come from the base.
            return NULL;
        }
    }
}

/*************************************/
/* Accessors that create new objects */
/*************************************/

// For the next four methods, it is important to realize that, if a URL supplies any part of the net location (host, user, port, or password), it must supply all of the net location (i.e. none of it comes from its base URL).  Also, it is impossible for a URL to be relative, supply none of the net location, and still have its (empty) net location take precedence over its base URL (because there's nothing that precedes the net location except the scheme, and if the URL supplied the scheme, it would be absolute, and there would be no base).
CFStringRef  CFURLCopyHostName(CFURLRef  anURL) {
    CFStringRef tmp;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        tmp = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, host);
        if (tmp) CFRetain(tmp);
        return tmp;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    tmp = _retainedComponentString(anURL, HAS_HOST, true, true);
    if (tmp) {
        if (anURL->_flags & IS_IPV6_ENCODED) {
            // Have to strip off the brackets to get the true hostname.
            // Assume that to be legal the first and last characters are brackets!
            CFStringRef	strippedHost = CFStringCreateWithSubstring(CFGetAllocator(anURL), tmp, CFRangeMake(1, CFStringGetLength(tmp) - 2));
            CFRelease(tmp);
            tmp = strippedHost;
        }
        return tmp;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLCopyHostName(anURL->_base);
    } else {
        return NULL;
    }
}

// Return -1 to indicate no port is specified
SInt32 CFURLGetPortNumber(CFURLRef  anURL) {
    CFStringRef port;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        CFNumberRef cfPort = (CFNumberRef) CF_OBJC_CALLV((NSURL *)anURL, port);
        SInt32 num;
        if (cfPort && CFNumberGetValue(cfPort, kCFNumberSInt32Type, &num)) return num;
        return -1;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    port = _retainedComponentString(anURL, HAS_PORT, true, false);
    if (port) {
        SInt32 portNum, idx, length = CFStringGetLength(port);
        CFStringInlineBuffer buf;
        CFStringInitInlineBuffer(port, &buf, CFRangeMake(0, length));
        idx = 0;
        if (!__CFStringScanInteger(&buf, NULL, &idx, false, &portNum) || (idx != length)) {
            portNum = -1;
        }
        CFRelease(port);
        return portNum;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLGetPortNumber(anURL->_base);
    } else {
        return -1;
    }
}

CFStringRef  CFURLCopyUserName(CFURLRef  anURL) {
    CFStringRef user;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        user = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, user);
        if (user) CFRetain(user);
        return user;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    user = _retainedComponentString(anURL, HAS_USER, true, true);
    if (user) {
        return user;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLCopyUserName(anURL->_base);
    } else {
        return NULL;
    }
}

CFStringRef  CFURLCopyPassword(CFURLRef  anURL) {
    CFStringRef passwd;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        passwd = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, password);
        if (passwd) CFRetain(passwd);
        return passwd;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    passwd = _retainedComponentString(anURL, HAS_PASSWORD, true, true);
    if (passwd) {
        return passwd;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLCopyPassword(anURL->_base);
    } else {
        return NULL;
    }
}

// The NSURL methods do not deal with escaping escape characters at all; therefore, in order to properly bridge NSURL methods, and still provide the escaping behavior that we want, we need to create functions that match the ObjC behavior exactly, and have the public CFURL... functions call these. -- REW, 10/29/98

static CFStringRef  _unescapedParameterString(CFURLRef  anURL) {
    CFStringRef str;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        str = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, parameterString);
        if (str) CFRetain(str);
        return str;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    str = _retainedComponentString(anURL, HAS_PARAMETERS, false, false);
    if (str) return str;
    if (!(anURL->_flags & IS_DECOMPOSABLE)) return NULL;
    if (!anURL->_base || (anURL->_flags & (NET_LOCATION_MASK | HAS_PATH | HAS_SCHEME))) {
        return NULL;
        // Parameter string definitely coming from the relative portion of the URL
    }
    return _unescapedParameterString( anURL->_base);
}

CFStringRef  CFURLCopyParameterString(CFURLRef  anURL, CFStringRef charactersToLeaveEscaped) {
    CFStringRef  param = _unescapedParameterString(anURL);
    if (param) {
        CFStringRef result;
        if (anURL->_encoding == kCFStringEncodingUTF8) {
            result = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(anURL), param, charactersToLeaveEscaped);
        } else {
            result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(anURL), param, charactersToLeaveEscaped, anURL->_encoding);
        }
        CFRelease(param);
        return result;
    }
    return NULL;
}

static CFStringRef  _unescapedQueryString(CFURLRef  anURL) {
    CFStringRef str;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        str = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, query);
        if (str) CFRetain(str);
        return str;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    str = _retainedComponentString(anURL, HAS_QUERY, false, false);
    if (str) return str;
    if (!(anURL->_flags & IS_DECOMPOSABLE)) return NULL;
    if (!anURL->_base || (anURL->_flags & (HAS_SCHEME | NET_LOCATION_MASK | HAS_PATH | HAS_PARAMETERS))) {
        return NULL;
    }
    return _unescapedQueryString(anURL->_base);
}

CFStringRef  CFURLCopyQueryString(CFURLRef  anURL, CFStringRef  charactersToLeaveEscaped) {
    CFStringRef  query = _unescapedQueryString(anURL);
    if (query) {
        CFStringRef tmp;
        if (anURL->_encoding == kCFStringEncodingUTF8) {
            tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(anURL), query, charactersToLeaveEscaped);
        } else {
            tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(anURL), query, charactersToLeaveEscaped, anURL->_encoding);
        }
        CFRelease(query);
        return tmp;
    }
    return NULL;
}

// Fragments are NEVER taken from a base URL
static CFStringRef  _unescapedFragment(CFURLRef  anURL) {
    CFStringRef str;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        str = (CFStringRef) CF_OBJC_CALLV((NSURL *)anURL, fragment);
        if (str) CFRetain(str);
        return str;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    str = _retainedComponentString(anURL, HAS_FRAGMENT, false, false);
    return str;
}

CFStringRef  CFURLCopyFragment(CFURLRef  anURL, CFStringRef  charactersToLeaveEscaped) {
    CFStringRef  fragment = _unescapedFragment(anURL);
    if (fragment) {
        CFStringRef tmp;
        if (anURL->_encoding == kCFStringEncodingUTF8) {
            tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(anURL), fragment, charactersToLeaveEscaped);
        } else {
            tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(anURL), fragment, charactersToLeaveEscaped, anURL->_encoding);
        }
        CFRelease(fragment);
        return tmp;
    }
    return NULL;
}

static CFIndex insertionLocationForMask(CFURLRef url, CFOptionFlags mask) {
    CFIndex firstMaskFlag = 1;
    CFIndex lastComponentBeforeMask = 0;
    while (firstMaskFlag <= HAS_FRAGMENT) {
        if (firstMaskFlag & mask) break;
        if (url->_flags & firstMaskFlag) lastComponentBeforeMask = firstMaskFlag;
        firstMaskFlag = firstMaskFlag << 1;
    }
    if (lastComponentBeforeMask == 0) {
        // mask includes HAS_SCHEME
        return 0;
    } else if (lastComponentBeforeMask == HAS_SCHEME) {
        // Do not have to worry about the non-decomposable case here.  However, we must be prepared for the degenerate
        // case file:/path/immediately/without/host
        CFRange schemeRg = _rangeForComponent(url->_flags, url->_ranges, HAS_SCHEME);
        CFRange pathRg = _rangeForComponent(url->_flags, url->_ranges, HAS_PATH);
        if (schemeRg.length + 1 == pathRg.location) {
            return schemeRg.length + 1;
        } else {
            return schemeRg.length + 3;
        }
    } else {
        // For all other components, the separator precedes the component, so there's no need
        // to add extra chars to get to the next insertion point
        CFRange rg = _rangeForComponent(url->_flags, url->_ranges, lastComponentBeforeMask);
        return rg.location + rg.length;
    }
}

static CFRange _CFURLGetCharRangeForMask(CFURLRef url, CFOptionFlags mask, CFRange *charRangeWithSeparators) {
    CFOptionFlags currentOption;
    CFOptionFlags firstMaskFlag = HAS_SCHEME;
    Boolean haveReachedMask = false;
    CFIndex beforeMask = 0;
    CFIndex afterMask = kCFNotFound;
    CFRange *currRange = url->_ranges;
    CFRange maskRange = {kCFNotFound, 0};
    for (currentOption = 1; currentOption <= HAS_FRAGMENT; currentOption = currentOption << 1) {
        if (!haveReachedMask && (currentOption & mask) != 0) {
            firstMaskFlag = currentOption;
            haveReachedMask = true;
        }
        if (!(url->_flags & currentOption)) continue;
        if (!haveReachedMask) {
            beforeMask = currRange->location + currRange->length;
        } else if (currentOption <= mask) {
            if (maskRange.location == kCFNotFound) {
                maskRange = *currRange;
            } else {
                maskRange.length = currRange->location + currRange->length - maskRange.location;
            }
        } else {
            afterMask = currRange->location;
            break;
        }
        currRange ++;
    }
    if (afterMask == kCFNotFound) {
        afterMask = maskRange.location + maskRange.length;
    }
    charRangeWithSeparators->location = beforeMask;
    charRangeWithSeparators->length = afterMask - beforeMask;
    return maskRange;
}

static CFRange _getCharRangeInDecomposableURL(CFURLRef url, CFURLComponentType component, CFRange *rangeIncludingSeparators) {
    CFOptionFlags mask;
    switch (component) {
        case kCFURLComponentScheme: 
            mask = HAS_SCHEME; 
            break;
        case kCFURLComponentNetLocation: 
            mask = NET_LOCATION_MASK; 
            break;
        case kCFURLComponentPath: 
            mask = HAS_PATH; 
            break;
        case kCFURLComponentResourceSpecifier: 
            mask = RESOURCE_SPECIFIER_MASK; 
            break;
        case kCFURLComponentUser: 
            mask = HAS_USER; 
            break;
        case kCFURLComponentPassword:
            mask = HAS_PASSWORD;
            break;
        case kCFURLComponentUserInfo:
            mask = HAS_USER | HAS_PASSWORD;
            break;
        case kCFURLComponentHost:
            mask = HAS_HOST;
            break;
        case kCFURLComponentPort:
            mask = HAS_PORT;
            break;
        case kCFURLComponentParameterString:
            mask = HAS_PARAMETERS;
            break;
        case kCFURLComponentQuery:
            mask = HAS_QUERY;
            break;
        case kCFURLComponentFragment:
            mask = HAS_FRAGMENT;
            break;
        default:
            rangeIncludingSeparators->location = kCFNotFound;
            rangeIncludingSeparators->length = 0;
            return CFRangeMake(kCFNotFound, 0);
    }

    if ((url->_flags & mask) == 0) {
        rangeIncludingSeparators->location = insertionLocationForMask(url, mask);
        rangeIncludingSeparators->length = 0;
        return CFRangeMake(kCFNotFound, 0);
    } else {
        return _CFURLGetCharRangeForMask(url, mask, rangeIncludingSeparators);
    }
}

static CFRange _getCharRangeInNonDecomposableURL(CFURLRef url, CFURLComponentType component, CFRange *rangeIncludingSeparators) {
    if (component == kCFURLComponentScheme) {
        CFRange schemeRg = _rangeForComponent(url->_flags, url->_ranges, HAS_SCHEME);
        rangeIncludingSeparators->location = 0;
        rangeIncludingSeparators->length = schemeRg.length + 1;
        return schemeRg;
    } else if (component == kCFURLComponentResourceSpecifier) {
        CFRange schemeRg = _rangeForComponent(url->_flags, url->_ranges, HAS_SCHEME);
        CFIndex stringLength = CFStringGetLength(url->_string);
        if (schemeRg.length + 1 == stringLength) {
            rangeIncludingSeparators->location = schemeRg.length + 1;
            rangeIncludingSeparators->length = 0;
            return CFRangeMake(kCFNotFound, 0);
        } else {
            rangeIncludingSeparators->location = schemeRg.length;
            rangeIncludingSeparators->length = stringLength - schemeRg.length;
            return CFRangeMake(schemeRg.length + 1, rangeIncludingSeparators->length - 1);
        }
    } else {
        rangeIncludingSeparators->location = kCFNotFound;
        rangeIncludingSeparators->length = 0;
        return CFRangeMake(kCFNotFound, 0);
    }
    
}

CFRange CFURLGetByteRangeForComponent(CFURLRef url, CFURLComponentType component, CFRange *rangeIncludingSeparators) {
    CFRange charRange, charRangeWithSeparators;
    CFRange byteRange;
    CFAssert2(component > 0 && component < 13, __kCFLogAssertion, "%s(): passed invalid component %d", __PRETTY_FUNCTION__, component);
    url = _CFURLFromNSURL(url);

    if (!(url->_flags & IS_DECOMPOSABLE)) {
        // Special-case this because non-decomposable URLs have a slightly strange flags setup
        charRange = _getCharRangeInNonDecomposableURL(url, component, &charRangeWithSeparators);
    } else {
        charRange = _getCharRangeInDecomposableURL(url, component, &charRangeWithSeparators);
    }
    
    if (charRangeWithSeparators.location == kCFNotFound) {
        if (rangeIncludingSeparators) {
            rangeIncludingSeparators->location = kCFNotFound;
            rangeIncludingSeparators->length = 0;
        }
        return CFRangeMake(kCFNotFound, 0);
    } else if (rangeIncludingSeparators) {
        CFStringGetBytes(url->_string, CFRangeMake(0, charRangeWithSeparators.location), url->_encoding, 0, false, NULL, 0, &(rangeIncludingSeparators->location));

        if (charRange.location == kCFNotFound) {
            byteRange = charRange;
            CFStringGetBytes(url->_string, charRangeWithSeparators, url->_encoding, 0, false, NULL, 0, &(rangeIncludingSeparators->length));
        } else {
            CFIndex maxCharRange = charRange.location + charRange.length;
            CFIndex maxCharRangeWithSeparators = charRangeWithSeparators.location + charRangeWithSeparators.length;

            if (charRangeWithSeparators.location == charRange.location) {
                byteRange.location = rangeIncludingSeparators->location;
            } else {
                CFIndex numBytes;
                CFStringGetBytes(url->_string, CFRangeMake(charRangeWithSeparators.location, charRange.location - charRangeWithSeparators.location), url->_encoding, 0, false, NULL, 0, &numBytes);
                byteRange.location = charRangeWithSeparators.location + numBytes;
            }
            CFStringGetBytes(url->_string, charRange, url->_encoding, 0, false, NULL, 0, &(byteRange.length));
            if (maxCharRangeWithSeparators == maxCharRange) {
                rangeIncludingSeparators->length = byteRange.location + byteRange.length - rangeIncludingSeparators->location;
            } else {
                CFIndex numBytes;
                CFRange rg;
                rg.location = maxCharRange;
                rg.length = maxCharRangeWithSeparators - rg.location;
                CFStringGetBytes(url->_string, rg, url->_encoding, 0, false, NULL, 0, &numBytes);
                rangeIncludingSeparators->length = byteRange.location + byteRange.length + numBytes - rangeIncludingSeparators->location;
            }
        }
    } else if (charRange.location == kCFNotFound) {
        byteRange = charRange;
    } else {
        CFStringGetBytes(url->_string, CFRangeMake(0, charRange.location), url->_encoding, 0, false, NULL, 0, &(byteRange.location));
        CFStringGetBytes(url->_string, charRange, url->_encoding, 0, false, NULL, 0, &(byteRange.length));
    }
    return byteRange;
}

/* Component support */

static Boolean decomposeToNonHierarchical(CFURLRef url, CFURLComponentsNonHierarchical *components) {
    if ( CFURLGetBaseURL(url) != NULL)  {
        components->scheme = NULL;
    } else {
        components->scheme = CFURLCopyScheme(url);
    }
    components->schemeSpecific = CFURLCopyResourceSpecifier(url);
    return true;
}

static CFURLRef composeFromNonHierarchical(CFAllocatorRef alloc, const CFURLComponentsNonHierarchical *components) {
    CFStringRef str;
    if (components->scheme) {
        UniChar ch = ':';
        str = CFStringCreateMutableCopy(alloc, CFStringGetLength(components->scheme) + 1 + (components->schemeSpecific ? CFStringGetLength(components->schemeSpecific): 0), components->scheme);
        CFStringAppendCharacters((CFMutableStringRef)str, &ch, 1);
        if (components->schemeSpecific) CFStringAppend((CFMutableStringRef)str, components->schemeSpecific);
    } else if (components->schemeSpecific) {
        str = components->schemeSpecific;
        CFRetain(str);
    } else {
        str = NULL;
    }
    if (str) {
        CFURLRef url = CFURLCreateWithString(alloc, str, NULL);
        CFRelease(str);
        return url;
    } else {
        return NULL;
    }
}

static Boolean decomposeToRFC1808(CFURLRef url, CFURLComponentsRFC1808 *components) {
    CFAllocatorRef alloc = CFGetAllocator(url);
    static CFStringRef emptyStr = NULL;
    if (!emptyStr) {
        emptyStr = CFSTR("");
    }

    if (!CFURLCanBeDecomposed(url)) {
        return false;
    }
    
    CFStringRef path = CFURLCopyPath(url);
    if (path) {
        components->pathComponents = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR("/"));
        CFRelease(path);
    } else {
        components->pathComponents = NULL;
    }
    components->baseURL = CFURLGetBaseURL(url);
    if (components->baseURL)  {
        CFRetain(components->baseURL);
        components->scheme = NULL;
    } else {
        components->scheme = _retainedComponentString(url, HAS_SCHEME, true, false);
    }
    components->user = _retainedComponentString(url, HAS_USER, false, false);
    components->password = _retainedComponentString(url, HAS_PASSWORD, false, false);
    components->host = _retainedComponentString(url, HAS_HOST, false, false);
    if (url->_flags & HAS_PORT) {
        components->port = CFURLGetPortNumber(url);
    } else {
        components->port = kCFNotFound;
    }
    components->parameterString = _retainedComponentString(url, HAS_PARAMETERS, false, false);
    components->query = _retainedComponentString(url, HAS_QUERY, false, false);
    components->fragment = _retainedComponentString(url, HAS_FRAGMENT, false, false);
    return true;
}

static CFURLRef composeFromRFC1808(CFAllocatorRef alloc, const CFURLComponentsRFC1808 *comp) {
    CFMutableStringRef urlString = CFStringCreateMutable(alloc, 0);
    CFURLRef base = comp->baseURL;
    CFURLRef url;
    Boolean hadPrePathComponent = false;
    if (comp->scheme) {
        base = NULL;
        CFStringAppend(urlString, comp->scheme);
        CFStringAppend(urlString, CFSTR("://"));
        hadPrePathComponent = true;
    }
    if (comp->user || comp->password) {
        if (comp->user) {
            CFStringAppend(urlString, comp->user);
        }
        if (comp->password) {
            CFStringAppend(urlString, CFSTR(":"));
            CFStringAppend(urlString, comp->password);
        }
        CFStringAppend(urlString, CFSTR("@"));
        hadPrePathComponent = true;
    }
    if (comp->host) {
        CFStringAppend(urlString, comp->host);
        hadPrePathComponent = true;
    }
    if (comp->port != kCFNotFound) {
        CFStringAppendFormat(urlString, NULL, CFSTR(":%d"), comp->port);
        hadPrePathComponent = true;
    }

    if (hadPrePathComponent && (comp->pathComponents == NULL || CFArrayGetCount( comp->pathComponents ) == 0 || CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(comp->pathComponents, 0)) != 0)) {
        CFStringAppend(urlString, CFSTR("/"));
    }
    if (comp->pathComponents) {
        CFStringRef pathStr = CFStringCreateByCombiningStrings(alloc, comp->pathComponents, CFSTR("/"));
        CFStringAppend(urlString, pathStr);
        CFRelease(pathStr);
    }
    if (comp->parameterString) {
        CFStringAppend(urlString, CFSTR(";"));
        CFStringAppend(urlString, comp->parameterString);
    }
    if (comp->query) {
        CFStringAppend(urlString, CFSTR("?"));
        CFStringAppend(urlString, comp->query);
    }
    if (comp->fragment) {
        CFStringAppend(urlString, CFSTR("#"));
        CFStringAppend(urlString, comp->fragment);
    }
    url = CFURLCreateWithString(alloc, urlString, base);
    CFRelease(urlString);
    return url;
}

static Boolean decomposeToRFC2396(CFURLRef url, CFURLComponentsRFC2396 *comp) {
    CFAllocatorRef alloc = CFGetAllocator(url);
    CFURLComponentsRFC1808 oldComp;
    CFStringRef tmpStr;
    if (!decomposeToRFC1808(url, &oldComp)) {
        return false;
    }
    comp->scheme = oldComp.scheme;
    if (oldComp.user) {
        if (oldComp.password) {
            comp->userinfo = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@:%@"), oldComp.user, oldComp.password);
            CFRelease(oldComp.password);
            CFRelease(oldComp.user);
        } else {
            comp->userinfo = oldComp.user;
        }
    } else {
        comp->userinfo = NULL;
    }
    comp->host = oldComp.host;
    comp->port = oldComp.port;
    if (!oldComp.parameterString) {
        comp->pathComponents = oldComp.pathComponents;
    } else {
        int length = CFArrayGetCount(oldComp.pathComponents);
        comp->pathComponents = CFArrayCreateMutableCopy(alloc, length, oldComp.pathComponents);
        tmpStr = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@;%@"), CFArrayGetValueAtIndex(comp->pathComponents, length - 1), oldComp.parameterString);
        CFArraySetValueAtIndex((CFMutableArrayRef)comp->pathComponents, length - 1, tmpStr);
        CFRelease(tmpStr);
        CFRelease(oldComp.pathComponents);
        CFRelease(oldComp.parameterString);
    }
    comp->query = oldComp.query;
    comp->fragment = oldComp.fragment;
    comp->baseURL = oldComp.baseURL;
    return true;
}

static CFURLRef composeFromRFC2396(CFAllocatorRef alloc, const CFURLComponentsRFC2396 *comp) {
    CFMutableStringRef urlString = CFStringCreateMutable(alloc, 0);
    CFURLRef base = comp->baseURL;
    CFURLRef url;
    Boolean hadPrePathComponent = false;
    if (comp->scheme) {
        base = NULL;
        CFStringAppend(urlString, comp->scheme);
        CFStringAppend(urlString, CFSTR("://"));
        hadPrePathComponent = true;
    }
    if (comp->userinfo) {
        CFStringAppend(urlString, comp->userinfo);
        CFStringAppend(urlString, CFSTR("@"));
        hadPrePathComponent = true;
    }
    if (comp->host) {
        CFStringAppend(urlString, comp->host);
        if (comp->port != kCFNotFound) {
            CFStringAppendFormat(urlString, NULL, CFSTR(":%d"), comp->port);
        }
        hadPrePathComponent = true;
    }
    if (hadPrePathComponent && (comp->pathComponents == NULL || CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(comp->pathComponents, 0)) != 0)) {
        CFStringAppend(urlString, CFSTR("/"));
    }
    if (comp->pathComponents) {
        CFStringRef pathStr = CFStringCreateByCombiningStrings(alloc, comp->pathComponents, CFSTR("/"));
        CFStringAppend(urlString, pathStr);
        CFRelease(pathStr);
    }
    if (comp->query) {
        CFStringAppend(urlString, CFSTR("?"));
        CFStringAppend(urlString, comp->query);
    }
    if (comp->fragment) {
        CFStringAppend(urlString, CFSTR("#"));
        CFStringAppend(urlString, comp->fragment);
    }
    url = CFURLCreateWithString(alloc, urlString, base);
    CFRelease(urlString);
    return url;
}

#undef CFURLCopyComponents
#undef CFURLCreateFromComponents

CF_EXPORT
Boolean _CFURLCopyComponents(CFURLRef url, CFURLComponentDecomposition decompositionType, void *components) {
    url = _CFURLFromNSURL(url);
    switch (decompositionType) {
    case kCFURLComponentDecompositionNonHierarchical:
        return decomposeToNonHierarchical(url, (CFURLComponentsNonHierarchical *)components);
    case kCFURLComponentDecompositionRFC1808:
        return decomposeToRFC1808(url, (CFURLComponentsRFC1808 *)components);
    case kCFURLComponentDecompositionRFC2396:
        return decomposeToRFC2396(url, (CFURLComponentsRFC2396 *)components);
    default:
        return false;
    }
}

CF_EXPORT
CFURLRef _CFURLCreateFromComponents(CFAllocatorRef alloc, CFURLComponentDecomposition decompositionType, const void *components) {
    switch (decompositionType) {
    case kCFURLComponentDecompositionNonHierarchical:
        return composeFromNonHierarchical(alloc, (const CFURLComponentsNonHierarchical *)components);
    case kCFURLComponentDecompositionRFC1808:
        return composeFromRFC1808(alloc, (const CFURLComponentsRFC1808 *)components);
    case kCFURLComponentDecompositionRFC2396:
        return composeFromRFC2396(alloc, (const CFURLComponentsRFC2396 *)components);
    default:
        return NULL;
    }
}

CF_EXPORT void *__CFURLReservedPtr(CFURLRef  url) {
    return _getReserved(url);
}

CF_EXPORT void __CFURLSetReservedPtr(CFURLRef  url, void *ptr) {
    _setReserved ( (struct __CFURL*) url, ptr );
}

CF_EXPORT void *__CFURLResourceInfoPtr(CFURLRef url) {
    return _getResourceInfo(url);
}

CF_EXPORT void __CFURLSetResourceInfoPtr(CFURLRef url, void *ptr) {
    _setResourceInfo ( (struct __CFURL*) url, ptr );
}

/* File system stuff */

/* HFSPath<->URLPath functions at the bottom of the file */
static CFArrayRef WindowsPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef tmp;
    CFMutableArrayRef urlComponents = NULL;
    CFIndex i=0;

    tmp = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR("\\"));
    urlComponents = CFArrayCreateMutableCopy(alloc, 0, tmp);
    CFRelease(tmp);

    CFStringRef str = (CFStringRef)CFArrayGetValueAtIndex(urlComponents, 0);
    if (CFStringGetLength(str) == 2 && CFStringGetCharacterAtIndex(str, 1) == ':') {
        CFArrayInsertValueAtIndex(urlComponents, 0, CFSTR("")); // So we get a leading '/' below
        i = 2; // Skip over the drive letter and the empty string we just inserted
    }
    CFIndex c;
    for (c = CFArrayGetCount(urlComponents); i < c; i ++) {
        CFStringRef fileComp = (CFStringRef)CFArrayGetValueAtIndex(urlComponents,i);
        CFStringRef urlComp = _replacePathIllegalCharacters(fileComp, alloc, false);
        if (!urlComp) {
            // Couldn't decode fileComp
            CFRelease(urlComponents);
            return NULL;
        }
        if (urlComp != fileComp) {
            CFArraySetValueAtIndex(urlComponents, i, urlComp);
        }
        CFRelease(urlComp);
    }

    if (isDir) {
        if (CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(urlComponents, CFArrayGetCount(urlComponents) - 1)) != 0)
            CFArrayAppendValue(urlComponents, CFSTR(""));
    }
    return urlComponents;
}

static CFStringRef WindowsPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef urlComponents;
    CFStringRef str;

    if (CFStringGetLength(path) == 0) return CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);
    urlComponents = WindowsPathToURLComponents(path, alloc, isDir);
    if (!urlComponents) return CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);

    // WindowsPathToURLComponents already added percent escapes for us; no need to add them again here.
    str = CFStringCreateByCombiningStrings(alloc, urlComponents, CFSTR("/"));
    CFRelease(urlComponents);
    return str;
}

static CFStringRef POSIXPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDirectory) {
    CFStringRef pathString = _replacePathIllegalCharacters(path, alloc, true);
    if (isDirectory && CFStringGetCharacterAtIndex(path, CFStringGetLength(path)-1) != '/') {
        CFStringRef tmp = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@/"), pathString);
        CFRelease(pathString);
        pathString = tmp;
    }
    return pathString;
}

static CFStringRef URLPathToPOSIXPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding) {
    // This is the easiest case; just remove the percent escape codes and we're done
    CFStringRef result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, path, CFSTR(""), encoding);
    if (result) {
        CFIndex length = CFStringGetLength(result);
        if (length > 1 && CFStringGetCharacterAtIndex(result, length-1) == '/') {
            CFStringRef tmp = CFStringCreateWithSubstring(allocator, result, CFRangeMake(0, length-1));
            CFRelease(result);
            result = tmp;
        }
        }
    return result;
                }

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX
static Boolean CanonicalFileURLStringToFileSystemRepresentation(CFStringRef str, CFAllocatorRef alloc, UInt8 *inBuffer, CFIndex inBufferLen)
{
    Boolean result;
    if ( inBuffer && inBufferLen ) {
        STACK_BUFFER_DECL(UInt8, stackEscapedBuf, PATH_MAX * 3);    // worst case size is every unicode code point could be a 3-byte UTF8 sequence
        UInt8 *escapedBuf;
        CFIndex strLength = CFStringGetLength(str) - (sizeof(fileURLPrefixWithAuthority) - 1);
        if ( strLength != 0 ) {
            CFIndex maxBufLength = strLength * 3;
            CFIndex usedBufLen;
            CFIndex charsConverted;
            if ( strLength <= PATH_MAX ) {
                escapedBuf = &stackEscapedBuf[0];
            }
            else {
                // worst case size is every unicode code point could be a 3-byte UTF8 sequence
                escapedBuf = (UInt8 *)malloc(maxBufLength);
            }
            if ( escapedBuf != NULL ) {
                charsConverted = CFStringGetBytes(str, CFRangeMake(sizeof(fileURLPrefixWithAuthority) - 1, strLength), kCFStringEncodingUTF8, 0, false, escapedBuf, maxBufLength, &usedBufLen);
                if ( charsConverted ) {
                    static const UInt8 hexvalues[] = {
                        /* 00 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 08 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 10 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 18 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 20 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 28 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 30 */  0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                        /* 38 */  0x8, 0x9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 40 */  0x0, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x0,
                        /* 48 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 50 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 58 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 60 */  0x0, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x0,
                        /* 68 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 70 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 78 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        
                        /* 80 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 88 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 90 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* 98 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* A0 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* A8 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* B0 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* B8 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* C0 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* C8 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* D0 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* D8 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* E0 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* E8 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* F0 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        /* F8 */  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                    };
                    UInt8 *bufStartPtr;
                    UInt8 *bufEndPtr;
                    UInt8 *bufPtr;
                    const UInt8 *bytePtr = escapedBuf;
                    CFIndex idx;
                    Boolean trailingSlash = false;
                    
                    bufPtr = bufStartPtr = inBuffer;
                    bufEndPtr = inBuffer + inBufferLen;
                    result = TRUE;
                    
                    for ( idx = 0; (idx < usedBufLen) && result; ++idx ) {
                        if ( bufPtr == bufEndPtr ) {
                            // ooops, ran out of inBuffer
                            *bufStartPtr = '\0';
                            result = FALSE;
                        }
                        else {
                            switch ( *bytePtr ) {
                                case '%':
                                    idx += 2;
                                    if ( idx < usedBufLen ) {
                                        // skip over %
                                        bytePtr++;
                                        // convert hex digits
                                        *bufPtr = hexvalues[*bytePtr++] << 4;
                                        *bufPtr += hexvalues[*bytePtr++];
                                        trailingSlash = (*bufPtr == '/');
                                    }
                                    break;
                                default:
                                    // copy everything else
                                    *bufPtr = *bytePtr++;
                                    trailingSlash = (*bufPtr == '/');
                                    break;
                            }
                            ++bufPtr;
                        }
                    }
                    if ( result ) {
                        // remove trailing slash (if any)
                        if ( (bufPtr > (bufStartPtr + 1)) && trailingSlash ) {
                            --bufPtr;
                        }
                        if ( bufPtr < bufEndPtr ) {
                            *bufPtr = '\0';
                        }
                    }
                }
                else {
                    // CFStringGetBytes failed
                    result = FALSE;
                }
                
                // free the buffer if we malloc'd it
                if ( escapedBuf != &stackEscapedBuf[0] ) {
                    free(escapedBuf);
                }
            }
            else {
                // could not allocate escapedBuf
                result = FALSE;
            }
        }
        else {
            // str was zero characters
            *inBuffer = '\0';
            result = TRUE;
        }
    }
    else {
        // no inBuffer or inBufferLen is zero
        result = FALSE;
    }
    
    return ( result );
}
#endif

#if DEPLOYMENT_TARGET_WINDOWS
// From CFPlatform.c
extern CFStringRef CFCreateWindowsDrivePathFromVolumeName(CFStringRef volNameStr);
#endif

static CFStringRef URLPathToWindowsPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding) {
    // Check for a drive letter, then flip all the slashes
    CFStringRef result;
    CFArrayRef tmp = CFStringCreateArrayBySeparatingStrings(allocator, path, CFSTR("/"));
    SInt32 count = CFArrayGetCount(tmp);
    CFMutableArrayRef components = CFArrayCreateMutableCopy(allocator, count, tmp);
    CFStringRef newPath;
    
    
    
    CFRelease(tmp);
    if (CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(components,count-1)) == 0) {
        CFArrayRemoveValueAtIndex(components, count-1);
        count --;
    }
    
    if (count > 1 && CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(components, 0)) == 0) {
        // Absolute path; we need to check for a drive letter in the second component, and if so, remove the first component
        CFStringRef firstComponent = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, (CFStringRef)CFArrayGetValueAtIndex(components, 1), CFSTR(""), encoding);
        UniChar ch;

	{
            if (firstComponent) {
		if (CFStringGetLength(firstComponent) == 2 && ((ch = CFStringGetCharacterAtIndex(firstComponent, 1)) == '|' || ch == ':')) {
		    // Drive letter
		    CFArrayRemoveValueAtIndex(components, 0);
		    if (ch == '|') {
			CFStringRef driveStr = CFStringCreateWithFormat(allocator, NULL, CFSTR("%c:"), CFStringGetCharacterAtIndex(firstComponent, 0));
			CFArraySetValueAtIndex(components, 0, driveStr);
			CFRelease(driveStr);
		    }
		}
#if DEPLOYMENT_TARGET_WINDOWS
		else {
		    // From <rdar://problem/5623405> [DEFECT] CFURL returns a Windows path that contains volume name instead of a drive letter
		    // we need to replace the volume name (it is not valid on Windows) with the drive mounting point path
		    // remove the first component and set the component with the drive letter to be the first component
		    CFStringRef driveRootPath = CFCreateWindowsDrivePathFromVolumeName(firstComponent);
		    
		    if (driveRootPath) {
			// remove trailing slash
			if (CFStringHasSuffix(driveRootPath, CFSTR("\\"))) {
			    CFStringRef newDriveRootPath = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, driveRootPath, CFRangeMake(0, CFStringGetLength(driveRootPath) - 1));
			    CFRelease(driveRootPath);
			    driveRootPath = newDriveRootPath;
			}
			
			// replace the first component of the path with the drive path
			CFArrayRemoveValueAtIndex(components, 0);
			CFArraySetValueAtIndex(components, 0, driveRootPath);
			
			CFRelease(driveRootPath);
		    }
		}
#endif
	    }
        }
        if ( firstComponent ) {
            CFRelease(firstComponent);
        }
    }
    newPath = CFStringCreateByCombiningStrings(allocator, components, CFSTR("\\"));
    CFRelease(components);
    result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, newPath, CFSTR(""), encoding);
    CFRelease(newPath);
    return result;
}



// converts url from a file system path representation to a standard representation
static void _convertToURLRepresentation(struct __CFURL *url, UInt32 fsType) {
    CFStringRef path = NULL;
    Boolean isDir = ((url->_flags & IS_DIRECTORY) != 0);
    Boolean isFileReferencePath = false;
    CFAllocatorRef alloc = CFGetAllocator(url);
    
    switch (fsType) {
        case kCFURLPOSIXPathStyle:
            isFileReferencePath = _pathHasFileIDPrefix(url->_string);
            if (url->_flags & POSIX_AND_URL_PATHS_MATCH) {
                path = (CFStringRef)CFRetain(url->_string);
            } else {
                path = POSIXPathToURLPath(url->_string, alloc, isDir);
            }
            break;
        case kCFURLWindowsPathStyle:
            path = WindowsPathToURLPath(url->_string, alloc, isDir);
            break;
    }
    CFAssert2(path != NULL, __kCFLogAssertion, "%s(): Encountered malformed file system URL %@", __PRETTY_FUNCTION__, url);
    if ( path )
    {
        if (!url->_base) {
            CFMutableStringRef str = CFStringCreateMutable(alloc, 0);
            CFStringAppend(str, isFileReferencePath ? CFSTR(FILE_PREFIX) : CFSTR(FILE_PREFIX_WITH_AUTHORITY));
            CFStringAppend(str, path);
            url->_flags = (url->_flags & (IS_DIRECTORY)) | IS_DECOMPOSABLE | IS_ABSOLUTE | HAS_SCHEME | HAS_PATH | ORIGINAL_AND_URL_STRINGS_MATCH | ( isFileReferencePath ? PATH_HAS_FILE_ID : HAS_HOST );
            _setSchemeTypeInFlags(&url->_flags, kHasFileScheme);
            CFRelease(url->_string);
            url->_string = CFStringCreateCopy(alloc, str);
            CFRelease(str);
            if (isFileReferencePath) {
                url->_ranges = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange) * 2, 0);
                url->_ranges[0] = CFRangeMake(0, 4); // scheme "file"
                url->_ranges[1] = CFRangeMake(7, CFStringGetLength(path)); // path
            }
            else {
                url->_ranges = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange) * 3, 0);
                url->_ranges[0] = CFRangeMake(0, 4); // scheme "file"
                url->_ranges[1] = CFRangeMake(7, 9); // host "localhost"
                url->_ranges[2] = CFRangeMake(16, CFStringGetLength(path)); // path
            }
            CFRelease(path);
        } else {
            CFRelease(url->_string);
            url->_flags = (url->_flags & (IS_DIRECTORY)) | IS_DECOMPOSABLE | HAS_PATH | ORIGINAL_AND_URL_STRINGS_MATCH;
            url->_string = CFStringCreateCopy(alloc, path);
            CFRelease(path);
            url->_ranges = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
            *(url->_ranges) = CFRangeMake(0, CFStringGetLength(url->_string));
        }
    }
}

// Caller must release the returned string
static CFStringRef _resolveFileSystemPaths(CFStringRef relativePath, CFStringRef basePath, Boolean baseIsDir, CFURLPathStyle fsType, CFAllocatorRef alloc) {
    CFIndex baseLen = CFStringGetLength(basePath);
    CFIndex relLen = CFStringGetLength(relativePath);
    UniChar pathDelimiter = '/';
    UniChar *buf = (UniChar *)CFAllocatorAllocate(alloc, sizeof(UniChar)*(relLen + baseLen + 2), 0);
    CFStringGetCharacters(basePath, CFRangeMake(0, baseLen), buf);
    if (baseIsDir) {
        if (buf[baseLen-1] != pathDelimiter) {
            buf[baseLen] = pathDelimiter;
            baseLen ++;
        }
    } else {
        UniChar *ptr = buf + baseLen - 1;
        while (ptr > buf && *ptr != pathDelimiter) {
            ptr --;
        }
        baseLen = ptr - buf + 1;
    }
    if (fsType == kCFURLHFSPathStyle) {
        // HFS relative paths will begin with a colon, so we must remove the trailing colon from the base path first.
        baseLen --;
    }
    CFStringGetCharacters(relativePath, CFRangeMake(0, relLen), buf + baseLen);
    *(buf + baseLen + relLen) = '\0';
    return _resolvedPath(buf, buf + baseLen + relLen, pathDelimiter, false, true, alloc);
}

CFURLRef _CFURLCreateCurrentDirectoryURL(CFAllocatorRef allocator) {
    CFURLRef url = NULL;
    uint8_t buf[CFMaxPathSize + 1];
    if (_CFGetCurrentDirectory((char *)buf, CFMaxPathLength)) {
        url = CFURLCreateFromFileSystemRepresentation(allocator, buf, strlen((char *)buf), true);
    }
    return url;
}

CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef allocator, CFStringRef filePath, CFURLPathStyle fsType, Boolean isDirectory) {
    Boolean isAbsolute = true;
    CFIndex len;
    CFURLRef baseURL, result;

    CFAssert2(fsType == kCFURLPOSIXPathStyle || fsType == kCFURLHFSPathStyle || fsType == kCFURLWindowsPathStyle, __kCFLogAssertion, "%s(): encountered unknown path style %d", __PRETTY_FUNCTION__, fsType);
    
    CFAssert1(filePath != NULL, __kCFLogAssertion, "%s(): NULL filePath argument not permitted", __PRETTY_FUNCTION__);

	len = CFStringGetLength(filePath);
	
    switch(fsType) {
        case kCFURLPOSIXPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) == '/');
            break;
        case kCFURLWindowsPathStyle:
            isAbsolute = (len >= 3 && CFStringGetCharacterAtIndex(filePath, 1) == ':' && CFStringGetCharacterAtIndex(filePath, 2) == '\\');
	    /* Absolute path under Win32 can begin with "\\"
	     * (Sergey Zubarev)
	     */
            if (!isAbsolute) isAbsolute = (len > 2 && CFStringGetCharacterAtIndex(filePath, 0) == '\\' && CFStringGetCharacterAtIndex(filePath, 1) == '\\');
            break;
        case kCFURLHFSPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) != ':');
            break;
    }
    if (isAbsolute) {
        baseURL = NULL;
    } else {
        baseURL = _CFURLCreateCurrentDirectoryURL(allocator);
    }
    result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, filePath, fsType, isDirectory, baseURL);
    if (baseURL) CFRelease(baseURL);
    return result;
}

CF_EXPORT CFURLRef CFURLCreateWithFileSystemPathRelativeToBase(CFAllocatorRef allocator, CFStringRef filePath, CFURLPathStyle fsType, Boolean isDirectory, CFURLRef baseURL) {
    CFURLRef url;
    Boolean isAbsolute = true, releaseFilePath = false, releaseBaseURL = false;
    UniChar pathDelim = '\0';
    CFIndex len;

    CFAssert1(filePath != NULL, __kCFLogAssertion, "%s(): NULL path string not permitted", __PRETTY_FUNCTION__);
    CFAssert2(fsType == kCFURLPOSIXPathStyle || fsType == kCFURLHFSPathStyle || fsType == kCFURLWindowsPathStyle, __kCFLogAssertion, "%s(): encountered unknown path style %d", __PRETTY_FUNCTION__, fsType);
    
	len = CFStringGetLength(filePath);

    switch(fsType) {
        case kCFURLPOSIXPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) == '/');
			
            pathDelim = '/';
            break;
        case kCFURLWindowsPathStyle: 
            isAbsolute = (len >= 3 && CFStringGetCharacterAtIndex(filePath, 1) == ':' && CFStringGetCharacterAtIndex(filePath, 2) == '\\');
	    /* Absolute path under Win32 can begin with "\\"
	     * (Sergey Zubarev)
	     */
            if (!isAbsolute)
                isAbsolute = (len > 2 && CFStringGetCharacterAtIndex(filePath, 0) == '\\' && CFStringGetCharacterAtIndex(filePath, 1) == '\\');
             pathDelim = '\\';
            break;
        case kCFURLHFSPathStyle: 
		{	CFRange	fullStrRange = CFRangeMake( 0, CFStringGetLength( filePath ) );
		
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) != ':');
            pathDelim = ':';
			
			if ( filePath && CFStringFindWithOptions( filePath, CFSTR("::"), fullStrRange, 0, NULL ) ) {
				UniChar *	chars = (UniChar *) malloc( fullStrRange.length * sizeof( UniChar ) );
				CFIndex index, writeIndex, firstColonOffset = -1;
								
				CFStringGetCharacters( filePath, fullStrRange, chars );

				for ( index = 0, writeIndex = 0 ; index < fullStrRange.length; index ++ ) {
					if ( chars[ index ] == ':' ) {
						if ( index + 1 < fullStrRange.length && chars[ index + 1 ] == ':' ) {

							// Don't let :: go off the 'top' of the path -- which means that there always has to be at
							//	least one ':' to the left of the current write position to go back to.
							if ( writeIndex > 0 && firstColonOffset >= 0 )
							{
								writeIndex --;
								while ( writeIndex > 0 && writeIndex >= firstColonOffset && chars[ writeIndex ] != ':' )
									writeIndex --;
							}
							index ++;	// skip over the first ':', so we replace the ':' which is there with a new one
						}
						
						if ( firstColonOffset == -1 )
							firstColonOffset = writeIndex;
					}
					
					chars[ writeIndex ++ ] = chars[ index ];
				}
								
				if ( releaseFilePath && filePath )
					CFRelease( filePath );

				filePath = CFStringCreateWithCharacters( allocator, chars, writeIndex );
				// reset len because a canonical HFS path can be a different length than the original CFString
				len = CFStringGetLength(filePath);
				releaseFilePath = true;
				
				free( chars );
			}
			
            break;
		}
    }
    if (isAbsolute) {
        baseURL = NULL;
    }
    else if ( baseURL == NULL ) {
        baseURL = _CFURLCreateCurrentDirectoryURL(allocator);
        releaseBaseURL = true;
    }

	
    if (isDirectory && len > 0 && CFStringGetCharacterAtIndex(filePath, len-1) != pathDelim) {
        CFMutableStringRef tempRef = CFStringCreateMutable(allocator, 0);
	CFStringAppend(tempRef, filePath);
	CFStringAppendCharacters(tempRef, &pathDelim, 1);
    	if ( releaseFilePath && filePath ) CFRelease( filePath );
    	filePath = tempRef;
        releaseFilePath = true;
    } else if (!isDirectory && len > 0 && CFStringGetCharacterAtIndex(filePath, len-1) == pathDelim) {
        if (len == 1 || CFStringGetCharacterAtIndex(filePath, len-2) == pathDelim) {
            // Override isDirectory
            isDirectory = true;
        } else {
            CFStringRef tempRef = CFStringCreateWithSubstring(allocator, filePath, CFRangeMake(0, len-1));
			if ( releaseFilePath && filePath )
				CFRelease( filePath );
			filePath = tempRef;
            releaseFilePath = true;
        }
    }
    if (!filePath || CFStringGetLength(filePath) == 0) {
        if (releaseFilePath && filePath) CFRelease(filePath);
        if (releaseBaseURL && baseURL) CFRelease(baseURL);
        return NULL;
    }
    url = _CFURLAlloc(allocator);
    _CFURLInit((struct __CFURL *)url, filePath, fsType, baseURL);
    if (releaseFilePath) CFRelease(filePath);
    if (releaseBaseURL && baseURL) CFRelease(baseURL);
    if (isDirectory) ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
    if (fsType == kCFURLPOSIXPathStyle) {
        // Check if relative path is equivalent to URL representation; this will be true if url->_string contains only characters from the unreserved character set, plus '/' to delimit the path, plus ';', '@', '&', '=', '+', '$', ',' (according to RFC 2396) -- REW, 12/1/2000
        // Per Section 5 of RFC 2396, there's a special problem if a colon apears in the first path segment - in this position, it can be mistaken for the scheme name.  Otherwise, it's o.k., and can be safely identified as part of the path.  In this one case, we need to prepend "./" to make it clear what's going on.... -- REW, 8/24/2001
        CFStringInlineBuffer buf;
        Boolean sawSlash = FALSE;
        Boolean mustPrependDotSlash = FALSE;
        CFIndex idx, length = CFStringGetLength(url->_string);
        CFStringInitInlineBuffer(url->_string, &buf, CFRangeMake(0, length));
        for (idx = 0; idx < length; idx ++) {
            UniChar ch = CFStringGetCharacterFromInlineBuffer(&buf, idx);
            if (!isPathLegalCharacter(ch)) break;
            if (!sawSlash) {
                if (ch == '/') {
                    sawSlash = TRUE;
                } else if (ch == ':') {
                    mustPrependDotSlash = TRUE;
                }
            }
        }
        if (idx == length) {
            ((struct __CFURL *)url)->_flags |= POSIX_AND_URL_PATHS_MATCH;
        }
        if (mustPrependDotSlash) {
            CFMutableStringRef newString = CFStringCreateMutable(allocator, 0);
	    CFStringAppend(newString, CFSTR("./"));
	    CFStringAppend(newString, url->_string);
            CFRelease(url->_string);
            ((struct __CFURL *)url)->_string = CFStringCreateCopy(allocator, newString);
            CFRelease(newString);
        }
    }
    return url;
}

static Boolean _pathHasFileIDPrefix( CFStringRef path )
{
    // path is not NULL, path has prefix "/.file/" and has at least one character following the prefix.
#ifdef __CONSTANT_STRINGS__
    static const 
#endif
    CFStringRef fileIDPrefix = CFSTR( "/" FILE_ID_PREFIX "/" );
    return path && CFStringHasPrefix( path, fileIDPrefix ) && CFStringGetLength( path ) > CFStringGetLength( fileIDPrefix );
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
static Boolean _pathHasFileIDOnly( CFStringRef path )
{
    // Is file ID rooted and contains no additonal path segments
    CFRange slashRange;
    return _pathHasFileIDPrefix( path ) && ( !CFStringFindWithOptions( path, CFSTR("/"), CFRangeMake( sizeof(FILE_ID_PREFIX) + 1, CFStringGetLength( path ) - sizeof(FILE_ID_PREFIX) - 1), 0, &slashRange ) || slashRange.location == CFStringGetLength( path ) - 1 );
}
#endif

CF_EXPORT CFStringRef CFURLCopyFileSystemPath(CFURLRef anURL, CFURLPathStyle pathStyle) {
    CFAssert2(pathStyle == kCFURLPOSIXPathStyle || pathStyle == kCFURLHFSPathStyle || pathStyle == kCFURLWindowsPathStyle, __kCFLogAssertion, "%s(): Encountered unknown path style %d", __PRETTY_FUNCTION__, pathStyle);
    
    CFStringRef result;
    
    result = CFURLCreateStringWithFileSystemPath(CFGetAllocator(anURL), anURL, pathStyle, false);
    return ( result );
}


// There is no matching ObjC method for this functionality; because this function sits on top of the CFURL primitives, it's o.k. not to check for the need to dispatch an ObjC method instead, but this means care must be taken that this function never call anything that will result in dereferencing anURL without first checking for an ObjC dispatch.  -- REW, 10/29/98
CFStringRef CFURLCreateStringWithFileSystemPath(CFAllocatorRef allocator, CFURLRef anURL, CFURLPathStyle fsType, Boolean resolveAgainstBase) {
    CFURLRef base = resolveAgainstBase ? CFURLGetBaseURL(anURL) : NULL;
    CFStringRef basePath = base ? CFURLCreateStringWithFileSystemPath(allocator, base, fsType, false) : NULL;
    CFStringRef relPath = NULL;
    
    if (!CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        // We can grope the ivars
        if (fsType == kCFURLPOSIXPathStyle) {
            if (anURL->_flags & POSIX_AND_URL_PATHS_MATCH) {
                relPath = _retainedComponentString(anURL, HAS_PATH, true, true);
            }
        }
    }

    if (relPath == NULL) {
        CFStringRef urlPath = CFURLCopyPath(anURL);
        CFStringEncoding enc = anURL->_encoding;
        if (urlPath) {
            switch (fsType) {
                case kCFURLPOSIXPathStyle:
                    relPath = URLPathToPOSIXPath(urlPath, allocator, enc);
                    break;
                case kCFURLHFSPathStyle:
		    relPath = NULL;
                    break;
                case kCFURLWindowsPathStyle:
                    relPath = URLPathToWindowsPath(urlPath, allocator, enc);
                    break;
                default:
                    CFAssert2(true, __kCFLogAssertion, "%s(): Received unknown path type %d", __PRETTY_FUNCTION__, fsType);
            }
            CFRelease(urlPath);
        }            
    }
	
    //	For Tiger, leave this behavior in for all path types.  For Leopard, it would be nice to remove this entirely
    //	and do a linked-on-or-later check so we don't break third parties.
    //	See <rdar://problem/4003028> Converting volume name from POSIX to HFS form fails and
    //	<rdar://problem/4018895> CF needs to back out 4003028 for icky details.
    if ( relPath && CFURLHasDirectoryPath(anURL) && CFStringGetLength(relPath) > 1 && CFStringGetCharacterAtIndex(relPath, CFStringGetLength(relPath)-1) == '/') {
        CFStringRef tmp = CFStringCreateWithSubstring(allocator, relPath, CFRangeMake(0, CFStringGetLength(relPath)-1));
        CFRelease(relPath);
        relPath = tmp;
    }
    
    if ( relPath ) {
        if ( basePath ) {
            // we have both basePath and relPath -- resolve them
            CFStringRef result = _resolveFileSystemPaths(relPath, basePath, CFURLHasDirectoryPath(base), fsType, allocator);
            CFRelease(basePath);
            CFRelease(relPath);
            return result;
        }
        else {
            // we only have the relPath -- return it
            return relPath;
        }
    }
    else if ( basePath ) {
        // we only have the basePath --- return it
        return basePath;
    }
    else {
        // we have nothing to return
        return NULL;
    }
}

Boolean CFURLGetFileSystemRepresentation(CFURLRef url, Boolean resolveAgainstBase, uint8_t *buffer, CFIndex bufLen) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_WINDOWS
    CFAllocatorRef alloc = CFGetAllocator(url);
    CFStringRef path;

    if (!url) return false;
#endif
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX
    if ( !resolveAgainstBase || (CFURLGetBaseURL(url) == NULL) ) {
        if (!CF_IS_OBJC(__kCFURLTypeID, url)) {
            // We can grope the ivars
            if ( url->_flags & IS_CANONICAL_FILE_URL ) {
                return CanonicalFileURLStringToFileSystemRepresentation(url->_string, alloc, buffer, bufLen);
            }
        }
    }
    // else fall back to slower way.
    path = CFURLCreateStringWithFileSystemPath(alloc, url, kCFURLPOSIXPathStyle, resolveAgainstBase);
    if (path) {
        Boolean convResult = _CFStringGetFileSystemRepresentation(path, buffer, bufLen);
        CFRelease(path);
        return convResult;
    }
#elif DEPLOYMENT_TARGET_WINDOWS
    path = CFURLCreateStringWithFileSystemPath(alloc, url, kCFURLWindowsPathStyle, resolveAgainstBase);
    if (path) {
        CFIndex usedLen;
        CFIndex pathLen = CFStringGetLength(path);
        CFIndex numConverted = CFStringGetBytes(path, CFRangeMake(0, pathLen), CFStringFileSystemEncoding(), 0, true, buffer, bufLen-1, &usedLen); // -1 because we need one byte to zero-terminate.
        CFRelease(path);
        if (numConverted == pathLen) {
            buffer[usedLen] = '\0';
            return true;
        }
    }
#endif
    return false;
}

#if DEPLOYMENT_TARGET_WINDOWS
CF_EXPORT Boolean _CFURLGetWideFileSystemRepresentation(CFURLRef url, Boolean resolveAgainstBase, wchar_t *buffer, CFIndex bufferLength) {
	CFStringRef path = CFURLCreateStringWithFileSystemPath(CFGetAllocator(url), url, kCFURLWindowsPathStyle, resolveAgainstBase);
	CFIndex pathLength, charsConverted, usedBufLen;
	if (!path) return false;
	pathLength = CFStringGetLength(path);
	if (pathLength+1 > bufferLength) {
		CFRelease(path);
		return false;
	}
	charsConverted = CFStringGetBytes(path, CFRangeMake(0, pathLength), kCFStringEncodingUTF16, 0, false, (UInt8 *)buffer, bufferLength*sizeof(wchar_t), &usedBufLen);
//	CFStringGetCharacters(path, CFRangeMake(0, pathLength), (UniChar *)buffer);
	CFRelease(path);
	if (charsConverted != pathLength || usedBufLen%sizeof(wchar_t) != 0) {
		return false;
	} else {
		buffer[usedBufLen/sizeof(wchar_t)] = 0;
//		buffer[pathLength] = 0;
		return true;
	}
}
#endif

static CFStringRef _createPathFromFileSystemRepresentation(CFAllocatorRef allocator, const uint8_t *buffer, CFIndex bufLen, Boolean isDirectory) {
    char pathDelim = PATH_SEP;
    CFStringRef path;
    if ( isDirectory ) {
	// it is a directory: if it doesn't end with pathDelim, append a pathDelim. Limit stack buffer to PATH_MAX+1 in case a large bogus value is passed.
	if ( (bufLen > 0) && (bufLen <= PATH_MAX) && (buffer[bufLen-1] != pathDelim) ) {
	    STACK_BUFFER_DECL(uint8_t, tempBuf, bufLen + 1);
	    memcpy(tempBuf, buffer, bufLen);
	    tempBuf[bufLen] = pathDelim;
	    path = CFStringCreateWithBytes(allocator, tempBuf, bufLen + 1, CFStringFileSystemEncoding(), false);
	}
	else {
	    // already had pathDelim at end of buffer or bufLen is really large
	    path = CFStringCreateWithBytes(allocator, buffer, bufLen, CFStringFileSystemEncoding(), false);
	}
    }
    else {
	// it is not a directory: remove any pathDelim characters at end (leaving at least one character)
	while ( (bufLen > 1) && (buffer[bufLen-1] == pathDelim) ) {
	    --bufLen;
	}
	path = CFStringCreateWithBytes(allocator, buffer, bufLen, CFStringFileSystemEncoding(), false);
    }
    return path;
}

/*
 CreatePOSIXFileURLStringFromPOSIXAbsolutePath creates file URL string from the native file system representation.
 
 The rules for path-absolute from rfc3986 are:
 path-absolute = "/" [ segment-nz *( "/" segment ) ]
 segment       = *pchar
 segment-nz    = 1*pchar
 pchar         = unreserved / pct-encoded / sub-delims / ":" / "@"
 pct-encoded   = "%" HEXDIG HEXDIG
 unreserved    = ALPHA / DIGIT / "-" / "." / "_" / "~"
 sub-delims    = "!" / "$" / "&" / "'" / "(" / ")" / "*" / "+" / "," / ";" / "="
 */
static CFStringRef CreatePOSIXFileURLStringFromPOSIXAbsolutePath(CFAllocatorRef alloc, const UInt8 *bytes, CFIndex numBytes, Boolean isDirectory, Boolean *addedPercentEncoding)
{
    static const UInt8 hexchars[] = "0123456789ABCDEF";
    STACK_BUFFER_DECL(UInt8, stackBuf, (PATH_MAX * 3) + sizeof(fileURLPrefixWithAuthority));    // worst case is every byte needs to be percent-escaped
    UInt8 *bufStartPtr;
    UInt8 *bufBytePtr;
    const UInt8 *bytePtr = bytes;
    CFIndex idx;
    CFStringRef result;
    Boolean addedPercent = FALSE;
    
    // choose a buffer to percent-escape into.
    if ( numBytes <= PATH_MAX ) {
        bufStartPtr = &stackBuf[0];
    }
    else {
        // worst case is every byte needs to be percent-escaped (numBytes * 3)
        bufStartPtr = (UInt8 *)malloc((numBytes * 3) + sizeof(fileURLPrefixWithAuthority));
    }
    
    if ( bufStartPtr != NULL ) {
        // start with the fileURLPrefixWithAuthority
        strcpy((char *)bufStartPtr, (char *)fileURLPrefixWithAuthority);
        bufBytePtr = bufStartPtr + sizeof(fileURLPrefixWithAuthority) - 1;
        // append the percent-escaped path
        for ( idx = 0; (idx < numBytes) && (*bytePtr != 0); ++idx ) {
            switch ( *bytePtr ) {
                    // these are the visible 7-bit ascii characters that are not legal pchar octets
                case '"':
                case '#':
                case '%':
                case ';':	// we need to percent-escape ';' in file system paths so it won't be mistaken for the start of the obsolete param rule (rfc2396) that CFURL still supports
                case '<':
                case '>':
                case '?':	// we need to percent-escape '?' in file system paths so it won't be mistaken for the start of a query
                case '[':
                case '\\':
                case ']':
                case '^':
                case '`':
                case '{':
                case '|':
                case '}':
                    // percent-escape non-pchar octets spread throughout the visible 7-bit ascii range
                    *bufBytePtr++ = '%';
                    *bufBytePtr++ = hexchars[*bytePtr >> 4];
                    *bufBytePtr++ = hexchars[*bytePtr & 0x0f];
                    addedPercent = TRUE;
                    break;
                default:
                    if ( (*bytePtr <= ' ') ||	// percent-escape non-pchar octets that are space or less (control characters)
                        (*bytePtr >= 0x7f) ) {	// percent-escape non-pchar octets that del and 8-bit ascii with the high bit set
                        *bufBytePtr++ = '%';
                        *bufBytePtr++ = hexchars[*bytePtr >> 4];
                        *bufBytePtr++ = hexchars[*bytePtr & 0x0f];
                        addedPercent = TRUE;
                    }
                    else {
                        // copy everything else
                        *bufBytePtr++ = *bytePtr;
                    }
                    break;
            }
            ++bytePtr;
        }
        
        // did we convert numBytes?
        if ( idx == numBytes ) {
            // if it is a directory and it doesn't end with PATH_SEP, append a PATH_SEP.
            if ( isDirectory && bytes[numBytes-1] != PATH_SEP) {
                *bufBytePtr++ = PATH_SEP;
            }
            
            // create the result
            result = CFStringCreateWithBytes(alloc, bufStartPtr, (CFIndex)(bufBytePtr-bufStartPtr), __CFStringGetEightBitStringEncoding(), FALSE);
        }
        else {
            // no, but it's OK if the remaining bytes are all nul (embedded nul bytes are not allowed)
            const UInt8 *nullBytePtr = bytePtr;
            for ( /* start where we left off */; (idx < numBytes) && (*nullBytePtr == '\0'); ++idx, ++nullBytePtr ) {
                // do nothing
            }
            if ( idx == numBytes ) {
                // create the result
                result = CFStringCreateWithBytes(alloc, bufStartPtr, (CFIndex)(bufBytePtr-bufStartPtr), __CFStringGetEightBitStringEncoding(), FALSE);
            }
            else {
                // the remaining bytes were not all nul
                result = NULL;
            }
        }
        // free the buffer if we malloc'd it
        if ( bufStartPtr != &stackBuf[0] ) {
            free(bufStartPtr);
        }
    }
    else {
        result = NULL;
    }
    
    if ( addedPercentEncoding ) {
        *addedPercentEncoding = addedPercent;
    }
    
    return ( result );
}

CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef allocator, const uint8_t *buffer, CFIndex bufLen, Boolean isDirectory) {
    CFURLRef newURL = NULL;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX
    if ( bufLen && (*buffer == '/') ) {
        Boolean addedPercentEncoding;
        
        CFStringRef urlStr = CreatePOSIXFileURLStringFromPOSIXAbsolutePath(allocator, buffer, bufLen, isDirectory, &addedPercentEncoding);
        if ( urlStr ) {
            newURL = _CFURLAlloc(allocator);
            if ( newURL ) {
                ((struct __CFURL *)newURL)->_flags |= USES_EIGHTBITSTRINGENCODING;
                // we know urlStr has a scheme and is absolute 
                _CFURLInit((struct __CFURL *)newURL, urlStr, FULL_URL_REPRESENTATION, NULL);
                ((struct __CFURL *)newURL)->_flags |= (IS_ABSOLUTE | IS_CANONICAL_FILE_URL);
                if ( !addedPercentEncoding ) {
                    ((struct __CFURL *)newURL)->_flags |= POSIX_AND_URL_PATHS_MATCH;
                }
            }
            CFRelease(urlStr);
        }
        else {
            newURL = NULL;
        }
    }
    else {
        CFStringRef path = _createPathFromFileSystemRepresentation(allocator, buffer, bufLen, isDirectory);
        if ( path ) {
            newURL = CFURLCreateWithFileSystemPath(allocator, path, kCFURLPOSIXPathStyle, isDirectory);
            CFRelease(path);
        }
        else {
            newURL = NULL;
        }
    }
#elif DEPLOYMENT_TARGET_WINDOWS
    CFStringRef path = _createPathFromFileSystemRepresentation(allocator, buffer, bufLen, isDirectory);
    if ( path ) {
        newURL = CFURLCreateWithFileSystemPath(allocator, path, kCFURLWindowsPathStyle, isDirectory);
        CFRelease(path);
    }
    else {
        newURL = NULL;
    }
#endif
    return newURL;
}

CF_EXPORT CFURLRef CFURLCreateFromFileSystemRepresentationRelativeToBase(CFAllocatorRef allocator, const uint8_t *buffer, CFIndex bufLen, Boolean isDirectory, CFURLRef baseURL) {
    CFStringRef path = _createPathFromFileSystemRepresentation(allocator, buffer, bufLen, isDirectory);
    CFURLRef newURL = NULL;
    if (!path) return NULL;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX
    newURL = CFURLCreateWithFileSystemPathRelativeToBase(allocator, path, kCFURLPOSIXPathStyle, isDirectory, baseURL);
#elif DEPLOYMENT_TARGET_WINDOWS
    newURL = CFURLCreateWithFileSystemPathRelativeToBase(allocator, path, kCFURLWindowsPathStyle, isDirectory, baseURL);
#endif
    CFRelease(path);
    return newURL;
}


/******************************/
/* Support for path utilities */
/******************************/

// Assumes url is a CFURL (not an Obj-C NSURL)
static CFRange _rangeOfLastPathComponent(CFURLRef url) {
    CFRange pathRg, componentRg;
    
    pathRg = _rangeForComponent(url->_flags, url->_ranges, HAS_PATH);

    if (pathRg.location == kCFNotFound || pathRg.length == 0) {
        // No path
        return pathRg;
    }
    if (CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) == '/') {
        pathRg.length --;
        if (pathRg.length == 0) {
            pathRg.length ++;
            return pathRg;
        }
    }
    if (CFStringFindWithOptions(url->_string, CFSTR("/"), pathRg, kCFCompareBackwards, &componentRg)) {
        componentRg.location ++;
        componentRg.length = pathRg.location + pathRg.length - componentRg.location;
    } else {
        componentRg = pathRg;
    }
    return componentRg;
}

CFStringRef CFURLCopyLastPathComponent(CFURLRef url) {
    CFStringRef result;

    if (CF_IS_OBJC(__kCFURLTypeID, url)) {
        CFStringRef path = CFURLCreateStringWithFileSystemPath(CFGetAllocator(url), url, kCFURLPOSIXPathStyle, false);
        CFIndex length;
        CFRange rg, compRg;
        if (!path) return NULL;
        rg = CFRangeMake(0, CFStringGetLength(path));
	if ( rg.length == 0 ) return path;
        length = rg.length; // Remember this for comparison later
        if (CFStringGetCharacterAtIndex(path, rg.length - 1) == '/' ) {
            rg.length --;
        }
	if ( rg.length == 0 )
	{
	    //	If we have reduced the string to empty, then it's "/", and that's what we return as
	    //	the last path component.
	   return path;
	}
        else if (CFStringFindWithOptions(path, CFSTR("/"), rg, kCFCompareBackwards, &compRg)) {
            rg.length = rg.location + rg.length - (compRg.location+1);
            rg.location = compRg.location + 1;
        }
        if (rg.location == 0 && rg.length == length) {
            result = path;
        } else {
            result = CFStringCreateWithSubstring(CFGetAllocator(url), path, rg);
            CFRelease(path);
        }
    } else {
        Boolean filePathURLCreated = false;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
        if ( _CFURLIsFileReferenceURL(url) ) {
            // use a file path URL or fail
            CFURLRef filePathURL = CFURLCreateFilePathURL(CFGetAllocator(url), url, NULL);
            if ( filePathURL ) {
                filePathURLCreated = TRUE;
                url = filePathURL;
            }
            else {
                return NULL;
            }
        }
#endif
        
        CFRange rg = _rangeOfLastPathComponent(url);
        if (rg.location == kCFNotFound || rg.length == 0) {
            // No path
            if ( filePathURLCreated ) {
                CFRelease(url);
            }
            return (CFStringRef)CFRetain(CFSTR(""));
        }
        if (rg.length == 1 && CFStringGetCharacterAtIndex(url->_string, rg.location) == '/') {
            if ( filePathURLCreated ) {
                CFRelease(url);
            }
            return (CFStringRef)CFRetain(CFSTR("/"));
        }
        result = CFStringCreateWithSubstring(CFGetAllocator(url), url->_string, rg);
        if (!(url->_flags & POSIX_AND_URL_PATHS_MATCH)) {
            CFStringRef tmp;
            if (url->_encoding == kCFStringEncodingUTF8) {
                tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(url), result, CFSTR(""));
            } else {
                tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(url), result, CFSTR(""), url->_encoding);
            }
            CFRelease(result);
            result = tmp;
        }
        if ( filePathURLCreated ) {
            CFRelease(url);
        }
    }
    return result;
}

CFStringRef CFURLCopyPathExtension(CFURLRef url) {
    CFStringRef lastPathComp = CFURLCopyLastPathComponent(url);
    CFStringRef ext = NULL;

    if (lastPathComp) {
        CFRange rg = CFStringFind(lastPathComp, CFSTR("."), kCFCompareBackwards);
        if (rg.location != kCFNotFound) {
            rg.location ++;
            rg.length = CFStringGetLength(lastPathComp) - rg.location;
            if (rg.length > 0) {
                ext = CFStringCreateWithSubstring(CFGetAllocator(url), lastPathComp, rg);
            } else {
                ext = (CFStringRef)CFRetain(CFSTR(""));
            }
        }
        CFRelease(lastPathComp);
    }
    return ext;
}

CFURLRef CFURLCreateCopyAppendingPathComponent(CFAllocatorRef allocator, CFURLRef url, CFStringRef pathComponent, Boolean isDirectory) {
    CFURLRef result;
    url = _CFURLFromNSURL(url);
    __CFGenericValidateType(url, __kCFURLTypeID);
    CFAssert1(pathComponent != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL component to append", __PRETTY_FUNCTION__);

    Boolean filePathURLCreated = false;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if ( _CFURLIsFileReferenceURL(url) ) {
        // use a file path URL if possible (only because this is appending a path component)
        CFURLRef filePathURL = CFURLCreateFilePathURL(allocator, url, NULL);
        if ( filePathURL ) {
            filePathURLCreated = TRUE;
            url = filePathURL;
        }
    }
#endif

    CFMutableStringRef newString;
    CFStringRef newComp;
    CFRange pathRg;
    if (!(url->_flags & HAS_PATH)) {
        result = NULL;
    }
    else {
        newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
        newComp = CFURLCreateStringByAddingPercentEscapes(allocator, pathComponent, NULL, CFSTR(";?"), url->_encoding);
        pathRg = _rangeForComponent(url->_flags, url->_ranges, HAS_PATH);
        if ( (!pathRg.length || CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) != '/') && (CFStringGetCharacterAtIndex(newComp, 0) != '/') ) {
            CFStringInsert(newString, pathRg.location + pathRg.length, CFSTR("/"));
            pathRg.length ++;
        }
        CFStringInsert(newString, pathRg.location + pathRg.length, newComp);
        if (isDirectory) {
            CFStringInsert(newString, pathRg.location + pathRg.length + CFStringGetLength(newComp), CFSTR("/"));
        }
        CFRelease(newComp);
        result = _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
        CFRelease(newString);
    }
    if ( filePathURLCreated ) {
        CFRelease(url);
    }
    return result;
}

CFURLRef CFURLCreateCopyDeletingLastPathComponent(CFAllocatorRef allocator, CFURLRef url) {
    CFURLRef result;
    CFMutableStringRef newString;
    CFRange lastCompRg, pathRg;
    Boolean appendDotDot = false;

    url = _CFURLFromNSURL(url);
    CFAssert1(url != NULL, __kCFLogAssertion, "%s(): NULL argument not allowed", __PRETTY_FUNCTION__);
    __CFGenericValidateType(url, __kCFURLTypeID);

    Boolean filePathURLCreated = false;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if ( _CFURLIsFileReferenceURL(url) ) {
        // use a file path URL or fail
        CFURLRef filePathURL = CFURLCreateFilePathURL(allocator, url, NULL);
        if ( filePathURL ) {
            filePathURLCreated = TRUE;
            url = filePathURL;
        }
        else {
            return NULL;
        }
    }
#endif
    
    if (!(url->_flags & HAS_PATH)) {
        if ( filePathURLCreated ) {
            CFRelease(url);
        }
        return NULL;
    }
    pathRg = _rangeForComponent(url->_flags, url->_ranges, HAS_PATH);
    lastCompRg = _rangeOfLastPathComponent(url);
    if (lastCompRg.length == 0) {
        appendDotDot = true;
    } else if (lastCompRg.length == 1) {
        UniChar ch = CFStringGetCharacterAtIndex(url->_string, lastCompRg.location);
        if (ch == '.' || ch == '/') {
            appendDotDot = true;
        }
    } else if (lastCompRg.length == 2 && CFStringGetCharacterAtIndex(url->_string, lastCompRg.location) == '.' && CFStringGetCharacterAtIndex(url->_string, lastCompRg.location+1) == '.') {
        appendDotDot = true;
    }

    newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
    if (appendDotDot) {
        CFIndex delta = 0;
        if (pathRg.length > 0 && CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) != '/') {
            CFStringInsert(newString, pathRg.location + pathRg.length, CFSTR("/"));
            delta ++;
        }
        CFStringInsert(newString, pathRg.location + pathRg.length + delta, CFSTR(".."));
        delta += 2;
        CFStringInsert(newString, pathRg.location + pathRg.length + delta, CFSTR("/"));
        delta ++;
        // We know we have "/../" at the end of the path; we wish to know if that's immediately preceded by "/." (but that "/." doesn't start the string), in which case we want to delete the "/.".
        if (pathRg.length + delta > 4 && CFStringGetCharacterAtIndex(newString, pathRg.location + pathRg.length + delta - 5) == '.') {
            if (pathRg.length+delta > 7 && CFStringGetCharacterAtIndex(newString, pathRg.location + pathRg.length + delta - 6) == '/') {
                CFStringDelete(newString, CFRangeMake(pathRg.location + pathRg.length + delta - 6, 2));
            } else if (pathRg.length+delta == 5) {
                CFStringDelete(newString, CFRangeMake(pathRg.location + pathRg.length + delta - 5, 2));
            }
        }
    } else if (lastCompRg.location == pathRg.location) {
        CFStringReplace(newString, pathRg, CFSTR("."));
        CFStringInsert(newString, 1, CFSTR("/"));
    } else {
        CFStringDelete(newString, CFRangeMake(lastCompRg.location, pathRg.location + pathRg.length - lastCompRg.location));
    }
    result = _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
    CFRelease(newString);
    if ( filePathURLCreated ) {
        CFRelease(url);
    }
    return result;
}

CFURLRef CFURLCreateCopyAppendingPathExtension(CFAllocatorRef allocator, CFURLRef url, CFStringRef extension) {
    CFMutableStringRef newString;
    CFURLRef result;
    CFRange rg;
    
    CFAssert1(url != NULL && extension != NULL, __kCFLogAssertion, "%s(): NULL argument not allowed", __PRETTY_FUNCTION__);
    url = _CFURLFromNSURL(url);
    __CFGenericValidateType(url, __kCFURLTypeID);
    __CFGenericValidateType(extension, CFStringGetTypeID());

    Boolean filePathURLCreated = false;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if ( _CFURLIsFileReferenceURL(url) ) {
        // use a file path URL or fail
        CFURLRef filePathURL = CFURLCreateFilePathURL(allocator, url, NULL);
        if ( filePathURL ) {
            filePathURLCreated = TRUE;
            url = filePathURL;
        }
        else {
            return NULL;
        }
    }
#endif
    
    rg = _rangeOfLastPathComponent(url);
    if (rg.location < 0) {
        if ( filePathURLCreated ) {
            CFRelease(url);
        }
        return NULL; // No path
    }

    newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
    CFStringInsert(newString, rg.location + rg.length, CFSTR("."));
    CFStringRef newExt = CFURLCreateStringByAddingPercentEscapes(allocator, extension, NULL, CFSTR(";?/"), url->_encoding);
    CFStringInsert(newString, rg.location + rg.length + 1, newExt);
    CFRelease(newExt);
    result =  _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
    CFRelease(newString);
    if ( filePathURLCreated ) {
        CFRelease(url);
    }
    return result;
}

CFURLRef CFURLCreateCopyDeletingPathExtension(CFAllocatorRef allocator, CFURLRef url) {
    CFRange rg, dotRg;
    CFURLRef result;

    CFAssert1(url != NULL, __kCFLogAssertion, "%s(): NULL argument not allowed", __PRETTY_FUNCTION__);
    url = _CFURLFromNSURL(url);
    __CFGenericValidateType(url, __kCFURLTypeID);
    
    Boolean filePathURLCreated = false;
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    if ( _CFURLIsFileReferenceURL(url) ) {
        // use a file path URL or fail
        CFURLRef filePathURL = CFURLCreateFilePathURL(allocator, url, NULL);
        if ( filePathURL ) {
            filePathURLCreated = TRUE;
            url = filePathURL;
        }
        else {
            return NULL;
        }
    }
#endif
    
    rg = _rangeOfLastPathComponent(url);
    if (rg.location < 0) {
        result = NULL;
    } else if (rg.length && CFStringFindWithOptions(url->_string, CFSTR("."), rg, kCFCompareBackwards, &dotRg)) {
        CFMutableStringRef newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
        dotRg.length = rg.location + rg.length - dotRg.location;
        CFStringDelete(newString, dotRg);
        result = _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
        CFRelease(newString);
    } else {
        result = (CFURLRef)CFRetain(url);
    }
    if ( filePathURLCreated ) {
        CFRelease(url);
    }
    return result;
}


#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
static CFArrayRef HFSPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef components = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR(":"));
    CFMutableArrayRef newComponents = CFArrayCreateMutableCopy(alloc, 0, components);
    Boolean doSpecialLeadingColon = false;
    UniChar firstChar = CFStringGetCharacterAtIndex(path, 0);
    UInt32 i, cnt;
    CFRelease(components);


    if (!doSpecialLeadingColon && firstChar != ':') {
        CFArrayInsertValueAtIndex(newComponents, 0, CFSTR(""));
    } else if (firstChar != ':') {
        // see what we need to add at the beginning. Under MacOS, if the
        // first character isn't a ':', then the first component is the
        // volume name, and we need to find the mount point.  Bleah. If we
        // don't find a mount point, we're going to have to lie, and make something up.
        CFStringRef firstComp = (CFStringRef)CFArrayGetValueAtIndex(newComponents, 0);
        if (CFStringGetLength(firstComp) == 1 && CFStringGetCharacterAtIndex(firstComp, 0) == '/') {
            // "/" is the "magic" path for a UFS root directory
            CFArrayRemoveValueAtIndex(newComponents, 0);
            CFArrayInsertValueAtIndex(newComponents, 0, CFSTR(""));
        } else {
            // See if we can get a mount point.
            Boolean foundMountPoint = false;
            if (!foundMountPoint) {
                // Fall back to treating the volume name as the top level directory
                CFArrayInsertValueAtIndex(newComponents, 0, CFSTR(""));
            }
        }
    } else {
        CFArrayRemoveValueAtIndex(newComponents, 0);
    }

    cnt = CFArrayGetCount(newComponents);
    for (i = 0; i < cnt; i ++) {
        CFStringRef comp = (CFStringRef)CFArrayGetValueAtIndex(newComponents, i);
        CFStringRef newComp = NULL;
        CFRange searchRg, slashRg;
        searchRg.location = 0;
        searchRg.length = CFStringGetLength(comp);
        while (CFStringFindWithOptions(comp, CFSTR("/"), searchRg, 0, &slashRg)) {
            if (!newComp) {
                newComp = CFStringCreateMutableCopy(alloc, searchRg.location + searchRg.length, comp);
            }
            CFStringReplace((CFMutableStringRef)newComp, slashRg, CFSTR(":"));
            searchRg.length = searchRg.location + searchRg.length - slashRg.location - 1;
            searchRg.location = slashRg.location + 1;
        }
        if (newComp) {
            CFArraySetValueAtIndex(newComponents, i, newComp);
            CFRelease(newComp);
        }
    }
    if (isDir && CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(newComponents, cnt-1)) != 0) {
            CFArrayAppendValue(newComponents, CFSTR(""));
    }
    return newComponents;
}

typedef CFStringRef (*StringTransformation)(CFAllocatorRef, CFStringRef, CFIndex);
static CFArrayRef copyStringArrayWithTransformation(CFArrayRef array, StringTransformation transformation) {
    CFAllocatorRef alloc = CFGetAllocator(array);
    CFMutableArrayRef mArray = NULL;
    CFIndex i, c = CFArrayGetCount(array);
    for (i = 0; i < c; i ++) {
        CFStringRef origComp = (CFStringRef)CFArrayGetValueAtIndex(array, i);
        CFStringRef unescapedComp = transformation(alloc, origComp, i);
        if (!unescapedComp) { 
            break;
        }
        if (unescapedComp != origComp) {
            if (!mArray) {
                mArray = CFArrayCreateMutableCopy(alloc, c, array);
            }
            CFArraySetValueAtIndex(mArray, i, unescapedComp);
        }
        CFRelease(unescapedComp);
    }
    if (i != c) {
        if (mArray) CFRelease(mArray);
        return NULL;
    } else if (mArray) {
        return mArray;
    } else {
        CFRetain(array);
        return array;
    }
}

static CFStringRef escapePathComponent(CFAllocatorRef alloc, CFStringRef origComponent, CFIndex componentIndex) {
    return CFURLCreateStringByAddingPercentEscapes(alloc, origComponent, NULL, CFSTR(";?/"), kCFStringEncodingUTF8);
}

static CFStringRef HFSPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef components = HFSPathToURLComponents(path, alloc, isDir);
    CFArrayRef newComponents = components ? copyStringArrayWithTransformation(components, escapePathComponent) : NULL;
    CFIndex cnt;
    CFStringRef result;
    if (components) CFRelease(components);
    if (!newComponents) return NULL;

    cnt = CFArrayGetCount(newComponents);
    if (cnt == 1 && CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(newComponents, 0)) == 0) {
        result = (CFStringRef)CFRetain(CFSTR("/"));
    } else {
        result = CFStringCreateByCombiningStrings(alloc, newComponents, CFSTR("/"));
    }
    CFRelease(newComponents);
    return result;
}
#endif



// keys and vals must have space for at least 4 key/value pairs.  No argument can be NULL.
// Caller must release values, but not keys
static void __CFURLCopyPropertyListKeysAndValues(CFURLRef url, CFTypeRef *keys, CFTypeRef *vals, CFIndex *count) {
    CFAllocatorRef alloc = CFGetAllocator(url);
    CFURLRef base = CFURLGetBaseURL(url);
    keys[0] = CFSTR("_CFURLStringType");
    keys[1] = CFSTR("_CFURLString");
    keys[2] = CFSTR("_CFURLBaseStringType");
    keys[3] = CFSTR("_CFURLBaseURLString");
    if (CF_IS_OBJC(__kCFURLTypeID, url)) {
        SInt32 urlType = FULL_URL_REPRESENTATION;
        vals[0] = CFNumberCreate(alloc, kCFNumberSInt32Type, &urlType);
        vals[1] = CFURLGetString(url);
    } else {
        SInt32 urlType = FULL_URL_REPRESENTATION;
        vals[0] = CFNumberCreate(alloc, kCFNumberSInt32Type, &urlType);
        if (url->_flags & IS_DIRECTORY) {
            if (CFStringGetCharacterAtIndex(url->_string, CFStringGetLength(url->_string) - 1) == '/') {
                vals[1] = CFRetain(url->_string);
            } else {
                vals[1] = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%c"), url->_string, '/');
            }
        } else {
            if (CFStringGetCharacterAtIndex(url->_string, CFStringGetLength(url->_string) - 1) != '/') {
                vals[1] = CFRetain(url->_string);
            } else {
                vals[1] = CFStringCreateWithSubstring(alloc, url->_string, CFRangeMake(0, CFStringGetLength(url->_string) - 1));
            }
        }
    }
    if (base != NULL) {
        if (CF_IS_OBJC(__kCFURLTypeID, base)) {
            SInt32 urlType = FULL_URL_REPRESENTATION;
            vals[2] = CFNumberCreate(alloc, kCFNumberSInt32Type, &urlType);
            vals[3] = CFURLGetString(base);
        } else {
            SInt32 urlType = FULL_URL_REPRESENTATION;
            vals[2] = CFNumberCreate(alloc, kCFNumberSInt32Type, &urlType);
            if (base->_flags & IS_DIRECTORY) {
                if (CFStringGetCharacterAtIndex(base->_string, CFStringGetLength(base->_string) - 1) == '/') {
                    vals[3] = CFRetain(base->_string);
                } else {
                    vals[3] = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@%c"), base->_string, '/');
                }
            } else {
                if (CFStringGetCharacterAtIndex(base->_string, CFStringGetLength(base->_string) - 1) != '/') {
                    vals[3] = CFRetain(base->_string);
                } else {
                    vals[3] = CFStringCreateWithSubstring(alloc, base->_string, CFRangeMake(0, CFStringGetLength(base->_string) - 1));
                }
            }
        }
        *count = 4;
    } else {
        *count = 2;
    }
}

// Private API for Finder to use
CFPropertyListRef _CFURLCopyPropertyListRepresentation(CFURLRef url) {
    CFTypeRef keys[4], vals[4];
    CFDictionaryRef dict;
    CFIndex count, idx;
    __CFURLCopyPropertyListKeysAndValues(url, keys, vals, &count);
    dict = CFDictionaryCreate(CFGetAllocator(url), keys, vals, count, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (idx = 0; idx < count; idx ++) {
        CFRelease(vals[idx]);
    }
    return dict;
}

CFURLRef _CFURLCreateFromPropertyListRepresentation(CFAllocatorRef alloc, CFPropertyListRef pListRepresentation) {
    CFStringRef baseString, string;
    CFNumberRef baseTypeNum, urlTypeNum;
    SInt32 baseType, urlType;
    CFURLRef baseURL = NULL, url;
    CFDictionaryRef dict = (CFDictionaryRef)pListRepresentation;

    // Start by getting all the pieces and verifying they're of the correct type.
    if (CFGetTypeID(pListRepresentation) != CFDictionaryGetTypeID()) {
        return NULL;
    }
    string = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("_CFURLString"));
    if (!string || CFGetTypeID(string) != CFStringGetTypeID()) {
        return NULL;
    }
    urlTypeNum = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("_CFURLStringType"));
    if (!urlTypeNum || CFGetTypeID(urlTypeNum) != CFNumberGetTypeID() || !CFNumberGetValue(urlTypeNum, kCFNumberSInt32Type, &urlType) || (urlType != FULL_URL_REPRESENTATION && urlType != kCFURLPOSIXPathStyle && urlType != kCFURLHFSPathStyle && urlType != kCFURLWindowsPathStyle)) {
        return NULL;
    }
    baseString = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("_CFURLBaseURLString"));
    if (baseString) {
        if (CFGetTypeID(baseString) != CFStringGetTypeID()) {
            return NULL;
        }
        baseTypeNum = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("_CFURLBaseStringType"));
        if (!baseTypeNum || CFGetTypeID(baseTypeNum) != CFNumberGetTypeID() || !CFNumberGetValue(baseTypeNum, kCFNumberSInt32Type, &baseType) ||
            (baseType != FULL_URL_REPRESENTATION && baseType != kCFURLPOSIXPathStyle && baseType != kCFURLHFSPathStyle && baseType != kCFURLWindowsPathStyle)) {
            return NULL;
        }
        if (baseType == FULL_URL_REPRESENTATION) {
            baseURL = _CFURLCreateWithArbitraryString(alloc, baseString, NULL);
        } else {
            baseURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, baseString, (CFURLPathStyle)baseType, CFStringGetCharacterAtIndex(baseString, CFStringGetLength(baseString)-1) == '/', NULL);
        }
    }
    if (urlType == FULL_URL_REPRESENTATION) {
        url = _CFURLCreateWithArbitraryString(alloc, string, baseURL);
    } else {
        url = CFURLCreateWithFileSystemPathRelativeToBase(alloc, string, (CFURLPathStyle)urlType, CFStringGetCharacterAtIndex(string, CFStringGetLength(string)-1) == '/', baseURL);
    }
    if (baseURL) CFRelease(baseURL);
    return url;
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
Boolean _CFURLIsFileReferenceURL(CFURLRef url)
{
    // returns TRUE if url is is a file URL whose path starts with a file ID reference
    Boolean result = false;
    CFURLRef baseURL = CFURLGetBaseURL(url);
    if ( baseURL ) {
	result = _CFURLIsFileReferenceURL(baseURL);
    }
    else {
        if ( CF_IS_OBJC(__kCFURLTypeID, url) ) {
            result = (Boolean) CF_OBJC_CALLV((NSURL *)url, isFileReferenceURL);
        }
        else {
            result = ((_getSchemeTypeFromFlags(url->_flags) == kHasFileScheme) && (url->_flags & PATH_HAS_FILE_ID));
        }
    }
    return ( result );
}

static Boolean _CFURLHasFileURLScheme(CFURLRef url, Boolean *hasScheme)
{
    Boolean result;
    CFURLRef baseURL = CFURLGetBaseURL(url);
    
    if ( baseURL ) {
	result = _CFURLHasFileURLScheme(baseURL, hasScheme);
    }
    else {
        if ( CF_IS_OBJC(__kCFURLTypeID, url) ) {
            CFStringRef scheme = CFURLCopyScheme(url);
            if ( scheme ) {
                if ( scheme == kCFURLFileScheme ) {
                    result = true;
                }
                else {
                    result = CFStringCompare(scheme, kCFURLFileScheme, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
                }
                if ( hasScheme ) {
                    *hasScheme = true;
                }
                CFRelease(scheme);
            }
            else {
                if ( hasScheme ) {
                    *hasScheme = false;
                }
                result = false;
            }
        }
        else {
            if ( hasScheme ) {
                *hasScheme = (url->_flags & HAS_SCHEME) != 0;
            }
            result = (_getSchemeTypeFromFlags(url->_flags) == kHasFileScheme);
        }
    }
    return ( result );
}

Boolean _CFURLIsFileURL(CFURLRef url)
{
    Boolean result = _CFURLHasFileURLScheme(url, NULL);
    return ( result );
}

CFURLRef CFURLCreateFilePathURL(CFAllocatorRef alloc, CFURLRef url, CFErrorRef *error)
{
    CFURLRef result = NULL;
    Boolean hasScheme;
    if (!_CFURLHasFileURLScheme(url, &hasScheme)) {
        if ( !hasScheme ) {
            CFLog(kCFLogLevelWarning, CFSTR("CFURLCreateFilePathURL failed because it was passed this URL which has no scheme: %@"), url);
        }
        if ( error ) {
            *error = CFErrorCreate( kCFAllocatorDefault, kCFErrorDomainCocoa, kCFURLReadUnsupportedSchemeError, NULL );
        }
        result = NULL;
    } else {
	// File URL. Form of the path is unknown. Make a new URL.
	CFStringRef newURLString;
	CFStringRef netLoc;
	CFStringRef fsPath;
	CFStringRef rSpec;
        
	if ( CFURLGetBaseURL( url )) {
	    CFURLRef absURL = CFURLCopyAbsoluteURL( url );
	    fsPath = CFURLCreateStringWithFileSystemPath(CFGetAllocator(absURL), absURL, kCFURLPOSIXPathStyle, false);
	    netLoc = CFURLCopyNetLocation( absURL );
	    rSpec = CFURLCopyResourceSpecifier( absURL );
	    CFRelease( absURL );
	} else {
	    fsPath = CFURLCreateStringWithFileSystemPath(CFGetAllocator(url), url, kCFURLPOSIXPathStyle, false);
	    netLoc = CFURLCopyNetLocation( url );
	    rSpec = CFURLCopyResourceSpecifier( url );
	}
	if ( fsPath ) {
	    CFStringRef urlPath = _replacePathIllegalCharacters( fsPath, alloc, true );
	    newURLString = CFStringCreateWithFormat( alloc, NULL, CFSTR("file://%@%@%@%@"), (netLoc ? netLoc : CFSTR("")), urlPath, ((CFStringCompare(urlPath, CFSTR("/"), 0) != kCFCompareEqualTo) ? (CFURLHasDirectoryPath( url ) ? CFSTR("/") : CFSTR("")) : CFSTR("")), (rSpec ? rSpec : CFSTR("")));
	    result = CFURLCreateWithString( alloc, newURLString, NULL );
	    CFRelease( newURLString );
	    CFRelease( urlPath );
	    CFRelease( fsPath );	    
	} else {
	    if ( error ) { 
		// Would be better here to get an underlying error back from CFURLCreateStringWithFileSystemPath
		*error = CFErrorCreate( kCFAllocatorDefault, kCFErrorDomainCocoa, kCFURLNoSuchResourceError, NULL );
	    }
	    result = NULL;
	}
	if ( netLoc ) {
	    CFRelease( netLoc );
	}
	if ( rSpec ) {
	    CFRelease( rSpec );
	}
    }
    return result;
}

#endif


CFURLRef CFURLCreateFileReferenceURL(CFAllocatorRef alloc, CFURLRef url, CFErrorRef *error) { return NULL; }

