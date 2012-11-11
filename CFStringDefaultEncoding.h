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
/*	CFStringDefaultEncoding.h
	Copyright (c) 1998-2007, Apple Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFSTRINGDEFAULTENCODING__)
#define __COREFOUNDATION_CFSTRINGDEFAULTENCODING__ 1

/* This file defines static inline functions used both by CarbonCore & CF. */

#include <CoreFoundation/CFBase.h>

#if defined(__MACH__)
#include <stdlib.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/param.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <xlocale.h>

CF_EXTERN_C_BEGIN

#define __kCFUserEncodingEnvVariableName ("__CF_USER_TEXT_ENCODING")
#define __kCFMaxDefaultEncodingFileLength (24)
#define __kCFUserEncodingFileName ("/.CFUserTextEncoding")

/* This function is used to obtain users' default script/region code.
   The function first looks at environment variable __kCFUserEncodingEnvVariableName, then, reads the configuration file in user's home directory.
*/
CF_INLINE void __CFStringGetUserDefaultEncoding(UInt32 *oScriptValue, UInt32 *oRegionValue) {
    char *stringValue;
    char buffer[__kCFMaxDefaultEncodingFileLength];
    int uid = getuid();

    if ((stringValue = getenv(__kCFUserEncodingEnvVariableName)) != NULL) {
        if ((uid == strtol_l(stringValue, &stringValue, 0, NULL)) && (':' == *stringValue)) {
            ++stringValue;
        } else {
            stringValue = NULL;
        }
    }

    if ((stringValue == NULL) && ((uid > 0) || getenv("HOME"))) {
        struct passwd *passwdp;

        if ((passwdp = getpwuid((uid_t)uid))) {
            char filename[MAXPATHLEN + 1];

	    const char *path = NULL;
	    if (!issetugid()) {
		path = getenv("CFFIXED_USER_HOME");
	    }
	    if (!path) {
		path = passwdp->pw_dir;
	    }

            strlcpy(filename, path, sizeof(filename));
            strlcat(filename, __kCFUserEncodingFileName, sizeof(filename));

	    int no_hang_fd = open("/dev/autofs_nowait", 0);
            int fd = open(filename, O_RDONLY, 0);
            if (fd == -1) {
                // Cannot open the file. Let's fallback to smRoman/verUS
                snprintf(filename, sizeof(filename), "0x%X:0:0", uid);
                setenv(__kCFUserEncodingEnvVariableName, filename, 1);
            } else {
                int readSize;

		// cjk: We do not turn on F_NOCACHE on the fd here, because
		// many processes read this file on startup, and caching the
		// is probably a good thing, for the system as a whole.
                readSize = read(fd, buffer, __kCFMaxDefaultEncodingFileLength - 1);
                buffer[(readSize < 0 ? 0 : readSize)] = '\0';
                close(fd);
                stringValue = buffer;

                // Well, we already have a buffer, let's reuse it
                snprintf(filename, sizeof(filename), "0x%X:%s", uid, buffer);
                setenv(__kCFUserEncodingEnvVariableName, filename, 1);
            }
	    close(no_hang_fd);
        }
    }

    if (stringValue) {
        *oScriptValue = strtol_l(stringValue, &stringValue, 0, NULL);
        if (*stringValue == ':') {
            if (oRegionValue) *oRegionValue = strtol_l(++stringValue, NULL, 0, NULL);
            return;
        }
    }

    // Falling back
    *oScriptValue = 0; // smRoman
    if (oRegionValue) *oRegionValue = 0; // verUS
}

