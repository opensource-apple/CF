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
/*	CFFileUtilities.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFInternal.h"
#include "CFPriv.h"
#if defined(__WIN32__)
    #include <io.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <errno.h>
    #define timeval xxx_timeval
    #define BOOLEAN xxx_BOOLEAN
        #include <windows.h>
    #undef BOOLEAN
    #undef timeval
    #define fstat _fstat
    #define open _open
    #define close _close
    #define write _write
    #define read _read
    #define stat _stat
#else
    #include <string.h>
    #include <unistd.h>
    #include <dirent.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <pwd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <stdio.h>
#endif

#if defined(__WIN32__)
    #define CF_OPENFLGS	(_O_BINARY|_O_NOINHERIT)
#else
    #define CF_OPENFLGS	(0)
#endif


__private_extern__ CFStringRef _CFCopyExtensionForAbstractType(CFStringRef abstractType) {
    return (abstractType ? CFRetain(abstractType) : NULL);
}


__private_extern__ Boolean _CFCreateDirectory(const char *path) {
#if defined(__WIN32__)
    return CreateDirectoryA(path, (LPSECURITY_ATTRIBUTES)NULL);
#else
    return ((mkdir(path, 0777) == 0) ? true : false);
#endif
}

__private_extern__ Boolean _CFRemoveDirectory(const char *path) {
#if defined(__WIN32__)
    return RemoveDirectoryA(path);
#else
    return ((rmdir(path) == 0) ? true : false);
#endif
}

__private_extern__ Boolean _CFDeleteFile(const char *path) {
#if defined(__WIN32__)
    return DeleteFileA(path);
#else
    return unlink(path) == 0;
#endif
}

__private_extern__ Boolean _CFReadBytesFromFile(CFAllocatorRef alloc, CFURLRef url, void **bytes, CFIndex *length, CFIndex maxLength) {
    // maxLength is the number of bytes desired, or 0 if the whole file is desired regardless of length.
    struct stat statBuf;
    int fd = -1;
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, path, CFMaxPathSize)) {
        return false;
    }

    *bytes = NULL;

__CFSetNastyFile(url);

#if defined(__WIN32__)
    fd = open(path, O_RDONLY|CF_OPENFLGS, 0666|_S_IREAD);
#else
    fd = open(path, O_RDONLY|CF_OPENFLGS, 0666);
#endif
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &statBuf) < 0) {
        int saveerr = thread_errno();
        close(fd);
        thread_set_errno(saveerr);
        return false;
    }
    if ((statBuf.st_mode & S_IFMT) != S_IFREG) {
        close(fd);
        thread_set_errno(EACCES);
        return false;
    }
    if (statBuf.st_size == 0) {
        *bytes = CFAllocatorAllocate(alloc, 4, 0); // don't return constant string -- it's freed!
	if (__CFOASafe) __CFSetLastAllocationEventName(*bytes, "CFUtilities (file-bytes)");
        *length = 0;
    } else {
        CFIndex desiredLength;
        if ((maxLength >= statBuf.st_size) || (maxLength == 0)) {
            desiredLength = statBuf.st_size;
        } else {
            desiredLength = maxLength;
        }
        *bytes = CFAllocatorAllocate(alloc, desiredLength, 0);
	if (__CFOASafe) __CFSetLastAllocationEventName(*bytes, "CFUtilities (file-bytes)");
        if (read(fd, *bytes, desiredLength) < 0) {
            CFAllocatorDeallocate(alloc, *bytes);
            close(fd);
            return false;
        }
        *length = desiredLength;
    }
    close(fd);
    return true;
}

__private_extern__ Boolean _CFWriteBytesToFile(CFURLRef url, const void *bytes, CFIndex length) {
    struct stat statBuf;
    int fd = -1;
    int mode, mask;
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, path, CFMaxPathSize)) {
        return false;
    }

#if defined(__WIN32__)
    mask = 0;
#else
    mask = umask(0);
    umask(mask);
#endif
    mode = 0666 & ~mask;
    if (0 == stat(path, &statBuf)) {
        mode = statBuf.st_mode;
    } else if (thread_errno() != ENOENT) {
        return false;
    }
#if defined(__WIN32__)
    fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|CF_OPENFLGS, 0666|_S_IWRITE);
#else
    fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|CF_OPENFLGS, 0666);
#endif
    if (fd < 0) {
        return false;
    }
    if (length && write(fd, bytes, length) != length) {
        int saveerr = thread_errno();
        close(fd);
        thread_set_errno(saveerr);
        return false;
    }
#if defined(__WIN32__)
    FlushFileBuffers((HANDLE)_get_osfhandle(fd));
#else
    fsync(fd);
#endif
    close(fd);
    return true;
}


/* On Mac OS 8/9, one of dirSpec and dirURL must be non-NULL.  On all other platforms, one of path and dirURL must be non-NULL
If both are present, they are assumed to be in-synch; that is, they both refer to the same directory.  */
__private_extern__ CFMutableArrayRef _CFContentsOfDirectory(CFAllocatorRef alloc, char *dirPath, void *dirSpec, CFURLRef dirURL, CFStringRef matchingAbstractType) {
    CFMutableArrayRef files = NULL;
    Boolean releaseBase = false;
    CFIndex pathLength = dirPath ? strlen(dirPath) : 0;
    // MF:!!! Need to use four-letter type codes where appropriate.
    CFStringRef extension = (matchingAbstractType ? _CFCopyExtensionForAbstractType(matchingAbstractType) : NULL);
    CFIndex extLen = (extension ? CFStringGetLength(extension) : 0);
    uint8_t extBuff[CFMaxPathSize];

#if defined(__WIN32__)
    /* Windows Variables */
    /* The Win32 code has not been updated for:
        path has been renamed dirPath
        base has been renamed dirURL
        dirPath may be NULL (in which case dirURL is not)
        if dirPath is NULL, pathLength is 0
    */
    WIN32_FIND_DATA file;
    HANDLE handle;
#elif defined(__svr4__) || defined(__hpux__) || defined(__LINUX__) || defined(__FREEBSD__)
    /* Solaris and HPUX Variables */
    /* The Solaris and HPUX code has not been updated for:
        base has been renamed dirURL
        dirPath may be NULL (in which case dirURL is not)
        if dirPath is NULL, pathLength is 0
    */
    DIR *dirp;
    struct dirent *dp;
    int err;
#elif defined(__MACH__)
    /* Mac OS X Variables */
    int fd, numread;
    long basep;
    char dirge[8192];
    uint8_t pathBuf[CFMaxPathSize];
#endif

    
    if (extLen > 0) {
        CFStringGetBytes(extension, CFRangeMake(0, extLen), CFStringFileSystemEncoding(), 0, false, extBuff, CFMaxPathSize, &extLen);
        extBuff[extLen] = '\0';
    }
    
#if defined(__WIN32__)
    /* Windows Implementation */
    
    if (pathLength + 2 >= CFMaxPathLength) {
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
    }
    if (NULL != dirPath) {
	dirPath[pathLength] = '\'';
	dirPath[pathLength + 1] = '*';
	dirPath[pathLength + 2] = '\0';
	handle = FindFirstFileA(dirPath, &file);
	if (INVALID_HANDLE_VALUE == handle) {
	    dirPath[pathLength] = '\0';
	    if (extension) {
		CFRelease(extension);
	    }
	    return NULL;
	}
    } else {
	pathLength = 0;
    }
    files = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);

    do {
        CFURLRef fileURL;
        CFIndex namelen = strlen(file.cFileName);
        if (file.cFileName[0] == '.' && (namelen == 1 || (namelen == 2  && file.cFileName[1] == '.'))) {
            continue;
        }
        if (extLen > 0) {
            // Check to see if it matches the extension we're looking for.
            if (_stricmp(&(file.cFileName[namelen - extLen]), extBuff) != 0) {
                continue;
            }
        }
	if (dirURL == NULL) {
	    dirURL = CFURLCreateFromFileSystemRepresentation(alloc, dirPath, pathLength, true);
            releaseBase = true;
        }
        // MF:!!! What about the trailing slash?
        fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, file.cFileName, namelen, false, dirURL);
        CFArrayAppendValue(files, fileURL);
        CFRelease(fileURL);
    } while (FindNextFileA(handle, &file));
    FindClose(handle);
    dirPath[pathLength] = '\0';

