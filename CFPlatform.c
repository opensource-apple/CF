/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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

/*	CFPlatform.c
	Copyright (c) 1999-2009, Apple Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFInternal.h"
#include <CoreFoundation/CFPriv.h>
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
    #include <stdlib.h>
    #include <sys/stat.h>
    #include <string.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <pwd.h>
    #include <crt_externs.h>
    #include <mach-o/dyld.h>
#endif

#if DEPLOYMENT_TARGET_WINDOWS
#include <shellapi.h>
#include <shlobj.h>

#define getcwd _NS_getcwd

#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_WINDOWS_SYNC
#define kCFPlatformInterfaceStringEncoding	kCFStringEncodingUTF8
#else
#define kCFPlatformInterfaceStringEncoding	CFStringGetSystemEncoding()
#endif

static CFStringRef _CFUserName(void);

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
// CoreGraphics and LaunchServices are only projects (1 Dec 2006) that use these
char **_CFArgv(void) { return *_NSGetArgv(); }
int _CFArgc(void) { return *_NSGetArgc(); }
#endif


__private_extern__ Boolean _CFGetCurrentDirectory(char *path, int maxlen) {
    return getcwd(path, maxlen) != NULL;
}

static Boolean __CFIsCFM = false;

// If called super early, we just return false
__private_extern__ Boolean _CFIsCFM(void) {
    return __CFIsCFM;
}

#if DEPLOYMENT_TARGET_WINDOWS
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif


#if DEPLOYMENT_TARGET_WINDOWS
// Returns the path to the CF DLL, which we can then use to find resources like char sets
bool bDllPathCached = false;
__private_extern__ const wchar_t *_CFDLLPath(void) {
    static wchar_t cachedPath[MAX_PATH+1];

    if (!bDllPathCached) {
#ifdef _DEBUG
        // might be nice to get this from the project file at some point
        wchar_t *DLLFileName = L"CoreFoundation_debug.dll";
#else
        wchar_t *DLLFileName = L"CoreFoundation.dll";
#endif
        HMODULE ourModule = GetModuleHandleW(DLLFileName);
        
        CFAssert(ourModule, __kCFLogAssertion, "GetModuleHandle failed");

        DWORD wResult = GetModuleFileNameW(ourModule, cachedPath, MAX_PATH+1);
        CFAssert1(wResult > 0, __kCFLogAssertion, "GetModuleFileName failed: %d", GetLastError());
        CFAssert1(wResult < MAX_PATH+1, __kCFLogAssertion, "GetModuleFileName result truncated: %s", cachedPath);

        // strip off last component, the DLL name
        CFIndex idx;
        for (idx = wResult - 1; idx; idx--) {
            if ('\\' == cachedPath[idx]) {
                cachedPath[idx] = '\0';
                break;
            }
        }
        bDllPathCached = true;
    }
    return cachedPath;
}
#endif

static const char *__CFProcessPath = NULL;
static const char *__CFprogname = NULL;

const char **_CFGetProgname(void) {
    if (!__CFprogname)
        _CFProcessPath();		// sets up __CFprogname as a side-effect
    return &__CFprogname;
}

const char **_CFGetProcessPath(void) {
    if (!__CFProcessPath)
        _CFProcessPath();		// sets up __CFProcessPath as a side-effect
    return &__CFProcessPath;
}

#if DEPLOYMENT_TARGET_WINDOWS
const char *_CFProcessPath(void) {
    if (__CFProcessPath) return __CFProcessPath;
    wchar_t buf[CFMaxPathSize] = {0};
    DWORD rlen = GetModuleFileNameW(NULL, buf, sizeof(buf) / sizeof(buf[0]));
    if (0 < rlen) {
	char asciiBuf[CFMaxPathSize] = {0};
	int res = WideCharToMultiByte(CP_UTF8, 0, buf, rlen, asciiBuf, sizeof(asciiBuf) / sizeof(asciiBuf[0]), NULL, NULL);
	if (0 < res) {
	    __CFProcessPath = strdup(asciiBuf);
	    __CFprogname = strrchr(__CFProcessPath, PATH_SEP);
	    __CFprogname = (__CFprogname ? __CFprogname + 1 : __CFProcessPath);
	}
    }
    if (!__CFProcessPath) {
	__CFProcessPath = "";
        __CFprogname = __CFProcessPath;
    }
    return __CFProcessPath;
}
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED
const char *_CFProcessPath(void) {
    if (__CFProcessPath) return __CFProcessPath;
#if DEPLOYMENT_TARGET_MACOSX
    if (!issetugid()) {
	const char *path = (char *)__CFgetenv("CFProcessPath");
	if (path) {
	    __CFProcessPath = strdup(path);
	    __CFprogname = strrchr(__CFProcessPath, PATH_SEP);
	    __CFprogname = (__CFprogname ? __CFprogname + 1 : __CFProcessPath);
	    return __CFProcessPath;
	}
    }
#endif
    uint32_t size = CFMaxPathSize;
    char buffer[size];
    if (0 == _NSGetExecutablePath(buffer, &size)) {
#if DEPLOYMENT_TARGET_MACOSX && defined(__ppc__)
	size_t len = strlen(buffer);
	if (12 <= len && 0 == strcmp("LaunchCFMApp", buffer + len - 12)) {
	    struct stat exec, lcfm;
	    const char *launchcfm = "/System/Library/Frameworks/Carbon.framework/Versions/Current/Support/LaunchCFMApp";
	    if (0 == stat(launchcfm, &lcfm) && 0 == stat(buffer, &exec) && (lcfm.st_dev == exec.st_dev) && (lcfm.st_ino == exec.st_ino)) {
		// Executable is LaunchCFMApp, take special action
		__CFIsCFM = true;
		if ((*_NSGetArgv())[1] && '/' == *((*_NSGetArgv())[1])) {
		    strlcpy(buffer, (*_NSGetArgv())[1], sizeof(buffer));
		}
	    }
	}
#endif
	__CFProcessPath = strdup(buffer);
	__CFprogname = strrchr(__CFProcessPath, PATH_SEP);
	__CFprogname = (__CFprogname ? __CFprogname + 1 : __CFProcessPath);
    }
    if (!__CFProcessPath) {
	__CFProcessPath = "";
        __CFprogname = __CFProcessPath;
    }
    return __CFProcessPath;
}
#endif

__private_extern__ CFStringRef _CFProcessNameString(void) {
    static CFStringRef __CFProcessNameString = NULL;
    if (!__CFProcessNameString) {
        const char *processName = *_CFGetProgname();
        if (!processName) processName = "";
        CFStringRef newStr = CFStringCreateWithCString(kCFAllocatorSystemDefault, processName, kCFPlatformInterfaceStringEncoding);
        if (!OSAtomicCompareAndSwapPtrBarrier(NULL, (void *) newStr, (void * volatile *)& __CFProcessNameString)) {
            CFRelease(newStr);    // someone else made the assignment, so just release the extra string.
        }
    }
    return __CFProcessNameString;
}

static CFStringRef __CFUserName = NULL;

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
static CFURLRef __CFHomeDirectory = NULL;
static uint32_t __CFEUID = -1;
static uint32_t __CFUID = -1;

static CFURLRef _CFCopyHomeDirURLForUser(struct passwd *upwd) {
    CFURLRef home = NULL;
    if (!issetugid()) {
	const char *path = __CFgetenv("CFFIXED_USER_HOME");
	if (path) {
	    home = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (uint8_t *)path, strlen(path), true);
	}
    }
    if (!home) {
        if (upwd && upwd->pw_dir) {
            home = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (uint8_t *)upwd->pw_dir, strlen(upwd->pw_dir), true);
	}
    }
    return home;
}

static void _CFUpdateUserInfo(void) {
    struct passwd *upwd;

    __CFEUID = geteuid();
    __CFUID = getuid();
    if (__CFHomeDirectory)  CFRelease(__CFHomeDirectory);
    __CFHomeDirectory = NULL;
    if (__CFUserName) CFRelease(__CFUserName);
    __CFUserName = NULL;

    upwd = getpwuid(__CFEUID ? __CFEUID : __CFUID);
    __CFHomeDirectory = _CFCopyHomeDirURLForUser(upwd);
    if (!__CFHomeDirectory) {
        const char *cpath = __CFgetenv("HOME");
        if (cpath) {
            __CFHomeDirectory = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (uint8_t *)cpath, strlen(cpath), true);
        }
    }

    // This implies that UserManager stores directory info in CString
    // rather than FileSystemRep.  Perhaps this is wrong & we should
    // expect NeXTSTEP encodings.  A great test of our localized system would
    // be to have a user "O-umlat z e r".  XXX
    if (upwd && upwd->pw_name) {
        __CFUserName = CFStringCreateWithCString(kCFAllocatorSystemDefault, upwd->pw_name, kCFPlatformInterfaceStringEncoding);
    } else {
        const char *cuser = __CFgetenv("USER");
        if (cuser)
            __CFUserName = CFStringCreateWithCString(kCFAllocatorSystemDefault, cuser, kCFPlatformInterfaceStringEncoding);
    }
}
#endif

#if DEPLOYMENT_TARGET_WINDOWS
typedef DWORD (*NetUserGetInfoCall)(wchar_t *, wchar_t *, DWORD, char* *);
#endif

static CFURLRef _CFCreateHomeDirectoryURLForUser(CFStringRef uName) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
    if (!uName) {
        if (geteuid() != __CFEUID || getuid() != __CFUID || !__CFHomeDirectory)
            _CFUpdateUserInfo();
        if (__CFHomeDirectory) CFRetain(__CFHomeDirectory);
        return __CFHomeDirectory;
    } else {
        struct passwd *upwd = NULL;
        char buf[128], *user;
        SInt32 len = CFStringGetLength(uName), size = CFStringGetMaximumSizeForEncoding(len, kCFPlatformInterfaceStringEncoding);
        CFIndex usedSize;
        if (size < 127) {
            user = buf;
        } else {
            user = CFAllocatorAllocate(kCFAllocatorSystemDefault, size+1, 0);
            if (__CFOASafe) __CFSetLastAllocationEventName(user, "CFUtilities (temp)");
        }
        if (CFStringGetBytes(uName, CFRangeMake(0, len), kCFPlatformInterfaceStringEncoding, 0, true, (uint8_t *)user, size, &usedSize) == len) {
            user[usedSize] = '\0';
            upwd = getpwnam(user);
        }
        if (buf != user) {
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, user);
        }
        return _CFCopyHomeDirURLForUser(upwd);
    }
#elif DEPLOYMENT_TARGET_WINDOWS
    CFStringRef user = !uName ? _CFUserName() : uName;
    CFURLRef retVal = NULL;
    CFIndex len = 0;
    CFStringRef str = NULL;

    if (!uName || CFEqual(user, _CFUserName())) {
        const char *cpath = __CFgetenv("HOMEPATH");
        const char *cdrive = __CFgetenv("HOMEDRIVE");
        if (cdrive && cpath) {
            char fullPath[CFMaxPathSize];
            strlcpy(fullPath, cdrive, sizeof(fullPath));
            strlcat(fullPath, cpath, sizeof(fullPath));
            str = CFStringCreateWithCString(kCFAllocatorSystemDefault, fullPath, kCFPlatformInterfaceStringEncoding);
            retVal = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str, kCFURLWindowsPathStyle, true);
            CFRelease(str);
	}
    }
    if (retVal == NULL) {
		UniChar pathChars[MAX_PATH];
		if (S_OK == SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, (wchar_t *) pathChars)) {
			UniChar* p = pathChars;
			while (*p++ != 0)
				++len;
			str = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, pathChars, len);
            		retVal = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str, kCFURLWindowsPathStyle, true);
		} else {
			// We have to get "some" directory location, so fall-back to the
			// processes current directory.
			UniChar currDir[MAX_PATH];
			DWORD dwChars = GetCurrentDirectoryW(MAX_PATH + 1, (wchar_t *)currDir);
			if (dwChars > 0) {
				UniChar* p = currDir;
				while (*p++ != 0)
					++len;
				str = CFStringCreateWithCharacters(kCFAllocatorDefault, currDir, len);
				retVal = CFURLCreateWithFileSystemPath(NULL, str, kCFURLWindowsPathStyle, true);
			}
		    CFRelease(str);
                }
    }
#if 0
        char fullPath[CFMaxPathSize];
        HRESULT hr = SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0 /* SHGFP_TYPE_CURRENT */, fullPath);
        if (SUCCEEDED(hr)) {
            CFStringRef str = CFStringCreateWithCString(NULL, fullPath, kCFPlatformInterfaceStringEncoding);
            retVal = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str, kCFURLWindowsPathStyle, true);
            CFRelease(str);
        }
    }
    if (!retVal) {
        struct _USER_INFO_2 *userInfo;
        HINSTANCE hinstDll = GetModuleHandleA("NETAPI32");
        if (!hinstDll)
            hinstDll = LoadLibraryExA("NETAPI32", NULL, 0);
        if (hinstDll) {
            NetUserGetInfoCall lpfn = (NetUserGetInfoCall)GetProcAddress(hinstDll, "NetUserGetInfo");
            if (lpfn) {
        unsigned namelen = CFStringGetLength(user);
                UniChar *username;
                username = (UniChar *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UniChar) * (namelen + 1), 0);
        CFStringGetCharacters(user, CFRangeMake(0, namelen), username);
                if (!(*lpfn)(NULL, (wchar_t *)username, 2, (char * *)&userInfo)) {
            UInt32 len = 0;
            CFMutableStringRef str;
            while (userInfo->usri2_home_dir[len] != 0) len ++;
            str = CFStringCreateMutable(kCFAllocatorSystemDefault, len+1);
            CFStringAppendCharacters(str, (const UniChar *)userInfo->usri2_home_dir, len);
                    retVal = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, str, kCFURLWindowsPathStyle, true);
            CFRelease(str);
        }
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, username);
    }
        } else {
        }
    }
