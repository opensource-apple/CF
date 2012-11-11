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
/*	CFURL.c
	Copyright 1998-2002, Apple, Inc. All rights reserved.
	Responsibility: Becky Willrich
*/

#include <CoreFoundation/CFURL.h>
#include "CFPriv.h"
#include "CFCharacterSetPriv.h"
#include <CoreFoundation/CFNumber.h>
#include "CFInternal.h"
#include "CFStringEncodingConverter.h"
#include "CFUtilities.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__)
#include <unistd.h>
#endif


static CFArrayRef HFSPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFArrayRef WindowsPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFStringRef WindowsPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFStringRef POSIXPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDirectory);
static CFStringRef HFSPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir);
static CFStringRef URLPathToHFSPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding);
CFStringRef CFURLCreateStringWithFileSystemPath(CFAllocatorRef allocator, CFURLRef anURL, CFURLPathStyle fsType, Boolean resolveAgainstBase);
static Boolean _CFGetFSRefFromURL(CFAllocatorRef alloc, CFURLRef url, void *voidRef);
static CFURLRef _CFCreateURLFromFSRef(CFAllocatorRef alloc, const void *voidRef, Boolean isDirectory);
CFURLRef _CFURLCreateCurrentDirectoryURL(CFAllocatorRef allocator);

#if defined(__MACH__) || defined(__LINUX__) || defined(__FREEBSD__) || defined(__WIN32__)
#if !defined(HAVE_CARBONCORE)
typedef void *FSRef;
#define noErr 0
#define FSGetVolumeInfo(A, B, C, D, E, F, G) (-3296)
#define FSGetCatalogInfo(A, B, C, D, E, F) (-3296)
#define FSMakeFSRefUnicode(A, B, C, D, E) (-3296)
#define FSPathMakeRef(A, B, C) (-3296)
#define FSRefMakePath(A, B, C) (-3296)
#define FSpMakeFSRef(A, B) (-3296)
#define FSNewAlias(A, B, C) (-3296)
#define DisposeHandle(A) (-3296)
#else

#include <mach-o/dyld.h>

static void __CF_DisposeHandle_internal(Handle h) {
    static void (*dyfunc)(Handle) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_DisposeHandle", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    dyfunc(h);
}

static OSErr __CF_FSNewAlias_internal(const FSRef *A, const FSRef *B, AliasHandle *C) {
    static OSErr (*dyfunc)(const FSRef *, const FSRef *, AliasHandle *) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSNewAlias", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B, C);
}

static OSErr __CF_FSGetVolumeInfo_internal(FSVolumeRefNum A, ItemCount B, FSVolumeRefNum *C, FSVolumeInfoBitmap D, FSVolumeInfo *E, HFSUniStr255*F, FSRef *G) {
    static OSErr (*dyfunc)(FSVolumeRefNum, ItemCount, FSVolumeRefNum *, FSVolumeInfoBitmap, FSVolumeInfo *, HFSUniStr255 *, FSRef *) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSGetVolumeInfo", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B, C, D, E, F, G);
}    

static OSErr __CF_FSGetCatalogInfo_internal(const FSRef *A, FSCatalogInfoBitmap B, FSCatalogInfo *C, HFSUniStr255 *D, FSSpec *E, FSRef *F) {
    static OSErr (*dyfunc)(const FSRef *, FSCatalogInfoBitmap, FSCatalogInfo *, HFSUniStr255 *, FSSpec *, FSRef *) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSGetCatalogInfo", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B, C, D, E, F);
}

static OSErr __CF_FSMakeFSRefUnicode_internal(const FSRef *A, UniCharCount B, const UniChar *C, TextEncoding D, FSRef *E) {
    static OSErr (*dyfunc)(const FSRef *, UniChar, const UniChar *, TextEncoding, FSRef *) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSMakeFSRefUnicode", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B, C, D, E);
}

static OSStatus __CF_FSPathMakeRef_internal(const uint8_t *A, FSRef *B, Boolean *C) {
    static OSStatus (*dyfunc)(const uint8_t *, FSRef *, Boolean *) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
	dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSPathMakeRef", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B, C);
}

static OSStatus __CF_FSRefMakePath_internal(const FSRef *A, uint8_t *B, UInt32 C) {
    static OSStatus (*dyfunc)(const FSRef *, uint8_t *, UInt32) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSRefMakePath", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B, C);
}

static OSErr __CF_FSpMakeFSRef_internal(const FSSpec *A, FSRef *B) {
    static OSErr (*dyfunc)(const FSSpec *, FSRef *) = NULL;
    if (NULL == dyfunc) {
	void *image = __CFLoadCarbonCore();
        dyfunc = NSAddressOfSymbol(NSLookupSymbolInImage(image, "_FSpMakeFSRef", NSLOOKUPSYMBOLINIMAGE_OPTION_BIND));
    }
    return dyfunc(A, B);
}

#define FSGetVolumeInfo(A, B, C, D, E, F, G) __CF_FSGetVolumeInfo_internal(A, B, C, D, E, F, G)
#define FSGetCatalogInfo(A, B, C, D, E, F) __CF_FSGetCatalogInfo_internal(A, B, C, D, E, F)
#define FSMakeFSRefUnicode(A, B, C, D, E) __CF_FSMakeFSRefUnicode_internal(A, B, C, D, E)
#define FSPathMakeRef(A, B, C) __CF_FSPathMakeRef_internal(A, B, C)
#define FSRefMakePath(A, B, C) __CF_FSRefMakePath_internal(A, B, C)
#define FSpMakeFSRef(A, B) __CF_FSpMakeFSRef_internal(A, B)
#define FSNewAlias(A, B, C) __CF_FSNewAlias_internal(A, B, C)
#define DisposeHandle(A) __CF_DisposeHandle_internal(A)
#endif
#endif

#if defined(__MACOS8__)

#include <CodeFragments.h>
#include <MacErrors.h>
#include <Gestalt.h>

static Boolean __CFMacOS8HasFSRefs() {
       static Boolean sHasFSRefs = (Boolean) -1;
       if ( sHasFSRefs == (Boolean) -1 ) {
               long result;
               sHasFSRefs = Gestalt( gestaltFSAttr, &result ) == noErr &&
                                   ( result & (1 << gestaltHasHFSPlusAPIs) ) != 0;
       }
       return sHasFSRefs;
}

static Ptr __CFGropeAroundForMacOS8Symbol(ConstStr255Param name) {
     static const char *libraries[] = {"\pCarbonLib", "\pInterfaceLib", "\pPrivateInterfaceLib"};
     int idx;
     for (idx = 0; idx < sizeof(libraries) / sizeof(libraries[0]); idx++) {
         CFragConnectionID connID;  /* We get the connections ONLY if already available. */
         OSErr err = GetSharedLibrary(libraries[idx], kPowerPCCFragArch, kFindCFrag, &connID, NULL, NULL);
         if (err == noErr) {
             Ptr cfmfunc = NULL;
             CFragSymbolClass symbolClass;
             err = FindSymbol(connID, name, &cfmfunc, &symbolClass);
             if (err == noErr && symbolClass == kTVectorCFragSymbol) {
                 return cfmfunc;
             }
         }
    }
    return NULL;
}

static OSErr __CF_FSMakeFSRefUnicode_internal(const FSRef *A, UniCharCount B, const UniChar *C, TextEncoding D, FSRef *E) {
    static OSErr (*cfmfunc)(const FSRef *, UniChar, const UniChar *, TextEncoding, FSRef *) = NULL;
    static Boolean looked = false;
    if (!looked && __CFMacOS8HasFSRefs()) {
        cfmfunc = __CFGropeAroundForMacOS8Symbol("\pFSMakeFSRefUnicode");
        looked = true;
    }
    return (cfmfunc != NULL) ? cfmfunc(A, B, C, D, E) : cfragNoSymbolErr;
}

static OSErr __CF_FSGetVolumeInfo_internal(FSVolumeRefNum A, ItemCount B, FSVolumeRefNum *C, FSVolumeInfoBitmap D, FSVolumeInfo *E, HFSUniStr255*F, FSRef *G) {
    static OSErr (*cfmfunc)(FSVolumeRefNum, ItemCount, FSVolumeRefNum *, FSVolumeInfoBitmap, FSVolumeInfo *, HFSUniStr255 *, FSRef *) = NULL;
    static Boolean looked = false;
    if (!looked && __CFMacOS8HasFSRefs()) {
        cfmfunc = __CFGropeAroundForMacOS8Symbol("\pFSGetVolumeInfo");
        looked = true;
    }
    return (cfmfunc != NULL) ? cfmfunc(A, B, C, D, E, F, G) : cfragNoSymbolErr;
}    

static OSErr __CF_FSGetCatalogInfo_internal(const FSRef *A, FSCatalogInfoBitmap B, FSCatalogInfo *C, HFSUniStr255 *D, FSSpec *E, FSRef *F) {
    static OSErr (*cfmfunc)(const FSRef *, FSCatalogInfoBitmap, FSCatalogInfo *, HFSUniStr255 *, FSSpec *, FSRef *) = NULL;
    static Boolean looked = false;
    if (!looked && __CFMacOS8HasFSRefs()) {
        cfmfunc = __CFGropeAroundForMacOS8Symbol("\pFSGetCatalogInfo");
        looked = true;
    }
    return (cfmfunc != NULL) ? cfmfunc(A, B, C, D, E, F) : cfragNoSymbolErr;
}

static OSStatus __CF_FSRefMakePath_internal(const FSRef *A, uint8_t *B, UInt32 C) {
    static OSStatus (*cfmfunc)(const FSRef *, uint8_t *buf, UInt32) = NULL;
    static Boolean looked = false;
    if (!looked && __CFMacOS8HasFSRefs()) {
        cfmfunc = __CFGropeAroundForMacOS8Symbol("\pFSRefMakePath");
        looked = true;
    }
    return (cfmfunc != NULL) ? cfmfunc(A, B, C) : cfragNoSymbolErr;
}

#define FSMakeFSRefUnicode(A, B, C, D, E) __CF_FSMakeFSRefUnicode_internal(A, B, C, D, E)
#define FSGetVolumeInfo(A, B, C, D, E, F, G) __CF_FSGetVolumeInfo_internal(A, B, C, D, E, F, G)
#define FSGetCatalogInfo(A, B, C, D, E, F) __CF_FSGetCatalogInfo_internal(A, B, C, D, E, F)
#define FSRefMakePath(A, B, C) __CF_FSRefMakePath_internal(A, B, C)
#endif

#if defined(__MACH__)
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(__MACOS8__)
#include <string.h>
#include <Files.h>
#endif

#define DEBUG_URL_MEMORY_USAGE 0

/* The bit flags in myURL->_flags */
#define HAS_SCHEME      (0x0001)
#define HAS_USER        (0x0002)
#define HAS_PASSWORD    (0x0004)
#define HAS_HOST        (0x0008)
#define HAS_PORT        (0x0010)
#define HAS_PATH        (0x0020)
#define HAS_PARAMETERS  (0x0040)
#define HAS_QUERY       (0x0080)
#define HAS_FRAGMENT    (0x0100)
// Last free bit (0x200) in lower word goes here!
#define IS_IPV6_ENCODED (0x0400)
#define IS_OLD_UTF8_STYLE (0x0800)
#define IS_DIRECTORY    (0x1000)
#define IS_PARSED       (0x2000)
#define IS_ABSOLUTE     (0x4000)
#define IS_DECOMPOSABLE (0x8000)

#define PATH_TYPE_MASK                 (0x000F0000)
/* POSIX_AND_URL_PATHS_MATCH will only be true if the URL and POSIX paths are identical, character for character, except for the presence/absence of a trailing slash on directories */
#define POSIX_AND_URL_PATHS_MATCH      (0x00100000)
#define ORIGINAL_AND_URL_STRINGS_MATCH (0x00200000)

/* If ORIGINAL_AND_URL_STRINGS_MATCH is false, these bits determine where they differ */
// Scheme can actually never differ because if there were escaped characters prior to the colon, we'd interpret the string as a relative path
#define SCHEME_DIFFERS     (0x00400000)
#define USER_DIFFERS       (0x00800000)
#define PASSWORD_DIFFERS   (0x01000000)
#define HOST_DIFFERS       (0x02000000)
// Port can actually never differ because if there were a non-digit following a colon in the net location, we'd interpret the whole net location as the host 
#define PORT_DIFFERS       (0x04000000)
#define PATH_DIFFERS       (0x08000000)
#define PARAMETERS_DIFFER  (0x10000000)
#define QUERY_DIFFERS      (0x20000000)
#define FRAGMENT_DIFfERS   (0x40000000)

// Number of bits to shift to get from HAS_FOO to FOO_DIFFERS flag
#define BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG (22)

// Other useful defines
#define NET_LOCATION_MASK (HAS_HOST | HAS_USER | HAS_PASSWORD | HAS_PORT)
#define RESOURCE_SPECIFIER_MASK  (HAS_PARAMETERS | HAS_QUERY | HAS_FRAGMENT)
#define FULL_URL_REPRESENTATION (0xF)

/* URL_PATH_TYPE(anURL) will be one of the CFURLPathStyle constants, in which case string is a file system path, or will be FULL_URL_REPRESENTATION, in which case the string is the full URL string. One caveat - string always has a trailing path delimiter if the url is a directory URL.  This must be stripped before returning file system representations!  */
#define URL_PATH_TYPE(url) (((url->_flags) & PATH_TYPE_MASK) >> 16)
#define PATH_DELIM_FOR_TYPE(fsType) ((fsType) == kCFURLHFSPathStyle ? ':' : (((fsType) == kCFURLWindowsPathStyle) ? '\\' : '/'))
#define PATH_DELIM_AS_STRING_FOR_TYPE(fsType) ((fsType) == kCFURLHFSPathStyle ? CFSTR(":") : (((fsType) == kCFURLWindowsPathStyle) ? CFSTR("\\") : CFSTR("/")))


struct __CFURL {
    CFRuntimeBase _cfBase;
    UInt32 _flags;
    CFStringRef  _string; // Never NULL; the meaning of _string depends on URL_PATH_TYPE(myURL) (see above)
    CFURLRef  _base;       // NULL for absolute URLs; if present, _base is guaranteed to itself be absolute.
    CFRange *ranges;
    void *_reserved; // Reserved for URLHandle's use.
    CFStringEncoding _encoding; // The encoding to use when asked to remove percent escapes; this is never consulted if IS_OLD_UTF8_STYLE is set.
    CFMutableStringRef _sanatizedString; // The fully compliant RFC string.  This is only non-NULL if ORIGINAL_AND_URL_STRINGS_MATCH is false.  This should never be mutated except when the sanatized string is first computed
};

static void _convertToURLRepresentation(struct __CFURL *url);
static CFURLRef _CFURLCopyAbsoluteFileURL(CFURLRef relativeURL);
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
    _parseComponents(CFGetAllocator(url), url->_string, url->_base, &(((struct __CFURL *)url)->_flags), &(((struct __CFURL *)url)->ranges));
}

static Boolean _createOldUTF8StyleURLs = false;

CF_INLINE Boolean createOldUTF8StyleURLs(void) {
    if (_createOldUTF8StyleURLs) {
        return true;
    }
    return !_CFExecutableLinkedOnOrAfter(CFSystemVersionJaguar);
}