#elif defined(__svr4__) || defined(__hpux__) || defined(__LINUX__) || defined(__FREEBSD__)
    /* Solaris and HPUX Implementation */

    dirp = opendir(dirPath);
    if (!dirp) {
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
        // raiseErrno("opendir", path);
    }
    files = CFArrayCreateMutable(alloc, 0, & kCFTypeArrayCallBacks);

    while((dp = readdir(dirp)) != NULL) {
        CFURLRef fileURL;
	unsigned namelen = strlen(dp->d_name);

        // skip . & ..; they cause descenders to go berserk
	if (dp->d_name[0] == '.' && (namelen == 1 || (namelen == 2 && dp->d_name[1] == '.'))) {
            continue;
        }

        if (extLen > 0) {
            // Check to see if it matches the extension we're looking for.
            if (strncmp(&(dp->d_name[namelen - extLen]), extBuff, extLen) != 0) {
                continue;
            }
        }
        if (dirURL == NULL) {
            dirURL = CFURLCreateFromFileSystemRepresentation(alloc, dirPath, pathLength, true);
            releaseBase = true;
        }
        // MF:!!! What about the trailing slash?
        fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, dp->d_name, namelen, false, dirURL);
        CFArrayAppendValue(files, fileURL);
        CFRelease(fileURL);
    }
    err = closedir(dirp);
    if (err != 0) {
        CFRelease(files);
        if (releaseBase) {
            CFRelease(dirURL);
        }
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
        // raiseErrno("closedir", path);
    }
    