#endif

    // We could do more here (as in KB Article Q101507). If that article is to
    // be believed, we should only run into this case on Win95, or through
    // user error.
#if DEPLOYMENT_TARGET_WINDOWS_SAFARI
    if (retVal) {
        CFStringRef str = CFURLCopyFileSystemPath(retVal, kCFURLWindowsPathStyle);
        if (str && CFStringGetLength(str) == 0) {
            CFRelease(retVal);
            retVal=NULL;
        }
        if (str) CFRelease(str);
    }
#else
    CFStringRef testPath = CFURLCopyPath(retVal);
    if (CFStringGetLength(testPath) == 0) {
        CFRelease(retVal);
        retVal=NULL;
        }
    if (testPath) {
        CFRelease(testPath);
    }
#endif
    return retVal;

#else
#error Dont know how to compute users home directories on this platform
#endif
}

static CFStringRef _CFUserName(void) {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
    if (geteuid() != __CFEUID || getuid() != __CFUID)
	_CFUpdateUserInfo();
#elif DEPLOYMENT_TARGET_WINDOWS
    if (!__CFUserName) {
	wchar_t username[1040];
	DWORD size = 1040;
	username[0] = 0;
	if (GetUserNameW(username, &size)) {
	    // discount the extra NULL by decrementing the size
	    __CFUserName = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar *)username, size - 1);
	} else {
	    const char *cname = __CFgetenv("USERNAME");
	    if (cname)
                __CFUserName = CFStringCreateWithCString(kCFAllocatorSystemDefault, cname, kCFPlatformInterfaceStringEncoding);
	}
    }