CF_INLINE uint32_t __CFStringGetInstallationRegion() {
    char *stringValue = NULL;
    char buffer[__kCFMaxDefaultEncodingFileLength];
    struct passwd *passwdp;
    
    if ((passwdp = getpwuid((uid_t)0))) {
        char filename[MAXPATHLEN + 1];
        
	const char *path = NULL;
	if (!issetugid()) {
	    path = getenv("CFFIXED_USER_HOME");
	}
	if (!path) {
	    path = passwdp->pw_dir;
	}

        strlcpy(filename, path, sizeof(filename));
        strlcat(filename, __kCFUserEncodingFileName, sizeof(filename));
        
        int no_hang_fd = open("/dev/autofs_nowait", 0);
	int fd = open(filename, O_RDONLY, 0);
	if (fd == -1) {
            int readSize;
            
            // cjk: We do not turn on F_NOCACHE on the fd here, because
            // many processes read this file on startup, and caching the
            // is probably a good thing, for the system as a whole.
            readSize = read(fd, buffer, __kCFMaxDefaultEncodingFileLength - 1);
            buffer[(readSize < 0 ? 0 : readSize)] = '\0';
            close(fd);
            stringValue = buffer;
        }
	close(no_hang_fd);
    }
    
    if (stringValue) {
        (void)strtol_l(stringValue, &stringValue, 0, NULL);
        if (*stringValue == ':') return strtol_l(++stringValue, NULL, 0, NULL);
    }

    return 0; // verUS
}

CF_INLINE void __CFStringGetInstallationEncodingAndRegion(uint32_t *encoding, uint32_t *region) {
    char *stringValue = NULL;
    char buffer[__kCFMaxDefaultEncodingFileLength];
    struct passwd *passwdp;

    *encoding = 0; *region = 0;

    if ((passwdp = getpwuid((uid_t)0))) {
        char filename[MAXPATHLEN + 1];

	const char *path = NULL;
	if (!issetugid()) {
	    path = getenv("CFFIXED_USER_HOME");
	}
	if (!path) {
	    path = passwdp->pw_dir;
	}

        strlcpy(filename, path, sizeof(filename));
        strlcat(filename, __kCFUserEncodingFileName, sizeof(filename));
        
	int no_hang_fd = open("/dev/autofs_nowait", 0);
	int fd = open(filename, O_RDONLY, 0);
	if (fd == -1) {
            int readSize;
            
            readSize = read(fd, buffer, __kCFMaxDefaultEncodingFileLength - 1);
            buffer[(readSize < 0 ? 0 : readSize)] = '\0';
            close(fd);
            stringValue = buffer;
        }
	close(no_hang_fd);
    }
    
    if (stringValue) {
        *encoding = strtol_l(stringValue, &stringValue, 0, NULL);
        if (*stringValue == ':') *region = strtol_l(++stringValue, NULL, 0, NULL);
    }
}

CF_INLINE void __CFStringSaveUserDefaultEncoding(UInt32 iScriptValue, UInt32 iRegionValue) {
    struct passwd *passwdp;

    if ((passwdp = getpwuid(getuid()))) {
        char filename[MAXPATHLEN + 1];

	const char *path = NULL;
	if (!issetugid()) {
	    path = getenv("CFFIXED_USER_HOME");
	}
	if (!path) {
	    path = passwdp->pw_dir;
	}

        strlcpy(filename, path, sizeof(filename));
        strlcat(filename, __kCFUserEncodingFileName, sizeof(filename));

	int no_hang_fd = open("/dev/autofs_nowait", 0);
        (void)unlink(filename);		// In case, file exists
	int fd = open(filename, O_WRONLY|O_CREAT, 0400);
	if (fd == -1) {
            char buffer[__kCFMaxDefaultEncodingFileLength];
            unsigned int writeSize;

            writeSize = snprintf(buffer, __kCFMaxDefaultEncodingFileLength, "0x%X:0x%X", (unsigned int)iScriptValue, (unsigned int)iRegionValue);
            (void)write(fd, buffer, (writeSize > __kCFMaxDefaultEncodingFileLength ? __kCFMaxDefaultEncodingFileLength : writeSize));
            close(fd);
        }
	close(no_hang_fd);
    }
}

CF_EXTERN_C_END

#endif

#endif /* ! __COREFOUNDATION_CFSTRINGDEFAULTENCODING__ */

