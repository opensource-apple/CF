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

/*	CFFileUtilities.c
	Copyright (c) 1999-2012, Apple Inc. All rights reserved.
	Responsibility: Tony Parker
*/

#include "CFInternal.h"
#include <CoreFoundation/CFPriv.h>

#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#if DEPLOYMENT_TARGET_WINDOWS
#include <io.h>
#include <fcntl.h>

#define close _close
#define write _write
#define read _read
#define open _NS_open
#define stat _NS_stat
#define fstat _fstat
#define mkdir(a,b) _NS_mkdir(a)
#define rmdir _NS_rmdir
#define unlink _NS_unlink

#define statinfo _stat

#else

#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <pwd.h>
#include <fcntl.h>

#define statinfo stat

#endif

CF_INLINE int openAutoFSNoWait() {
#if DEPLOYMENT_TARGET_WINDOWS
    return -1;
#else
    return (__CFProphylacticAutofsAccess ? open("/dev/autofs_nowait", 0) : -1);
#endif
}

CF_INLINE void closeAutoFSNoWait(int fd) {
    if (-1 != fd) close(fd);
}

__private_extern__ CFStringRef _CFCopyExtensionForAbstractType(CFStringRef abstractType) {
    return (abstractType ? (CFStringRef)CFRetain(abstractType) : NULL);
}


__private_extern__ Boolean _CFCreateDirectory(const char *path) {
    int no_hang_fd = openAutoFSNoWait();
    int ret = ((mkdir(path, 0777) == 0) ? true : false);
    closeAutoFSNoWait(no_hang_fd);
    return ret;
}

__private_extern__ Boolean _CFRemoveDirectory(const char *path) {
    int no_hang_fd = openAutoFSNoWait();
    int ret = ((rmdir(path) == 0) ? true : false);
    closeAutoFSNoWait(no_hang_fd);
    return ret;
}

__private_extern__ Boolean _CFDeleteFile(const char *path) {
    int no_hang_fd = openAutoFSNoWait();
    int ret = unlink(path) == 0;
    closeAutoFSNoWait(no_hang_fd);
    return ret;
}

__private_extern__ Boolean _CFReadBytesFromPathAndGetFD(CFAllocatorRef alloc, const char *path, void **bytes, CFIndex *length, CFIndex maxLength, int extraOpenFlags, int *fd) {    // maxLength is the number of bytes desired, or 0 if the whole file is desired regardless of length.
    struct statinfo statBuf;
    
    *bytes = NULL;
    
    
    int no_hang_fd = openAutoFSNoWait();
    *fd = open(path, O_RDONLY|extraOpenFlags|CF_OPENFLGS, 0666);
    
    if (*fd < 0) {
        closeAutoFSNoWait(no_hang_fd);
        return false;
    }
    if (fstat(*fd, &statBuf) < 0) {
        int saveerr = thread_errno();
        close(*fd);
        *fd = -1;
        closeAutoFSNoWait(no_hang_fd);
        thread_set_errno(saveerr);
        return false;
    }
    if ((statBuf.st_mode & S_IFMT) != S_IFREG) {
        close(*fd);
        *fd = -1;
        closeAutoFSNoWait(no_hang_fd);
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
        if (read(*fd, *bytes, desiredLength) < 0) {
            CFAllocatorDeallocate(alloc, *bytes);
            close(*fd);
            *fd = -1;
            closeAutoFSNoWait(no_hang_fd);
            return false;
        }
        *length = desiredLength;
    }
    closeAutoFSNoWait(no_hang_fd);
    return true;
}

__private_extern__ Boolean _CFReadBytesFromPath(CFAllocatorRef alloc, const char *path, void **bytes, CFIndex *length, CFIndex maxLength, int extraOpenFlags) {
    int fd = -1;
    Boolean result = _CFReadBytesFromPathAndGetFD(alloc, path, bytes, length, maxLength, extraOpenFlags, &fd);
    if (fd >= 0) {
        close(fd);
    }
    return result;
}
__private_extern__ Boolean _CFReadBytesFromFile(CFAllocatorRef alloc, CFURLRef url, void **bytes, CFIndex *length, CFIndex maxLength, int extraOpenFlags) {
    // maxLength is the number of bytes desired, or 0 if the whole file is desired regardless of length.
    
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathSize)) {
        return false;
    }
    return _CFReadBytesFromPath(alloc, (const char *)path, bytes, length, maxLength, extraOpenFlags);
}

