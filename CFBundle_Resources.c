/*
 * Copyright (c) 2013 Apple Inc. All rights reserved.
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
        Copyright (c) 1999-2013, Apple Inc.  All rights reserved.
        Responsibility: Tony Parker
*/

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

#pragma mark -
#pragma mark Directory Contents and Caches

// These are here for compatibility, but they do nothing anymore
CF_EXPORT void _CFBundleFlushCachesForURL(CFURLRef url) { }
CF_EXPORT void _CFBundleFlushCaches(void) { }

#pragma mark -
#pragma mark Resource URL Lookup

static inline Boolean _CFIsResourceCommon(char *path, Boolean *isDir) {
    Boolean exists;
    SInt32 mode;
    if (_CFGetPathProperties(kCFAllocatorSystemDefault, path, &exists, &mode, NULL, NULL, NULL, NULL) == 0) {
        if (isDir) *isDir = ((exists && ((mode & S_IFMT) == S_IFDIR)) ? true : false);
        return (exists && (mode & 0444));
    }
    return false;
}

CF_PRIVATE Boolean _CFIsResourceAtURL(CFURLRef url, Boolean *isDir) {
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathLength)) return false;
    
    return _CFIsResourceCommon(path, isDir);
}

CF_PRIVATE Boolean _CFIsResourceAtPath(CFStringRef path, Boolean *isDir) {
    char pathBuf[CFMaxPathSize];
    if (!CFStringGetFileSystemRepresentation(path, pathBuf, CFMaxPathSize)) return false;
    
    return _CFIsResourceCommon(pathBuf, isDir);
}


static CFStringRef _CFBundleGetResourceDirForVersion(uint8_t version) {
    if (1 == version) {
        return _CFBundleSupportFilesDirectoryName1WithResources;
    } else if (2 == version) {
        return _CFBundleSupportFilesDirectoryName2WithResources;
    } else if (0 == version) {
        return _CFBundleResourcesDirectoryName;
    }
    return CFSTR("");
}

CF_PRIVATE void _CFBundleAppendResourceDir(CFMutableStringRef path, uint8_t version) {
    if (1 == version) {
        // /path/to/bundle/Support Files/
        CFStringAppend(path, _CFBundleSupportFilesDirectoryName1);
        _CFAppendTrailingPathSlash2(path);
    } else if (2 == version) {
        // /path/to/bundle/Contents/
        CFStringAppend(path, _CFBundleSupportFilesDirectoryName2);
        _CFAppendTrailingPathSlash2(path);
    }
    if (0 == version || 1 == version || 2 == version) {
        // /path/to/bundle/<above>/Resources
        CFStringAppend(path, _CFBundleResourcesDirectoryName);
    }
}

CF_EXPORT CFURLRef CFBundleCopyResourceURL(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName) {
    if (!bundle) return NULL;
    CFURLRef result = (CFURLRef) _CFBundleCopyFindResources(bundle, NULL, NULL, resourceName, resourceType, subDirName, NULL, NO, NO, NULL);
    return result;
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfType(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName) {
    if (!bundle) return CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFArrayRef result = (CFArrayRef) _CFBundleCopyFindResources(bundle, NULL, NULL, NULL, resourceType, subDirName, NULL, YES, NO, NULL);
    return result;
}

CF_EXPORT CFURLRef _CFBundleCopyResourceURLForLanguage(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {
    return CFBundleCopyResourceURLForLocalization(bundle, resourceName, resourceType, subDirName, language);
}

CF_EXPORT CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef bundle, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
    if (!bundle) return NULL;
    CFURLRef result = (CFURLRef) _CFBundleCopyFindResources(bundle, NULL, NULL, resourceName, resourceType, subDirName, localizationName, NO, YES, NULL);
    return result;
}

CF_EXPORT CFArrayRef _CFBundleCopyResourceURLsOfTypeForLanguage(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef language) {
    return CFBundleCopyResourceURLsOfTypeForLocalization(bundle, resourceType, subDirName, language);
}

CF_EXPORT CFArrayRef CFBundleCopyResourceURLsOfTypeForLocalization(CFBundleRef bundle, CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName) {
    if (!bundle) return CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFArrayRef result = (CFArrayRef) _CFBundleCopyFindResources(bundle, NULL, NULL, NULL, resourceType, subDirName, localizationName, YES, YES, NULL);
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
        result = (CFURLRef) _CFBundleCopyFindResources(NULL, bundleURL, NULL, resourceName, resourceType, subDirName, NULL, NO, NO, NULL);
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
        array = (CFArrayRef) _CFBundleCopyFindResources(NULL, bundleURL, NULL, NULL, resourceType, subDirName, NULL, YES, NO, NULL);
    }
    if (newURL) CFRelease(newURL);
    return array;
}

#pragma mark -
#pragma mark Lanaguages and Locales

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

CF_PRIVATE CFStringRef _CFBundleCopyLocaleAbbreviationForRegionCode(SInt32 regionCode) {
    CFStringRef result = NULL;
    if (0 <= regionCode && regionCode < NUM_LOCALE_ABBREVIATIONS) {
        const char *localeAbbreviation = __CFBundleLocaleAbbreviationsArray + regionCode * LOCALE_ABBREVIATION_LENGTH;
        if (localeAbbreviation && *localeAbbreviation != '\0') {
            result = CFStringCreateWithCStringNoCopy(kCFAllocatorSystemDefault, localeAbbreviation, kCFStringEncodingASCII, kCFAllocatorNull);
        }
    }
    return result;
}

CF_EXPORT Boolean CFBundleGetLocalizationInfoForLocalization(CFStringRef localizationName, SInt32 *languageCode, SInt32 *regionCode, SInt32 *scriptCode, CFStringEncoding *stringEncoding) {
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
        if (!languages) languages = _CFBundleCopyUserLanguages();
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

// Get a list of localizations for a particular resource directory URL. Uncached. Does not include any predefined localizations from an Info.plist.
static CFArrayRef _CFBundleCopyURLLocalizations(CFAllocatorRef allocator, CFURLRef url) {
    __block CFMutableArrayRef result = NULL;
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
    CFStringRef directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFRelease(absoluteURL);
    
    CFStringRef lproj = _CFBundleLprojExtensionWithDot;
    CFIndex lprojLen = CFStringGetLength(lproj);
    
    _CFIterateDirectory(directoryPath, ^Boolean(CFStringRef fileName, uint8_t fileType) {
        // See if the fileName ends in .lproj
        // The comparison starts at the end of the fileName, backed up by the length of .lproj
        CFIndex fileNameLen = CFStringGetLength(fileName);
        if (fileNameLen > lprojLen && CFStringCompareWithOptions(fileName, lproj, CFRangeMake(fileNameLen - lprojLen, lprojLen), 0) == kCFCompareEqualTo) {
            // Chop off the .lproj part before creating a string
            CFStringRef lprojDirectoryName = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, fileName, CFRangeMake(0, fileNameLen - lprojLen));
            if (!result) result = CFArrayCreateMutable(allocator, 0, &kCFTypeArrayCallBacks);
            CFArrayAppendValue(result, lprojDirectoryName);
            CFRelease(lprojDirectoryName);
        }
        return true;
    });
    
    CFRelease(directoryPath);
    return (CFArrayRef)result;
}

static Boolean _CFBundleTryOnePreferredLprojNameInURL(CFAllocatorRef alloc, CFArrayRef localizations, CFStringRef curLangStr, CFMutableArrayRef lprojNames, Boolean fallBackToLanguage);