#else
#error Dont know how to compute user name on this platform
#endif
    if (!__CFUserName)
        __CFUserName = (CFStringRef)CFRetain(CFSTR(""));
    return __CFUserName;
}

__private_extern__ CFStringRef _CFGetUserName(void) {
    return CFStringCreateCopy(kCFAllocatorSystemDefault, _CFUserName());
}

#define CFMaxHostNameLength	256
#define CFMaxHostNameSize	(CFMaxHostNameLength+1)

__private_extern__ CFStringRef _CFStringCreateHostName(void) {
    char myName[CFMaxHostNameSize];

    // return @"" instead of nil a la CFUserName() and Ali Ozer
    if (0 != gethostname(myName, CFMaxHostNameSize)) myName[0] = '\0';
    return CFStringCreateWithCString(kCFAllocatorSystemDefault, myName, kCFPlatformInterfaceStringEncoding);
}

/* These are sanitized versions of the above functions. We might want to eliminate the above ones someday.
   These can return NULL.
*/
CF_EXPORT CFStringRef CFGetUserName(void) {
    return _CFUserName();
}

CF_EXPORT CFURLRef CFCopyHomeDirectoryURLForUser(CFStringRef uName) {
    return _CFCreateHomeDirectoryURLForUser(uName);
}

#undef CFMaxHostNameLength
#undef CFMaxHostNameSize