// Our backdoor in case removing the UTF8 constraint for URLs creates unexpected problems.  See radar 2902530 -- REW
CF_EXPORT
void _CFURLCreateOnlyUTF8CompatibleURLs(Boolean createUTF8URLs) {
    _createOldUTF8StyleURLs = createUTF8URLs;
}

CF_INLINE Boolean scheme_valid(UniChar c) {
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')) return true;
    if ('0' <= c && c <= '9') return true; // Added 12/1/2000; RFC 2396 extends schemes to include digits
    if ((c == '.') || (c == '-') || (c == '+')) return true;
    return false;
}

// "Unreserved" as defined by RFC 2396
CF_INLINE Boolean isUnreservedCharacter(UniChar ch) {
    // unreserved characters are ASCII-7 alphanumerics, plus certain punctuation marks, all in the 0-127 range
    if (ch > 0x7f) return false;
    if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')) return true;
    if ('0' <= ch && ch <= '9') return true;
    if (ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '~' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') return true;
    return false;
}

CF_INLINE Boolean isPathLegalCharacter(UniChar ch) {
    // the unreserved chars plus a couple others
    if (ch > 0x7f) return false;
    if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')) return true;
    if ('0' <= ch && ch <= '9') return true;
    if (ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '~' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') return true;
    if (ch == '/' || ch == ':' || ch == '@' || ch == '&' || ch == '=' || ch == '+' || ch == '$' || ch == ',') return true;
    return false;
}

CF_INLINE Boolean isURLLegalCharacter(UniChar ch) {
    if (ch > 0x7f) return false;
    if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')) return true;
    if ('0' <= ch && ch <= '9') return true;
    if (ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '~' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') return true;
    if (ch == ';' || ch == '/' || ch == '?' || ch == ':' || ch == '@' || ch == '&' || ch == '=' || ch == '+' || ch == '$' || ch == ',') return true;
    return false;
}

CF_INLINE Boolean isHexDigit(UniChar ch) {
    return ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'));
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
    return ((url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) != 0) || (url->_sanatizedString != NULL);
}

typedef CFStringRef (*StringTransformation)(CFAllocatorRef, CFStringRef, CFIndex);
static CFArrayRef copyStringArrayWithTransformation(CFArrayRef array, StringTransformation transformation) {
    CFAllocatorRef alloc = CFGetAllocator(array);
    CFMutableArrayRef mArray = NULL;
    CFIndex i, c = CFArrayGetCount(array);
    for (i = 0; i < c; i ++) {
        CFStringRef origComp = CFArrayGetValueAtIndex(array, i);
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

// Returns NULL if str cannot be converted for whatever reason, str if str contains no characters in need of escaping, or a newly-created string with the appropriate % escape codes in place.  Caller must always release the returned string.
CF_INLINE CFStringRef _replacePathIllegalCharacters(CFStringRef str, CFAllocatorRef alloc, Boolean preserveSlashes) {
    if (preserveSlashes) {
        return CFURLCreateStringByAddingPercentEscapes(alloc, str, NULL, CFSTR(";?"), kCFStringEncodingUTF8);
    } else {
        return CFURLCreateStringByAddingPercentEscapes(alloc, str, NULL, CFSTR(";?/"), kCFStringEncodingUTF8);
    }        
}

static CFStringRef escapePathComponent(CFAllocatorRef alloc, CFStringRef origComponent, CFIndex componentIndex) {
    return CFURLCreateStringByAddingPercentEscapes(alloc, origComponent, NULL, CFSTR(";?/"), kCFStringEncodingUTF8);
}

static CFStringRef escapeWindowsPathComponent(CFAllocatorRef alloc, CFStringRef origComponent, CFIndex componentIndex) {
    if (CFStringGetLength(origComponent) == 2 && CFStringGetCharacterAtIndex(origComponent, 1) == '|') {
        // Don't corrupt a drive letter component
        CFRetain(origComponent);
        return origComponent;
    } else {
        return CFURLCreateStringByAddingPercentEscapes(alloc, origComponent, NULL, CFSTR(";?/"), kCFStringEncodingUTF8);
    }
}

// We have 2 UniChars of a surrogate; we must convert to the correct percent-encoded UTF8 string and append to str.  Added so that file system URLs can always be converted from POSIX to full URL representation.  -- REW, 8/20/2001
static Boolean _hackToConvertSurrogates(UniChar highChar, UniChar lowChar, CFMutableStringRef str) {
    UniChar surrogate[2];
    uint8_t bytes[6]; // Aki sez it should never take more than 6 bytes
    UInt32 len; 
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
    UInt32 byteLength;
    CFAllocatorRef alloc = NULL;
    if (CFStringEncodingUnicodeToBytes(encoding, 0, &ch, 1, NULL, bytePtr, 6, &byteLength) != kCFStringEncodingConversionSuccess) {
        byteLength = CFStringEncodingByteLengthForCharacters(encoding, 0, &ch, 1);
        if (byteLength <= 6) {
            // The encoding cannot accomodate the character
            return false;
        }
        alloc = CFGetAllocator(str);
        bytePtr = CFAllocatorAllocate(alloc, byteLength, 0);
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
        return CFStringCreateCopy(alloc, originalString);
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
            escapedStr = strForEscapedChar;
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
                escapedStr = strForEscapedChar;
            }
            if (failed) break;
        }

        // The new character is in escapedChar; the number of percent escapes it took is in numBytesExpected.
        searchRange.location = percentRange.location + 3 * numBytesExpected;
        searchRange.length = length - searchRange.location;
        
        if (!escapeAll) {
            if (CFStringFind(charactersToLeaveEscaped, escapedStr, 0).location != kCFNotFound) {
                if (escapedStr != strForEscapedChar) {
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
        if (escapedStr != strForEscapedChar) {
            CFRelease(escapedStr);
            escapedStr = NULL;
        }
        mark = searchRange.location;// We need mark to be the index of the first character beyond the escape sequence
    }

    if (escapedStr && escapedStr != strForEscapedChar) CFRelease(escapedStr);
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
	return CFStringCreateCopy(alloc, originalString);
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
            return CFStringCreateCopy(alloc, originalString);
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
                        bytes = CFAllocatorAllocate(alloc, 16 * sizeof(uint8_t), 0);
                        memmove(bytes, byteBuffer, capacityOfBytes);
                        capacityOfBytes = 16;
                    } else {
                        capacityOfBytes = 2*capacityOfBytes;
                        bytes = CFAllocatorReallocate(alloc, bytes, capacityOfBytes * sizeof(uint8_t), 0);
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
                CFRelease(convertedString);
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
            return CFStringCreateCopy(alloc, originalString);
        }
    }
}
    

static CFStringRef _addPercentEscapesToString(CFAllocatorRef allocator, CFStringRef originalString, Boolean (*shouldReplaceChar)(UniChar, void*), CFIndex (*handlePercentChar)(CFIndex, CFStringRef, CFStringRef *, void *), CFStringEncoding encoding, void *context) {
    CFMutableStringRef newString = NULL;
    CFIndex idx, length;
    CFStringInlineBuffer buf;

    if (!originalString) return NULL;
    length = CFStringGetLength(originalString);
    if (length == 0) return CFStringCreateCopy(allocator, originalString);
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
        return CFStringCreateCopy(CFGetAllocator(originalString), originalString);
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
    CFURLRef  url1 = cf1;
    CFURLRef  url2 = cf2;
    UInt32 pathType1, pathType2;
    
    __CFGenericValidateType(cf1, CFURLGetTypeID());
    __CFGenericValidateType(cf2, CFURLGetTypeID());

    if (url1 == url2) return true;
    if ((url1->_flags & IS_PARSED) && (url2->_flags & IS_PARSED) && (url1->_flags & IS_DIRECTORY) != (url2->_flags & IS_DIRECTORY)) return false;
    if (url1->_base) {
        if (!url2->_base) return false;
        if (!CFEqual(url1->_base, url2->_base)) return false;
    } else if (url2->_base) {
        return false;
    }
    
    pathType1 = URL_PATH_TYPE(url1);
    pathType2 = URL_PATH_TYPE(url2);
    if (pathType1 == pathType2) {
        if (pathType1 != FULL_URL_REPRESENTATION) {
            return CFEqual(url1->_string, url2->_string);
        } else {
            // Do not compare the original strings; compare the sanatized strings.
            return CFEqual(CFURLGetString(url1), CFURLGetString(url2));
        }
    } else {
        // Try hard to avoid the expensive conversion from a file system representation to the canonical form
        CFStringRef scheme1 = CFURLCopyScheme(url1);
        CFStringRef scheme2 = CFURLCopyScheme(url2);
        Boolean eq;
        if (scheme1 && scheme2) {
            eq = CFEqual(scheme1, scheme2);
            CFRelease(scheme1);
            CFRelease(scheme2);
        } else if (!scheme1 && !scheme2) {
            eq = TRUE;
        } else {
            eq = FALSE;
            if (scheme1) CFRelease(scheme1);
            else CFRelease(scheme2);
        }
        if (!eq) return false;

        if (pathType1 == FULL_URL_REPRESENTATION) {
            if (!(url1->_flags & IS_PARSED)) {
                _parseComponentsOfURL(url1);
            }
            if (url1->_flags & (HAS_USER | HAS_PORT | HAS_PASSWORD | HAS_QUERY | HAS_PARAMETERS | HAS_FRAGMENT )) {
                return false;
            }
        }

        if (pathType2 == FULL_URL_REPRESENTATION) {
            if (!(url2->_flags & IS_PARSED)) {
                _parseComponentsOfURL(url2);
            }
            if (url2->_flags & (HAS_USER | HAS_PORT | HAS_PASSWORD | HAS_QUERY | HAS_PARAMETERS | HAS_FRAGMENT )) {
                return false;
            }
        }

        // No help for it; we now must convert to the canonical representation and compare.
        return CFEqual(CFURLGetString(url1), CFURLGetString(url2));
    }
}

static UInt32 __CFURLHash(CFTypeRef  cf) {
    /* This is tricky, because we do not want the hash value to change as a file system URL is changed to its canonical representation, nor do we wish to force the conversion to the canonical representation. We choose instead to take the last path component (or "/" in the unlikely case that the path is empty), then hash on that. */
    CFURLRef  url = cf;
    UInt32 result;
    if (CFURLCanBeDecomposed(url)) {
        CFStringRef lastComp = CFURLCopyLastPathComponent(url);
        if (lastComp) {
            result = CFHash(lastComp);
            CFRelease(lastComp);
        } else {
            result = 0;
        }
    } else {
        result = CFHash(CFURLGetString(url));
    }
    return result;
}

static CFStringRef  __CFURLCopyFormattingDescription(CFTypeRef  cf, CFDictionaryRef formatOptions) {
    CFURLRef  url = cf;
    __CFGenericValidateType(cf, CFURLGetTypeID());
    if (!url->_base) {
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
    if (url->_base) {
        CFStringRef baseString = CFCopyDescription(url->_base);
        result = CFStringCreateWithFormat(alloc, NULL, CFSTR("<CFURL %p [%p]>{type = %d, string = %@,\n\tbase = %@}"), cf, alloc, URL_PATH_TYPE(url), url->_string, baseString);
        CFRelease(baseString);
    } else {
        result = CFStringCreateWithFormat(alloc, NULL, CFSTR("<CFURL %p [%p]>{type = %d, string = %@, base = (null)}"), cf, alloc, URL_PATH_TYPE(url), url->_string);
    }
    return result;
}

#if DEBUG_URL_MEMORY_USAGE
static CFAllocatorRef URLAllocator = NULL;
static UInt32 numFileURLsCreated = 0;
static UInt32 numFileURLsConverted = 0;
static UInt32 numFileURLsDealloced = 0;
static UInt32 numURLs = 0;
static UInt32 numDealloced = 0;
void __CFURLDumpMemRecord(void) {
    CFStringRef str = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d URLs created; %d destroyed\n%d file URLs created; %d converted; %d destroyed\n"), numURLs, numDealloced, numFileURLsCreated, numFileURLsConverted, numFileURLsDealloced);
    CFShow(str);
    CFRelease(str);
    if (URLAllocator) CFCountingAllocatorPrintPointers(URLAllocator);
}
#endif

static void __CFURLDeallocate(CFTypeRef  cf) {
    CFURLRef  url = cf;
    CFAllocatorRef alloc;
    __CFGenericValidateType(cf, CFURLGetTypeID());
    alloc = CFGetAllocator(url);
#if DEBUG_URL_MEMORY_USAGE
    numDealloced ++;
    if (URL_PATH_TYPE(url) != FULL_URL_REPRESENTATION) {
        numFileURLsDealloced ++;
    }
#endif
    CFRelease(url->_string);
    if (url->_base) CFRelease(url->_base);
    if (url->ranges) CFAllocatorDeallocate(alloc, url->ranges);
    if (url->_sanatizedString) CFRelease(url->_sanatizedString);
}

static CFTypeID __kCFURLTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFURLClass = {
    0,
    "CFURL",
    NULL,      // init
    NULL,      // copy
    __CFURLDeallocate,
    __CFURLEqual,
    __CFURLHash,
    __CFURLCopyFormattingDescription,
    __CFURLCopyDescription
};

CONST_STRING_DECL(kCFURLFileScheme, "file")
CONST_STRING_DECL(kCFURLLocalhost, "localhost")

__private_extern__ void __CFURLInitialize(void) {
    __kCFURLTypeID = _CFRuntimeRegisterClass(&__CFURLClass);
}

/* Toll-free bridging support; get the true CFURL from an NSURL */
CF_INLINE CFURLRef _CFURLFromNSURL(CFURLRef url) {
    CF_OBJC_FUNCDISPATCH0(__kCFURLTypeID, CFURLRef, url, "_cfurl");
    return url;
}

CFTypeID CFURLGetTypeID(void) {
    return __kCFURLTypeID;
}

__private_extern__ void CFShowURL(CFURLRef url) {
    if (!url) {
        printf("(null)\n");
        return;
    }
    printf("<CFURL 0x%x>{", (unsigned)url);
    if (CF_IS_OBJC(__kCFURLTypeID, url)) {
        printf("ObjC bridged object}\n");
        return;
    }
    printf("\n\tPath type: ");
    switch (URL_PATH_TYPE(url)) {
        case kCFURLPOSIXPathStyle:
            printf("POSIX");
            break;
        case kCFURLHFSPathStyle:
            printf("HFS");
            break;
        case kCFURLWindowsPathStyle:
            printf("NTFS");
            break;
        case FULL_URL_REPRESENTATION:
            printf("Native URL");
            break;
        default:
            printf("UNRECOGNIZED PATH TYPE %d", (char)URL_PATH_TYPE(url));
    }
    printf("\n\tRelative string: ");
    CFShow(url->_string);
    printf("\tBase URL: ");
    if (url->_base) {
        printf("<0x%x> ", (unsigned)url->_base);
        CFShow(url->_base);
    } else {
        printf("(null)\n");
    }
    printf("\tFlags: 0x%x\n}\n", (unsigned)url->_flags);
}


/***************************************************/
/* URL creation and String/Data creation from URLS */
/***************************************************/
static void constructBuffers(CFAllocatorRef alloc, CFStringRef string, const unsigned char **cstring, const UniChar **ustring, Boolean *useCString, Boolean *freeCharacters) {
    CFIndex neededLength;
    CFIndex length;
    CFRange rg;

    *cstring = CFStringGetCStringPtr(string, kCFStringEncodingISOLatin1);
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

    *freeCharacters = true;
    length = CFStringGetLength(string);
    rg = CFRangeMake(0, length);
    CFStringGetBytes(string, rg, kCFStringEncodingISOLatin1, 0, false, NULL, INT_MAX, &neededLength);
    if (neededLength == length) {
        char *buf = CFAllocatorAllocate(alloc, length, 0);
        CFStringGetBytes(string, rg, kCFStringEncodingISOLatin1, 0, false, buf, length, NULL);
        *cstring = buf;
        *useCString = true;
    } else {
        UniChar *buf = CFAllocatorAllocate(alloc, length * sizeof(UniChar), 0);
        CFStringGetCharacters(string, rg, buf);
        *useCString = false;
        *ustring = buf;
    }
}

#define STRING_CHAR(x) (useCString ? cstring[(x)] : ustring[(x)])
static void _parseComponents(CFAllocatorRef alloc, CFStringRef string, CFURLRef baseURL, UInt32 *theFlags, CFRange **range) {
    CFRange ranges[9];
    /* index gives the URL part involved; to calculate the correct range index, use the number of the bit of the equivalent flag (i.e. the host flag is HAS_HOST, which is 0x8.  so the range index for the host is 3.)  Note that this is true in this function ONLY, since the ranges stored in (*range) are actually packed, skipping those URL components that don't exist.  This is why the indices are hard-coded in this function. */

    CFIndex idx, base_idx = 0;
    CFIndex string_length;
    UInt32 flags = (IS_PARSED | *theFlags);
    Boolean useCString, freeCharacters, isCompliant;
    uint8_t numRanges = 0;
    const unsigned char *cstring = NULL;
    const UniChar *ustring = NULL;
    
    string_length = CFStringGetLength(string);
    constructBuffers(alloc, string, &cstring, &ustring, &useCString, &freeCharacters);
    
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
            break;
        } else if (!scheme_valid(ch)) {
            break;	// invalid scheme character -- no scheme
        }
    }

    // Make sure we have an RFC-1808 compliant URL - that's either something without a scheme, or scheme:/(stuff) or scheme://(stuff) 
    if (!(flags & HAS_SCHEME)) {
        isCompliant = true;
    } else if (!(base_idx < string_length)) {
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
        (*range) = CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
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
        Boolean insideIPV6Host = false;
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
                    insideIPV6Host = true;
                    flags |= IS_IPV6_ENCODED;
                }
                if (']' == STRING_CHAR(idx)) {	// ending IPV6 explicit address
                    insideIPV6Host = false;
                }
                // there is a port if we see a colon outside ipv6 address	
                if (!insideIPV6Host && ':' == STRING_CHAR(idx)) {	
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
    (*range) = CFAllocatorAllocate(alloc, sizeof(CFRange)*numRanges, 0);
    numRanges = 0;
    for (idx = 0, flags = 1; flags != (1<<9); flags = (flags<<1), idx ++) {
        if ((*theFlags) & flags) {
            (*range)[numRanges] = ranges[idx];
            numRanges ++;
        }
    }
    if (((*theFlags) & HAS_PATH) && !CFStringFindWithOptions(string, CFSTR("%"), ranges[5], 0, NULL)) {
        (*theFlags) |= POSIX_AND_URL_PATHS_MATCH;
    }
}

static Boolean scanCharacters(CFAllocatorRef alloc, CFMutableStringRef *escapedString, UInt32 *flags, const unsigned char *cstring, const UniChar *ustring, Boolean useCString, CFIndex base, CFIndex end, CFIndex *mark, UInt32 componentFlag, CFStringEncoding encoding) {
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
        if (!shouldEscape) continue;
        
        sawIllegalChar = true;
        if (componentFlag && flags) {
            *flags |= (componentFlag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG);
        }
        if (!*escapedString) {
            *escapedString = CFStringCreateMutable(alloc, 0);
        }
        if (useCString) {
            CFStringRef tempString = CFStringCreateWithBytes(alloc, &(cstring[*mark]), idx - *mark, kCFStringEncodingISOLatin1, false);
            CFStringAppend(*escapedString, tempString);
            CFRelease(tempString);
        } else {
            CFStringAppendCharacters(*escapedString, &(ustring[*mark]), idx - *mark);
        }
        *mark = idx + 1;
        _appendPercentEscapesForCharacter(ch, encoding, *escapedString); // This can never fail because anURL->_string was constructed from the encoding passed in
    }
    return sawIllegalChar;
} 

static void computeSanitizedString(CFURLRef url) {
    CFAllocatorRef alloc = CFGetAllocator(url);
    CFIndex string_length = CFStringGetLength(url->_string);
    Boolean useCString, freeCharacters;
    const unsigned char *cstring = NULL;
    const UniChar *ustring = NULL;
    CFIndex base; // where to scan from
    CFIndex mark; // first character not-yet copied to sanitized string
    if (!(url->_flags & IS_PARSED)) {
        _parseComponentsOfURL(url);
    }
    constructBuffers(alloc, url->_string, &cstring, &ustring, &useCString, &freeCharacters);
    if (!(url->_flags & IS_DECOMPOSABLE)) {
        // Impossible to have a problem character in the scheme
        base = _rangeForComponent(url->_flags, url->ranges, HAS_SCHEME).length + 1;
        mark = 0;
        if (!scanCharacters(alloc, &(((struct __CFURL *)url)->_sanatizedString), &(((struct __CFURL *)url)->_flags), cstring, ustring, useCString, base, string_length, &mark, 0, url->_encoding)) {
            ((struct __CFURL *)url)->_flags |= ORIGINAL_AND_URL_STRINGS_MATCH;
        }
    } else {
        // Go component by component
        CFIndex currentComponent = HAS_USER;
        mark = 0;
        while (currentComponent < (HAS_FRAGMENT << 1)) {
            CFRange componentRange = _rangeForComponent(url->_flags, url->ranges, currentComponent);
            if (componentRange.location != kCFNotFound) {
                scanCharacters(alloc, &(((struct __CFURL *)url)->_sanatizedString), &(((struct __CFURL *)url)->_flags), cstring, ustring, useCString, componentRange.location, componentRange.location + componentRange.length, &mark, currentComponent, url->_encoding);
            }
            currentComponent = currentComponent << 1;
        }
        if (!url->_sanatizedString) {
            ((struct __CFURL *)url)->_flags |= ORIGINAL_AND_URL_STRINGS_MATCH;
        }
    }
    if (url->_sanatizedString && mark != string_length) {
        if (useCString) {
            CFStringRef tempString = CFStringCreateWithBytes(alloc, &(cstring[mark]), string_length - mark, kCFStringEncodingISOLatin1, false);
            CFStringAppend(url->_sanatizedString, tempString);
            CFRelease(tempString);
        } else {
            CFStringAppendCharacters(url->_sanatizedString, &(ustring[mark]), string_length - mark);
        }
    }
    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void *)cstring : (void *)ustring);
    }
}