CF_EXPORT CFArrayRef CFBundleCopyBundleLocalizations(CFBundleRef bundle) {
    CFArrayRef result = NULL;
    
    __CFSpinLock(&bundle->_lock);
    if (bundle->_lookedForLocalizations) {
        result = (CFArrayRef)CFRetain(bundle->_localizations);
        __CFSpinUnlock(&bundle->_lock);
        return result;
    }
    __CFSpinUnlock(&bundle->_lock);
    
    CFDictionaryRef infoDict = CFBundleGetInfoDictionary(bundle);
    if (infoDict) {
        CFArrayRef predefinedLocalizations = (CFArrayRef)CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
        if (predefinedLocalizations && CFGetTypeID(predefinedLocalizations) != CFArrayGetTypeID()) {
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleLocalizationsKey);
        } else if (predefinedLocalizations) {
            // <rdar://problem/14255685> Some people put bad things inside this array =(
            CFMutableArrayRef realPredefinedLocalizations = CFArrayCreateMutable(CFGetAllocator(bundle), CFArrayGetCount(predefinedLocalizations), &kCFTypeArrayCallBacks);
            for (CFIndex i = 0; i < CFArrayGetCount(predefinedLocalizations); i++) {
                CFStringRef oneEntry = CFArrayGetValueAtIndex(predefinedLocalizations, i);
                if (CFGetTypeID(oneEntry) == CFStringGetTypeID() && CFStringGetLength(oneEntry) > 0) {
                    CFArrayAppendValue(realPredefinedLocalizations, oneEntry);
                }
            }
            result = CFArrayCreateCopy(CFGetAllocator(bundle), realPredefinedLocalizations);
            CFRelease(realPredefinedLocalizations);
        }
    }
    
    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
    if (resourcesURL) {
        CFArrayRef lprojDirectoriesInResources = _CFBundleCopyURLLocalizations(CFGetAllocator(bundle), resourcesURL);
        if (lprojDirectoriesInResources) {
            if (result) {
                // Append the lproj result to the predefined localization array
                CFMutableArrayRef newResult = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, result);
                CFRelease(result);
                CFArrayAppendArray(newResult, lprojDirectoriesInResources, CFRangeMake(0, CFArrayGetCount(lprojDirectoriesInResources)));
                CFRelease(lprojDirectoriesInResources);
                result = newResult;
            } else {
                result = lprojDirectoriesInResources;
            }
        }
        CFRelease(resourcesURL);
    }

    CFStringRef developmentLocalization = CFBundleGetDevelopmentRegion(bundle);
    if (result) {
        if (developmentLocalization) {
            CFRange entireRange = CFRangeMake(0, CFArrayGetCount(result));
            if (CFArrayContainsValue(result, entireRange, _CFBundleBaseDirectory)) {
                // Base.lproj contains localizations for the development region. Insert the devleopment region into the existing array if there isn't already a match so that resource lookup doesn't default to another language.
                CFMutableArrayRef tempArray = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
                if (tempArray) {
                    if (!_CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, result, developmentLocalization, tempArray, false) && CFArrayGetCount(tempArray) == 0) {
                        CFMutableArrayRef newResult = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, result);
                        CFRelease(result);
                        CFArrayAppendValue(newResult, developmentLocalization);
                        result = newResult;
                    }
                    CFRelease(tempArray);
                }
            }
        }
    } else {
        if (developmentLocalization) {
            result = CFArrayCreate(CFGetAllocator(bundle), (const void **)&developmentLocalization, 1, &kCFTypeArrayCallBacks);
        } else {
            result = CFArrayCreate(CFGetAllocator(bundle), NULL, 0, &kCFTypeArrayCallBacks);
        }
    }
    
    // Cache the result.
    __CFSpinLock(&bundle->_lock);
    if (bundle->_localizations) CFRelease(bundle->_localizations);
    bundle->_localizations = (CFArrayRef)CFRetain(result);
    bundle->_lookedForLocalizations = true;
    __CFSpinUnlock(&bundle->_lock);
    
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
        CFDictionaryRef infoDict = _CFBundleCopyInfoDictionaryInExecutable(url);
        if (infoDict) {
            CFArrayRef predefinedLocalizations = (CFArrayRef)CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
            if (predefinedLocalizations && CFGetTypeID(predefinedLocalizations) == CFArrayGetTypeID()) result = (CFArrayRef)CFRetain(predefinedLocalizations);
            if (!result) {
                devLang = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
                if (devLang && (CFGetTypeID(devLang) == CFStringGetTypeID() && CFStringGetLength(devLang) > 0)) result = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)&devLang, 1, &kCFTypeArrayCallBacks);
            }
            CFRelease(infoDict);
        }
    }
    return result;
}

extern void *__CFAppleLanguages;


