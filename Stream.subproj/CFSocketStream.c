/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
/*	CFSocketStream.c
	Copyright 2000-2002, Apple, Inc. All rights reserved.
	Responsibility: Jeremy Wyld
*/
//	Original Author: Becky Willrich


#include <CoreFoundation/CoreFoundation.h>
#include "CFInternal.h"
#include "CFStreamPriv.h"

#if defined(__WIN32__)
#include <winsock2.h>
#elif defined(__MACH__)
#endif

#if defined(__MACH__)
// On Mach these live in CF for historical reasons, even though they are declared in CFNetwork

const int kCFStreamErrorDomainSSL = 3;
const int kCFStreamErrorDomainSOCKS = 5;

CONST_STRING_DECL(kCFStreamPropertyShouldCloseNativeSocket, "kCFStreamPropertyShouldCloseNativeSocket")
CONST_STRING_DECL(kCFStreamPropertyAutoErrorOnSystemChange, "kCFStreamPropertyAutoErrorOnSystemChange");

CONST_STRING_DECL(kCFStreamPropertySOCKSProxy, "kCFStreamPropertySOCKSProxy")
CONST_STRING_DECL(kCFStreamPropertySOCKSProxyHost, "SOCKSProxy")
CONST_STRING_DECL(kCFStreamPropertySOCKSProxyPort, "SOCKSPort")
CONST_STRING_DECL(kCFStreamPropertySOCKSVersion, "kCFStreamPropertySOCKSVersion")
CONST_STRING_DECL(kCFStreamSocketSOCKSVersion4, "kCFStreamSocketSOCKSVersion4")
CONST_STRING_DECL(kCFStreamSocketSOCKSVersion5, "kCFStreamSocketSOCKSVersion5")
CONST_STRING_DECL(kCFStreamPropertySOCKSUser, "kCFStreamPropertySOCKSUser")
CONST_STRING_DECL(kCFStreamPropertySOCKSPassword, "kCFStreamPropertySOCKSPassword")

CONST_STRING_DECL(kCFStreamPropertySocketSecurityLevel, "kCFStreamPropertySocketSecurityLevel");
CONST_STRING_DECL(kCFStreamSocketSecurityLevelNone, "kCFStreamSocketSecurityLevelNone");
CONST_STRING_DECL(kCFStreamSocketSecurityLevelSSLv2, "kCFStreamSocketSecurityLevelSSLv2");
CONST_STRING_DECL(kCFStreamSocketSecurityLevelSSLv3, "kCFStreamSocketSecurityLevelSSLv3");
CONST_STRING_DECL(kCFStreamSocketSecurityLevelTLSv1, "kCFStreamSocketSecurityLevelTLSv1");
CONST_STRING_DECL(kCFStreamSocketSecurityLevelNegotiatedSSL, "kCFStreamSocketSecurityLevelNegotiatedSSL");
#endif // !__MACH__

// These are duplicated in CFNetwork, who actually externs them in its headers
CONST_STRING_DECL(kCFStreamPropertySocketSSLContext, "kCFStreamPropertySocketSSLContext")
CONST_STRING_DECL(_kCFStreamPropertySocketSecurityAuthenticatesServerCertificate, "_kCFStreamPropertySocketSecurityAuthenticatesServerCertificate");


CF_EXPORT
void _CFSocketStreamSetAuthenticatesServerCertificateDefault(Boolean shouldAuthenticate) {
    CFLog(__kCFLogAssertion, CFSTR("_CFSocketStreamSetAuthenticatesServerCertificateDefault(): This call has been deprecated.  Use SetProperty(_kCFStreamPropertySocketSecurityAuthenticatesServerCertificate, kCFBooleanTrue/False)\n"));
}


/* CF_EXPORT */ Boolean
_CFSocketStreamGetAuthenticatesServerCertificateDefault(void) {
    CFLog(__kCFLogAssertion, CFSTR("_CFSocketStreamGetAuthenticatesServerCertificateDefault(): This call has been removed as a security risk.  Use security properties on individual streams instead.\n"));
    return FALSE;
}


/* CF_EXPORT */ void
_CFSocketStreamPairSetAuthenticatesServerCertificate(CFReadStreamRef rStream, CFWriteStreamRef wStream, Boolean authenticates) {
    
    CFBooleanRef value = (!authenticates ? kCFBooleanFalse : kCFBooleanTrue);
    
    if (rStream)
        CFReadStreamSetProperty(rStream, _kCFStreamPropertySocketSecurityAuthenticatesServerCertificate, value);
    else
        CFWriteStreamSetProperty(wStream, _kCFStreamPropertySocketSecurityAuthenticatesServerCertificate, value);
}


// Flags for dyld loading of libraries.
enum {
    kTriedToLoad = 0,
    kInitialized
};

static struct {
    CFSpinLock_t		lock;
    UInt32				flags;
    