__private_extern__ Boolean _CFWriteBytesToFile(CFURLRef url, const void *bytes, CFIndex length) {
    int fd = -1;
    int mode;
    struct statinfo statBuf;
    char path[CFMaxPathSize];
    if (!CFURLGetFileSystemRepresentation(url, true, (uint8_t *)path, CFMaxPathSize)) {
        return false;
    }

    int no_hang_fd = openAutoFSNoWait();
    mode = 0666;
    if (0 == stat(path, &statBuf)) {
        mode = statBuf.st_mode;
    } else if (thread_errno() != ENOENT) {
        closeAutoFSNoWait(no_hang_fd);
        return false;
    }
    fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|CF_OPENFLGS, 0666);
    if (fd < 0) {
        closeAutoFSNoWait(no_hang_fd);
        return false;
    }
    if (length && write(fd, bytes, length) != length) {
        int saveerr = thread_errno();
        close(fd);
        closeAutoFSNoWait(no_hang_fd);
        thread_set_errno(saveerr);
        return false;
    }
#if DEPLOYMENT_TARGET_WINDOWS
    FlushFileBuffers((HANDLE)_get_osfhandle(fd));
#else
    fsync(fd);
#endif
    close(fd);
    closeAutoFSNoWait(no_hang_fd);
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
    CFIndex targetExtLen = (extension ? CFStringGetLength(extension) : 0);