CF_PRIVATE CFArrayRef _CFBundleCopyUserLanguages() {
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
    if (!languages) languages = _CFBundleCopyUserLanguages();
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



static Boolean _CFBundleTryOnePreferredLprojNameInArray(CFArrayRef array, CFStringRef curLangStr, CFMutableArrayRef lprojNames, Boolean fallBackToLanguage) {
    CFRange range = CFRangeMake(0, CFArrayGetCount(array));
    if (range.length == 0) return false;
    
    Boolean foundOne = false, specifiesScript = false;
    CFStringRef altLangStr = NULL, modifiedLangStr = NULL, languageAbbreviation = NULL, languageName = NULL, canonicalLanguageIdentifier = NULL, canonicalLanguageAbbreviation = NULL;
    CFMutableDictionaryRef canonicalLanguageIdentifiers = NULL;
    
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
    if (foundOne && altLangStr) return foundOne;
#endif /* __CONSTANT_CFSTRINGS__ */
    
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

// localizations array must include both predefined and actual lproj localizations
static Boolean _CFBundleTryOnePreferredLprojNameInURL(CFAllocatorRef alloc, CFArrayRef localizations, CFStringRef curLangStr, CFMutableArrayRef lprojNames, Boolean fallBackToLanguage) {
    CFStringRef altLangStr = NULL, modifiedLangStr = NULL, languageAbbreviation = NULL, languageName = NULL, canonicalLanguageIdentifier = NULL, canonicalLanguageAbbreviation = NULL;
    CFMutableDictionaryRef canonicalLanguageIdentifiers = NULL;
    Boolean foundOne = false, specifiesScript = false;

    if (!localizations) return false;
    
    CFRange localizationsRange = CFRangeMake(0, CFArrayGetCount(localizations));
            
    // this use of contents is only checking for language strings - it could get the list of existing localizations and use that instead
    if (CFArrayContainsValue(localizations, localizationsRange, curLangStr)) {
        if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), curLangStr)) CFArrayAppendValue(lprojNames, curLangStr);
        foundOne = true;
        if (CFStringGetLength(curLangStr) <= 2) {
            return foundOne;
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
        return foundOne;
    }
    if (altLangStr) {
        if (CFArrayContainsValue(localizations, localizationsRange, altLangStr)) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), altLangStr)) CFArrayAppendValue(lprojNames, altLangStr);
            foundOne = true;
            return foundOne;
        }
    }

    if (!altLangStr && (modifiedLangStr = _CFBundleCopyModifiedLocalization(curLangStr))) {
        if (CFArrayContainsValue(localizations, localizationsRange, modifiedLangStr)) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), modifiedLangStr)) CFArrayAppendValue(lprojNames, modifiedLangStr);
            foundOne = true;
        }
    }
    
    
    if (!specifiesScript && (foundOne || fallBackToLanguage) && !altLangStr && (languageAbbreviation = _CFBundleCopyLanguageAbbreviationForLocalization(curLangStr)) && !CFEqual(curLangStr, languageAbbreviation)) {
        if (CFArrayContainsValue(localizations, localizationsRange, languageAbbreviation)) {
            if (!CFArrayContainsValue(lprojNames, CFRangeMake(0, CFArrayGetCount(lprojNames)), languageAbbreviation)) CFArrayAppendValue(lprojNames, languageAbbreviation);
            foundOne = true;
        }
    }
    if (!specifiesScript && (foundOne || fallBackToLanguage) && !altLangStr && (languageName = _CFBundleCopyLanguageNameForLocalization(curLangStr)) && !CFEqual(curLangStr, languageName)) {
        if (CFArrayContainsValue(localizations, localizationsRange, languageName)) {
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

static void _CFBundleAddPreferredLprojNamesInDirectory(CFAllocatorRef alloc, CFURLRef bundleURL, CFArrayRef localizations, CFMutableArrayRef lprojNames, CFStringRef devLang) {
    // This function will add zero, one or two elements to the lprojNames array.
    // It examines the users preferred language list and the lproj directories inside the bundle directory.  It picks the lproj directory that is highest on the users list.
    // The users list can contain region names (like "en_US" for US English).  In this case, if the region lproj exists, it will be added, and, if the region's associated language lproj exists that will be added.

    Boolean foundOne = false;
    
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
                        CFStringRef curLangStr = (CFStringRef)CFArrayGetValueAtIndex(mainBundleLangs, 0);
                        foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, curLangStr, lprojNames, true);
                    }
                }
                CFRelease(mainBundleURL);
            }
        }
    }

    if (!foundOne) {
        // If we didn't find the main bundle's preferred language, look at the users' prefs again and find the best one.
        CFArrayRef userLanguages = _CFBundleCopyUserLanguages();
        if (userLanguages) {
            CFIndex count = CFArrayGetCount(userLanguages);
            CFIndex idx, startIdx;
            for (idx = 0, startIdx = -1; !foundOne && idx < count; idx++) {
                CFStringRef curLangStr = (CFStringRef)CFArrayGetValueAtIndex(userLanguages, idx);
                CFStringRef nextLangStr = (idx + 1 < count) ? (CFStringRef)CFArrayGetValueAtIndex(userLanguages, idx + 1) : NULL;
                if (nextLangStr && _CFBundleLocalizationsHaveCommonPrefix(curLangStr, nextLangStr)) {
                    foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, curLangStr, lprojNames, false);
                    if (startIdx < 0) startIdx = idx;
                } else if (startIdx >= 0 && startIdx <= idx) {
                    foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, curLangStr, lprojNames, false);
                    for (; !foundOne && startIdx <= idx; startIdx++) {
                        curLangStr = (CFStringRef)CFArrayGetValueAtIndex(userLanguages, startIdx);
                        foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, curLangStr, lprojNames, true);
                    }
                    startIdx = -1;
                } else {
                    foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, curLangStr, lprojNames, true);
                    startIdx = -1;
                }
            }
        }
        // use development region and U.S. English as backstops
        if (!foundOne && devLang) foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, devLang, lprojNames, true);
        if (!foundOne) foundOne = _CFBundleTryOnePreferredLprojNameInURL(kCFAllocatorSystemDefault, localizations, CFSTR("en_US"), lprojNames, true);
        if (userLanguages) CFRelease(userLanguages);
    }
}

static CFArrayRef _CFBundleCopyLanguageSearchListInDirectory(CFAllocatorRef alloc, CFURLRef url, uint8_t *version) {
    uint8_t localVersion = 0;
    CFDictionaryRef infoDict = _CFBundleCopyInfoDictionaryInDirectory(alloc, url, &localVersion);

    CFArrayRef predefinedLocalizations = NULL;
    CFStringRef devLang = NULL;
    if (infoDict) {
        devLang = (CFStringRef)CFDictionaryGetValue(infoDict, kCFBundleDevelopmentRegionKey);
        if (devLang && (CFGetTypeID(devLang) != CFStringGetTypeID() || CFStringGetLength(devLang) == 0)) devLang = NULL;

        predefinedLocalizations = (CFArrayRef)CFDictionaryGetValue(infoDict, kCFBundleLocalizationsKey);
        if (predefinedLocalizations && CFGetTypeID(predefinedLocalizations) != CFArrayGetTypeID()) {
            predefinedLocalizations = NULL;
            CFDictionaryRemoveValue((CFMutableDictionaryRef)infoDict, kCFBundleLocalizationsKey);
        }
    }
    
    CFURLRef resourcesURL = _CFBundleCopyResourcesDirectoryURLInDirectory(url, localVersion);
    CFArrayRef localizations = _CFBundleCopyURLLocalizations(alloc, resourcesURL);
    CFRelease(resourcesURL);
    
    if (predefinedLocalizations && localizations) {
        CFMutableArrayRef newLocalizations = CFArrayCreateMutableCopy(alloc, 0, predefinedLocalizations);
        CFArrayAppendArray(newLocalizations, localizations, CFRangeMake(0, CFArrayGetCount(localizations)));
        CFRelease(localizations);
        localizations = (CFArrayRef)newLocalizations;
    } else if (predefinedLocalizations) {
        localizations = (CFArrayRef)CFRetain(predefinedLocalizations);
    } else if (!localizations) {
        localizations = CFArrayCreate(alloc, NULL, 0, &kCFTypeArrayCallBacks);
    }
    
    CFMutableArrayRef langs = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);
    _CFBundleAddPreferredLprojNamesInDirectory(alloc, url, localizations, langs, devLang);
    CFRelease(localizations);
    
    if (devLang && CFArrayGetFirstIndexOfValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), devLang) < 0) CFArrayAppendValue(langs, devLang);
    
    // Total backstop behavior to avoid having an empty array. 
    if (CFArrayGetCount(langs) == 0) CFArrayAppendValue(langs, CFSTR("en"));
    
    if (infoDict) CFRelease(infoDict);
    if (version) *version = localVersion;
    return langs;
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
            prefArray = _CFBundleCopyUserLanguages();
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

static CFStringRef _defaultLocalization = NULL;

CF_EXPORT void _CFBundleSetDefaultLocalization(CFStringRef localizationName) {
    CFStringRef newLocalization = localizationName ? (CFStringRef)CFStringCreateCopy(kCFAllocatorSystemDefault, localizationName) : NULL;
    if (_defaultLocalization) CFRelease(_defaultLocalization);
    _defaultLocalization = newLocalization;
}