static CFStringRef correctedComponent(CFStringRef comp, UInt32 compFlag, CFStringEncoding enc) {
    CFAllocatorRef alloc = CFGetAllocator(comp);
    CFIndex string_length = CFStringGetLength(comp);
    Boolean useCString, freeCharacters;
    const unsigned char *cstring = NULL;
    const UniChar *ustring = NULL;
    CFIndex mark = 0; // first character not-yet copied to sanitized string
    CFMutableStringRef result = NULL;

    constructBuffers(alloc, comp, &cstring, &ustring, &useCString, &freeCharacters);
    scanCharacters(alloc, &result, NULL, cstring, ustring, useCString, 0, string_length, &mark, compFlag, enc);
    if (result) {
        if (mark < string_length) {
            if (useCString) {
                CFStringRef tempString = CFStringCreateWithBytes(alloc, &(cstring[mark]), string_length - mark, kCFStringEncodingISOLatin1, false);
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
    if (!URLAllocator) {
        URLAllocator = CFCountingAllocatorCreate(NULL);
    }
    allocator = URLAllocator;
#endif
    url = (struct __CFURL *)_CFRuntimeCreateInstance(allocator, __kCFURLTypeID, sizeof(struct __CFURL) - sizeof(CFRuntimeBase), NULL);
    if (url) {
        url->_flags = 0;
        if (createOldUTF8StyleURLs()) {
            url->_flags |= IS_OLD_UTF8_STYLE;
        }
        url->_string = NULL;
        url->_base = NULL;
        url->ranges = NULL;
        url->_reserved = NULL;
        url->_encoding = kCFStringEncodingUTF8;
        url->_sanatizedString = NULL;
    }
    return url;
}

// It is the caller's responsibility to guarantee that if URLString is absolute, base is NULL.  This is necessary to avoid duplicate processing for file system URLs, which had to decide whether to compute the cwd for the base; we don't want to duplicate that work.  This ALSO means it's the caller's responsibility to set the IS_ABSOLUTE bit, since we may have a degenerate URL whose string is relative, but lacks a base.
static void _CFURLInit(struct __CFURL *url, CFStringRef URLString, UInt32 fsType, CFURLRef base) {
    CFAssert1(URLString != NULL && CFGetTypeID(URLString) == CFStringGetTypeID() && CFStringGetLength(URLString) != 0, __kCFLogAssertion, "%s(): internal CF error; empty string encountered", __PRETTY_FUNCTION__);
    CFAssert2((fsType == FULL_URL_REPRESENTATION) || (fsType == kCFURLPOSIXPathStyle) || (fsType == kCFURLWindowsPathStyle) || (fsType == kCFURLHFSPathStyle), __kCFLogAssertion, "%s(): Received bad fsType %d", __PRETTY_FUNCTION__, fsType);
    
    // Coming in, the url has its allocator flag properly set, and its base initialized, and nothing else.    
    url->_string = CFStringCreateCopy(CFGetAllocator(url), URLString);
    url->_flags |= (fsType << 16);
    url->_base = base ? CFURLCopyAbsoluteURL(base) : NULL;
#if DEBUG_URL_MEMORY_USAGE
    if (fsType != FULL_URL_REPRESENTATION) {
        numFileURLsCreated ++;
    }
#endif
}

CF_EXPORT void _CFURLInitFSPath(CFURLRef url, CFStringRef path) {
    CFIndex len = CFStringGetLength(path);
    if (len && CFStringGetCharacterAtIndex(path, 0) == '/') {
        _CFURLInit((struct __CFURL *)url, path, kCFURLPOSIXPathStyle, NULL);
        ((struct __CFURL *)url)->_flags |= IS_ABSOLUTE;
    } else {
        CFURLRef cwdURL = _CFURLCreateCurrentDirectoryURL(CFGetAllocator(url));
        _CFURLInit((struct __CFURL *)url, path, kCFURLPOSIXPathStyle, cwdURL);
        CFRelease(cwdURL);
    }
    if (!len || '/' == CFStringGetCharacterAtIndex(path, len - 1))
        ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
}

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
        if (ch >= 'a' && ch <= 'z') continue;
        if (ch >= '0' && ch <= '9') continue;
        if (ch >= 'A' && ch <= 'Z') continue;
        if (ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '~' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') continue;
        if (ch == ';' || ch == '/' || ch == '?' || ch == ':' || ch == '@' || ch == '&' || ch == '=' || ch == '+' || ch == '$' || ch == ',') continue;
            if (ch == '[' || ch == ']') continue; // IPV6 support (RFC 2732) DCJ June/10/2002
        if (ch == '#') {
            if (sawHash) break;
            sawHash = true;
            continue;
        }
        // Commenting out all the CFAsserts below because they cause the program to abort if running against the debug library.  If we have a non-fatal assert, we should use that instead. -- REW 5/20/2002
        if (ch != '%') {
            //CFAssert1(false, __kCFLogAssertion, "Detected illegal URL character 0x%x when trying to create a CFURL", ch);
            break;
        }
        if (idx + 2 > length) {
            //CFAssert1(false, __kCFLogAssertion, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-1);
            idx = -1;  // To guarantee index < length, and our failure case is triggered
            break;
        }
        ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
        idx ++;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            //CFAssert1(false, __kCFLogAssertion, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-2);
            idx = -1;
            break;
        }
        ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
        idx ++;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            //CFAssert1(false, __kCFLogAssertion, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-3);
            idx = -1;
            break;
        }
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
            char ch = CFStringGetCharacterAtIndex(string, i);
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

static Boolean _shouldEscapeForEncodingConversion(UniChar ch, void *context) {
    struct __CFURLEncodingTranslationParameters *info = (struct __CFURLEncodingTranslationParameters *)context;
    if (info->escapeHighBit && ch > 0x7F) {
        return true;
    } else if (ch == '%' && info->escapePercents) {
        return true;
    } else if (info->addlChars) {
        const UniChar *escChar = info->addlChars;
        int i; 
        for (i = 0; i < info->count; escChar ++, i ++) {
            if (*escChar == ch) {
                return true;
            }
        }
    }
    return false;
}

static CFIndex _convertEscapeSequence(CFIndex percentIndex, CFStringRef urlString, CFStringRef *newString, void *context) {
    struct __CFURLEncodingTranslationParameters *info = (struct __CFURLEncodingTranslationParameters *)context;
    CFMutableDataRef  newData;
    Boolean sawNonASCIICharacter = false;
    CFIndex i = percentIndex;
    CFIndex length;
    *newString = NULL;
    if (info->encodingsMatch) return percentIndex + 3; // +3 because we want the two characters of the percent encoding to not be copied verbatim, as well
    newData = CFDataCreateMutable(CFGetAllocator(urlString), 0);
    length = CFStringGetLength(urlString);
    
    while (i < length && CFStringGetCharacterAtIndex(urlString, i) == '%') {
        uint8_t byte;
        if (i+2 >= length || !_translateBytes(CFStringGetCharacterAtIndex(urlString, i+1), CFStringGetCharacterAtIndex(urlString, i+2), &byte)) {
            CFRelease(newData);
            return -1;
        }
        if (byte > 0x7f) sawNonASCIICharacter = true;
        CFDataAppendBytes(newData, &byte, 1);
        i += 3;
    }
    if (!sawNonASCIICharacter && info->agreesOverASCII) {
        return i;
    } else {
        CFStringRef tmp = CFStringCreateWithBytes(CFGetAllocator(urlString), CFDataGetBytePtr(newData), CFDataGetLength(newData), info->fromEnc, false);
        CFIndex tmpIndex, tmpLen;
        if (!tmp) {
            CFRelease(newData);
            return -1;
        }
        tmpLen = CFStringGetLength(tmp);
        *newString = CFStringCreateMutable(CFGetAllocator(urlString), 0);
        for (tmpIndex = 0; tmpIndex < tmpLen; tmpIndex ++) {
            if (!_appendPercentEscapesForCharacter(CFStringGetCharacterAtIndex(tmp, tmpIndex), info->toEnc, (CFMutableStringRef)(*newString))) {
                break;
            }
        }
        CFRelease(tmp);
        CFRelease(newData);
        if (tmpIndex < tmpLen) {
            CFRelease(*newString);
            *newString = NULL;
            return -1;
        } else {
            return i;
        }
    }
}

/* Returned string is retained for the caller; if escapePercents is true, then we do not look for any %-escape encodings in urlString */
static CFStringRef  _convertPercentEscapes(CFStringRef urlString, CFStringEncoding fromEncoding, CFStringEncoding toEncoding, Boolean escapeAllHighBitCharacters, Boolean escapePercents, const UniChar *addlCharsToEscape, int numAddlChars) {
    struct __CFURLEncodingTranslationParameters context;
    context.fromEnc = fromEncoding;
    context.toEnc = toEncoding;
    context.addlChars = addlCharsToEscape;
    context.count = numAddlChars;
    context.escapeHighBit = escapeAllHighBitCharacters;
    context.escapePercents = escapePercents;
    context.agreesOverASCII = (__CFStringEncodingIsSupersetOfASCII(toEncoding) && __CFStringEncodingIsSupersetOfASCII(fromEncoding)) ? true : false;
    context.encodingsMatch = (fromEncoding == toEncoding) ? true : false;
    return _addPercentEscapesToString(CFGetAllocator(urlString), urlString, _shouldEscapeForEncodingConversion, _convertEscapeSequence, toEncoding, &context);
}

// encoding will be used both to interpret the bytes of URLBytes, and to interpret any percent-escapes within the bytes.
CFURLRef CFURLCreateWithBytes(CFAllocatorRef allocator, const uint8_t *URLBytes, CFIndex length, CFStringEncoding encoding, CFURLRef baseURL) {
    CFStringRef  urlString = CFStringCreateWithBytes(allocator, URLBytes, length, encoding, false);
    CFURLRef  result;
    if (!urlString || CFStringGetLength(urlString) == 0) {
        if (urlString) CFRelease(urlString);
        return NULL;
    }
    if (createOldUTF8StyleURLs()) {
        if (encoding != kCFStringEncodingUTF8) {
            CFStringRef  tmp = _convertPercentEscapes(urlString, encoding, kCFStringEncodingUTF8, false, false, NULL, 0);
            CFRelease(urlString);
            urlString = tmp;
            if (!urlString) return NULL;
        }
    }
    
    result = _CFURLAlloc(allocator);
    if (result) {
        _CFURLInitWithString(result, urlString, baseURL);
        if (encoding != kCFStringEncodingUTF8 && !createOldUTF8StyleURLs()) {
            ((struct __CFURL *)result)->_encoding = encoding;
        }
    }
    CFRelease(urlString); // it's retained by result, now.
    return result;
}

CFDataRef CFURLCreateData(CFAllocatorRef allocator, CFURLRef  url, CFStringEncoding encoding, Boolean escapeWhitespace) {
    static const UniChar whitespaceChars[4] = {' ', '\n', '\r', '\t'};
    CFStringRef  myStr = CFURLGetString(url);	
    CFStringRef newStr;
    CFDataRef result;
    if (url->_flags & IS_OLD_UTF8_STYLE) {
        newStr = (encoding == kCFStringEncodingUTF8) ? CFRetain(myStr) : _convertPercentEscapes(myStr, kCFStringEncodingUTF8, encoding, true, false, escapeWhitespace ? whitespaceChars : NULL, escapeWhitespace ? 4 : 0);
    } else {
        newStr=myStr;
        CFRetain(newStr);
    }	
    result = CFStringCreateExternalRepresentation(allocator, newStr, encoding, 0);
    CFRelease(newStr);
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
    CFStringRef relativeString = CFStringCreateWithBytes(alloc, relativeURLBytes, length, encoding, false);
    if (!relativeString) {
        return NULL;
    }
    if (!useCompatibilityMode) {
        CFURLRef url = _CFURLCreateWithArbitraryString(alloc, relativeString, baseURL);
        CFRelease(relativeString);
        ((struct __CFURL *)url)->_encoding = encoding;
        if (url) {
            CFURLRef absURL = CFURLCopyAbsoluteURL(url);
            CFRelease(url);
            return absURL;
        } else {
            return NULL;
        }
    } else {
        UInt32 absFlags = 0;
        CFRange *absRanges;
        CFStringRef absString = NULL;
        Boolean absStringIsMutable = false;
        CFURLRef absURL;
        if (!baseURL) {
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
                        if (!(baseURL->_flags & IS_PARSED)) {
                            _parseComponentsOfURL(baseURL);
                        }
                        absString = resolveAbsoluteURLString(alloc, relString, relFlags, relRanges, baseURL->_string, baseURL->_flags, baseURL->ranges);
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
        _parseComponents(alloc, absString, NULL, &absFlags, &absRanges);
        if (absFlags & HAS_PATH) {
            CFRange pathRg = _rangeForComponent(absFlags, absRanges, HAS_PATH);
            // This is expensive, but it allows us to reuse _resolvedPath.  It should be cleaned up to get this allocation removed at some point. - REW
            UniChar *buf = CFAllocatorAllocate(alloc, sizeof(UniChar) * (pathRg.length + 1), NULL);
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
        }
        return absURL;
    }
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
            } else if (*(idx+1) == '.' && (idx+2 == end || *(idx+2) == pathDelimiter)) {
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
    if (stripTrailingDelimiter && end != pathStr && end-1 != pathStr && *(end-1) == pathDelimiter) {
        end --;
    }
    return CFStringCreateWithCharactersNoCopy(alloc, pathStr, end - pathStr, alloc);
}

static CFMutableStringRef resolveAbsoluteURLString(CFAllocatorRef alloc, CFStringRef relString, UInt32 relFlags, CFRange *relRanges, CFStringRef baseString, UInt32 baseFlags, CFRange *baseRanges) {
    CFMutableStringRef newString = CFStringCreateMutable(alloc, 0);
    CFIndex bufLen = CFStringGetLength(baseString) + CFStringGetLength(relString); // Overkill, but guarantees we never allocate again
    UniChar *buf = CFAllocatorAllocate(alloc, bufLen * sizeof(UniChar), 0);
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
                UniChar *newPathBuf = CFAllocatorAllocate(alloc, sizeof(UniChar) * (relPathRg.length + basePathRg.length + 1), 0);
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
#if defined(__WIN32__)
            if (CFStringGetCharacterAtIndex(newPath, 0) != '/') {
		CFStringAppend(newString, CFSTR("/"));
	    }
#endif
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
    CFURLPathStyle fsType;
    CFAllocatorRef alloc = CFGetAllocator(relativeURL);
    CFStringRef baseString, newString;
    UInt32 baseFlags;
    CFRange *baseRanges;
    Boolean baseIsObjC;

    CFAssert1(relativeURL != NULL, __kCFLogAssertion, "%s(): Cannot create an absolute URL from a NULL relative URL", __PRETTY_FUNCTION__);
    if (CF_IS_OBJC(__kCFURLTypeID, relativeURL)) {
        CFURLRef (*absoluteURLMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
        static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("absoluteURL"); 
        anURL = absoluteURLMsg((const void *)relativeURL, s);
        if (anURL) CFRetain(anURL);
        return anURL;
    } 

    __CFGenericValidateType(relativeURL, __kCFURLTypeID);

    base = relativeURL->_base;
    if (!base) {
        return CFRetain(relativeURL);
    }
    baseIsObjC = CF_IS_OBJC(__kCFURLTypeID, base);
    fsType = URL_PATH_TYPE(relativeURL);

    if (!baseIsObjC && fsType != FULL_URL_REPRESENTATION && fsType == URL_PATH_TYPE(base)) {
        return _CFURLCopyAbsoluteFileURL(relativeURL);
    }
    if (fsType != FULL_URL_REPRESENTATION) {
        _convertToURLRepresentation((struct __CFURL *)relativeURL);
        fsType = FULL_URL_REPRESENTATION;
    }
    if (!(relativeURL->_flags & IS_PARSED)) {
        _parseComponentsOfURL(relativeURL);
    }
    if ((relativeURL->_flags & POSIX_AND_URL_PATHS_MATCH) && !(relativeURL->_flags & (RESOURCE_SPECIFIER_MASK | NET_LOCATION_MASK)) && !baseIsObjC && (URL_PATH_TYPE(base) == kCFURLPOSIXPathStyle)) {
        // There's nothing to relativeURL's string except the path
        CFStringRef newPath = _resolveFileSystemPaths(relativeURL->_string, base->_string, CFURLHasDirectoryPath(base), kCFURLPOSIXPathStyle, alloc);
        CFURLRef result = CFURLCreateWithFileSystemPath(alloc, newPath, kCFURLPOSIXPathStyle, CFURLHasDirectoryPath(relativeURL));
        CFRelease(newPath);
        return result;
    }

    if (!baseIsObjC) {
        CFURLPathStyle baseType = URL_PATH_TYPE(base);
        if (baseType != FULL_URL_REPRESENTATION) {
            _convertToURLRepresentation((struct __CFURL *)base);
        } else if (!(base->_flags & IS_PARSED)) {
            _parseComponentsOfURL(base);
        }
        baseString = base->_string;
        baseFlags = base->_flags;
        baseRanges = base->ranges;
    } else {
        baseString = CFURLGetString(base);
        baseFlags = 0;
        baseRanges = NULL;
        _parseComponents(alloc, baseString, NULL, &baseFlags, &baseRanges);
    }
    
    newString = resolveAbsoluteURLString(alloc, relativeURL->_string, relativeURL->_flags, relativeURL->ranges, baseString, baseFlags, baseRanges);
    if (baseIsObjC) {
        CFAllocatorDeallocate(alloc, baseRanges);
    }
    anURL = _CFURLCreateWithArbitraryString(alloc, newString, NULL);
    CFRelease(newString);
    ((struct __CFURL *)anURL)->_encoding = relativeURL->_encoding;
    return anURL;
}


/*******************/
/* Basic accessors */
/*******************/

Boolean CFURLCanBeDecomposed(CFURLRef  anURL) {
    anURL = _CFURLFromNSURL(anURL);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) return true;
    if (!(anURL->_flags & IS_PARSED)) {
        _parseComponentsOfURL(anURL);
    }
    return ((anURL->_flags & IS_DECOMPOSABLE) != 0);
}

CFStringRef  CFURLGetString(CFURLRef  url) {
    CF_OBJC_FUNCDISPATCH0(__kCFURLTypeID, CFStringRef  , url, "relativeString");
    if (URL_PATH_TYPE(url) != FULL_URL_REPRESENTATION) {
        if (url->_base && (url->_flags & POSIX_AND_URL_PATHS_MATCH)) {
            return url->_string;
        }
        _convertToURLRepresentation((struct __CFURL *)url);
    }
    if (!_haveTestedOriginalString(url)) {
        computeSanitizedString(url);
    }
    if (url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) {
        return url->_string;
    } else {
        return url->_sanatizedString;
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
    CF_OBJC_FUNCDISPATCH0(__kCFURLTypeID, CFURLRef, anURL, "baseURL");
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
    CFAssert1(URL_PATH_TYPE(url) == FULL_URL_REPRESENTATION, __kCFLogAssertion, "%s(): passed a file system URL", __PRETTY_FUNCTION__);
    if (removePercentEscapes) fromOriginalString = true;
    if (!(url->_flags & IS_PARSED)) {
        _parseComponentsOfURL(url);
    }
    rg = _rangeForComponent(url->_flags, url->ranges, compFlag);
    if (rg.location == kCFNotFound) return NULL;
    comp = CFStringCreateWithSubstring(alloc, url->_string, rg);
    if (!fromOriginalString) {
        if (!_haveTestedOriginalString(url)) {
            computeSanitizedString(url);
        }
        if (!(url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) && (url->_flags & (compFlag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG))) {
            CFStringRef newComp = correctedComponent(comp, compFlag, url->_encoding);
            CFRelease(comp);
            comp = newComp;
        }
    }
    if (removePercentEscapes) {
        CFStringRef tmp;
        if (url->_flags & IS_OLD_UTF8_STYLE || url->_encoding == kCFStringEncodingUTF8) {
            tmp = CFURLCreateStringByReplacingPercentEscapes(alloc, comp, CFSTR(""));
        } else {
            tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(alloc, comp, CFSTR(""), url->_encoding);
        }
        CFRelease(comp);
        comp = tmp;
    }
    return comp;
}

CFStringRef  CFURLCopyScheme(CFURLRef  anURL) {
    CFStringRef scheme;
    if (CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        CFStringRef (*schemeMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("scheme"); 
	scheme = schemeMsg((const void *)anURL, s);
        if (scheme) CFRetain(scheme);
        return scheme;
    } 
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyScheme(anURL->_base);
        } else {
            CFRetain(kCFURLFileScheme); // because caller will release it
            return kCFURLFileScheme;
        }
    } 
    scheme = _retainedComponentString(anURL, HAS_SCHEME, true, false);
    if (scheme) {
        return scheme;
    } else if (anURL->_base) {
        return CFURLCopyScheme(anURL->_base);
    } else {
        return NULL;
    }
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
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        // !!! This won't work if we go to putting the vol ref num in the net location for HFS
        if (anURL->_base) {
            return CFURLCopyNetLocation(anURL->_base);
        } else {
            CFRetain(kCFURLLocalhost);
            return kCFURLLocalhost;
        }
    }
    if (!(anURL->_flags & IS_PARSED)) {
        _parseComponentsOfURL(anURL);
    }
    if (anURL->_flags & NET_LOCATION_MASK) {
        // We provide the net location
        CFRange netRg = _netLocationRange(anURL->_flags, anURL->ranges);
        CFStringRef netLoc;
        if (!_haveTestedOriginalString(anURL)) {
            computeSanitizedString(anURL);
        }
        if (!(anURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) && (anURL->_flags & (USER_DIFFERS | PASSWORD_DIFFERS | HOST_DIFFERS | PORT_DIFFERS))) {
            // Only thing that can come before the net location is the scheme.  It's impossible for the scheme to contain percent escapes.  Therefore, we can use the location of netRg in _sanatizedString, just not the length. 
            CFRange netLocEnd;
            netRg.length = CFStringGetLength(anURL->_sanatizedString) - netRg.location;
            if (CFStringFindWithOptions(anURL->_sanatizedString, CFSTR("/"), netRg, 0, &netLocEnd)) {
                netRg.length = netLocEnd.location - netRg.location;
            }
            netLoc = CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_sanatizedString, netRg);
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
    if (URL_PATH_TYPE(anURL) == kCFURLPOSIXPathStyle && (anURL->_flags & POSIX_AND_URL_PATHS_MATCH)) {
        CFRetain(anURL->_string);
        return anURL->_string;
    }
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        _convertToURLRepresentation((struct __CFURL *)anURL);
    }
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
    if (URL_PATH_TYPE(anURL) == FULL_URL_REPRESENTATION) {
        if (!(anURL->_flags & IS_PARSED)) {
            _parseComponentsOfURL(anURL);
        }
        if (!anURL->_base || (anURL->_flags & (HAS_PATH | NET_LOCATION_MASK))) {
            return ((anURL->_flags & IS_DIRECTORY) != 0);
        }
        return CFURLHasDirectoryPath(anURL->_base);
    }
    return ((anURL->_flags & IS_DIRECTORY) != 0);
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
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    if (!(anURL->_flags & IS_PARSED)) {
        _parseComponentsOfURL(anURL);
    }
    if (!(anURL->_flags & IS_DECOMPOSABLE)) {
        CFRange schemeRg = _rangeForComponent(anURL->_flags, anURL->ranges, HAS_SCHEME);
        CFIndex base = schemeRg.location + schemeRg.length + 1;
        if (!_haveTestedOriginalString(anURL)) {
            computeSanitizedString(anURL);
        }
        if (anURL->_sanatizedString) {
            // It is impossible to have a percent escape in the scheme (if there were one, we would have considered the URL a relativeURL with a  colon in the path instead), so this range computation is always safe.
            return CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_sanatizedString, CFRangeMake(base, CFStringGetLength(anURL->_sanatizedString)-base));
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
            if (!(anURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH)) {
                // See if any pieces in the resource specifier differ between sanitized string and original string
                for (flag = firstRsrcSpecFlag; flag != (HAS_FRAGMENT << 1); flag = flag << 1) {
                    if (anURL->_flags & (flag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG)) {
                        canUseOriginalString = false;
                        break;
                    }
                }
            }
            if (!canUseOriginalString) {
                // If none of the pieces prior to the first resource specifier flag differ, then we can use the offset from the original string as the offset in the sanitized string.
                for (flag = firstRsrcSpecFlag >> 1; flag != 0; flag = flag >> 1) {
                    if (anURL->_flags & (flag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG)) {
                        canUseSanitizedString = false;
                        break;
                    }
                }
            }
            if (canUseOriginalString) {
                CFRange rg = _rangeForComponent(anURL->_flags, anURL->ranges, firstRsrcSpecFlag);
                rg.location --; // Include the character that demarcates the component
                rg.length = CFStringGetLength(anURL->_string) - rg.location;
                return CFStringCreateWithSubstring(alloc, anURL->_string, rg);
            } else if (canUseSanitizedString) {
                CFRange rg = _rangeForComponent(anURL->_flags, anURL->ranges, firstRsrcSpecFlag);
                rg.location --; // Include the character that demarcates the component
                rg.length = CFStringGetLength(anURL->_sanatizedString) - rg.location;
                return CFStringCreateWithSubstring(alloc, anURL->_sanatizedString, rg);
            } else {
                // Must compute the correct string to return; just reparse....
                UInt32 sanFlags = 0;
                CFRange *sanRanges = NULL;
                CFRange rg; 
                _parseComponents(alloc, anURL->_sanatizedString, anURL->_base, &sanFlags, &sanRanges);
                rg = _rangeForComponent(sanFlags, sanRanges, firstRsrcSpecFlag);
                CFAllocatorDeallocate(alloc, sanRanges);
                rg.location --; // Include the character that demarcates the component
                rg.length = CFStringGetLength(anURL->_sanatizedString) - rg.location;
                return CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_sanatizedString, rg);
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
        CFStringRef (*hostMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("host"); 
	tmp = hostMsg((const void *)anURL, s);
        if (tmp) CFRetain(tmp);
        return tmp;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyHostName(anURL->_base);
        } else {
            CFRetain(kCFURLLocalhost);
            return kCFURLLocalhost;
        }
    }
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
        CFNumberRef (*portMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("port"); 
	CFNumberRef cfPort = portMsg((const void *)anURL, s);
        SInt32 num;
        if (cfPort && CFNumberGetValue(cfPort, kCFNumberSInt32Type, &num)) return num;
        return -1;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLGetPortNumber(anURL->_base);
        }
        return -1;
    }
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
        CFStringRef (*userMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("user"); 
	user = userMsg((const void *)anURL, s);
        if (user) CFRetain(user);
        return user;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyUserName(anURL->_base);
        }
        return NULL;
    }
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
        CFStringRef (*passwordMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("password"); 
	passwd = passwordMsg((const void *)anURL, s);
        if (passwd) CFRetain(passwd);
        return passwd;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyPassword(anURL->_base);
        }
        return NULL;
    }
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
        CFStringRef (*paramMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("parameterString"); 
	str = paramMsg((const void *)anURL, s);
        if (str) CFRetain(str);
        return str;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    str = _retainedComponentString(anURL, HAS_PARAMETERS, false, false);
    if (str) return str;
    if (!(anURL->_flags & IS_DECOMPOSABLE)) return NULL;
    if (!anURL->_base || (anURL->_flags & (NET_LOCATION_MASK | HAS_PATH | HAS_SCHEME))) {
        return NULL;
        // Parameter string definitely coming from the relative portion of the URL
    }
    return _unescapedParameterString(anURL->_base);
}