#elif defined(__MACH__)
    /* Mac OS X Variables - repeated for convenience */
    // int fd, numread;
    // long basep;
    // char dirge[8192];
    // UInt8 pathBuf[CFMaxPathSize];
    /* Mac OS X Implementation */

    if (!dirPath) {
        if (!CFURLGetFileSystemRepresentation(dirURL, true, pathBuf, CFMaxPathLength)) {
            if (extension) CFRelease(extension);
            return NULL;
        } else {
            dirPath = pathBuf;
            pathLength = strlen(dirPath);
        }
    }
    fd = open(dirPath, O_RDONLY, 0777);
    if (fd < 0) {
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
    }
    files = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);

    while ((numread = getdirentries(fd, dirge, sizeof(dirge), &basep)) > 0) {
        struct dirent *dent;
        for (dent = (struct dirent *)dirge; dent < (struct dirent *)(dirge + numread); dent = (struct dirent *)((char *)dent + dent->d_reclen)) {
            CFURLRef fileURL;
            CFIndex nameLen;

            nameLen = dent->d_namlen;
            // skip . & ..; they cause descenders to go berserk
            if (0 == dent->d_fileno || (dent->d_name[0] == '.' && (nameLen == 1 || (nameLen == 2 && dent->d_name[1] == '.')))) {
                continue;
            }
            if (extLen > 0) {
                // Check to see if it matches the extension we're looking for.
                if (strncmp(&(dent->d_name[nameLen - extLen]), extBuff, extLen) != 0) {
                    continue;
                }
            }
            if (dirURL == NULL) {
                dirURL = CFURLCreateFromFileSystemRepresentation(alloc, dirPath, pathLength, true);
                releaseBase = true;
            }

            if (dent->d_type == DT_DIR || dent->d_type == DT_UNKNOWN) {
                Boolean isDir = (dent->d_type == DT_DIR);
                if (!isDir) {
                    // Ugh; must stat.
                    char subdirPath[CFMaxPathLength];
                    struct stat statBuf;
                    strncpy(subdirPath, dirPath, pathLength);
                    subdirPath[pathLength] = '/';
                    strncpy(subdirPath + pathLength + 1, dent->d_name, nameLen);
                    subdirPath[pathLength + nameLen + 1] = '\0';
                    if (stat(subdirPath, &statBuf) == 0) {
                        isDir = ((statBuf.st_mode & S_IFMT) == S_IFDIR);
                    }
                }
                fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, dent->d_name, nameLen, isDir, dirURL);
            } else {
                fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase (alloc, dent->d_name, nameLen, false, dirURL);
            }
            CFArrayAppendValue(files, fileURL);
            CFRelease(fileURL);
        }
    }
    close(fd);
    if  (-1 == numread) {
        CFRelease(files);
        if (releaseBase) {
            CFRelease(dirURL);
        }
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
    }