CF_EXPORT CFArrayRef _CFBundleGetLanguageSearchList(CFBundleRef bundle) {
    if (!bundle->_searchLanguages) {
        CFMutableArrayRef langs = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        CFStringRef devLang = CFBundleGetDevelopmentRegion(bundle);
        
#if DEPLOYMENT_TARGET_WINDOWS
        if (_defaultLocalization) CFArrayAppendValue(langs, _defaultLocalization);
#endif
        // includes predefined localizations
        CFArrayRef localizationsForBundle = CFBundleCopyBundleLocalizations(bundle);
        
        _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, localizationsForBundle, langs, devLang);
        
        if (CFArrayGetCount(langs) == 0) {
            // If the user does not prefer any of our languages, and devLang is not present, try English
            _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, localizationsForBundle, langs, CFSTR("en_US"));
        }
        
        if (CFArrayGetCount(langs) == 0) {
            // if none of the preferred localizations are present, fall back on a random localization that is present
            if (localizationsForBundle && CFArrayGetCount(localizationsForBundle) > 0) {
                CFStringRef firstLocalization = (CFStringRef)CFArrayGetValueAtIndex(localizationsForBundle, 0);
                _CFBundleAddPreferredLprojNamesInDirectory(CFGetAllocator(bundle), bundle->_url, localizationsForBundle, langs, firstLocalization);
            }
        }
        
        if (devLang && !CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), devLang)) {
            // Make sure that devLang is on the list as a fallback for individual resources that are not present
            CFArrayAppendValue(langs, devLang);
        } else if (!devLang) {
            if (localizationsForBundle) {
                CFStringRef en_US = CFSTR("en_US"), en = CFSTR("en"), English = CFSTR("English");
                CFRange range = CFRangeMake(0, CFArrayGetCount(localizationsForBundle));
                if (CFArrayContainsValue(localizationsForBundle, range, en)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), en)) CFArrayAppendValue(langs, en);
                } else if (CFArrayContainsValue(localizationsForBundle, range, English)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), English)) CFArrayAppendValue(langs, English);
                } else if (CFArrayContainsValue(localizationsForBundle, range, en_US)) {
                    if (!CFArrayContainsValue(langs, CFRangeMake(0, CFArrayGetCount(langs)), en_US)) CFArrayAppendValue(langs, en_US);
                }
            }
        }
        
        if (localizationsForBundle) CFRelease(localizationsForBundle);
        
        if (CFArrayGetCount(langs) == 0) {
            // Total backstop behavior to avoid having an empty array.
            if (_defaultLocalization) {
                CFArrayAppendValue(langs, _defaultLocalization);
            } else {
                CFArrayAppendValue(langs, CFSTR("en"));
            }
        }
        if (!OSAtomicCompareAndSwapPtrBarrier(NULL, (void *)langs, (void * volatile *)&(bundle->_searchLanguages))) CFRelease(langs);
    }
    return bundle->_searchLanguages;
}

#pragma mark -

CF_EXPORT Boolean _CFBundleURLLooksLikeBundle(CFURLRef url) {
    Boolean result = false;
    CFBundleRef bundle = _CFBundleCreateIfLooksLikeBundle(kCFAllocatorSystemDefault, url);
    if (bundle) {
        result = true;
        CFRelease(bundle);
    }
    return result;
}

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_WINDOWS
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
#endif