CFStringRef  CFURLCopyParameterString(CFURLRef  anURL, CFStringRef charactersToLeaveEscaped) {
    CFStringRef  param = _unescapedParameterString(anURL);
    if (param) {
        CFStringRef result;
        if (anURL->_flags & IS_OLD_UTF8_STYLE || anURL->_encoding == kCFStringEncodingUTF8) {
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
        CFStringRef (*queryMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("query"); 
	str = queryMsg((const void *)anURL, s);
        if (str) CFRetain(str);
        return str;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
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
        if (anURL->_flags & IS_OLD_UTF8_STYLE || anURL->_encoding == kCFStringEncodingUTF8) {
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
        CFStringRef (*fragmentMsg)(const void *, SEL) = (void *)__CFSendObjCMsg;
	static SEL s = NULL;  if (!s) s = __CFGetObjCSelector("fragment"); 
	str = fragmentMsg((const void *)anURL, s);
        if (str) CFRetain(str);
        return str;
    } 
    __CFGenericValidateType(anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    str = _retainedComponentString(anURL, HAS_FRAGMENT, false, false);
    return str;
}

CFStringRef  CFURLCopyFragment(CFURLRef  anURL, CFStringRef  charactersToLeaveEscaped) {
    CFStringRef  fragment = _unescapedFragment(anURL);
    if (fragment) {
        CFStringRef tmp;
        if (anURL->_flags & IS_OLD_UTF8_STYLE || anURL->_encoding == kCFStringEncodingUTF8) {
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
        // Do not have to worry about the non-decomposable case here.
        return _rangeForComponent(url->_flags, url->ranges, HAS_SCHEME).length + 3;
    } else {
        // For all other components, the separator precedes the component, so there's no need
        // to add extra chars to get to the next insertion point
        CFRange rg = _rangeForComponent(url->_flags, url->ranges, lastComponentBeforeMask);
        return rg.location + rg.length;
    }
}

static CFRange _CFURLGetCharRangeForMask(CFURLRef url, CFOptionFlags mask, CFRange *charRangeWithSeparators) {
    CFOptionFlags currentOption;
    CFOptionFlags firstMaskFlag = HAS_SCHEME;
    Boolean haveReachedMask = false;
    CFIndex beforeMask = 0;
    CFIndex afterMask = kCFNotFound;
    CFRange *currRange = url->ranges;
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
        CFRange schemeRg = _rangeForComponent(url->_flags, url->ranges, HAS_SCHEME);
        rangeIncludingSeparators->location = 0;
        rangeIncludingSeparators->length = schemeRg.length + 1;
        return schemeRg;
    } else if (component == kCFURLComponentResourceSpecifier) {
        CFRange schemeRg = _rangeForComponent(url->_flags, url->ranges, HAS_SCHEME);
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
    if (!(url->_flags & IS_PARSED)) {
        _parseComponentsOfURL(url);
    }

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

/* We convert to the CFURL immediately at the beginning of decomposition, so all the decomposition routines need not worry about having an ObjC NSURL */
static CFStringRef schemeSpecificString(CFURLRef url) {
    Boolean isDir;
    isDir = ((url->_flags & IS_DIRECTORY) != 0);
    switch (URL_PATH_TYPE(url)) {
    case kCFURLPOSIXPathStyle:
        if (url->_flags & POSIX_AND_URL_PATHS_MATCH) {
            return CFRetain(url->_string);
        } else {
            return POSIXPathToURLPath(url->_string, CFGetAllocator(url), isDir);
        }
    case kCFURLHFSPathStyle:
        return HFSPathToURLPath(url->_string, CFGetAllocator(url), isDir);
    case kCFURLWindowsPathStyle:
        return WindowsPathToURLPath(url->_string, CFGetAllocator(url), isDir);
    case FULL_URL_REPRESENTATION:
        return CFURLCopyResourceSpecifier(url);
    default:
        return NULL;
    }
}

static Boolean decomposeToNonHierarchical(CFURLRef url, CFURLComponentsNonHierarchical *components) {
    if (CFURLGetBaseURL(url) != NULL)  {
        components->scheme = NULL;
    } else {
        components->scheme = CFURLCopyScheme(url);
    }
    components->schemeSpecific = schemeSpecificString(url);
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
    int pathType;
    static CFStringRef emptyStr = NULL;
    if (!emptyStr) {
        emptyStr = CFSTR("");
    }

    if (!CFURLCanBeDecomposed(url)) {
        return false;
    }
    if ((pathType = URL_PATH_TYPE(url)) == FULL_URL_REPRESENTATION) {
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
    } else {
        switch (pathType) {
        case kCFURLPOSIXPathStyle: {
            CFStringRef pathStr;
            if (url->_flags & POSIX_AND_URL_PATHS_MATCH) {
                pathStr = url->_string;
                CFRetain(pathStr);
            } else {
                pathStr = POSIXPathToURLPath(url->_string, alloc, url->_flags & IS_DIRECTORY);
            }
            components->pathComponents = CFStringCreateArrayBySeparatingStrings(alloc, pathStr, CFSTR("/"));
            CFRelease(pathStr);
            break;
        }
        case kCFURLHFSPathStyle:
            components->pathComponents = HFSPathToURLComponents(url->_string, alloc, ((url->_flags & IS_DIRECTORY) != 0));
            break;
        case kCFURLWindowsPathStyle:
            components->pathComponents = WindowsPathToURLComponents(url->_string, alloc, ((url->_flags & IS_DIRECTORY) != 0));
            break;
        default:
            components->pathComponents = NULL;
        }
        if (!components->pathComponents) {
            return false;
        }
        components->scheme = CFRetain(kCFURLFileScheme);
        components->user = NULL;
        components->password = NULL;
        components->host = CFRetain(kCFURLLocalhost);
        components->port = kCFNotFound;
        components->parameterString = NULL;
        components->query = NULL;
        components->fragment = NULL;
        components->baseURL = CFURLGetBaseURL(url);
        if (components->baseURL) CFRetain(components->baseURL);
    }
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
        if (comp->port != kCFNotFound) {
            CFStringAppendFormat(urlString, NULL, CFSTR(":%d"), comp->port);
        }
        hadPrePathComponent = true;
    }
    if (hadPrePathComponent && (comp->pathComponents == NULL || CFStringGetLength(CFArrayGetValueAtIndex(comp->pathComponents, 0)) != 0)) {
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
    if (hadPrePathComponent && (comp->pathComponents == NULL || CFStringGetLength(CFArrayGetValueAtIndex(comp->pathComponents, 0)) != 0)) {
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
    return url->_reserved;
}

CF_EXPORT void __CFURLSetReservedPtr(CFURLRef  url, void *ptr) {
    ((struct __CFURL *)url)->_reserved = ptr;
}


/* File system stuff */

/* HFSPath<->URLPath functions at the bottom of the file */
static CFArrayRef WindowsPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef tmp;
    CFMutableArrayRef urlComponents = NULL;
    CFStringRef str;
    UInt32 i=0;

    tmp = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR("\\"));
    urlComponents = CFArrayCreateMutableCopy(alloc, 0, tmp);
    CFRelease(tmp);
/* We must not replace ".:" with ".|" on WIN32.
 * (Sergey Zubarev)
 */
#if !defined(__WIN32__)
    str = CFArrayGetValueAtIndex(urlComponents, 0);
    if (CFStringGetLength(str) == 2 && CFStringGetCharacterAtIndex(str, 1) == ':') {
        CFStringRef newComponent = CFStringCreateWithFormat(alloc, NULL, CFSTR("%c|"), CFStringGetCharacterAtIndex(str, 0));
        CFArraySetValueAtIndex(urlComponents, 0, newComponent);
        CFRelease(newComponent);
        CFArrayInsertValueAtIndex(urlComponents, 0, CFSTR("")); // So we get a leading '/' below
        i = 2; // Skip over the drive letter and the empty string we just inserted
    }
#endif // __WIN32__
#if defined(__WIN32__)
    // cjk: should this be done on all platforms?
    int c;
    for (c = CFArrayGetCount(urlComponents); i < c; i ++) {
        CFStringRef fileComp = CFArrayGetValueAtIndex(urlComponents,i);
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
#endif
    if (isDir) {
        if (CFStringGetLength(CFArrayGetValueAtIndex(urlComponents, CFArrayGetCount(urlComponents) - 1)) != 0)
            CFArrayAppendValue(urlComponents, CFSTR(""));
    }
    return urlComponents;
}

static CFStringRef WindowsPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef urlComponents;
    CFArrayRef newComponents;
    CFStringRef str;

    if (CFStringGetLength(path) == 0) return CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);
    urlComponents = WindowsPathToURLComponents(path, alloc, isDir);
    if (!urlComponents) return CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);

    newComponents = copyStringArrayWithTransformation(urlComponents, escapeWindowsPathComponent);
    if (newComponents) {
        str = CFStringCreateByCombiningStrings(alloc, newComponents, CFSTR("/"));
        CFRelease(newComponents);
    } else {
        str = CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);
    }
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


static CFStringRef URLPathToWindowsPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding) {
    // Check for a drive letter, then flip all the slashes
    CFStringRef result;
    CFArrayRef tmp = CFStringCreateArrayBySeparatingStrings(allocator, path, CFSTR("/"));
    SInt32 count = CFArrayGetCount(tmp);
    CFMutableArrayRef components = CFArrayCreateMutableCopy(allocator, count, tmp);
    CFStringRef newPath;
    
    CFRelease(tmp);
    if (CFStringGetLength(CFArrayGetValueAtIndex(components,count-1)) == 0) {
        CFArrayRemoveValueAtIndex(components, count-1);
        count --;
    }
    if (count > 1 && CFStringGetLength(CFArrayGetValueAtIndex(components, 0)) == 0) {
        // Absolute path; we need to remove the first component, and check for a drive letter in the second component
        CFStringRef firstComponent = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, CFArrayGetValueAtIndex(components, 1), CFSTR(""), encoding);
        CFArrayRemoveValueAtIndex(components, 0);
        if (CFStringGetLength(firstComponent) == 2 && CFStringGetCharacterAtIndex(firstComponent, 1) == '|') {
            // Drive letter
            CFStringRef driveStr = CFStringCreateWithFormat(allocator, NULL, CFSTR("%c:"), CFStringGetCharacterAtIndex(firstComponent, 0));
            CFArraySetValueAtIndex(components, 0, driveStr);
            CFRelease(driveStr);
        }
        CFRelease(firstComponent);
    }

    newPath = CFStringCreateByCombiningStrings(allocator, components, CFSTR("\\"));
    CFRelease(components);
    result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, newPath, CFSTR(""), encoding);
    CFRelease(newPath);
    return result;
}