#else
    
#error _CFContentsOfDirectory() unknown architechture, not implemented
    
#endif

    if (extension) {
        CFRelease(extension);
    }
    if (releaseBase) {
        CFRelease(dirURL);
    }
    return files;
}

__private_extern__ SInt32 _CFGetFileProperties(CFAllocatorRef alloc, CFURLRef pathURL, Boolean *exists, SInt32 *posixMode, int64_t *size, CFDateRef *modTime, SInt32 *ownerID, CFArrayRef *dirContents) {
    Boolean fileExists;
    Boolean isDirectory = false;

    struct stat statBuf;
    char path[CFMaxPathLength];

    if ((exists == NULL) && (posixMode == NULL) && (size == NULL) && (modTime == NULL) && (ownerID == NULL) && (dirContents == NULL)) {
        // Nothing to do.
        return 0;
    }

    if (!CFURLGetFileSystemRepresentation(pathURL, true, path, CFMaxPathLength)) {
        return -1;
    }

    if (stat(path, &statBuf) != 0) {
        // stat failed, but why?
        if (thread_errno() == ENOENT) {
            fileExists = false;
        } else {
            return thread_errno();
        }
    } else {
        fileExists = true;
        isDirectory = ((statBuf.st_mode & S_IFMT) == S_IFDIR);
    }


    if (exists != NULL) {
        *exists = fileExists;
    }

    if (posixMode != NULL) {
        if (fileExists) {

            *posixMode = statBuf.st_mode;

        } else {
            *posixMode = 0;
        }
    }

    if (size != NULL) {
        if (fileExists) {

            *size = statBuf.st_size;

        } else {
            *size = 0;
        }
    }

    if (modTime != NULL) {
        if (fileExists) {
            CFTimeInterval theTime;

            theTime = kCFAbsoluteTimeIntervalSince1970 + statBuf.st_mtime;

            *modTime = CFDateCreate(alloc, theTime);
        } else {
            *modTime = NULL;
        }
    }

    if (ownerID != NULL) {
        if (fileExists) {

            *ownerID = statBuf.st_uid;

        } else {
            *ownerID = -1;
        }
    }
    
    if (dirContents != NULL) {
        if (fileExists && isDirectory) {

            CFMutableArrayRef contents = _CFContentsOfDirectory(alloc, path, NULL, pathURL, NULL);

            if (contents) {
                *dirContents = contents;
            } else {
                *dirContents = NULL;
            }
        } else {
            *dirContents = NULL;
        }
    }
    return 0;
}


// MF:!!! Should pull in the rest of the UniChar based path utils from Foundation.
#if defined(__MACH__) || defined(__svr4__) || defined(__hpux__) || defined(__LINUX__) || defined(__FREEBSD__)
    #define UNIX_PATH_SEMANTICS