CF_PRIVATE uint8_t _CFBundleGetBundleVersionForURL(CFURLRef url) {
    // check for existence of "Resources" or "Contents" or "Support Files"
    // but check for the most likely one first
    // version 0:  old-style "Resources" bundles
    // version 1:  obsolete "Support Files" bundles
    // version 2:  modern "Contents" bundles
    // version 3:  none of the above (see below)
    // version 4:  not a bundle (for main bundle only)
    
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(url);
    CFStringRef directoryPath = CFURLCopyFileSystemPath(absoluteURL, PLATFORM_PATH_STYLE);
    CFRelease(absoluteURL);
    
    Boolean hasFrameworkSuffix = CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework/"));
#if DEPLOYMENT_TARGET_WINDOWS
    hasFrameworkSuffix = hasFrameworkSuffix || CFStringHasSuffix(CFURLGetString(url), CFSTR(".framework\\"));
#endif

    /*
     #define _CFBundleSupportFilesDirectoryName1 CFSTR("Support Files")
     #define _CFBundleSupportFilesDirectoryName2 CFSTR("Contents")
     #define _CFBundleResourcesDirectoryName CFSTR("Resources")
     #define _CFBundleExecutablesDirectoryName CFSTR("Executables")
     #define _CFBundleNonLocalizedResourcesDirectoryName CFSTR("Non-localized Resources")
    */
    __block uint8_t localVersion = 3;
    CFIndex resourcesDirectoryLength = CFStringGetLength(_CFBundleResourcesDirectoryName);
    CFIndex contentsDirectoryLength = CFStringGetLength(_CFBundleSupportFilesDirectoryName2);
    CFIndex supportFilesDirectoryLength = CFStringGetLength(_CFBundleSupportFilesDirectoryName1);
    
    __block Boolean foundResources = false;
    __block Boolean foundSupportFiles2 = false;
    __block Boolean foundSupportFiles1 = false;
    
    _CFIterateDirectory(directoryPath, ^Boolean (CFStringRef fileName, uint8_t fileType) {
        // We're looking for a few different names, and also some info on if it's a directory or not.
        // We don't stop looking once we find one of the names. Otherwise we could run into the situation where we have both "Contents" and "Resources" in a framework, and we see Contents first but Resources is more important.
        if (fileType == DT_DIR || fileType == DT_LNK) {
            CFIndex fileNameLen = CFStringGetLength(fileName);
            if (fileNameLen == resourcesDirectoryLength && CFStringCompareWithOptions(fileName, _CFBundleResourcesDirectoryName, CFRangeMake(0, resourcesDirectoryLength), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
                foundResources = true;
            } else if (fileNameLen == contentsDirectoryLength && CFStringCompareWithOptions(fileName, _CFBundleSupportFilesDirectoryName2, CFRangeMake(0, contentsDirectoryLength), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
                foundSupportFiles2 = true;
            } else if (fileNameLen == supportFilesDirectoryLength && CFStringCompareWithOptions(fileName, _CFBundleSupportFilesDirectoryName1, CFRangeMake(0, supportFilesDirectoryLength), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
                foundSupportFiles1 = true;
            }
        }
        return true;
    });
    
    // The order of these if statements is important - the Resources directory presence takes precedence over Contents, and so forth.
    if (hasFrameworkSuffix) {
        if (foundResources) {
            localVersion = 0;
        } else if (foundSupportFiles2) {
            localVersion = 2;
        } else if (foundSupportFiles1) {
            localVersion = 1;
        }
    } else {
        if (foundSupportFiles2) {
            localVersion = 2;
        } else if (foundResources) {
            localVersion = 0;
        } else if (foundSupportFiles1) {
            localVersion = 1;
        }
    }
    
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_WINDOWS
    // Do a more substantial check for the subdirectories that make up version 0/1/2 bundles. These are sometimes symlinks (like in Frameworks) and they would have been missed by our check above. Perhaps we can do a check for DT_LNK there as well, if it's sufficient instead of looking at the actual contents.
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

    CFRelease(directoryPath);
    return localVersion;
}

#pragma mark -
#pragma mark Platforms

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

CF_PRIVATE CFStringRef _CFBundleGetPlatformExecutablesSubdirectoryName(void) {
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

CF_PRIVATE CFStringRef _CFBundleGetAlternatePlatformExecutablesSubdirectoryName(void) {
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

CF_PRIVATE CFStringRef _CFBundleGetOtherPlatformExecutablesSubdirectoryName(void) {
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

CF_PRIVATE CFStringRef _CFBundleGetOtherAlternatePlatformExecutablesSubdirectoryName(void) {
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

#pragma mark -
#pragma mark Resource Lookup - Query Table

static void _CFBundleAddValueForType(CFStringRef type, CFMutableDictionaryRef queryTable, CFMutableDictionaryRef typeDir, CFTypeRef value, CFMutableDictionaryRef addedTypes, Boolean firstLproj) {
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

static _CFBundleFileVersion _CFBundleCheckFileProductAndPlatform(CFStringRef file, CFRange searchRange, CFStringRef product, CFStringRef platform)
{
    _CFBundleFileVersion version;
    Boolean foundprod, foundplat;
    foundplat = foundprod = NO;
    Boolean wrong = false;
    
    if (CFStringFindWithOptions(file, CFSTR("~"), searchRange, 0, NULL)) {
        if (CFStringGetLength(product) != 1) {
            // todo: really, search the same range again?
            if (CFStringFindWithOptions(file, product, searchRange, kCFCompareEqualTo, NULL)) {
                foundprod = YES;
            }
        }
        if (!foundprod) {
            wrong = _CFBundleSupportedProductName(file, searchRange);
        }
    }
    
    if (!wrong && CFStringFindWithOptions(file, CFSTR("-"), searchRange, 0, NULL)) {
        if (CFStringFindWithOptions(file, platform, searchRange, kCFCompareEqualTo, NULL)) {
            foundplat = YES;
        }
        if (!foundplat) {
            wrong = _CFBundleSupportedPlatformName(file, searchRange);
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

static _CFBundleFileVersion _CFBundleVersionForFileName(CFStringRef fileName, CFStringRef expectedProduct, CFStringRef expectedPlatform, CFRange *outProductRange, CFRange *outPlatformRange) {
    // Search for a product name, e.g.: foo~iphone.jpg or bar~ipad
    Boolean foundProduct = false;
    Boolean foundPlatform = false;
    CFIndex fileNameLen = CFStringGetLength(fileName);
    CFRange productRange;
    CFRange platformRange;
    
    CFIndex dotLocation = fileNameLen;
    for (CFIndex i = fileNameLen - 1; i > 0; i--) {
        UniChar c = CFStringGetCharacterAtIndex(fileName, i);
        if (c == '.') {
            dotLocation = i;
        }
#if DEPLOYMENT_TARGET_EMBEDDED
        // Product names are only supported on iOS
        // ref docs here: "iOS Supports Device-Specific Resources" in "Resource Programming Guide"
        else if (c == '~' && !foundProduct) {
            productRange = CFRangeMake(i, dotLocation - i);
            foundProduct = (CFStringCompareWithOptions(fileName, expectedProduct, productRange, kCFCompareAnchored) == kCFCompareEqualTo);
            if (foundProduct && outProductRange) *outProductRange = productRange;
        }
#endif
        else if (c == '-') {
            if (foundProduct) {
                platformRange = CFRangeMake(i, productRange.location - i);
            } else {
                platformRange = CFRangeMake(i, dotLocation - i);
            }
            foundPlatform = (CFStringCompareWithOptions(fileName, expectedPlatform, platformRange, kCFCompareAnchored) == kCFCompareEqualTo);
            if (foundPlatform && outPlatformRange) *outPlatformRange = platformRange;
            break;
        }
    }
    
    _CFBundleFileVersion version;
    if (foundPlatform && foundProduct) {
        version = _CFBundleFileVersionWithProductWithPlatform;
    } else if (foundPlatform) {
        version = _CFBundleFileVersionNoProductWithPlatform;
    } else if (foundProduct) {
        version = _CFBundleFileVersionWithProductNoPlatform;
    } else {
        version = _CFBundleFileVersionNoProductNoPlatform;
    }
    return version;
}

// Splits up a string into its various parts. Note that the out-types must be released by the caller if they exist.
static void _CFBundleSplitFileName(CFStringRef fileName, CFStringRef *noProductOrPlatform, CFStringRef *endType, CFStringRef *startType, CFStringRef expectedProduct, CFStringRef expectedPlatform, _CFBundleFileVersion *version) {
    CFIndex fileNameLen = CFStringGetLength(fileName);
    
    if (endType || startType) {
        // Search for the type from the end (type defined as everything after the last '.')
        // e.g., a file name like foo.jpg has a type of 'jpg'
        Boolean foundDot = false;
        uint16_t dotLocation = 0;
        for (CFIndex i = fileNameLen; i > 0; i--) {
            if (CFStringGetCharacterAtIndex(fileName, i - 1) == '.') {
                foundDot = true;
                dotLocation = i - 1;
                break;
            }
        }
        
        if (foundDot && dotLocation != fileNameLen - 1) {
            if (endType) *endType = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, fileName, CFRangeMake(dotLocation + 1, CFStringGetLength(fileName) - dotLocation - 1));
        }
        
        // Search for the type from the beginning (type defined as everything after the first '.')
        // e.g., a file name like foo.jpg.gz has a type of 'jpg.gz'
        if (startType) {
            for (CFIndex i = 0; i < fileNameLen; i++) {
                if (CFStringGetCharacterAtIndex(fileName, i) == '.') {
                    // no need to create this again if it's the same as previous
                    if (i != dotLocation) {
                        *startType = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, fileName, CFRangeMake(i + 1, CFStringGetLength(fileName) - i - 1));
                    }
                    break;
                }
            }
        }
    }
    
    CFRange productRange, platformRange;
    *version = _CFBundleVersionForFileName(fileName, expectedProduct, expectedPlatform, &productRange, &platformRange);
    
    Boolean foundPlatform = (*version == _CFBundleFileVersionNoProductWithPlatform || *version == _CFBundleFileVersionWithProductWithPlatform);
    Boolean foundProduct = (*version == _CFBundleFileVersionWithProductNoPlatform || *version == _CFBundleFileVersionWithProductWithPlatform);
    // Create a string that excludes both platform and product name
    // e.g., foo-iphone~iphoneos.jpg -> foo.jpg
    if (foundPlatform || foundProduct) {
        CFMutableStringRef fileNameScratch = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, fileName);
        CFIndex start, length = 0;
        
        // Because the platform always comes first and is immediately followed by product if it exists, we'll use the platform start location as the start of our range to delete.
        if (foundPlatform) {
            start = platformRange.location;
        } else {
            start = productRange.location;
        }
        
        if (foundPlatform && foundProduct) {
            length = platformRange.length + productRange.length;
        } else if (foundPlatform) {
            length = platformRange.length;
        } else if (foundProduct) {
            length = productRange.length;
        }
        CFStringDelete(fileNameScratch, CFRangeMake(start, length));
        *noProductOrPlatform = (CFStringRef)fileNameScratch;
    }    
}

static Boolean _CFBundleReadDirectory(CFStringRef pathOfDir, CFBundleRef bundle, CFStringRef subdirectory, CFMutableArrayRef allFiles, Boolean hasFileAdded, CFMutableStringRef type, CFMutableDictionaryRef queryTable, CFMutableDictionaryRef typeDir, CFMutableDictionaryRef addedTypes, Boolean firstLproj, CFStringRef product, CFStringRef platform, CFStringRef lprojName, Boolean appendLprojCharacters) {
    
    Boolean result = true;
    CFMutableStringRef pathPrefix = NULL;
    if (lprojName) {
        pathPrefix = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, lprojName);
        if (appendLprojCharacters) _CFAppendPathExtension2(pathPrefix, _CFBundleLprojExtension);
        _CFAppendTrailingPathSlash2(pathPrefix);
    }
    if (subdirectory) {
        if (pathPrefix) {
            CFStringAppend(pathPrefix, subdirectory);
        } else {
            pathPrefix = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, subdirectory);
        }
        UniChar lastChar = CFStringGetCharacterAtIndex(subdirectory, CFStringGetLength(subdirectory)-1);
        if (lastChar != _CFGetSlash()) {
            _CFAppendTrailingPathSlash2(pathPrefix);
        }
    }
    
    _CFIterateDirectory(pathOfDir, ^Boolean(CFStringRef fileName, uint8_t fileType) {
        CFStringRef startType = NULL, endType = NULL, noProductOrPlatform = NULL;
        _CFBundleFileVersion fileVersion;
        _CFBundleSplitFileName(fileName, &noProductOrPlatform, &endType, &startType, product, platform, &fileVersion);
        
        CFStringRef pathToFile;
        if (pathPrefix && CFStringGetLength(pathPrefix) > 0) {
            CFMutableStringRef tmp = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, pathPrefix);
            CFStringAppend(tmp, fileName);
            pathToFile = (CFStringRef)tmp;
        } else {
            pathToFile = (CFStringRef)CFRetain(fileName);
        }

        // If this file is a directory, the path needs to include a trailing slash so we can later create the right kind of CFURL object
        Boolean appendSlash = false;
        if (fileType == DT_DIR) {
            appendSlash = true;
        }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
        else if (fileType == DT_UNKNOWN) {
            Boolean isDir = false;
            char subdirPath[CFMaxPathLength];
            struct stat statBuf;
            if (CFStringGetFileSystemRepresentation(pathOfDir, subdirPath, sizeof(subdirPath))) {
                strlcat(subdirPath, "/", sizeof(subdirPath));
                char fileNameBuf[CFMaxPathLength];
                if (CFStringGetFileSystemRepresentation(fileName, fileNameBuf, sizeof(fileNameBuf))) {
                    strlcat(subdirPath, fileNameBuf, sizeof(subdirPath));
                    if (stat(subdirPath, &statBuf) == 0) {
                        isDir = ((statBuf.st_mode & S_IFMT) == S_IFDIR);
                    }
                    if (isDir) {
                        appendSlash = true;
                    }
                }
            }
        }
#endif
        if (appendSlash) {
            // This is fairly inefficient
            CFMutableStringRef tmp = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, pathToFile);
            _CFAppendTrailingPathSlash2(tmp);
            CFRelease(pathToFile);
            pathToFile = (CFStringRef)tmp;
        }
        
        // put it into all file array
        if (!hasFileAdded) {
            CFArrayAppendValue(allFiles, pathToFile);
        }
        
        if (startType) {
            _CFBundleAddValueForType(startType, queryTable, typeDir, pathToFile, addedTypes, firstLproj);
        }
        
        if (endType) {
            _CFBundleAddValueForType(endType, queryTable, typeDir, pathToFile, addedTypes, firstLproj);
        }
                
        if (fileVersion == _CFBundleFileVersionNoProductNoPlatform || fileVersion == _CFBundleFileVersionUnmatched) {
            // No product/no platform, or unmatched files get added directly to the query table.
            CFStringRef prevPath = (CFStringRef)CFDictionaryGetValue(queryTable, fileName);
            if (!prevPath) {
                CFDictionarySetValue(queryTable, fileName, pathToFile);
            }
        } else {
            // If the file has a product or platform extension, we add the full name to the query table so that it may be found using that name. But only if it doesn't already exist.
            CFStringRef prevPath = (CFStringRef)CFDictionaryGetValue(queryTable, fileName);
            if (!prevPath) {
                CFDictionarySetValue(queryTable, fileName, pathToFile);
            }
            
            // Then we add the more specific name as well, replacing the existing one if this is a more specific version.
            if (noProductOrPlatform) {
                // add the path of the key into the query table
                prevPath = (CFStringRef) CFDictionaryGetValue(queryTable, noProductOrPlatform);
                if (!prevPath) {
                    CFDictionarySetValue(queryTable, noProductOrPlatform, pathToFile);
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
                        _CFBundleFileVersion prevFileVersion = _CFBundleCheckFileProductAndPlatform(prevPath, searchRange, product, platform);
                        switch (prevFileVersion) {
                            case _CFBundleFileVersionNoProductNoPlatform:
                                CFDictionarySetValue(queryTable, noProductOrPlatform, pathToFile);
                                break;
                            case _CFBundleFileVersionWithProductNoPlatform:
                                if (fileVersion == _CFBundleFileVersionWithProductWithPlatform) CFDictionarySetValue(queryTable, noProductOrPlatform, pathToFile);
                                break;
                            case _CFBundleFileVersionNoProductWithPlatform:
                                CFDictionarySetValue(queryTable, noProductOrPlatform, pathToFile);
                                break;
                            default:
                                break;
                        }
                    }
                }
            }
        }
        
        if (pathToFile) CFRelease(pathToFile);
        if (startType) CFRelease(startType);
        if (endType) CFRelease(endType);
        if (noProductOrPlatform) CFRelease(noProductOrPlatform);
        
        return true;
    });
    
    if (pathPrefix) CFRelease(pathPrefix);
    return result;
}


static CFDictionaryRef _CFBundleCreateQueryTableAtPath(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, CFStringRef resourcesDirectory, CFStringRef subdirectory)
{
    
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
    
    CFMutableStringRef path = NULL;
    if (bundle) {
        path = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, bundle->_bundleBasePath);
    } else {
        CFURLRef url = CFURLCopyAbsoluteURL(bundleURL);
        CFStringRef bundlePath = CFURLCopyFileSystemPath(url, PLATFORM_PATH_STYLE);
        CFRelease(url);
        path = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, bundlePath);
        CFRelease(bundlePath);
    }
    
    if (resourcesDirectory) {
        _CFAppendPathComponent2(path, resourcesDirectory);
    }
    
    // Record the length of the base path, so we can strip off the stuff we'll be appending later
    CFIndex basePathLen = CFStringGetLength(path);
    
    if (subdirectory) {
        _CFAppendPathComponent2(path, subdirectory);
    }
    // read the content in sub dir and put them into query table
    _CFBundleReadDirectory(path, bundle, subdirectory, allFiles, false, type, queryTable, typeDir, NULL, false, product, platform, NULL, false);
    CFStringDelete(path, CFRangeMake(basePathLen, CFStringGetLength(path) - basePathLen));    // Strip the string back to the base path
    
    CFIndex numOfAllFiles = CFArrayGetCount(allFiles);
    
    if (bundle && !languages) {
        languages = _CFBundleGetLanguageSearchList(bundle);
    }
    CFIndex numLprojs = languages ? CFArrayGetCount(languages) : 0;
    CFMutableDictionaryRef addedTypes = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    
    Boolean hasFileAdded = false;
    Boolean firstLproj = true;
    
    // First, search lproj for user's chosen language
    if (numLprojs >= 1) {
        CFStringRef lprojTarget = (CFStringRef)CFArrayGetValueAtIndex(languages, 0);
        _CFAppendPathComponent2(path, lprojTarget);
        _CFAppendPathExtension2(path, _CFBundleLprojExtension);
        if (subdirectory) {
            _CFAppendPathComponent2(path, subdirectory);
        }
        _CFBundleReadDirectory(path, bundle, subdirectory, allFiles, hasFileAdded, type, queryTable, typeDir, addedTypes, firstLproj, product, platform, lprojTarget, true);
        CFStringDelete(path, CFRangeMake(basePathLen, CFStringGetLength(path) - basePathLen));         // Strip the string back to the base path

        if (!hasFileAdded && numOfAllFiles < CFArrayGetCount(allFiles)) {
            hasFileAdded = true;
        }
        firstLproj = false;
    }
    
    // Next, search Base.lproj folder
    _CFAppendPathComponent2(path, _CFBundleBaseDirectory);
    _CFAppendPathExtension2(path, _CFBundleLprojExtension);
    if (subdirectory) {
        _CFAppendPathComponent2(path, subdirectory);
    }
    _CFBundleReadDirectory(path, bundle, subdirectory, allFiles, hasFileAdded, type, queryTable, typeDir, addedTypes, YES, product, platform, _CFBundleBaseDirectory, true);
    CFStringDelete(path, CFRangeMake(basePathLen, CFStringGetLength(path) - basePathLen));    // Strip the string back to the base path
    
    if (!hasFileAdded && numOfAllFiles < CFArrayGetCount(allFiles)) {
        hasFileAdded = true;
    }
    
    // Finally, search remaining languages (development language first)
    if (numLprojs >= 2) {
        // for each lproj we are interested in, read the content and put them into query table
        for (CFIndex i = 1; i < CFArrayGetCount(languages); i++) {
            CFStringRef lprojTarget = (CFStringRef) CFArrayGetValueAtIndex(languages, i);
            _CFAppendPathComponent2(path, lprojTarget);
            _CFAppendPathExtension2(path, _CFBundleLprojExtension);
            if (subdirectory) {
                _CFAppendPathComponent2(path, subdirectory);
            }
            _CFBundleReadDirectory(path, bundle, subdirectory, allFiles, hasFileAdded, type, queryTable, typeDir, addedTypes, false, product, platform, lprojTarget, true);
            CFStringDelete(path, CFRangeMake(basePathLen, CFStringGetLength(path) - basePathLen));         // Strip the string back to the base path
            
            if (!hasFileAdded && numOfAllFiles < CFArrayGetCount(allFiles)) {
                hasFileAdded = true;
            }
        }
    }
    
    CFRelease(addedTypes);
    CFRelease(path);
    
    // put the array of all files in sub dir to the query table
    if (CFArrayGetCount(allFiles) > 0) {
        CFDictionarySetValue(queryTable, _CFBundleAllFiles, allFiles);
    }
    
    CFRelease(platform);
    CFRelease(product);
    CFRelease(allFiles);
    CFRelease(typeDir);
    CFRelease(type);
    
    
    return queryTable;
}   

// caller need to release the table
static CFDictionaryRef _CFBundleCopyQueryTable(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, CFStringRef resourcesDirectory, CFStringRef subdirectory)
{
    CFDictionaryRef subTable = NULL;
    
    // take the lock
    if (bundle) {
        CFMutableStringRef argDirStr = NULL;
        if (subdirectory) {
            argDirStr = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, resourcesDirectory);
            _CFAppendPathComponent2(argDirStr, subdirectory);
        } else {
            argDirStr = (CFMutableStringRef)CFRetain(resourcesDirectory);
        }
        
        __CFSpinLock(&bundle->_queryLock);
        
        // check if the query table for the given sub dir has been created
        subTable = (CFDictionaryRef) CFDictionaryGetValue(bundle->_queryTable, argDirStr);
        
        if (!subTable) {
            // create the query table for the given sub dir
            subTable = _CFBundleCreateQueryTableAtPath(bundle, bundleURL, languages, resourcesDirectory, subdirectory);
            
            CFDictionarySetValue(bundle->_queryTable, argDirStr, subTable);
        } else {
            CFRetain(subTable);
        }
        __CFSpinUnlock(&bundle->_queryLock);
        CFRelease(argDirStr);
    } else {
        subTable = _CFBundleCreateQueryTableAtPath(NULL, bundleURL, languages, resourcesDirectory, subdirectory);
    }
    
    return subTable;
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
        url = (CFURLRef)CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, path, PLATFORM_PATH_STYLE, true, base);
    } else {
        url = (CFURLRef)CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, path, PLATFORM_PATH_STYLE, false, base);
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