// converts url from a file system path representation to a standard representation
static void _convertToURLRepresentation(struct __CFURL *url) {
    CFStringRef path = NULL;
    Boolean isDir = ((url->_flags & IS_DIRECTORY) != 0);
    CFAllocatorRef alloc = CFGetAllocator(url);

#if DEBUG_URL_MEMORY_USAGE
    numFileURLsConverted ++;
#endif

    switch (URL_PATH_TYPE(url)) {
        case kCFURLPOSIXPathStyle:
            if (url->_flags & POSIX_AND_URL_PATHS_MATCH) {
                path = CFRetain(url->_string);
            } else {
                path = POSIXPathToURLPath(url->_string, alloc, isDir);
            }
            break;
        case kCFURLHFSPathStyle:
            path = HFSPathToURLPath(url->_string, alloc, isDir);
            break;
        case kCFURLWindowsPathStyle:
            path = WindowsPathToURLPath(url->_string, alloc, isDir);
            break;
    }
    CFAssert2(path != NULL, __kCFLogAssertion, "%s(): Encountered malformed file system URL %@", __PRETTY_FUNCTION__, url);
    if (!url->_base) {
        CFStringRef str;
        str = CFStringCreateWithFormat(alloc, NULL, CFSTR("file://localhost%@"), path);
        url->_flags = (url->_flags & (IS_DIRECTORY)) | (FULL_URL_REPRESENTATION << 16) | IS_DECOMPOSABLE | IS_ABSOLUTE | IS_PARSED | HAS_SCHEME | HAS_HOST | HAS_PATH | ORIGINAL_AND_URL_STRINGS_MATCH;
        CFRelease(url->_string);
        url->_string = str;
        url->ranges = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange) * 3, 0);
        url->ranges[0] = CFRangeMake(0, 4);
        url->ranges[1] = CFRangeMake(7, 9);
        url->ranges[2] = CFRangeMake(16, CFStringGetLength(path));
        CFRelease(path);
    } else {
        CFRelease(url->_string);
        url->_flags = (url->_flags & (IS_DIRECTORY)) | (FULL_URL_REPRESENTATION << 16) | IS_DECOMPOSABLE | IS_PARSED | HAS_PATH | ORIGINAL_AND_URL_STRINGS_MATCH;
        url->_string = path;
        url->ranges = (CFRange *)CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
        *(url->ranges) = CFRangeMake(0, CFStringGetLength(path));
    }
}