#elif defined(__WIN32__)
    #define WINDOWS_PATH_SEMANTICS
#else
#error Unknown platform
#endif

#if defined(WINDOWS_PATH_SEMANTICS)
    #define CFPreferredSlash	((UniChar)'\\')
#elif defined(UNIX_PATH_SEMANTICS)
    #define CFPreferredSlash	((UniChar)'/')
#elif defined(HFS_PATH_SEMANTICS)
    #define CFPreferredSlash	((UniChar)':')
#else
    #error Cannot define NSPreferredSlash on this platform
#endif

#if defined(HFS_PATH_SEMANTICS)
#define HAS_DRIVE(S) (false)
#define HAS_NET(S) (false)
#else
#define HAS_DRIVE(S) ((S)[1] == ':' && (('A' <= (S)[0] && (S)[0] <= 'Z') || ('a' <= (S)[0] && (S)[0] <= 'z')))
#define HAS_NET(S) ((S)[0] == '\\' && (S)[1] == '\\')
#endif

#if defined(WINDOWS_PATH_SEMANTICS)
    #define IS_SLASH(C)	((C) == '\\' || (C) == '/')
#elif defined(UNIX_PATH_SEMANTICS)
    #define IS_SLASH(C)	((C) == '/')
#elif defined(HFS_PATH_SEMANTICS)
    #define IS_SLASH(C)	((C) == ':')
#endif

__private_extern__ Boolean _CFIsAbsolutePath(UniChar *unichars, CFIndex length) {
    if (length < 1) {
        return false;
    }
#if defined(WINDOWS_PATH_SEMANTICS)
    if (unichars[0] == '~') {
        return true;
    }
    if (length < 2) {
        return false;
    }
    if (HAS_NET(unichars)) {
        return true;
    }
    if (length < 3) {
        return false;
    }
    if (IS_SLASH(unichars[2]) && HAS_DRIVE(unichars)) {
        return true;
    }
#elif defined(HFS_PATH_SEMANTICS)
    return !IS_SLASH(unichars[0]);
#else
    if (unichars[0] == '~') {
        return true;
    }
    if (IS_SLASH(unichars[0])) {
        return true;
    }
#endif
    return false;
}

__private_extern__ Boolean _CFStripTrailingPathSlashes(UniChar *unichars, CFIndex *length) {
    Boolean destHasDrive = (1 < *length) && HAS_DRIVE(unichars);
    CFIndex oldLength = *length;
    while (((destHasDrive && 3 < *length) || (!destHasDrive && 1 < *length)) && IS_SLASH(unichars[*length - 1])) {
        (*length)--;
    }
    return (oldLength != *length);
}

__private_extern__ Boolean _CFAppendPathComponent(UniChar *unichars, CFIndex *length, CFIndex maxLength, UniChar *component, CFIndex componentLength) {
    if (0 == componentLength) {
        return true;
    }
    if (maxLength < *length + 1 + componentLength) {
        return false;
    }
    switch (*length) {
    case 0:
        break;
    case 1:
        if (!IS_SLASH(unichars[0])) {
            unichars[(*length)++] = CFPreferredSlash;
        }
        break;
    case 2:
        if (!HAS_DRIVE(unichars) && !HAS_NET(unichars)) {
            unichars[(*length)++] = CFPreferredSlash;
        }
        break;
    default:
        unichars[(*length)++] = CFPreferredSlash;
        break;
    }
    memmove(unichars + *length, component, componentLength * sizeof(UniChar));
    *length += componentLength;
    return true;
}