#if DEPLOYMENT_TARGET_WINDOWS
    // This is a replacement for 'dirent' below, and also uses wchar_t to support unicode paths
    wchar_t extBuff[CFMaxPathSize];
    int extBuffInteriorDotCount = 0; //people insist on using extensions like ".trace.plist", so we need to know how many dots back to look :(
    
    if (targetExtLen > 0) {
        CFIndex usedBytes = 0;
        CFStringGetBytes(extension, CFRangeMake(0, targetExtLen), kCFStringEncodingUTF16, 0, false, (uint8_t *)extBuff, CFMaxPathLength, &usedBytes);
        targetExtLen = usedBytes / sizeof(wchar_t);
        extBuff[targetExtLen] = '\0';
        wchar_t *extBuffStr = (wchar_t *)extBuff;
        if (extBuffStr[0] == '.')
            extBuffStr++; //skip the first dot, it's legitimate to have ".plist" for example
        
        wchar_t *extBuffDotPtr = extBuffStr;
        while ((extBuffDotPtr = wcschr(extBuffStr, '.'))) { //find the next . in the extension...
            extBuffInteriorDotCount++;
            extBuffStr = extBuffDotPtr + 1;
        }
    }
    
    wchar_t pathBuf[CFMaxPathSize];
    
    if (!dirPath) {
        if (!_CFURLGetWideFileSystemRepresentation(dirURL, true, pathBuf, CFMaxPathLength)) {
            if (extension) CFRelease(extension);
            return NULL;
        }
        
        pathLength = wcslen(pathBuf);

    } else {
        // Convert dirPath to a wide representation and put it into our pathBuf
        // Get the real length of the string in UTF16 characters
        CFStringRef dirPathStr = CFStringCreateWithCString(kCFAllocatorSystemDefault, dirPath, kCFStringEncodingUTF8);
        CFIndex strLen = CFStringGetLength(dirPathStr);
        
        // Copy the string into the buffer and terminate
        CFStringGetCharacters(dirPathStr, CFRangeMake(0, strLen), (UniChar *)pathBuf);
        pathBuf[strLen] = 0;
        
        CFRelease(dirPathStr);
    }
    
    WIN32_FIND_DATAW  file;
    HANDLE handle;
    
    if (pathLength + 2 >= CFMaxPathLength) {
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
    }

    pathBuf[pathLength] = '\\';
    pathBuf[pathLength + 1] = '*';
    pathBuf[pathLength + 2] = '\0';
    handle = FindFirstFileW(pathBuf, (LPWIN32_FIND_DATAW)&file);
    if (INVALID_HANDLE_VALUE == handle) {
        pathBuf[pathLength] = '\0';
        if (extension) {
            CFRelease(extension);
        }
        return NULL;
    }

    files = CFArrayCreateMutable(alloc, 0, &kCFTypeArrayCallBacks);

    do {
        CFURLRef fileURL;
        CFIndex namelen = wcslen(file.cFileName);
        if (file.cFileName[0] == '.' && (namelen == 1 || (namelen == 2  && file.cFileName[1] == '.'))) {
            continue;
        }

        if (targetExtLen > namelen) continue;    // if the extension is the same length or longer than the name, it can't possibly match.

        if (targetExtLen > 0) {
	    if (file.cFileName[namelen - 1] == '.') continue; //filename ends with a dot, no extension
	    
	    wchar_t *fileExt = NULL;
            
            if (extBuffInteriorDotCount == 0) {
                fileExt = wcsrchr(file.cFileName, '.');
            } else { //find the Nth occurrence of . from the end of the string, to handle ".foo.bar"
                wchar_t *save = file.cFileName;
                while ((save = wcschr(save, '.')) && !fileExt) {
                    wchar_t *temp = save;
                    int moreDots = 0;
                    while ((temp = wcschr(temp, '.'))) {
                        if (++moreDots == extBuffInteriorDotCount) break;
                    }
                    if (moreDots == extBuffInteriorDotCount) {
                        fileExt = save;
                    }
                }
            }
	    
	    if (!fileExt) continue; //no extension
	    
	    if (((const wchar_t *)extBuff)[0] != '.')
		fileExt++; //omit the dot if the target file extension omits the dot
	    
	    CFIndex fileExtLen = wcslen(fileExt);
	    
	    //if the extensions are different lengths, they can't possibly match
	    if (fileExtLen != targetExtLen) continue;
	    
            // Check to see if it matches the extension we're looking for.
            if (_wcsicmp(fileExt, (const wchar_t *)extBuff) != 0) {
                continue;
            }
        }
	if (dirURL == NULL) {
	    CFStringRef dirURLStr = CFStringCreateWithBytes(alloc, (const uint8_t *)pathBuf, pathLength * sizeof(wchar_t), kCFStringEncodingUTF16, NO);
	    dirURL = CFURLCreateWithFileSystemPath(alloc, dirURLStr, kCFURLWindowsPathStyle, true);
	    CFRelease(dirURLStr);
            releaseBase = true;
        }
        // MF:!!! What about the trailing slash?
        CFStringRef fileURLStr = CFStringCreateWithBytes(alloc, (const uint8_t *)file.cFileName, namelen * sizeof(wchar_t), kCFStringEncodingUTF16, NO);
        fileURL = CFURLCreateWithFileSystemPathRelativeToBase(alloc, fileURLStr, kCFURLWindowsPathStyle, (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? true : false, dirURL);
        CFArrayAppendValue(files, fileURL);
        CFRelease(fileURL);
        CFRelease(fileURLStr);
    } while (FindNextFileW(handle, &file));
    FindClose(handle);
    pathBuf[pathLength] = '\0';

#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI || DEPLOYMENT_TARGET_LINUX || DEPLOYMENT_TARGET_FREEBSD
    uint8_t extBuff[CFMaxPathSize];
    int extBuffInteriorDotCount = 0; //people insist on using extensions like ".trace.plist", so we need to know how many dots back to look :(
    
    if (targetExtLen > 0) {
        CFStringGetBytes(extension, CFRangeMake(0, targetExtLen), CFStringFileSystemEncoding(), 0, false, extBuff, CFMaxPathLength, &targetExtLen);
        extBuff[targetExtLen] = '\0';
        char *extBuffStr = (char *)extBuff;
        if (extBuffStr[0] == '.')
            extBuffStr++; //skip the first dot, it's legitimate to have ".plist" for example
        
        char *extBuffDotPtr = extBuffStr;
        while ((extBuffDotPtr = strchr(extBuffStr, '.'))) { //find the next . in the extension...
            extBuffInteriorDotCount++;
            extBuffStr = extBuffDotPtr + 1;
        }
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
    
    struct dirent buffer;
    struct dirent *dp;
    int err;
   
    int no_hang_fd = __CFProphylacticAutofsAccess ? open("/dev/autofs_nowait", 0) : -1;
 
    DIR *dirp = opendir(dirPath);
    if (!dirp) {
        if (extension) {
            CFRelease(extension);
        }
	if (-1 != no_hang_fd) close(no_hang_fd);
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
        
        if (targetExtLen > namelen) continue;    // if the extension is the same length or longer than the name, it can't possibly match.
                
        if (targetExtLen > 0) {
	    if (dp->d_name[namelen - 1] == '.') continue; //filename ends with a dot, no extension
	      
            char *fileExt = NULL;
            if (extBuffInteriorDotCount == 0) {
                fileExt = strrchr(dp->d_name, '.'); 
            } else { //find the Nth occurrence of . from the end of the string, to handle ".foo.bar"
                char *save = dp->d_name;
                while ((save = strchr(save, '.')) && !fileExt) {
                    char *temp = save;
                    int moreDots = 0;
                    while ((temp = strchr(temp, '.'))) {
                        if (++moreDots == extBuffInteriorDotCount) break;
                    }
                    if (moreDots == extBuffInteriorDotCount) {
                        fileExt = save;
                    }
                }
            }
	    
	    if (!fileExt) continue; //no extension
	    
	    if (((char *)extBuff)[0] != '.')
		fileExt++; //omit the dot if the target extension omits the dot; safe, because we checked to make sure it isn't the last character just before
	    
	    size_t fileExtLen = strlen(fileExt);
	    
	    //if the extensions are different lengths, they can't possibly match
	    if (fileExtLen != targetExtLen) continue;
	    
	    // Check to see if it matches the extension we're looking for.
            if (strncmp(fileExt, (char *)extBuff, fileExtLen) != 0) {
                continue;
            }
        }
        if (dirURL == NULL) {
            dirURL = CFURLCreateFromFileSystemRepresentation(alloc, (uint8_t *)dirPath, pathLength, true);
            releaseBase = true;
        }
        if (dp->d_type == DT_DIR || dp->d_type == DT_UNKNOWN || dp->d_type == DT_LNK || dp->d_type == DT_WHT) {
            Boolean isDir = (dp->d_type == DT_DIR);
            if (!isDir) {
                // Ugh; must stat.
                char subdirPath[CFMaxPathLength];
                struct statinfo statBuf;
                strlcpy(subdirPath, dirPath, sizeof(subdirPath));
                strlcat(subdirPath, "/", sizeof(subdirPath));
                strlcat(subdirPath, dp->d_name, sizeof(subdirPath));
                if (stat(subdirPath, &statBuf) == 0) {
                    isDir = ((statBuf.st_mode & S_IFMT) == S_IFDIR);
                }
            }
#if DEPLOYMENT_TARGET_LINUX
            fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, (uint8_t *)dp->d_name, namelen, isDir, dirURL);
#else
            fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, (uint8_t *)dp->d_name, dp->d_namlen, isDir, dirURL);
#endif
        } else {
#if DEPLOYMENT_TARGET_LINUX
            fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase (alloc, (uint8_t *)dp->d_name, namelen, false, dirURL);
#else
            fileURL = CFURLCreateFromFileSystemRepresentationRelativeToBase (alloc, (uint8_t *)dp->d_name, dp->d_namlen, false, dirURL);
#endif
        }
        CFArrayAppendValue(files, fileURL);
        CFRelease(fileURL);
    }
    err = closedir(dirp);
    if (-1 != no_hang_fd) close(no_hang_fd);
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
    