// relativeURL is known to be a file system URL whose base is a matching file system URL
static CFURLRef _CFURLCopyAbsoluteFileURL(CFURLRef relativeURL) {
    CFAllocatorRef alloc = CFGetAllocator(relativeURL);
    CFURLPathStyle fsType = URL_PATH_TYPE(relativeURL);
    CFURLRef base = relativeURL->_base;
    CFStringRef newPath = _resolveFileSystemPaths(relativeURL->_string, base->_string, (base->_flags & IS_DIRECTORY) != 0, fsType, alloc);
    CFURLRef result =  CFURLCreateWithFileSystemPath(alloc, newPath, fsType, (relativeURL->_flags & IS_DIRECTORY) != 0);
    CFRelease(newPath);
    return result;
}

// Caller must release the returned string
static CFStringRef _resolveFileSystemPaths(CFStringRef relativePath, CFStringRef basePath, Boolean baseIsDir, CFURLPathStyle fsType, CFAllocatorRef alloc) {
    CFIndex baseLen = CFStringGetLength(basePath);
    CFIndex relLen = CFStringGetLength(relativePath);
    UniChar pathDelimiter = PATH_DELIM_FOR_TYPE(fsType);
    UniChar *buf = CFAllocatorAllocate(alloc, sizeof(UniChar)*(relLen + baseLen + 2), 0);
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
#if defined(__MACOS8__)
    short vRefNum;
    long dirID;
    FSSpec fsSpec;
    if (HGetVol(NULL, &vRefNum, &dirID) == noErr && FSMakeFSSpec(vRefNum, dirID, NULL, &fsSpec) == noErr) {
	url = _CFCreateURLFromFSSpec(allocator, (void *)(&fsSpec), true);
    }
#else
    uint8_t buf[CFMaxPathSize + 1];
    if (_CFGetCurrentDirectory(buf, CFMaxPathLength)) {
        url = CFURLCreateFromFileSystemRepresentation(allocator, buf, strlen(buf), true);
    }
#endif
    return url;
}

CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef allocator, CFStringRef filePath, CFURLPathStyle fsType, Boolean isDirectory) {
    Boolean isAbsolute = true;
    CFIndex len = CFStringGetLength(filePath);
    CFURLRef baseURL, result;

    CFAssert2(fsType == kCFURLPOSIXPathStyle || fsType == kCFURLHFSPathStyle || fsType == kCFURLWindowsPathStyle, __kCFLogAssertion, "%s(): encountered unknown path style %d", __PRETTY_FUNCTION__, fsType);
    CFAssert1(filePath != NULL, __kCFLogAssertion, "%s(): NULL filePath argument not permitted", __PRETTY_FUNCTION__);

    switch(fsType) {
        case kCFURLPOSIXPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) == '/');
            break;
        case kCFURLWindowsPathStyle:
            isAbsolute = (len > 3 && CFStringGetCharacterAtIndex(filePath, 1) == ':' && CFStringGetCharacterAtIndex(filePath, 2) == '\\');
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
    Boolean isAbsolute = true, releaseFilePath = false;
    UniChar pathDelim = '\0';
    CFIndex len = CFStringGetLength(filePath);

    CFAssert1(filePath != NULL, __kCFLogAssertion, "%s(): NULL path string not permitted", __PRETTY_FUNCTION__);
    CFAssert2(fsType == kCFURLPOSIXPathStyle || fsType == kCFURLHFSPathStyle || fsType == kCFURLWindowsPathStyle, __kCFLogAssertion, "%s(): encountered unknown path style %d", __PRETTY_FUNCTION__, fsType);
    
    switch(fsType) {
        case kCFURLPOSIXPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) == '/');
            pathDelim = '/';
            break;
        case kCFURLWindowsPathStyle: 
            isAbsolute = (len > 3 && CFStringGetCharacterAtIndex(filePath, 1) == ':' && CFStringGetCharacterAtIndex(filePath, 2) == '\\');
	    /* Absolute path under Win32 can begin with "\\"
	     * (Sergey Zubarev)
	     */
	    if (!isAbsolute) isAbsolute = (len > 2 && CFStringGetCharacterAtIndex(filePath, 0) == '\\' && CFStringGetCharacterAtIndex(filePath, 1) == '\\');
             pathDelim = '\\';
            break;
        case kCFURLHFSPathStyle: 
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) != ':');
            pathDelim = ':';
            break;
    }
    if (isAbsolute) {
        baseURL = NULL;
    } 
    if (isDirectory && len > 0 && CFStringGetCharacterAtIndex(filePath, len-1) != pathDelim) {
        filePath = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%c"), filePath, pathDelim);
        releaseFilePath = true;
    } else if (!isDirectory && len > 0 && CFStringGetCharacterAtIndex(filePath, len-1) == pathDelim) {
        if (len == 1 || CFStringGetCharacterAtIndex(filePath, len-2) == pathDelim) {
            // Override isDirectory
            isDirectory = true;
        } else {
            filePath = CFStringCreateWithSubstring(allocator, filePath, CFRangeMake(0, len-1));
            releaseFilePath = true;
        }
    }
    if (!filePath || CFStringGetLength(filePath) == 0) {
        if (releaseFilePath && filePath) CFRelease(filePath);
        return NULL;
    }
    url = _CFURLAlloc(allocator);
    _CFURLInit((struct __CFURL *)url, filePath, fsType, baseURL);
    if (releaseFilePath) CFRelease(filePath);
    if (isDirectory) ((struct __CFURL *)url)->_flags |= IS_DIRECTORY;
    if (fsType == kCFURLPOSIXPathStyle) {
        // Check if relative path is equivalent to URL representation; this will be true if url->_string contains only characters from the unreserved character set, plus '/' to delimit the path, plus ':', '@', '&', '=', '+', '$', ',' (according to RFC 2396) -- REW, 12/1/2000
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
            CFStringRef newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("./%@"), url->_string);
            CFRelease(url->_string);
            ((struct __CFURL *)url)->_string = newString;
        }
    }
    return url;
}

CFStringRef CFURLCopyFileSystemPath(CFURLRef anURL, CFURLPathStyle pathStyle) {
    CFAssert2(pathStyle == kCFURLPOSIXPathStyle || pathStyle == kCFURLHFSPathStyle || pathStyle == kCFURLWindowsPathStyle, __kCFLogAssertion, "%s(): Encountered unknown path style %d", __PRETTY_FUNCTION__, pathStyle);
    return CFURLCreateStringWithFileSystemPath(CFGetAllocator(anURL), anURL, pathStyle, false);
}

// There is no matching ObjC method for this functionality; because this function sits on top of the CFURL primitives, it's o.k. not to check for the need to dispatch an ObjC method instead, but this means care must be taken that this function never call anything that will result in dereferencing anURL without first checking for an ObjC dispatch.  -- REW, 10/29/98
CFStringRef CFURLCreateStringWithFileSystemPath(CFAllocatorRef allocator, CFURLRef anURL, CFURLPathStyle fsType, Boolean resolveAgainstBase) {
    CFURLRef base = resolveAgainstBase ? CFURLGetBaseURL(anURL) : NULL;
    CFStringRef basePath = base ? CFURLCreateStringWithFileSystemPath(allocator, base, fsType, false) : NULL;
    CFStringRef relPath = NULL;
    
    if (!CF_IS_OBJC(__kCFURLTypeID, anURL)) {
        // We can grope the ivars
        CFURLPathStyle myType = URL_PATH_TYPE(anURL);
        if (myType == fsType) {
            relPath = CFRetain(anURL->_string);
        } else if (fsType == kCFURLPOSIXPathStyle && myType == FULL_URL_REPRESENTATION) {
            if (!(anURL->_flags & IS_PARSED)) {
                _parseComponentsOfURL(anURL);
            }
            if (anURL->_flags & POSIX_AND_URL_PATHS_MATCH) {
                relPath = _retainedComponentString(anURL, HAS_PATH, true, true);
            }
        }
    }

    if (relPath == NULL) {
        CFStringRef urlPath = CFURLCopyPath(anURL);
        CFStringEncoding enc = (anURL->_flags & IS_OLD_UTF8_STYLE) ? kCFStringEncodingUTF8 : anURL->_encoding;
        if (urlPath) {
            switch (fsType) {
                case kCFURLPOSIXPathStyle:
                    relPath = URLPathToPOSIXPath(urlPath, allocator, enc);
                    break;
                case kCFURLHFSPathStyle:
                    relPath = URLPathToHFSPath(urlPath, allocator, enc);
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
    if (relPath && CFURLHasDirectoryPath(anURL) && CFStringGetLength(relPath) > 1 && CFStringGetCharacterAtIndex(relPath, CFStringGetLength(relPath)-1) == PATH_DELIM_FOR_TYPE(fsType)) {
        CFStringRef tmp = CFStringCreateWithSubstring(allocator, relPath, CFRangeMake(0, CFStringGetLength(relPath)-1));
        CFRelease(relPath);
        relPath = tmp;
    }

    // Note that !resolveAgainstBase implies !base
    if (!basePath || !relPath) {
        return relPath;
    } else {
        CFStringRef result = _resolveFileSystemPaths(relPath, basePath, CFURLHasDirectoryPath(base), fsType, allocator);
        CFRelease(basePath);
        CFRelease(relPath);
        return result;
    }
}

Boolean CFURLGetFileSystemRepresentation(CFURLRef url, Boolean resolveAgainstBase, uint8_t *buffer, CFIndex bufLen) {
    CFStringRef path;
    CFAllocatorRef alloc = CFGetAllocator(url);

    if (!url) return false;
#if defined(__WIN32__)
    path = CFURLCreateStringWithFileSystemPath(alloc, url, kCFURLWindowsPathStyle, resolveAgainstBase);
#elif defined(__MACOS8__)
    path = CFURLCreateStringWithFileSystemPath(alloc, url, kCFURLHFSPathStyle, resolveAgainstBase);
#else
    path = CFURLCreateStringWithFileSystemPath(alloc, url, kCFURLPOSIXPathStyle, resolveAgainstBase);
#endif
    if (path) {
#if defined(__MACH__)
        Boolean convResult = _CFStringGetFileSystemRepresentation(path, buffer, bufLen);
        CFRelease(path);
        return convResult;
#else
        CFIndex usedLen;
        CFIndex pathLen = CFStringGetLength(path);
        CFIndex numConverted = CFStringGetBytes(path, CFRangeMake(0, pathLen), CFStringFileSystemEncoding(), 0, true, buffer, bufLen-1, &usedLen); // -1 because we need one byte to zero-terminate.
        CFRelease(path);
        if (numConverted == pathLen) {
            buffer[usedLen] = '\0';
            return true;
        }
#endif
    }
    return false;
}

CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef allocator, const uint8_t *buffer, CFIndex bufLen, Boolean isDirectory) {
    CFStringRef path = CFStringCreateWithBytes(allocator, buffer, bufLen, CFStringFileSystemEncoding(), false);
    CFURLRef newURL;
    if (!path) return NULL;
#if defined(__WIN32__)
    newURL = CFURLCreateWithFileSystemPath(allocator, path, kCFURLWindowsPathStyle, isDirectory);
#elif defined(__MACOS8__)
    newURL = CFURLCreateWithFileSystemPath(allocator, path, kCFURLHFSPathStyle, isDirectory);
#else
    newURL = CFURLCreateWithFileSystemPath(allocator, path, kCFURLPOSIXPathStyle, isDirectory);
#endif
    CFRelease(path);
    return newURL;
}

CF_EXPORT CFURLRef CFURLCreateFromFileSystemRepresentationRelativeToBase(CFAllocatorRef allocator, const uint8_t *buffer, CFIndex bufLen, Boolean isDirectory, CFURLRef baseURL) {
    CFStringRef path = CFStringCreateWithBytes(allocator, buffer, bufLen, CFStringFileSystemEncoding(), false);
    CFURLRef newURL;
    if (!path) return NULL;
#if defined(__WIN32__)
    newURL = CFURLCreateWithFileSystemPathRelativeToBase(allocator, path, kCFURLWindowsPathStyle, isDirectory, baseURL);
#elif defined(__MACOS8__)
    newURL = CFURLCreateWithFileSystemPathRelativeToBase(allocator, path, kCFURLHFSPathStyle, isDirectory, baseURL);
#else
    newURL = CFURLCreateWithFileSystemPathRelativeToBase(allocator, path, kCFURLPOSIXPathStyle, isDirectory, baseURL);
#endif
    CFRelease(path);
    return newURL;
}


/******************************/
/* Support for path utilities */
/******************************/

// Assumes url is a CFURL (not an Obj-C NSURL)
static CFRange _rangeOfLastPathComponent(CFURLRef url) {
    UInt32 pathType = URL_PATH_TYPE(url);
    CFRange pathRg, componentRg;
    
    if (pathType ==  FULL_URL_REPRESENTATION) {
        if (!(url->_flags & IS_PARSED)) _parseComponentsOfURL(url);
        pathRg = _rangeForComponent(url->_flags, url->ranges, HAS_PATH);
    } else {
        pathRg = CFRangeMake(0, CFStringGetLength(url->_string));
    }

    if (pathRg.location == kCFNotFound || pathRg.length == 0) {
        // No path
        return pathRg;
    }
    if (CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) == PATH_DELIM_FOR_TYPE(pathType)) {
        pathRg.length --;
        if (pathRg.length == 0) {
            pathRg.length ++;
            return pathRg;
        }
    }
    if (CFStringFindWithOptions(url->_string, PATH_DELIM_AS_STRING_FOR_TYPE(pathType), pathRg, kCFCompareBackwards, &componentRg)) {
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
        CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFIndex length;
        CFRange rg, compRg;
        if (!path) return NULL;
        rg = CFRangeMake(0, CFStringGetLength(path));
        length = rg.length; // Remember this for comparison later
        if (CFStringGetCharacterAtIndex(path, rg.length - 1) == '/') {
            rg.length --;
        }
        if (CFStringFindWithOptions(path, CFSTR("/"), rg, kCFCompareBackwards, &compRg)) {
            rg.length = rg.location + rg.length - (compRg.location+1);
            rg.location = compRg.location + 1;
        }
        if (rg.location == 0 && rg.length == length) {
            result = path;
        } else {
            result = CFStringCreateWithSubstring(NULL, path, rg);
            CFRelease(path);
        }
    } else {
        CFRange rg = _rangeOfLastPathComponent(url);
        if (rg.location == kCFNotFound || rg.length == 0) {
            // No path
            return CFRetain(CFSTR(""));
        }
        if (rg.length == 1 && CFStringGetCharacterAtIndex(url->_string, rg.location) == PATH_DELIM_FOR_TYPE(URL_PATH_TYPE(url))) {
            return CFRetain(CFSTR("/"));
        }
        result = CFStringCreateWithSubstring(CFGetAllocator(url), url->_string, rg);
        if (URL_PATH_TYPE(url) == FULL_URL_REPRESENTATION && !(url->_flags & POSIX_AND_URL_PATHS_MATCH)) {
            CFStringRef tmp;
            if (url->_flags & IS_OLD_UTF8_STYLE || url->_encoding == kCFStringEncodingUTF8) {
                tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(url), result, CFSTR(""));
            } else {
                tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(url), result, CFSTR(""), url->_encoding);
            }
            CFRelease(result);
            result = tmp;
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
                ext = CFRetain(CFSTR(""));
            }
        }
        CFRelease(lastPathComp);
    }
    return ext;
}