#if DEPLOYMENT_TARGET_WINDOWS
CF_INLINE CFIndex strlen_UniChar(const UniChar* p) {
	CFIndex result = 0;
	while ((*p++) != 0)
		++result;
	return result;
}

//#include <shfolder.h>
/*
 * _CFCreateApplicationRepositoryPath returns the path to the application's
 * repository in a CFMutableStringRef. The path returned will be:
 *     <nFolder_path>\Apple Computer\<bundle_name>\
 * or if the bundle name cannot be obtained:
 *     <nFolder_path>\Apple Computer\
 * where nFolder_path is obtained by calling SHGetFolderPath with nFolder
 * (for example, with CSIDL_APPDATA or CSIDL_LOCAL_APPDATA).
 *
 * The CFMutableStringRef result must be released by the caller.
 *
 * If anything fails along the way, the result will be NULL.  
 */
CF_EXPORT CFMutableStringRef _CFCreateApplicationRepositoryPath(CFAllocatorRef alloc, int nFolder) {
    CFMutableStringRef result = NULL;
    UniChar szPath[MAX_PATH];
    
    // get the current path to the data repository: CSIDL_APPDATA (roaming) or CSIDL_LOCAL_APPDATA (nonroaming)
    if (S_OK == SHGetFolderPathW(NULL, nFolder, NULL, 0, (wchar_t *) szPath)) {
	CFStringRef directoryPath;
	
	// make it a CFString
	directoryPath = CFStringCreateWithCharacters(alloc, szPath, strlen_UniChar(szPath));
	if (directoryPath) {
	    CFBundleRef bundle;
	    CFStringRef bundleName;
	    CFStringRef completePath;
	    
	    // attempt to get the bundle name
	    bundle = CFBundleGetMainBundle();
	    if (bundle) {
		bundleName = (CFStringRef)CFBundleGetValueForInfoDictionaryKey(bundle, kCFBundleNameKey);
	    }
	    else {
		bundleName = NULL;
	    }
	    
	    if (bundleName) {
		// the path will be "<directoryPath>\Apple Computer\<bundleName>\" if there is a bundle name
		completePath = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@\\Apple Computer\\%@\\"), directoryPath, bundleName);
	    }
	    else {
		// or "<directoryPath>\Apple Computer\" if there is no bundle name.
		completePath = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@\\Apple Computer\\"), directoryPath);
	    }

	    CFRelease(directoryPath);

	    // make a mutable copy to return
	    if (completePath) {
		result = CFStringCreateMutableCopy(alloc, 0, completePath);
		CFRelease(completePath);
	    }
	}
    }

    return ( result );
}
#endif

