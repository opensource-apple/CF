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
/*	CFFileUtilities.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Christopher Kane
*/

#include "CFInternal.h"
#include "CFPriv.h"
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

#define CF_OPENFLGS	(0)


__private_extern__ CFStringRef _CFCopyExtensionForAbstractType(CFStringRef abstractType) {
    return (abstractType ? (CFStringRef)CFRetain(abstractType) : NULL);
}


__private_extern__ Boolean _CFCreateDirectory(const char *path) {
#if 0 || 0
    return CreateDirectoryA(path, (LPSECURITY_ATTRIBUTES)NULL);
#else
    int no_hang_fd = open("/dev/autofs_nowait", 0);
    int ret = ((mkdir(path, 0777) == 0) ? true : false);
    close(no_hang_fd);
    return ret;
#endif
}

__private_extern__ Boolean _CFRemoveDirectory(const char *path) {
#if 0 || 0
    return RemoveDirectoryA(path);
#else
    int no_hang_fd = open("/dev/autofs_nowait", 0);
    int ret = ((rmdir(path) == 0) ? true : false);
    close(no_hang_fd);
    return ret;
#endif
}

__private_extern__ Boolean _CFDeleteFile(const char *path) {
#if 0 || 0
    return DeleteFileA(path);
#else
    int no_hang_fd = open("/dev/autofs_nowait", 0);
    int ret = unlink(path) == 0;
    close(no_hang_fd);
    return ret;
#endif
}

__private_extern__ Boolean _CFReadBytesFromFile(CFAllocatorRef alloc, CFURLRef url, void **bytes, CFIndex *length, CFIndex maxLength) {
    // maxLength is the number of bytes desired, or 0 if the whole file is desired regardless of length.
    struct stat statBuf;
    int fd = -1;
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathSize)) {
        return false;
    }

    *bytes = NULL;

    
#if 0 || 0
    fd = open(path, O_RDONLY|CF_OPENFLGS, 0666|_S_IREAD);
#else
    int no_hang_fd = open("/dev/autofs_nowait", 0);
    fd = open(path, O_RDONLY|CF_OPENFLGS, 0666);
#endif
    if (fd < 0) {
        close(no_hang_fd);
        return false;
    }
    if (fstat(fd, &statBuf) < 0) {
        int saveerr = thread_errno();
        close(fd);
        close(no_hang_fd);
        thread_set_errno(saveerr);
        return false;
    }
    if ((statBuf.st_mode & S_IFMT) != S_IFREG) {
        close(fd);
        close(no_hang_fd);
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
//	fcntl(fd, F_NOCACHE, 1);
        if (read(fd, *bytes, desiredLength) < 0) {
            CFAllocatorDeallocate(alloc, *bytes);
            close(fd);
	    close(no_hang_fd);
            return false;
        }
        *length = desiredLength;
    }
    close(fd);
    close(no_hang_fd);
    return true;
}

__private_extern__ Boolean _CFWriteBytesToFile(CFURLRef url, const void *bytes, CFIndex length) {
    struct stat statBuf;
    int fd = -1;
    int mode;
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathSize)) {
        return false;
    }

#if 0 || 0
    mode = 0666;
    if (0 == stat(path, &statBuf)) {
        mode = statBuf.st_mode;
    } else if (thread_errno() != ENOENT) {
        return false;
    }
    fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|CF_OPENFLGS, 0666|_S_IWRITE);
    if (fd < 0) {
        return false;
    }
    if (length && write(fd, bytes, length) != length) {
        int saveerr = thread_errno();
        close(fd);
        thread_set_errno(saveerr);
        return false;
    }
    FlushFileBuffers((HANDLE)_get_osfhandle(fd));
    close(fd);
#else
    int no_hang_fd = open("/dev/autofs_nowait", 0);
    mode = 0666;
    if (0 == stat(path, &statBuf)) {
        mode = statBuf.st_mode;
    } else if (thread_errno() != ENOENT) {
	close(no_hang_fd);
        return false;
    }
    fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|CF_OPENFLGS, 0666);
    if (fd < 0) {
	close(no_hang_fd);
        return false;
    }
    if (length && write(fd, bytes, length) != length) {
        int saveerr = thread_errno();
        close(fd);
	close(no_hang_fd);
        thread_set_errno(saveerr);
        return false;
    }
    fsync(fd);
    close(fd);
    close(no_hang_fd);
#endif
    return true;
}