__private_extern__ Boolean _CFAppendPathExtension(UniChar *unichars, CFIndex *length, CFIndex maxLength, UniChar *extension, CFIndex extensionLength) {
    if (maxLength < *length + 1 + extensionLength) {
        return false;
    }
    if ((0 < extensionLength && IS_SLASH(extension[0])) || (1 < extensionLength && HAS_DRIVE(extension))) {
        return false;
    }
    _CFStripTrailingPathSlashes(unichars, length);
    switch (*length) {
    case 0:
        return false;
    case 1:
        if (IS_SLASH(unichars[0]) || unichars[0] == '~') {
            return false;
        }
        break;
    case 2:
        if (HAS_DRIVE(unichars) || HAS_NET(unichars)) {
            return false;
        }
        break;
    case 3:
        if (IS_SLASH(unichars[2]) && HAS_DRIVE(unichars)) {
            return false;
        }
        break;
    }
    if (0 < *length && unichars[0] == '~') {
        CFIndex idx;
        Boolean hasSlash = false;
        for (idx = 1; idx < *length; idx++) {
            if (IS_SLASH(unichars[idx])) {
                hasSlash = true;
                break;
            }
        }
        if (!hasSlash) {
            return false;
        }
    }
    unichars[(*length)++] = '.';
    memmove(unichars + *length, extension, extensionLength * sizeof(UniChar));
    *length += extensionLength;
    return true;
}

__private_extern__ Boolean _CFTransmutePathSlashes(UniChar *unichars, CFIndex *length, UniChar replSlash) {
    CFIndex didx, sidx, scnt = *length;
    sidx = (1 < *length && HAS_NET(unichars)) ? 2 : 0;
    didx = sidx;
    while (sidx < scnt) {
        if (IS_SLASH(unichars[sidx])) {
            unichars[didx++] = replSlash;
            for (sidx++; sidx < scnt && IS_SLASH(unichars[sidx]); sidx++);
        } else {
            unichars[didx++] = unichars[sidx++];
        }
    }
    *length = didx;
    return (scnt != didx);
}

__private_extern__ CFIndex _CFStartOfLastPathComponent(UniChar *unichars, CFIndex length) {
    CFIndex idx;
    if (length < 2) {
        return 0;
    }
    for (idx = length - 1; idx; idx--) {
        if (IS_SLASH(unichars[idx - 1])) {
            return idx;
        }
    }
    if ((2 < length) && HAS_DRIVE(unichars)) {
        return 2;
    }
    return 0;
}

__private_extern__ CFIndex _CFLengthAfterDeletingLastPathComponent(UniChar *unichars, CFIndex length) {
    CFIndex idx;
    if (length < 2) {
        return 0;
    }
    for (idx = length - 1; idx; idx--) {
        if (IS_SLASH(unichars[idx - 1])) {
            if ((idx != 1) && (!HAS_DRIVE(unichars) || idx != 3)) {
                return idx - 1;
            }
            return idx;
        }
    }
    if ((2 < length) && HAS_DRIVE(unichars)) {
        return 2;
    }
    return 0;
}

__private_extern__ CFIndex _CFStartOfPathExtension(UniChar *unichars, CFIndex length) {
    CFIndex idx;
    if (length < 2) {
        return 0;
    }
    for (idx = length - 1; idx; idx--) {
        if (IS_SLASH(unichars[idx - 1])) {
            return 0;
        }
        if (unichars[idx] != '.') {
            continue;
        }
        if (idx == 2 && HAS_DRIVE(unichars)) {
            return 0;
        }
        return idx;
    }
    return 0;
}

__private_extern__ CFIndex _CFLengthAfterDeletingPathExtension(UniChar *unichars, CFIndex length) {
    CFIndex start = _CFStartOfPathExtension(unichars, length);
    return ((0 < start) ? start : length);
}

#undef CF_OPENFLGS
#undef UNIX_PATH_SEMANTICS
#undef WINDOWS_PATH_SEMANTICS
#undef HFS_PATH_SEMANTICS
#undef CFPreferredSlash
#undef HAS_DRIVE
#undef HAS_NET
#undef IS_SLASH