#if DEPLOYMENT_TARGET_WINDOWS
/* On Windows, we want to use UTF-16LE for path names to get full unicode support. Internally, however, everything remains in UTF-8 representation. These helper functions stand between CF and the Microsoft CRT to ensure that we are using the right representation on both sides. */

#include <sys/stat.h>
#include <share.h>

// Creates a buffer of wchar_t to hold a UTF16LE version of the UTF8 str passed in. Caller must free the buffer when done. If resultLen is non-NULL, it is filled out with the number of characters in the string.
static wchar_t *createWideFileSystemRepresentation(const char *str, CFIndex *resultLen) {
    // Get the real length of the string in UTF16 characters
    CFStringRef cfStr = CFStringCreateWithCString(kCFAllocatorSystemDefault, str, kCFStringEncodingUTF8);
    CFIndex strLen = CFStringGetLength(cfStr);
    
    // Allocate a wide buffer to hold the converted string, including space for a NULL terminator
    wchar_t *wideBuf = (wchar_t *)malloc((strLen + 1) * sizeof(wchar_t));
    
    // Copy the string into the buffer and terminate
    CFStringGetCharacters(cfStr, CFRangeMake(0, strLen), (UniChar *)wideBuf);
    wideBuf[strLen] = 0;
    
    CFRelease(cfStr);
    if (resultLen) *resultLen = strLen;
    return wideBuf;
}

// Copies a UTF16 buffer into a supplied UTF8 buffer. 
static void copyToNarrowFileSystemRepresentation(const wchar_t *wide, CFIndex dstBufSize, char *dstbuf) {
    // Get the real length of the wide string in UTF8 characters
    CFStringRef cfStr = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar *)wide, wcslen(wide));
    CFIndex strLen = CFStringGetLength(cfStr);
    CFIndex bytesUsed;
    
    // Copy the wide string into the buffer and terminate
    CFStringGetBytes(cfStr, CFRangeMake(0, strLen), kCFStringEncodingUTF8, 0, false, (uint8_t *)dstbuf, dstBufSize, &bytesUsed);
    dstbuf[bytesUsed] = 0;
    
    CFRelease(cfStr);
}

CF_EXPORT int _NS_stat(const char *name, struct _stat *st) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int res = _wstat(wide, st);
    free(wide);
    return res;
}

CF_EXPORT int _NS_mkdir(const char *name) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int res = _wmkdir(wide);
    free(wide);
    return res;
}