/* On Mac OS 8/9, one of dirSpec and dirURL must be non-NULL.  On all other platforms, one of path and dirURL must be non-NULL
If both are present, they are assumed to be in-synch; that is, they both refer to the same directory.  */
/* Lately, dirSpec appears to be (rightfully) unused. */
__private_extern__ CFMutableArrayRef _CFContentsOfDirectory(CFAllocatorRef alloc, char *dirPath, void *dirSpec, CFURLRef dirURL, CFStringRef matchingAbstractType) {
    CFMutableArrayRef files = NULL;
    Boolean releaseBase = false;
    CFIndex pathLength = dirPath ? strlen(dirPath) : 0;
    // MF:!!! Need to use four-letter type codes where appropriate.
    CFStringRef extension = (matchingAbstractType ? _CFCopyExtensionForAbstractType(matchingAbstractType) : NULL);
    CFIndex extLen = (extension ? CFStringGetLength(extension) : 0);
    uint8_t extBuff[CFMaxPathSize];
    
    if (extLen > 0) {
        CFStringGetBytes(extension, CFRangeMake(0, extLen), CFStringFileSystemEncoding(), 0, false, extBuff, CFMaxPathLength, &extLen);
        extBuff[extLen] = '\0';
    }

    uint8_t pathBuf[CFMaxPathSize];

    if (!dirPath) {
        if (!CFURLGetFileSystemRepresentation(dirURL, true, pathBuf, CFMaxPathLength)) {
            if (extension) CFRelease(extension);
            return NULL;
        } else {
            dirPath = (char *)pathBuf;
            pathLength = strlen(dirPath);
        }
    }
    
#if (DEPLOYMENT_TARGET_MACOSX) || defined(__svr4__) || defined(__hpux__) || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
    struct dirent buffer;
    struct dirent *dp;
    int err;
   
    int no_hang_fd = open("/dev/autofs_nowait", 0);
 
    DIR *dirp = opendir(dirPath);
    if (!dirp) {
        if (extension) {
            CFRelease(extension);
        }
	close(no_hang_fd);
        return NULL;
        // raiseErrno("opendir", path);
    }
    files = CFArrayCreateMutable(alloc, 0, & kCFTypeArrayCallBacks);

    while((0 == readdir_r(dirp, &buffer, &dp)) && dp) {
        CFURLRef fileURL;
	unsigned namelen = strlen(dp->d_name);

        // skip . & ..; they cause descenders to go berserk
	if (dp->d_name[0] == '.' && (namelen == 1 || (namelen == 2 && dp->d_name[1] == '.'))) {
            continue;
        }
        
        if (extLen > namelen) continue;    // if the extension is the same length or longer than the name, it can't possibly match.
        
        if (extLen > 0) {
            // Check to see if it matches the extension we're looking for.
            if (strncmp(&(dp->d_name[namelen - extLen]), (char *)extBuff, extLen) != 0) {
                continue;
            }
        }
        if (dirURL == NULL) {
            dirURL = CFURLCreateFromFileSystemRepresentation(alloc, (uint8_t *)dirPath, pathLength, true);
            releaseBase = true;
        }
        if (dp->d_type == DT_DIR || dp->d_type == DT_UNKNOWN) {
            Boolean isDir = (dp->d_type == DT_DIR);
            if (!isDir) {
                // Ugh; must stat.
                char subdirPath[CFMaxPathLength];
                struct stat statBuf;
                strlcpy(subdirPath, dirPath, sizeof(subdirPath));
                strlcat(subdirPath, "/", sizeof(subdirPath));
                strlcat(subdirPath, dp->d_name, sizeof(subdirPath));
                if (stat(subdirPath, &statBuf) == 0) {
                    isDir = ((statBuf.st_mode & S_IFMT) == S_IFDIR);
                }
            }
            fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, (uint8_t *)dp->d_name, dp->d_namlen, isDir, dirURL);
        } else {
            fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase (alloc, (uint8_t *)dp->d_name, dp->d_namlen, false, dirURL);
        }
        CFArrayAppendValue(files, fileURL);
        CFRelease(fileURL);
    }
    err = closedir(dirp);
    close(no_hang_fd);
    if (err != 0) {
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

    struct stat64 statBuf;
    char path[CFMaxPathSize];

    if ((exists == NULL) && (posixMode == NULL) && (size == NULL) && (modTime == NULL) && (ownerID == NULL) && (dirContents == NULL)) {
        // Nothing to do.
        return 0;
    }

    if (!CFURLGetFileSystemRepresentation(pathURL, true, (uint8_t *)path, CFMaxPathLength)) {
        return -1;
    }

    if (stat64(path, &statBuf) != 0) {
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
            CFAbsoluteTime theTime = (CFAbsoluteTime)statBuf.st_mtimespec.tv_sec - kCFAbsoluteTimeIntervalSince1970;
	    theTime += (CFAbsoluteTime)statBuf.st_mtimespec.tv_nsec / 1000000000.0;
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
#if (DEPLOYMENT_TARGET_MACOSX) || defined(__svr4__) || defined(__hpux__) || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
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