CFURLRef CFURLCreateCopyAppendingPathComponent(CFAllocatorRef allocator, CFURLRef url, CFStringRef pathComponent, Boolean isDirectory) {
    UInt32 fsType;
    CFURLRef result;
    url = _CFURLFromNSURL(url);
    __CFGenericValidateType(url, __kCFURLTypeID);
    CFAssert1(pathComponent != NULL, __kCFLogAssertion, "%s(): Cannot be called with a NULL component to append", __PRETTY_FUNCTION__);

    fsType = URL_PATH_TYPE(url);
    if (fsType != FULL_URL_REPRESENTATION && CFStringFindWithOptions(pathComponent, PATH_DELIM_AS_STRING_FOR_TYPE(fsType), CFRangeMake(0, CFStringGetLength(pathComponent)), 0, NULL)) {
        // Must convert to full representation, and then work with it
        fsType = FULL_URL_REPRESENTATION;
        _convertToURLRepresentation((struct __CFURL *)url);
    }

    if (fsType == FULL_URL_REPRESENTATION) {
        CFMutableStringRef newString;
        CFStringRef newComp;
        CFRange pathRg;
        if (!(url->_flags & IS_PARSED)) _parseComponentsOfURL(url);
        if (!(url->_flags & HAS_PATH)) return NULL;

        newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
        newComp = CFURLCreateStringByAddingPercentEscapes(allocator, pathComponent, NULL, CFSTR(";?"),  (url->_flags & IS_OLD_UTF8_STYLE) ? kCFStringEncodingUTF8 : url->_encoding);
        pathRg = _rangeForComponent(url->_flags, url->ranges, HAS_PATH);
        if (!pathRg.length || CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) != '/') {
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
    } else {
        UniChar pathDelim = PATH_DELIM_FOR_TYPE(fsType);
        CFStringRef newString;
        if (CFStringGetCharacterAtIndex(url->_string, CFStringGetLength(url->_string) - 1) != pathDelim) {
            if (isDirectory) {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%c%@%c"), url->_string, pathDelim, pathComponent, pathDelim);
            } else {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%c%@"), url->_string, pathDelim, pathComponent);
            }
        } else {
            if (isDirectory) {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%@%c"), url->_string, pathComponent, pathDelim);
            } else {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%@"), url->_string, pathComponent);
            }
        }
        result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, newString, fsType, isDirectory, url->_base);
        CFRelease(newString);
    }
    return result;
}

CFURLRef CFURLCreateCopyDeletingLastPathComponent(CFAllocatorRef allocator, CFURLRef url) {
    CFURLRef result;
    CFMutableStringRef newString;
    CFRange lastCompRg, pathRg;
    Boolean appendDotDot = false;
    UInt32 fsType;

    url = _CFURLFromNSURL(url);
    CFAssert1(url != NULL, __kCFLogAssertion, "%s(): NULL argument not allowed", __PRETTY_FUNCTION__);
    __CFGenericValidateType(url, __kCFURLTypeID);

    fsType = URL_PATH_TYPE(url);
    if (fsType == FULL_URL_REPRESENTATION) {
        if (!(url->_flags & IS_PARSED)) _parseComponentsOfURL(url);
        if (!(url->_flags & HAS_PATH)) return NULL;
        pathRg = _rangeForComponent(url->_flags, url->ranges, HAS_PATH);
    } else {
        pathRg = CFRangeMake(0, CFStringGetLength(url->_string));
    }
    lastCompRg = _rangeOfLastPathComponent(url);
    if (lastCompRg.length == 0) {
        appendDotDot = true;
    } else if (lastCompRg.length == 1) {
        UniChar ch = CFStringGetCharacterAtIndex(url->_string, lastCompRg.location);
        if (ch == '.' || ch == PATH_DELIM_FOR_TYPE(fsType)) {
            appendDotDot = true;
        }
    } else if (lastCompRg.length == 2 && CFStringGetCharacterAtIndex(url->_string, lastCompRg.location) == '.' && CFStringGetCharacterAtIndex(url->_string, lastCompRg.location+1) == '.') {
        appendDotDot = true;
    }

    newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
    if (appendDotDot) {
        CFIndex delta = 0;
        if (pathRg.length > 0 && CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) != PATH_DELIM_FOR_TYPE(fsType)) {
            CFStringInsert(newString, pathRg.location + pathRg.length, PATH_DELIM_AS_STRING_FOR_TYPE(fsType));
            delta ++;
        }
        CFStringInsert(newString, pathRg.location + pathRg.length + delta, CFSTR(".."));
        delta += 2;
        CFStringInsert(newString, pathRg.location + pathRg.length + delta, PATH_DELIM_AS_STRING_FOR_TYPE(fsType));
        delta ++;
        // We know we have "/../" at the end of the path; we wish to know if that's immediately preceded by "/." (but that "/." doesn't start the string), in which case we want to delete the "/.".
        if (pathRg.length + delta > 4 && CFStringGetCharacterAtIndex(newString, pathRg.location + pathRg.length + delta - 5) == '.') {
            if (pathRg.length+delta > 7 && CFStringGetCharacterAtIndex(newString, pathRg.location + pathRg.length + delta - 6) == PATH_DELIM_FOR_TYPE(fsType)) {
                CFStringDelete(newString, CFRangeMake(pathRg.location + pathRg.length + delta - 6, 2));
            } else if (pathRg.length+delta == 5) {
                CFStringDelete(newString, CFRangeMake(pathRg.location + pathRg.length + delta - 5, 2));
            }
        }
    } else if (lastCompRg.location == pathRg.location) {
        CFStringReplace(newString, pathRg, CFSTR("."));
        CFStringInsert(newString, 1, PATH_DELIM_AS_STRING_FOR_TYPE(fsType));
    } else {
        CFStringDelete(newString, CFRangeMake(lastCompRg.location, pathRg.location + pathRg.length - lastCompRg.location));
    }
    if (fsType == FULL_URL_REPRESENTATION) {
        result = _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
    } else {
        result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, newString, fsType, true, url->_base);
    }
    CFRelease(newString);
    return result;
}

CFURLRef CFURLCreateCopyAppendingPathExtension(CFAllocatorRef allocator, CFURLRef url, CFStringRef extension) {
    CFMutableStringRef newString;
    CFURLRef result;
    CFRange rg;
    CFURLPathStyle fsType;
    
    CFAssert1(url != NULL && extension != NULL, __kCFLogAssertion, "%s(): NULL argument not allowed", __PRETTY_FUNCTION__);
    url = _CFURLFromNSURL(url);
    __CFGenericValidateType(url, __kCFURLTypeID);
    __CFGenericValidateType(extension, CFStringGetTypeID());

    rg = _rangeOfLastPathComponent(url);
    if (rg.location < 0) return NULL; // No path
    fsType = URL_PATH_TYPE(url);
    if (fsType != FULL_URL_REPRESENTATION && CFStringFindWithOptions(extension, PATH_DELIM_AS_STRING_FOR_TYPE(fsType), CFRangeMake(0, CFStringGetLength(extension)), 0, NULL)) {
        _convertToURLRepresentation((struct __CFURL *)url);
        fsType = FULL_URL_REPRESENTATION;
        rg = _rangeOfLastPathComponent(url);
    }

    newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
    CFStringInsert(newString, rg.location + rg.length, CFSTR("."));
    if (fsType == FULL_URL_REPRESENTATION) {
        CFStringRef newExt = CFURLCreateStringByAddingPercentEscapes(allocator, extension, NULL, CFSTR(";?/"), (url->_flags & IS_OLD_UTF8_STYLE) ? kCFStringEncodingUTF8 : url->_encoding);
        CFStringInsert(newString, rg.location + rg.length + 1, newExt);
        CFRelease(newExt);
        result =  _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
    } else {
        CFStringInsert(newString, rg.location + rg.length + 1, extension);
        result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, newString, fsType, (url->_flags & IS_DIRECTORY) != 0 ? true : false, url->_base);
    }
    CFRelease(newString);
    return result;
}

CFURLRef CFURLCreateCopyDeletingPathExtension(CFAllocatorRef allocator, CFURLRef url) {
    CFRange rg, dotRg;
    CFURLRef result;

    CFAssert1(url != NULL, __kCFLogAssertion, "%s(): NULL argument not allowed", __PRETTY_FUNCTION__);
    url = _CFURLFromNSURL(url);
    __CFGenericValidateType(url, __kCFURLTypeID);
    rg = _rangeOfLastPathComponent(url);
    if (rg.location < 0) {
        result = NULL;
    } else if (rg.length && CFStringFindWithOptions(url->_string, CFSTR("."), rg, kCFCompareBackwards, &dotRg)) {
        CFMutableStringRef newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
        dotRg.length = rg.location + rg.length - dotRg.location;
        CFStringDelete(newString, dotRg);
        if (URL_PATH_TYPE(url) == FULL_URL_REPRESENTATION) {
            result = _CFURLCreateWithArbitraryString(allocator, newString, url->_base);
        } else {
            result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, newString, URL_PATH_TYPE(url), (url->_flags & IS_DIRECTORY) != 0 ? true : false, url->_base);
        }
        CFRelease(newString);
    } else {
        result = CFRetain(url);
    }
    return result;
}

#if defined(HAVE_CARBONCORE)
// We deal in FSRefs because they handle Unicode strings.
// FSSpecs handle a much more limited set of characters.
static Boolean __CFFSRefForVolumeName(CFStringRef volName, FSRef *spec, CFAllocatorRef alloc) {
    HFSUniStr255 name;
    CFIndex volIndex;
    Boolean success = false;
    CFMutableStringRef str = CFStringCreateMutableWithExternalCharactersNoCopy(alloc, NULL, 0, 0, kCFAllocatorNull);

    for (volIndex = 1; FSGetVolumeInfo(0, volIndex, NULL, kFSVolInfoNone, NULL, &name, spec) == noErr; volIndex ++) {
        CFStringSetExternalCharactersNoCopy(str, name.unicode, name.length, name.length);
        if (CFStringCompare(str, volName, 0) == kCFCompareEqualTo) {
            success = true;
            break;
        }
    }
    CFRelease(str);
    return success;
}
#else
#define __CFFSRefForVolumeName(A, B, C) (-3296)
#endif

static CFArrayRef HFSPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef components = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR(":"));
    CFMutableArrayRef newComponents = CFArrayCreateMutableCopy(alloc, 0, components);
    Boolean doSpecialLeadingColon = false;
    UniChar firstChar = CFStringGetCharacterAtIndex(path, 0);
    UInt32 i, cnt;
    CFRelease(components);

#if defined(HAVE_CARBONCORE) && !defined(__MACOS8__)
    doSpecialLeadingColon = true;
#endif

    if (!doSpecialLeadingColon && firstChar != ':') {
        CFArrayInsertValueAtIndex(newComponents, 0, CFSTR(""));
    } else if (firstChar != ':') {
        // see what we need to add at the beginning. Under MacOS, if the
        // first character isn't a ':', then the first component is the
        // volume name, and we need to find the mount point.  Bleah. If we
        // don't find a mount point, we're going to have to lie, and make something up.
        CFStringRef firstComp = CFArrayGetValueAtIndex(newComponents, 0);
        if (CFStringGetLength(firstComp) == 1 && CFStringGetCharacterAtIndex(firstComp, 0) == '/') {
            // "/" is the "magic" path for a UFS root directory
            CFArrayRemoveValueAtIndex(newComponents, 0);
            CFArrayInsertValueAtIndex(newComponents, 0, CFSTR(""));
        } else {
            // See if we can get a mount point.
            Boolean foundMountPoint = false;
            uint8_t buf[CFMaxPathLength];
            FSRef volSpec;
            // Now produce an FSSpec from the volume, then try and get the mount point
            if (__CFFSRefForVolumeName(firstComp, &volSpec, alloc) && (FSRefMakePath(&volSpec, buf, CFMaxPathLength) == noErr)) {
                // We win!  Ladies and gentlemen, we have a mount point.
                if (buf[0] == '/' && buf[1] == '\0') {
                    // Special case this common case
                    foundMountPoint = true;
                    CFArrayRemoveValueAtIndex(newComponents, 0);
                    CFArrayInsertValueAtIndex(newComponents, 0, CFSTR(""));
                } else {
                    // This is pretty inefficient; we can do better.
                    CFStringRef mountPoint = CFStringCreateWithCString(alloc, buf, CFStringFileSystemEncoding());
                    CFArrayRef mountComponents = mountPoint ? CFStringCreateArrayBySeparatingStrings(alloc, mountPoint, CFSTR("/")) : NULL;
                    if (mountComponents) {
                        CFIndex idx = CFArrayGetCount(mountComponents) - 1;
                        CFArrayRemoveValueAtIndex(newComponents, 0);
                        for ( ; idx >= 0; idx --) {
                            CFArrayInsertValueAtIndex(newComponents, 0, CFArrayGetValueAtIndex(mountComponents, idx));
                        }
                        CFRelease(mountComponents);
                        foundMountPoint = true;
                    }
                    if (mountPoint) CFRelease(mountPoint);
                }
            }
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
        CFStringRef comp = CFArrayGetValueAtIndex(newComponents, i);
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
    if (isDir && CFStringGetLength(CFArrayGetValueAtIndex(newComponents, cnt-1)) != 0) {
            CFArrayAppendValue(newComponents, CFSTR(""));
    }
    return newComponents;
}

static CFStringRef HFSPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef components = HFSPathToURLComponents(path, alloc, isDir);
    CFArrayRef newComponents = components ? copyStringArrayWithTransformation(components, escapePathComponent) : NULL;
    CFIndex cnt;
    CFStringRef result;
    if (components) CFRelease(components);
    if (!newComponents) return NULL;

    cnt = CFArrayGetCount(newComponents);
    if (cnt == 1 && CFStringGetLength(CFArrayGetValueAtIndex(newComponents, 0)) == 0) {
        result = CFRetain(CFSTR("/"));
    } else {
        result = CFStringCreateByCombiningStrings(alloc, newComponents, CFSTR("/"));
    }
    CFRelease(newComponents);
    return result;
}