static CFTypeRef _CFBundleCopyURLsOfKey(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, CFStringRef resourcesDirectory, CFStringRef subDir, CFStringRef key, CFStringRef lproj, Boolean returnArray, Boolean localized, uint8_t bundleVersion, Boolean (^predicate)(CFStringRef filename, Boolean *stop))
{
    CFTypeRef value = NULL;
    Boolean stop = false; // for predicate
    CFMutableArrayRef interResult = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFDictionaryRef subTable = NULL;
    
    CFMutableStringRef path = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, resourcesDirectory);
    if (1 == bundleVersion) {
        CFIndex savedPathLength = CFStringGetLength(path);
        // add the non-localized resource dir
        _CFAppendPathComponent2(path, _CFBundleNonLocalizedResourcesDirectoryName);
        subTable = _CFBundleCopyQueryTable(bundle, bundleURL, languages, path, subDir);
        if (predicate) {
            _CFBundleFindResourcesWithPredicate(interResult, subTable, predicate, &stop);
        } else {
            value = CFDictionaryGetValue(subTable, key);
        }
        CFStringDelete(path, CFRangeMake(savedPathLength, CFStringGetLength(path) - savedPathLength));    // Strip the string back to the base path
    }
    
    if (!value && !stop) {
        if (subTable) CFRelease(subTable);
        subTable = _CFBundleCopyQueryTable(bundle, bundleURL, languages, path, subDir);
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
            if (subDir && CFStringGetLength(subDir) > 0) {
                if (CFStringFindWithOptions(pathValue, subDir, searchRange, kCFCompareEqualTo, &resultRange) && resultRange.location != searchRange.location) {
                    searchForLocalization = true;
                }
            } else if (!(subDir && CFStringGetLength(subDir) > 0) && searchRange.length != 0) {
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
        CFMutableStringRef lprojSubdirName = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, lproj);
        _CFAppendPathExtension2(lprojSubdirName, _CFBundleLprojExtension);
        if (subDir && CFStringGetLength(subDir) > 0) {
            _CFAppendTrailingPathSlash2(lprojSubdirName);
        }
        subTable = _CFBundleCopyQueryTable(bundle, bundleURL, languages, path, lprojSubdirName);
        CFRelease(lprojSubdirName);
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
        CFMutableStringRef urlStr = NULL;
        if (bundle) {
            urlStr = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, bundle->_bundleBasePath);
        } else {
            CFURLRef url = CFURLCopyAbsoluteURL(bundleURL);
            CFStringRef bundlePath = CFURLCopyFileSystemPath(url, PLATFORM_PATH_STYLE);
            urlStr = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, bundlePath);
            CFRelease(url);
            CFRelease(bundlePath);
        }
        
        if (resourcesDirectory && CFStringGetLength(resourcesDirectory)) {
            _CFAppendPathComponent2(urlStr, resourcesDirectory);
        }

        _CFAppendTrailingPathSlash2(urlStr);
        
        if (!returnArray) {
            Boolean isOnlyTypeOrAllFiles = CFStringHasPrefix(key, _CFBundleTypeIndicator);
            isOnlyTypeOrAllFiles |= CFStringHasPrefix(key, _CFBundleAllFiles);
            
            CFStringRef resultPath = (CFStringRef)CFArrayGetValueAtIndex((CFArrayRef)interResult, 0);
            if (!isOnlyTypeOrAllFiles) {
                // path is a part of an actual path in the query table, so it should not have a length greater than the buffer size
                CFStringAppend(urlStr, resultPath);
                if (CFStringGetCharacterAtIndex(resultPath, CFStringGetLength(resultPath)-1) == slash) {
                    result = (CFURLRef)CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, true);
                } else {
                    result = (CFURLRef)CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, false);
                }
            } else {
                // need to create relative URLs for binary compatibility issues
                CFURLRef base = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, true);
                result = (CFURLRef)_CFBundleCreateRelativeURLFromBaseAndPath(resultPath, base, slash, _CFGetSlashStr());
                CFRelease(base);
            }
        } else {
            // need to create relative URLs for binary compatibility issues
            CFIndex numOfPaths = CFArrayGetCount((CFArrayRef)interResult);
            CFURLRef base = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, urlStr, PLATFORM_PATH_STYLE, true);
            CFMutableArrayRef urls = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            for (CFIndex i = 0; i < numOfPaths; i++) {
                CFStringRef path = (CFStringRef)CFArrayGetValueAtIndex((CFArrayRef)interResult, i);
                CFURLRef url = _CFBundleCreateRelativeURLFromBaseAndPath(path, base, slash, _CFGetSlashStr());
                CFArrayAppendValue(urls, url);
                CFRelease(url);
            }
            result = urls;
            CFRelease(base);
        }
        CFRelease(urlStr);
    } else if (returnArray) {
        result = CFRetain(interResult);
    }
    if (path) CFRelease(path);
    CFRelease(interResult);
    return result;
}

