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
/*	CFPlatform.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFInternal.h"
#include "CFPriv.h"
#if defined(__WIN32__)
    #include <sys/stat.h>
    #include <string.h>
    #include <windows.h>
    #include <stdlib.h>
#else
    #include <sys/stat.h>
    #include <string.h>
    #include <unistd.h>
    #include <pwd.h>
#endif
#if defined(__MACH__)
    #include <mach-o/dyld.h>
    #include <crt_externs.h>
#endif

extern char *getenv(const char *name);

#if defined(__MACH__)
#define kCFPlatformInterfaceStringEncoding	kCFStringEncodingUTF8
#else
#define kCFPlatformInterfaceStringEncoding	CFStringGetSystemEncoding()
#endif

char **_CFArgv(void) {
#if defined(__MACH__)
    return *_NSGetArgv();
#else
    return NULL;
#endif
}

int _CFArgc(void) {
#if defined(__MACH__)
    return *_NSGetArgc();
#else
    return 0;
#endif
}


__private_extern__ Boolean _CFGetCurrentDirectory(char *path, int maxlen) {
#if defined(__WIN32__)
    DWORD len = GetCurrentDirectoryA(maxlen, path);
    return (0 != len && len + 1 <= maxlen);
#else
    return getcwd(path, maxlen) != NULL;
#endif
}

static Boolean __CFIsCFM = false;

// If called super early, we just return false
__private_extern__ Boolean _CFIsCFM(void) {
    return __CFIsCFM;
}


#if defined(__WIN32__)
#define PATH_LIST_SEP ';'
#else
#define PATH_LIST_SEP ':'
#endif

static char *_CFSearchForNameInPath(CFAllocatorRef alloc, const char *name, char *path) {
    struct stat statbuf;
    char *nname = CFAllocatorAllocate(alloc, strlen(name) + strlen(path) + 2, 0);
    if (__CFOASafe) __CFSetLastAllocationEventName(nname, "CFUtilities (temp)");
    for (;;) {
        char *p = (char *)strchr(path, PATH_LIST_SEP);
        if (NULL != p) {
            *p = '\0';
        }
        nname[0] = '\0';
        strcat(nname, path);
        strcat(nname, "/");
        strcat(nname, name);
        // Could also do access(us, X_OK) == 0 in next condition,
        // for executable-only searching
        if (0 == stat(nname, &statbuf) && (statbuf.st_mode & S_IFMT) == S_IFREG) {
            if (p != NULL) {
                *p = PATH_LIST_SEP;
            }
            return nname;
        }
        if (NULL == p) {
            break;
        }
        *p = PATH_LIST_SEP;
        path = p + 1;
    }
    CFAllocatorDeallocate(alloc, nname);
    return NULL;
}



static const char *__CFProcessPath = NULL;
static const char *__CFprogname = "";

const char **_CFGetProgname(void) {	// This is a hack around the broken _NSGetPrognam(), for now; will be removed
    return &__CFprogname;
}

const char *_CFProcessPath(void) {
    CFAllocatorRef alloc = NULL;
    char *thePath = NULL;
    int execIndex = 0;
    
    if (__CFProcessPath) return __CFProcessPath;
    if (!__CFProcessPath) {
        thePath = getenv("CFProcessPath");
        
        alloc = CFRetain(__CFGetDefaultAllocator());
        
	if (thePath) {
	    int len = strlen(thePath);
	    __CFProcessPath = CFAllocatorAllocate(alloc, len+1, 0);
            if (__CFOASafe) __CFSetLastAllocationEventName((void *)__CFProcessPath, "CFUtilities (process-path)");
	    memmove((char *)__CFProcessPath, thePath, len + 1);
	}
    }

#if defined(__MACH__)
    {
	struct stat exec, lcfm;
        unsigned long size = CFMaxPathSize;
        char buffer[CFMaxPathSize];
        if (0 == _NSGetExecutablePath(buffer, &size) &&
            strcasestr(buffer, "LaunchCFMApp") != NULL &&
                0 == stat("/System/Library/Frameworks/Carbon.framework/Versions/Current/Support/LaunchCFMApp", &lcfm) &&
                0 == stat(buffer, &exec) &&
                (lcfm.st_dev == exec.st_dev) &&
                (lcfm.st_ino == exec.st_ino)) {
            // Executable is LaunchCFMApp, take special action
            execIndex = 1;
            __CFIsCFM = true;
        }
    }
#endif
    
    if (!__CFProcessPath && NULL != (*_NSGetArgv())[execIndex]) {
	char buf[CFMaxPathSize] = {0};
#if defined(__WIN32__)
	HINSTANCE hinst = GetModuleHandle(NULL);
	DWORD rlen = hinst ? GetModuleFileName(hinst, buf, 1028) : 0;
	thePath = rlen ? buf : NULL;
#else
	struct stat statbuf;
        const char *arg0 = (*_NSGetArgv())[execIndex];
        if (arg0[0] == '/') {
            // We've got an absolute path; look no further;
            thePath = (char *)arg0;
        } else {
            char *theList = getenv("PATH");
            if (NULL != theList && NULL == strrchr(arg0, '/')) {
                thePath = _CFSearchForNameInPath(alloc, arg0, theList);
                if (thePath) {
                    // User could have "." or "../bin" or other relative path in $PATH
                    if (('/' != thePath[0]) && _CFGetCurrentDirectory(buf, CFMaxPathSize)) {
                        strcat(buf, "/");
                        strcat(buf, thePath);
                        if (0 == stat(buf, &statbuf)) {
                            CFAllocatorDeallocate(alloc, (void *)thePath);
                            thePath = buf;
                        }
                    }
                    if (thePath != buf) {
                        strcpy(buf, thePath);
                        CFAllocatorDeallocate(alloc, (void *)thePath);
                        thePath = buf;
                    }
                }
            }
        }
        
	// After attempting a search through $PATH, if existant,
	// try prepending the current directory to argv[0].
        if (!thePath && _CFGetCurrentDirectory(buf, CFMaxPathSize)) {
            if (buf[strlen(buf)-1] != '/') {
                strcat(buf, "/");
            }
	    strcat(buf, arg0);
            if (0 == stat(buf, &statbuf)) {
		thePath = buf;
            }
	}

        if (thePath) {
            // We are going to process the buffer replacing all "/./" with "/"
            CFIndex srcIndex = 0, dstIndex = 0;
            CFIndex len = strlen(thePath);
            for (srcIndex=0; srcIndex<len; srcIndex++) {
                thePath[dstIndex] = thePath[srcIndex];
                dstIndex++;
                if ((srcIndex < len-2) && (thePath[srcIndex] == '/') && (thePath[srcIndex+1] == '.') && (thePath[srcIndex+2] == '/')) {
                    // We are at the first slash of a "/./"  Skip the "./"
                    srcIndex+=2;
                }
            }
            thePath[dstIndex] = 0;
        }
#endif
        if (!thePath) {
	    thePath = (*_NSGetArgv())[execIndex];
        }
	if (thePath) {
	    int len = strlen(thePath);
	    __CFProcessPath = CFAllocatorAllocate(alloc, len + 1, 0);
            if (__CFOASafe) __CFSetLastAllocationEventName((void *)__CFProcessPath, "CFUtilities (process-path)");
            memmove((char *)__CFProcessPath, thePath, len + 1);
	}
	if (__CFProcessPath) {
	    const char *p = 0;
	    int i;
	    for (i = 0; __CFProcessPath[i] != 0; i++){
		if (__CFProcessPath[i] == '/')
		    p = __CFProcessPath + i; 
	    }
	    if (p != 0)
		__CFprogname = p + 1;
	    else
		__CFprogname = __CFProcessPath;
	}
    }
    if (!__CFProcessPath) {
	__CFProcessPath = "";
    }
    return __CFProcessPath;
}

__private_extern__ CFStringRef _CFProcessNameString(void) {
    static CFStringRef __CFProcessNameString = NULL;
    if (!__CFProcessNameString) {
        const char *processName = *_CFGetProgname();
        if (!processName) processName = "";
        __CFProcessNameString = CFStringCreateWithCString(__CFGetDefaultAllocator(), processName, kCFPlatformInterfaceStringEncoding);
    }
    return __CFProcessNameString;
}

static CFURLRef __CFHomeDirectory = NULL;
static CFStringRef __CFUserName = NULL;
static uint32_t __CFEUID = -1;
static uint32_t __CFUID = -1;

#if defined(__MACH__) || defined(__svr4__) || defined(__hpux__) || defined(__LINUX__) || defined(__FREEBSD__)
static CFURLRef _CFCopyHomeDirURLForUser(struct passwd *upwd) {
    CFURLRef home = NULL;
    if (upwd && upwd->pw_dir) {
        home = CFURLCreateFromFileSystemRepresentation(NULL, upwd->pw_dir, strlen(upwd->pw_dir), true);
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
        const char *cpath = getenv("HOME");
        if (cpath) {
            __CFHomeDirectory = CFURLCreateFromFileSystemRepresentation(NULL, cpath, strlen(cpath), true);
        }
    }

    // This implies that UserManager stores directory info in CString
    // rather than FileSystemRep.  Perhaps this is wrong & we should
    // expect NeXTSTEP encodings.  A great test of our localized system would
    // be to have a user "O-umlat z e r".  XXX
    if (upwd && upwd->pw_name) {
        __CFUserName = CFStringCreateWithCString(NULL, upwd->pw_name, kCFPlatformInterfaceStringEncoding);
    } else {
        const char *cuser = getenv("USER");
        if (cuser)
            __CFUserName = CFStringCreateWithCString(NULL, cuser, kCFPlatformInterfaceStringEncoding);
    }
}
#endif

static CFURLRef _CFCreateHomeDirectoryURLForUser(CFStringRef uName) {
#if defined(__MACH__) || defined(__svr4__) || defined(__hpux__) || defined(__LINUX__) || defined(__FREEBSD__)
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
            user = CFAllocatorAllocate(kCFAllocatorDefault, size+1, 0);
            if (__CFOASafe) __CFSetLastAllocationEventName(user, "CFUtilities (temp)");
        }
        if (CFStringGetBytes(uName, CFRangeMake(0, len), kCFPlatformInterfaceStringEncoding, 0, true, user, size, &usedSize) == len) {
            user[usedSize] = '\0';
            upwd = getpwnam(user);
        }
        if (buf != user) {
            CFAllocatorDeallocate(kCFAllocatorDefault, user);
        }
        return _CFCopyHomeDirURLForUser(upwd);
    }
#elif defined(__WIN32__)
#warning CF: Windows home directory goop disabled
    return NULL;
#if 0
    CFString *user = !uName ? CFUserName() : uName;

    if (!uName || CFEqual(user, CFUserName())) {
        const char *cpath = getenv("HOMEPATH");
        const char *cdrive = getenv("HOMEDRIVE");
        if (cdrive && cpath) {
            char fullPath[CFMaxPathSize];
            CFStringRef str;
            strcpy(fullPath, cdrive);
            strncat(fullPath, cpath, CFMaxPathSize-strlen(cdrive)-1);
            str = CFStringCreateWithCString(NULL, fullPath, kCFPlatformInterfaceStringEncoding);
            home = CFURLCreateWithFileSystemPath(NULL, str, kCFURLWindowsPathStyle, true);
            CFRelease(str);
        }
    }
    if (!home) {
        struct _USER_INFO_2 *userInfo;
        HINSTANCE hinstDll = GetModuleHandleA("NETAPI32");
        if (!hinstDll)
            hinstDll = LoadLibraryEx("NETAPI32", NULL, 0);
        if (hinstDll) {
            FARPROC lpfn = GetProcAddress(hinstDll, "NetUserGetInfo");
            if (lpfn) {
                unsigned namelen = CFStringGetLength(user);
                UniChar *username;
                username = CFAllocatorAllocate(kCFAllocatorDefault, sizeof(UniChar) * (namelen + 1), 0);
                if (__CFOASafe) __CFSetLastAllocationEventName(username, "CFUtilities (temp)");
                CFStringGetCharacters(user, CFRangeMake(0, namelen), username);
                if (!(*lpfn)(NULL, (LPWSTR)username, 2, (LPBYTE *)&userInfo)) {
                    UInt32 len = 0;
                    CFMutableStringRef str;
                    while (userInfo->usri2_home_dir[len] != 0) len ++;
                    str = CFStringCreateMutable(NULL, len+1);
                    CFStringAppendCharacters(str, userInfo->usri2_home_dir, len);
                    home = CFURLCreateWithFileSystemPath(NULL, str, kCFURLWindowsPathStyle, true);
                    CFRelease(str);
                }
                CFAllocatorDeallocate(kCFAllocatorDefault, username);
            }
        } else {
        }
    }
    // We could do more here (as in KB Article Q101507). If that article is to
    // be believed, we should only run into this case on Win95, or through
    // user error.
    if (CFStringGetLength(CFURLGetPath(home)) == 0) {
        CFRelease(home);
        home=NULL;
    }
#endif

#else
#error Dont know how to compute users home directories on this platform
#endif
}

static CFStringRef _CFUserName(void) {
#if defined(__MACH__) || defined(__svr4__) || defined(__hpux__) || defined(__LINUX__) || defined(__FREEBSD__)
    if (geteuid() != __CFEUID || getuid() != __CFUID)
	_CFUpdateUserInfo();
#elif defined(__WIN32__)
    if (!__CFUserName) {
	char username[1040];
	DWORD size = 1040;
	username[0] = 0;
	if (GetUserNameA(username, &size)) {
            __CFUserName = CFStringCreateWithCString(NULL, username, kCFPlatformInterfaceStringEncoding);
	} else {
	    const char *cname = getenv("USERNAME");
	    if (cname)
                __CFUserName = CFStringCreateWithCString(NULL, cname, kCFPlatformInterfaceStringEncoding);
	}
    }
#else
#error Dont know how to compute user name on this platform
#endif
    if (!__CFUserName)
        __CFUserName = CFRetain(CFSTR(""));
    return __CFUserName;
}

__private_extern__ CFStringRef _CFGetUserName(void) {
    return CFStringCreateCopy(NULL, _CFUserName());
}

#define CFMaxHostNameLength	256
#define CFMaxHostNameSize	(CFMaxHostNameLength+1)

__private_extern__ CFStringRef _CFStringCreateHostName(void) {
    char myName[CFMaxHostNameSize];

    // return @"" instead of nil a la CFUserName() and Ali Ozer
    if (0 != gethostname(myName, CFMaxHostNameSize)) myName[0] = '\0';
    return CFStringCreateWithCString(NULL, myName, kCFPlatformInterfaceStringEncoding);
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

#undef PATH_LIST_SEP
#undef CFMaxHostNameLength
#undef CFMaxHostNameSize