    const char* const	path;
#if defined(__MACH__)
#elif defined(__WIN32__)
    HMODULE				image;
#endif

    void (*_CFSocketStreamCreatePair)(CFAllocatorRef, CFStringRef, UInt32, CFSocketNativeHandle, const CFSocketSignature*, CFReadStreamRef*, CFWriteStreamRef*);
} CFNetworkSupport = {
    0,
    0x0,
    "/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/CFNetwork.framework/Versions/A/CFNetwork",
    NULL,
    NULL
};

#define CFNETWORK_CALL(sym, args)		((CFNetworkSupport.sym)args)
#if defined(__MACH__)
    #define CFNETWORK_LOAD_SYM(sym)   \
	__CFLookupCFNetworkFunction(#sym)
#elif defined(__WIN32__)
    #define CFNETWORK_LOAD_SYM(sym)   \
        (void *)GetProcAddress(CFNetworkSupport.image, #sym)
#endif

static void
createPair(CFAllocatorRef alloc, CFStringRef host, UInt32 port, CFSocketNativeHandle sock, const CFSocketSignature* sig, CFReadStreamRef *readStream, CFWriteStreamRef *writeStream)
{
    if (readStream)
        *readStream = NULL;
        
    if (writeStream)
        *writeStream = NULL;

    __CFSpinLock(&(CFNetworkSupport.lock));
    
    if (!__CFBitIsSet(CFNetworkSupport.flags, kTriedToLoad)) {
            
        __CFBitSet(CFNetworkSupport.flags, kTriedToLoad);

#if defined(__MACH__)
        CFNetworkSupport._CFSocketStreamCreatePair = CFNETWORK_LOAD_SYM(_CFSocketStreamCreatePair);
        
#elif defined(__WIN32__)

        // See if we can already find it in our address space.  This let's us check without
        // having to specify a filename.
#if defined(DEBUG)
        CFNetworkSupport.image = GetModuleHandle("CFNetwork_debug.dll");
#elif defined(PROFILE)
        CFNetworkSupport.image = GetModuleHandle("CFNetwork_profile.dll");
#endif
        // In any case, look for the release version
        if (!CFNetworkSupport.image) {
            CFNetworkSupport.image = GetModuleHandle("CFNetwork.dll");
        }

        if (!CFNetworkSupport.image) {
            // not loaded yet, try to load from the filesystem
            char path[MAX_PATH+1];
#if defined(DEBUG)
            strcpy(path, _CFDLLPath());
            strcat(path, "\\CFNetwork_debug.dll");
            CFNetworkSupport.image = LoadLibrary(path);
#elif defined(PROFILE)
            strcpy(path, _CFDLLPath());
            strcat(path, "\\CFNetwork_profile.dll");
            CFNetworkSupport.image = LoadLibrary(path);
#endif
            if (!CFNetworkSupport.image) {
                strcpy(path, _CFDLLPath());
                strcat(path, "\\CFNetwork.dll");
                CFNetworkSupport.image = LoadLibrary(path);
            }
        }
		
		if (!CFNetworkSupport.image)
			CFLog(__kCFLogAssertion, CFSTR("_CFSocketStreamCreatePair(): failed to dynamically load CFNetwork"));
		if (CFNetworkSupport.image)
			CFNetworkSupport._CFSocketStreamCreatePair = CFNETWORK_LOAD_SYM(_CFSocketStreamCreatePair);
#else
#warning _CFSocketStreamCreatePair unimplemented
#endif
		
	if (!CFNetworkSupport._CFSocketStreamCreatePair)
            CFLog(__kCFLogAssertion, CFSTR("_CFSocketStreamCreatePair(): failed to dynamically link symbol _CFSocketStreamCreatePair"));
		
		__CFBitSet(CFNetworkSupport.flags, kInitialized);
    }

    __CFSpinUnlock(&(CFNetworkSupport.lock));

    CFNETWORK_CALL(_CFSocketStreamCreatePair, (alloc, host, port, sock, sig, readStream, writeStream));
}


extern void CFStreamCreatePairWithSocket(CFAllocatorRef alloc, CFSocketNativeHandle sock, CFReadStreamRef *readStream, CFWriteStreamRef *writeStream) {
    createPair(alloc, NULL, 0, sock, NULL, readStream, writeStream);
}

extern void CFStreamCreatePairWithSocketToHost(CFAllocatorRef alloc, CFStringRef host, UInt32 port, CFReadStreamRef *readStream, CFWriteStreamRef *writeStream) {
    createPair(alloc, host, port, 0, NULL, readStream, writeStream);
}

extern void CFStreamCreatePairWithPeerSocketSignature(CFAllocatorRef alloc, const CFSocketSignature* sig, CFReadStreamRef *readStream, CFWriteStreamRef *writeStream) {
    createPair(alloc, NULL, 0, 0, sig, readStream, writeStream);
}