#error _CFContentsOfDirectory() unknown architecture, not implemented
    
#endif

    if (extension) {
        CFRelease(extension);
    }
    if (releaseBase) {
        CFRelease(dirURL);
    }
    return files;
}

__private_extern__ SInt32 _CFGetPathProperties(CFAllocatorRef alloc, char *path, Boolean *exists, SInt32 *posixMode, int64_t *size, CFDateRef *modTime, SInt32 *ownerID, CFArrayRef *dirContents) {
    Boolean fileExists;
    Boolean isDirectory = false;
    
    if ((exists == NULL) && (posixMode == NULL) && (size == NULL) && (modTime == NULL) && (ownerID == NULL) && (dirContents == NULL)) {
        // Nothing to do.
        return 0;
    }
    
    struct statinfo statBuf;
    
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
#if DEPLOYMENT_TARGET_WINDOWS || DEPLOYMENT_TARGET_LINUX
            struct timespec ts = {statBuf.st_mtime, 0};
#else
            struct timespec ts = statBuf.st_mtimespec;
#endif
            *modTime = CFDateCreate(alloc, _CFAbsoluteTimeFromFileTimeSpec(ts));
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
            
            CFMutableArrayRef contents = _CFContentsOfDirectory(alloc, (char *)path, NULL, NULL, NULL);
            
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

__private_extern__ SInt32 _CFGetFileProperties(CFAllocatorRef alloc, CFURLRef pathURL, Boolean *exists, SInt32 *posixMode, int64_t *size, CFDateRef *modTime, SInt32 *ownerID, CFArrayRef *dirContents) {
    
    char path[CFMaxPathSize];

    if (!CFURLGetFileSystemRepresentation(pathURL, true, (uint8_t *)path, CFMaxPathLength)) {
        return -1;
    }

    return _CFGetPathProperties(alloc, path, exists, posixMode, size, modTime, ownerID, dirContents);
}


#if DEPLOYMENT_TARGET_WINDOWS
#define WINDOWS_PATH_SEMANTICS
#else
#define UNIX_PATH_SEMANTICS
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

__private_extern__ UniChar _CFGetSlash(){
    return CFPreferredSlash;
}

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

__private_extern__ Boolean _CFAppendTrailingPathSlash(UniChar *unichars, CFIndex *length, CFIndex maxLength) {
    if (maxLength < *length + 1) {
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
    return true;
}

__private_extern__ Boolean _CFAppendPathComponent(UniChar *unichars, CFIndex *length, CFIndex maxLength, UniChar *component, CFIndex componentLength) {
    if (0 == componentLength) {
        return true;
    }
    if (maxLength < *length + 1 + componentLength) {
        return false;
    }
    _CFAppendTrailingPathSlash(unichars, length, maxLength);
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