static CFMutableStringRef filePathToHFSPath(unsigned char *buf, CFAllocatorRef allocator);
static CFStringRef colonToSlash(CFStringRef comp, CFAllocatorRef alloc);

static CFStringRef URLPathToHFSPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding) {
    CFStringRef result = NULL;
#if defined(__MACOS8__)
    // Slashes become colons; escaped slashes stay slashes.
    CFArrayRef components = CFStringCreateArrayBySeparatingStrings(allocator, path, CFSTR("/"));
    CFMutableArrayRef mutableComponents = CFArrayCreateMutableCopy(allocator, 0, components);
    SInt32 count = CFArrayGetCount(mutableComponents);
    CFStringRef newPath;
    CFRelease(components);

    if (count && CFStringGetLength(CFArrayGetValueAtIndex(mutableComponents, count-1)) == 0) {
        CFArrayRemoveValueAtIndex(mutableComponents, count-1);
        count --;
    }
    // On MacOS absolute paths do NOT begin with colon while relative paths DO.
    if ((count > 0) && CFEqual(CFArrayGetValueAtIndex(mutableComponents, 0), CFSTR(""))) {
        CFArrayRemoveValueAtIndex(mutableComponents, 0);
    } else {
        CFArrayInsertValueAtIndex(mutableComponents, 0, CFSTR(""));
    }
    newPath = CFStringCreateByCombiningStrings(allocator, mutableComponents, CFSTR(":"));
    CFRelease(mutableComponents);
    result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, newPath, CFSTR(""), encoding);
    CFRelease(newPath);
#elif defined(HAVE_CARBONCORE)
    if (CFStringGetLength(path) > 0 && CFStringGetCharacterAtIndex(path, 0) == '/') {
        // Absolute path; to do this properly, we need to go to the file system, generate an FSRef, then generate a path from there.  That's what filePathToHFSPath does.
        CFStringRef nativePath = URLPathToPOSIXPath(path, allocator, encoding); 
        unsigned char buf[CFMaxPathLength];
        if (nativePath && _CFStringGetFileSystemRepresentation(nativePath, buf, CFMaxPathLength)) {
            result = filePathToHFSPath(buf, allocator);
        }
        if (nativePath) CFRelease(nativePath);
    } else if (CFStringGetLength(path) == 0) {
        CFRetain(path);
        result = path;
    } else {
        // Relative path
        CFArrayRef components = CFStringCreateArrayBySeparatingStrings(allocator, path, CFSTR("/"));
        CFMutableArrayRef mutableComponents = CFArrayCreateMutableCopy(allocator, 0, components);
        SInt32 count = CFArrayGetCount(mutableComponents);
        CFIndex i, c;
        CFRelease(components);
    
        if (CFStringGetLength(CFArrayGetValueAtIndex(mutableComponents, count-1)) == 0) {
            // Strip off the trailing slash
            CFArrayRemoveValueAtIndex(mutableComponents, count-1);
        }
        // On MacOS absolute paths do NOT begin with colon while relative paths DO.
        CFArrayInsertValueAtIndex(mutableComponents, 0, CFSTR(""));
        for (i = 0, c = CFArrayGetCount(mutableComponents); i < c; i ++) {
            CFStringRef origComp, comp, newComp;
            origComp = CFArrayGetValueAtIndex(mutableComponents, i);
            comp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, origComp, CFSTR(""), encoding);
            newComp = colonToSlash(comp, allocator);
            if (newComp != origComp) {
                CFArraySetValueAtIndex(mutableComponents, i, newComp);
            }
            CFRelease(comp);
            CFRelease(newComp);
        }
        result = CFStringCreateByCombiningStrings(allocator, mutableComponents, CFSTR(":"));
        CFRelease(mutableComponents);
    }
#endif
    return result;
}

static CFStringRef colonToSlash(CFStringRef comp, CFAllocatorRef alloc) {
    CFStringRef newComp = NULL;
    CFRange searchRg, colonRg;
    searchRg.location = 0;
    searchRg.length = CFStringGetLength(comp);
    while (CFStringFindWithOptions(comp, CFSTR(":"), searchRg, 0, &colonRg)) {
        if (!newComp) {
            newComp = CFStringCreateMutableCopy(alloc, searchRg.location + searchRg.length, comp);
        }
        CFStringReplace((CFMutableStringRef)newComp, colonRg, CFSTR("/"));
        searchRg.length = searchRg.location + searchRg.length - colonRg.location - 1;
        searchRg.location = colonRg.location + 1;
    }
    if (newComp) {
        return newComp;
    } else {
        CFRetain(comp);
        return comp;
    }
}
    
static CFMutableStringRef filePathToHFSPath(unsigned char *buf, CFAllocatorRef allocator) {
#if defined(HAVE_CARBONCORE)
    // The only way to do this right is to get an FSSpec, and then work up the path from there.  This is problematic, of course, if the URL doesn't actually represent a file on the disk, but there's no way around that.  So - first get the POSIX path, then run it through NativePathNameToFSSpec to get a valid FSSpec.  If this succeeds, we iterate  upwards using FSGetCatalogInfo to find the names of the parent directories until we reach the volume.  REW, 10/29/99
    FSRef fsRef;
    if (FSPathMakeRef(buf, &fsRef, NULL) == noErr) {
        FSRef fsRef2, *parRef, *fileRef;
        CFMutableStringRef mString = CFStringCreateMutable(allocator, 0);
        CFMutableStringRef extString = CFStringCreateMutableWithExternalCharactersNoCopy(allocator, NULL, 0, 0, kCFAllocatorNull);
        OSErr err = noErr;

        fileRef = &fsRef;
        parRef = &fsRef2;
        while (err == noErr) {
            HFSUniStr255 name;
            FSCatalogInfo catInfo;
            err = FSGetCatalogInfo(fileRef, kFSCatInfoParentDirID, &catInfo, &name, NULL, parRef);
            if (err == noErr) {
                CFStringSetExternalCharactersNoCopy(extString, name.unicode, name.length, 255);
                CFStringInsert(mString, 0, extString);
                if (catInfo.parentDirID == fsRtParID) {
                    break;
                } else {
                    CFStringInsert(mString, 0, CFSTR(":"));
                }
                fileRef = parRef;
                parRef = (fileRef == &fsRef) ? &fsRef2 : &fsRef;
            }
        }
        CFRelease(extString);
        if (err == noErr) {
            return mString;
        } else {
            CFRelease(mString);
            return NULL;
        }
    } else {
        // recurse
        unsigned char *lastPathComponent = buf + strlen(buf);
        unsigned char *parentPath;
        CFMutableStringRef parentHFSPath;
        lastPathComponent --;
        if (*lastPathComponent == '/') {
            // We're not interested in trailing slashes
            *lastPathComponent = '\0';
            lastPathComponent --;
        }
        while (lastPathComponent > buf && *lastPathComponent != '/') {
            if (*lastPathComponent == ':') {
                *lastPathComponent = '/';
            }
            lastPathComponent --;
        }
        if (lastPathComponent == buf) {
            parentPath = (char *)"/";
            lastPathComponent ++;
        } else {
            *lastPathComponent = '\0';
            lastPathComponent ++;
            parentPath = buf;
        }
        parentHFSPath = filePathToHFSPath(parentPath, allocator);
        if (parentHFSPath) {
            CFStringAppendCString(parentHFSPath, ":", kCFStringEncodingASCII);
            CFStringAppendCString(parentHFSPath, lastPathComponent, CFStringFileSystemEncoding());
        } 
        return parentHFSPath;
    }
#else
//#warning filePathToHFSPath unimplemented in the non-CarbonCore case
    return CFStringCreateMutable(allocator, 0);
#endif
}

// FSSpec stuff
Boolean _CFGetFSSpecFromURL(CFAllocatorRef alloc, CFURLRef url, struct FSSpec *voidspec) {
    Boolean result = false;
#if defined (__MACOS8__)
    CFURLRef absURL = CFURLCopyAbsoluteURL(url);
    CFStringRef filePath;
    filePath = CFURLCopyFileSystemPath(absURL, kCFURLHFSPathStyle);
    CFRelease(absURL);
    if (filePath) {
        result = _CFGetFSSpecFromPathString(alloc, filePath, voidspec);
        CFRelease(filePath);
    }
#elif defined(HAVE_CARBONCORE)
    FSRef fileRef;
    if (_CFGetFSRefFromURL(alloc, url, &fileRef)) {
        result = (FSGetCatalogInfo(&fileRef, 0, NULL, NULL, (FSSpec *)voidspec, NULL) == noErr);
    }
#endif
    return result;
}

static Boolean _CFGetFSRefFromHFSPath(CFAllocatorRef alloc, CFStringRef path, void *voidRef) {
    CFArrayRef components = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR(":"));
    CFIndex idx, count, bufferLen = 0;
    UniChar *buffer = NULL;
    Boolean result = false;
    if (components && (count = CFArrayGetCount(components)) > 0 && __CFFSRefForVolumeName(CFArrayGetValueAtIndex(components, 0), voidRef, alloc)) {
        FSRef ref2, *parentRef, *newRef;
        parentRef = voidRef;
        newRef = &ref2;
        for (idx = 1; idx < count; idx ++ ) {
            CFStringRef comp = CFArrayGetValueAtIndex(components, idx);
            CFIndex compLength = CFStringGetLength(comp);
            UniChar *chars = (UniChar *)CFStringGetCharactersPtr(comp);
            if (!chars) {
                if (!buffer) {
                    bufferLen = (compLength < 32) ? 32 : compLength;
                    buffer = CFAllocatorAllocate(alloc, bufferLen * sizeof(UniChar), 0);
                } else if (bufferLen < compLength) {
                    buffer = CFAllocatorReallocate(alloc, buffer, compLength * sizeof(UniChar), 0);
                    bufferLen = compLength;
                }
                chars = buffer;
	            CFStringGetCharacters(comp, CFRangeMake(0, compLength), chars);
            }
            if (FSMakeFSRefUnicode(parentRef, compLength, chars, CFStringGetSystemEncoding(), newRef) != noErr) {
                break;
            }
            parentRef = newRef;
            newRef = (newRef == &ref2) ? voidRef : &ref2;
        }
        if (idx == count) {
        	result = true;
        	if (parentRef != voidRef) {
        		*((FSRef *)voidRef) = *parentRef;
        	}
        }
        if (components) CFRelease(components);
        if (buffer) CFAllocatorDeallocate(alloc, buffer);
    }
    return result;
}

static Boolean _CFGetFSRefFromURL(CFAllocatorRef alloc, CFURLRef url, void *voidRef) {
    Boolean result = false;
#if defined(__MACOS8__)
    CFURLRef absURL;
    CFStringRef hfsPath;
    if (!__CFMacOS8HasFSRefs()) return false;
    absURL = CFURLCopyAbsoluteURL(url);
    hfsPath = absURL ? CFURLCopyFileSystemPath(url, kCFURLHFSPathStyle) : NULL;
    result = hfsPath ? _CFGetFSRefFromHFSPath(alloc, hfsPath, voidRef) : false;
    if (absURL) CFRelease(absURL);
    if (hfsPath) CFRelease(hfsPath);
#elif defined(HAVE_CARBONCORE)
    CFURLRef absURL; 
    CFStringRef filePath, scheme;
    scheme = CFURLCopyScheme(url);
    if (scheme && !CFEqual(scheme, kCFURLFileScheme)) {
        CFRelease(scheme);
        return FALSE;
    } else if (scheme) {
        CFRelease(scheme);
    }
    absURL = CFURLCopyAbsoluteURL(url);
    if (!CF_IS_OBJC(__kCFURLTypeID, absURL) && URL_PATH_TYPE(absURL) == kCFURLHFSPathStyle) {
        // We special case kCFURLHFSPathStyle because we can avoid the expensive conversion to a POSIX native path -- REW, 2/23/2000
        result = _CFGetFSRefFromHFSPath(alloc, absURL->_string, voidRef);
        CFRelease(absURL);
    } else {
        filePath = CFURLCopyFileSystemPath(absURL, kCFURLPOSIXPathStyle);
        CFRelease(absURL);
        if (filePath) {
            char buf[CFMaxPathLength];

            result = (_CFStringGetFileSystemRepresentation(filePath, buf, CFMaxPathLength) && (FSPathMakeRef(buf, voidRef, NULL) == noErr) ? true  : false);
            CFRelease(filePath);
        }
    }
#endif
    return result;
}

CFURLRef _CFCreateURLFromFSSpec(CFAllocatorRef alloc, const struct FSSpec *voidspec, Boolean isDirectory) {
    CFURLRef url = NULL;
#if defined(__MACOS8__)
    CFStringRef str = _CFCreateStringWithHFSPathFromFSSpec(alloc, voidspec);
    if (str) {
        url = CFURLCreateWithFileSystemPath(alloc, str, kCFURLHFSPathStyle, isDirectory);
        CFRelease(str);
    }
#elif defined(HAVE_CARBONCORE)
    FSRef ref;
    if (FSpMakeFSRef((const FSSpec *)voidspec, &ref) == noErr) {
        url = _CFCreateURLFromFSRef(alloc, (void *)(&ref), isDirectory);
    }
#endif
    return url;
}

static CFURLRef _CFCreateURLFromFSRef(CFAllocatorRef alloc, const void *voidRef, Boolean isDirectory) {
    CFURLRef url = NULL;
#if defined(__MACOS8__)
    CFStringRef path = _CFCreateStringWithHFSPathFromFSRef(alloc, voidRef);
    if (path) {
        url = CFURLCreateWithFileSystemPath(alloc, path, kCFURLHFSPathStyle, isDirectory);
        CFRelease(path);
    }
#elif defined(HAVE_CARBONCORE)
    uint8_t buf[CFMaxPathLength];
    if (FSRefMakePath((const FSRef *)voidRef, buf, CFMaxPathLength) == noErr) {
        url = CFURLCreateFromFileSystemRepresentation(alloc, buf, strlen(buf), isDirectory);
    }
#endif
    return url;
}

CFURLRef CFURLCreateFromFSRef(CFAllocatorRef allocator, const FSRef *fsRef) {
#if defined(HAVE_CARBONCORE) || defined(__MACOS8__)
    Boolean isDirectory;
    FSCatalogInfo catInfo;
#if defined(__MACOS8__)
    if (!__CFMacOS8HasFSRefs()) return NULL;
#endif
    if (FSGetCatalogInfo(fsRef, kFSCatInfoNodeFlags, &catInfo, NULL, NULL, NULL) != noErr) {
        return NULL;
    }
    isDirectory = catInfo.nodeFlags & kFSNodeIsDirectoryMask;
    return _CFCreateURLFromFSRef(allocator, fsRef, isDirectory);
#else
    return NULL;
#endif
}

Boolean CFURLGetFSRef(CFURLRef url, FSRef *fsRef) {
#if defined(__MACOS8__)
    return __CFMacOS8HasFSRefs() ? _CFGetFSRefFromURL(CFGetAllocator(url), url, fsRef) : false;
#else
	Boolean result = false;
	
	if ( url ) {
	    result = _CFGetFSRefFromURL(CFGetAllocator(url), url, fsRef);
	}
		    
	return result;
#endif
}