CF_EXPORT int _NS_rmdir(const char *name) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int res = _wrmdir(wide);
    free(wide);
    return res;
}

CF_EXPORT int _NS_chmod(const char *name, int mode) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int res = _wchmod(wide, mode);
    free(wide);
    return res;
}

CF_EXPORT int _NS_unlink(const char *name) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int res = _wunlink(wide);
    free(wide);
    return res;
}

// Warning: this doesn't support dstbuf as null even though 'getcwd' does
CF_EXPORT char *_NS_getcwd(char *dstbuf, size_t size) {
    if (!dstbuf) {
	CFLog(kCFLogLevelWarning, CFSTR("CFPlatform: getcwd called with null buffer"));
	return 0;
    }
    
    wchar_t *buf = _wgetcwd(NULL, 0);
    if (!buf) {
        return NULL;
    }
        
    // Convert result to UTF8
    copyToNarrowFileSystemRepresentation(buf, (CFIndex)size, dstbuf);
    free(buf);
    return dstbuf;
}

CF_EXPORT char *_NS_getenv(const char *name) {
    // todo: wide env variables
    return getenv(name);
}

CF_EXPORT int _NS_rename(const char *oldName, const char *newName) {
    wchar_t *oldWide = createWideFileSystemRepresentation(oldName, NULL);
    wchar_t *newWide = createWideFileSystemRepresentation(newName, NULL);
    // _wrename on Windows does not behave exactly as rename() on Mac OS -- if the file exists, the Windows one will fail whereas the Mac OS version will replace
    // To simulate the Mac OS behavior, we use the Win32 API then fill out errno if something goes wrong
    BOOL winRes = MoveFileExW(oldWide, newWide, MOVEFILE_REPLACE_EXISTING);
    DWORD error = GetLastError();
    if (!winRes) {
	    switch (error) {
            case ERROR_SUCCESS:
                errno = 0;
                break;
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_OPEN_FAILED:
                errno = ENOENT;
                break;
            case ERROR_ACCESS_DENIED:
                errno = EACCES;
                break;
            default:
                errno = error;
        }
    }
    free(oldWide);
    free(newWide);
    return (winRes ? 0 : -1);
}

CF_EXPORT int _NS_open(const char *name, int oflag, int pmode) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int fd;
    _wsopen_s(&fd, wide, oflag, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    free(wide);
    return fd;
}

CF_EXPORT int _NS_chdir(const char *name) {
    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    int res = _wchdir(wide);
    free(wide);
    return res;
}

CF_EXPORT int _NS_access(const char *name, int amode) {
    // execute is always true
    if (amode == 1) return 0;

    wchar_t *wide = createWideFileSystemRepresentation(name, NULL);
    // we only care about the read-only (04) and write-only (02) bits, so mask octal 06
    int res = _waccess(wide, amode & 06);
    free(wide);
    return res;
}

// This is a bit different than the standard 'mkstemp', because the size parameter is needed so we know the size of the UTF8 buffer
// Also, we don't avoid the race between creating a temporary file name and opening it on Windows like we do on Mac
CF_EXPORT int _NS_mkstemp(char *name, int bufSize) {
    CFIndex nameLen;
    wchar_t *wide = createWideFileSystemRepresentation(name, &nameLen);
    
    // First check to see if the directory that this new temporary file will be created in exists. If not, set errno to ENOTDIR. This mimics the behavior of mkstemp on MacOS more closely.
    // Look for the last '\' in the path
    wchar_t *lastSlash = wcsrchr(wide, '\\');
    if (!lastSlash) {
	free(wide);
	return -1;
    }
    
    // Set the last slash to NULL temporarily and use it for _wstat
    *lastSlash = 0;
    struct _stat dirInfo;
    int res = _wstat(wide, &dirInfo);
    if (res < 0) {
	if (errno == ENOENT) {
	    errno = ENOTDIR;
	}
	free(wide);
	return -1;
    }
    // Restore the last slash
    *lastSlash = '\\';
    
    errno_t err = _wmktemp_s(wide, nameLen + 1);
    if (err != 0) {
        free(wide);
        return 0;
    }
    
    int fd;
    _wsopen_s(&fd, wide, _O_RDWR | _O_CREAT | CF_OPENFLGS, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    
    // Convert the wide name back into the UTF8 buffer the caller supplied
    copyToNarrowFileSystemRepresentation(wide, bufSize, name);
    free(wide);
    return fd;    
}

#endif