#pragma mark -

// This is the main entry point for all resource lookup.
// Research shows that by far the most common scenario is to pass in a bundle object, a resource name, and a resource type, using the default localization.
// It is probably the case that more than a few resources will be looked up, making the cost of a readdir less than repeated stats. But it is a relative waste of memory to create strings for every file name in the bundle, especially since those are not what are returned to the caller (URLs are). So, an idea: cache the existence of the most common file names (Info.plist, en.lproj, etc) instead of creating entries for them. If other resources are requested, then go ahead and do the readdir and cache the rest of the file names.
// Another idea: if you want caching, you should create a bundle object. Otherwise we'll happily readdir each time.
CF_EXPORT CFTypeRef _CFBundleCopyFindResources(CFBundleRef bundle, CFURLRef bundleURL, CFArrayRef languages, CFStringRef resourceName, CFStringRef resourceType, CFStringRef subPath, CFStringRef lproj, Boolean returnArray, Boolean localized, Boolean (^predicate)(CFStringRef filename, Boolean *stop))
{
    
    // Don't use any path info passed into the resource name
    CFStringRef realResourceName = NULL;
    CFStringRef subPathFromResourceName = NULL;

    if (resourceName) {
        CFIndex slashLocation = -1;
        realResourceName = _CFCreateLastPathComponent(kCFAllocatorSystemDefault, resourceName, &slashLocation);
        if (slashLocation > 0) {
            // do not include the /
            subPathFromResourceName = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, resourceName, CFRangeMake(0, slashLocation));
        }
        
        // Normalize the resource name by converting it to file system representation. Otherwise when we look for the key in our tables, it will not match.
        // TODO: remove this in some way to avoid the malloc?
        char buff[CFMaxPathSize];
        if (CFStringGetFileSystemRepresentation(realResourceName, buff, CFMaxPathSize)) {
            CFRelease(realResourceName);
            realResourceName = CFStringCreateWithFileSystemRepresentation(kCFAllocatorSystemDefault, buff);
        }
    }
        
    CFMutableStringRef key = NULL;
    const static UniChar extensionSep = '.';
    
    if (realResourceName && CFStringGetLength(realResourceName) > 0 && resourceType && CFStringGetLength(resourceType) > 0) {
        // Testing shows that using a mutable string here is significantly faster than using the format functions.
        key = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, realResourceName);
        // Don't re-append a . if the resource name already has one
        if (CFStringGetCharacterAtIndex(resourceType, 0) != '.') CFStringAppendCharacters(key, &extensionSep, 1);
        CFStringAppend(key, resourceType);
    } else if (realResourceName && CFStringGetLength(realResourceName) > 0) {
        key = (CFMutableStringRef)CFRetain(realResourceName);
    } else if (resourceType && CFStringGetLength(resourceType) > 0) {
        key = CFStringCreateMutableCopy(kCFAllocatorSystemDefault, 0, _CFBundleTypeIndicator);
        // Don't re-append a . if the resource name already has one
        if (CFStringGetCharacterAtIndex(resourceType, 0) != '.') CFStringAppendCharacters(key, &extensionSep, 1);
        CFStringAppend(key, resourceType);
    } else {
        key = (CFMutableStringRef)CFRetain(_CFBundleAllFiles);
    }
    
    CFStringRef realSubdirectory = NULL;
    
    if (subPath && CFStringGetLength(subPath) && !subPathFromResourceName) {
        realSubdirectory = (CFStringRef)CFRetain(subPath);
    } else if (subPathFromResourceName && CFStringGetLength(subPathFromResourceName)) {
        realSubdirectory = (CFStringRef)CFRetain(subPathFromResourceName);
    }
    
    uint8_t bundleVersion = bundle ? _CFBundleLayoutVersion(bundle) : 0;
    if (bundleURL && !languages) {
        languages = _CFBundleCopyLanguageSearchListInDirectory(kCFAllocatorSystemDefault, bundleURL, &bundleVersion);
    } else if (languages) {
        CFRetain(languages);
    }
    
    CFStringRef resDir = _CFBundleGetResourceDirForVersion(bundleVersion);
    
    CFTypeRef returnValue = _CFBundleCopyURLsOfKey(bundle, bundleURL, languages, resDir, realSubdirectory, key, lproj, returnArray, localized, bundleVersion, predicate);
    
    if ((!returnValue || (CFGetTypeID(returnValue) == CFArrayGetTypeID() && CFArrayGetCount((CFArrayRef)returnValue) == 0)) && (0 == bundleVersion || 2 == bundleVersion)) {
        CFStringRef bundlePath = NULL;
        if (bundle) {
            bundlePath = bundle->_bundleBasePath;
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
            if ((bundleVersion == 0 && realSubdirectory && CFEqual(realSubdirectory, CFSTR("Resources"))) || (bundleVersion == 2 && realSubdirectory && CFEqual(realSubdirectory, CFSTR("Contents/Resources")))) {
                if (realSubdirectory) CFRelease(realSubdirectory);
                realSubdirectory = CFSTR("");
            } else if ((bundleVersion == 0 && realSubdirectory && CFStringFindWithOptions(realSubdirectory, CFSTR("Resources/"), CFRangeMake(0, 10), kCFCompareEqualTo, &found) && found.location+10 < CFStringGetLength(realSubdirectory))) {
                CFStringRef tmpRealSubdirectory = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, realSubdirectory, CFRangeMake(10, CFStringGetLength(realSubdirectory) - 10));
                if (realSubdirectory) CFRelease(realSubdirectory);
                realSubdirectory = tmpRealSubdirectory;
            } else if ((bundleVersion == 2 && realSubdirectory && CFStringFindWithOptions(realSubdirectory, CFSTR("Contents/Resources/"), CFRangeMake(0, 19), kCFCompareEqualTo, &found) && found.location+19 < CFStringGetLength(realSubdirectory))) {
                CFStringRef tmpRealSubdirectory = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, realSubdirectory, CFRangeMake(19, CFStringGetLength(realSubdirectory) - 19));
                if (realSubdirectory) CFRelease(realSubdirectory);
                realSubdirectory = tmpRealSubdirectory;
            } else {
                // Assume no resources directory
                resDir = CFSTR("");
            }
            returnValue = _CFBundleCopyURLsOfKey(bundle, bundleURL, languages, resDir, realSubdirectory, key, lproj, returnArray, localized, bundleVersion, predicate);
        }
        CFRelease(bundlePath);
    }
    
    if (realResourceName) CFRelease(realResourceName);
    if (realSubdirectory) CFRelease(realSubdirectory);
    if (subPathFromResourceName) CFRelease(subPathFromResourceName);
    if (languages) CFRelease(languages);
    CFRelease(key);
    return returnValue;
}

#pragma mark -
#pragma mark Localized Strings


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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated"
                if (CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, tableURL, &tableData, NULL, NULL, &errCode)) {
#pragma GCC diagnostic pop
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

