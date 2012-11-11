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
/*	CFSocket.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Doug Davidson
*/

#include <CoreFoundation/CFSocket.h>
#include <sys/types.h>
#include <math.h>
#include <limits.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFPropertyList.h>
#include "CFInternal.h"
#if defined(__WIN32__)
#include <winsock2.h>
#include <stdio.h>
// Careful with remapping these - different WinSock routines return different errors than
// on BSD, so if these are used many places they won't work.
#define EINPROGRESS	WSAEINPROGRESS
#ifdef EBADF
#undef EBADF
#endif
#define EBADF 		WSAENOTSOCK
#elif defined(__MACH__)
#include <libc.h>
#else
#include <sys/filio.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

// On Mach we use a v0 RunLoopSource to make client callbacks.  That source is signalled by a
// separate SocketManager thread who uses select() to watch the sockets' fds.
//
// On Win32 we (primarily) use a v1 RunLoopSource.  Code protected by USE_V1_RUN_LOOP_SOURCE currently
// assumes __WIN32__ is defined.  The Win32 v1 RunLoopSource uses a Windows event object to be notified
// of socket events like FD_READ, FD_CONNECT, etc, at which point it can immediately make client
// callbacks instead of doing any inter-thread signaling.
//
// Because of the peculiar way that FD_WRITE is signalled (see WSAEventSelect doc in MSDN), we
// could not implement the current CFSocket client write callback semantics on top of the FD_WRITE
// events received by the v1 source.  However, because the performance gains on the read side
// were so great with the v1 source, we use a hybrid approach to implement the write side.  Read
// callbacks are triggered straightforwardly by FD_READ events.  Write callbacks are triggered in
// two ways.  Most commonly, as we return to the core run loop we poll a socket's writability
// using select().  If it can accept bytes, we signal our v1 RunLoopSource such that it will be
// immediately fired, and we can make the write callback.  Alternatively, if the socket is full,
// we then use the old v0-style approach of notifying the SocketManager thread to listen for
// notification that the socket can accept bytes using select().  Of course these two modes also
// must respect the various write callback settings and autoenabling flags setup by the client.
// The net effect is that we rarely must interact with the SocketMgr thread, which leads to a
// performance win.
//
// Because of this hybrid, we end up needing both a v1 RunLoopSource (to watch the Windows FD_*
// events) and a v0 RunLoopSource (to be signaled from the socket manager).  Since our API exports
// a single RunLoopSource that clients may schedule, we hand out the v0 RunLoopSource, and as it
// is scheduled and canceled we install the v1 RunLoopSource in the same modes.
#if defined(__WIN32__)
#define USE_V1_RUN_LOOP_SOURCE
#endif

//#define LOG_CFSOCKET

#if !defined(__WIN32__)
#define INVALID_SOCKET (CFSocketNativeHandle)(-1)
#endif /* __WIN32__ */

#define MAX_SOCKADDR_LEN 256
#define MAX_DATA_SIZE 32768

static uint16_t __CFSocketDefaultNameRegistryPortNumber = 2454;

CONST_STRING_DECL(kCFSocketCommandKey, "Command")
CONST_STRING_DECL(kCFSocketNameKey, "Name")
CONST_STRING_DECL(kCFSocketValueKey, "Value")
CONST_STRING_DECL(kCFSocketResultKey, "Result")
CONST_STRING_DECL(kCFSocketErrorKey, "Error")
CONST_STRING_DECL(kCFSocketRegisterCommand, "Register")
CONST_STRING_DECL(kCFSocketRetrieveCommand, "Retrieve")
CONST_STRING_DECL(__kCFSocketRegistryRequestRunLoopMode, "CFSocketRegistryRequest")

/* locks are to be acquired in the following order:
   (1) __CFAllSocketsLock
   (2) an individual CFSocket's lock
   (3) __CFActiveSocketsLock
*/
static CFSpinLock_t __CFAllSocketsLock = 0; /* controls __CFAllSockets */
static CFMutableDictionaryRef __CFAllSockets = NULL;
static CFSpinLock_t __CFActiveSocketsLock = 0; /* controls __CFRead/WriteSockets, __CFRead/WriteSocketsFds, __CFSocketManagerThread, and __CFSocketManagerIteration */
static volatile UInt32 __CFSocketManagerIteration = 0;
static CFMutableArrayRef __CFWriteSockets = NULL;
static CFMutableArrayRef __CFReadSockets = NULL;
static CFMutableDataRef __CFWriteSocketsFds = NULL;
static CFMutableDataRef __CFReadSocketsFds = NULL;
#if defined(__WIN32__)
// We need to select on exceptFDs on Win32 to hear of connect failures
static CFMutableDataRef __CFExceptSocketsFds = NULL;
#endif
static CFDataRef zeroLengthData = NULL;

static CFSocketNativeHandle __CFWakeupSocketPair[2] = {INVALID_SOCKET, INVALID_SOCKET};
static void *__CFSocketManagerThread = NULL;

#if !defined(__WIN32__)
#define CFSOCKET_USE_SOCKETPAIR
#define closesocket(a) close((a))
#define ioctlsocket(a,b,c) ioctl((a),(b),(c))
#endif

static CFTypeID __kCFSocketTypeID = _kCFRuntimeNotATypeID;
static void __CFSocketDoCallback(CFSocketRef s, CFDataRef data, CFDataRef address, CFSocketNativeHandle sock);
static void __CFSocketInvalidate(CFSocketRef s, Boolean wakeup);

struct __CFSocket {
    CFRuntimeBase _base;
    struct {
        unsigned client:8;	// flags set by client (reenable, CloseOnInvalidate)
        unsigned disabled:8;	// flags marking disabled callbacks
        unsigned connected:1;	// Are we connected yet?  (also true for connectionless sockets)
        unsigned writableHint:1;  // Did the polling the socket show it to be writable?
        unsigned closeSignaled:1;  // Have we seen FD_CLOSE? (only used on Win32)
        unsigned unused:13;
    } _f;
    CFSpinLock_t _lock;
    CFSpinLock_t _writeLock;
    CFSocketNativeHandle _socket;	/* immutable */
    SInt32 _socketType;
    SInt32 _errorCode;
    CFDataRef _address;
    CFDataRef _peerAddress;
    SInt32 _socketSetCount;
    CFRunLoopSourceRef _source0;	// v0 RLS, messaged from SocketMgr
    CFMutableArrayRef _runLoops;
    CFSocketCallBack _callout;		/* immutable */
    CFSocketContext _context;		/* immutable */
#if !defined(USE_V1_RUN_LOOP_SOURCE)
    CFIndex _maxQueueLen;		// queues to pass data from SocketMgr thread
    CFMutableArrayRef _dataQueue;
    CFMutableArrayRef _addressQueue;
#else
    CFRunLoopSourceRef _source1;	// v1 RLS, triggered by _event happenings
    HANDLE _event;		// used to hear about socket events
    long _oldEventMask;		// last event mask value set with WSAEventSelect
#endif
};

/* Bit 6 in the base reserved bits is used for write-signalled state (mutable) */
/* Bit 5 in the base reserved bits is used for read-signalled state (mutable) */
/* Bit 4 in the base reserved bits is used for invalid state (mutable) */
/* Bits 0-3 in the base reserved bits are used for callback types (immutable) */
/* Of this, bits 0-1 are used for the read callback type. */

CF_INLINE Boolean __CFSocketIsWriteSignalled(CFSocketRef s) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_info, 6, 6);
}

CF_INLINE void __CFSocketSetWriteSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 6, 6, 1);
}

CF_INLINE void __CFSocketUnsetWriteSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 6, 6, 0);
}

CF_INLINE Boolean __CFSocketIsReadSignalled(CFSocketRef s) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_info, 5, 5);
}

CF_INLINE void __CFSocketSetReadSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 5, 5, 1);
}

CF_INLINE void __CFSocketUnsetReadSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 5, 5, 0);
}

CF_INLINE Boolean __CFSocketIsValid(CFSocketRef s) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_info, 4, 4);
}

CF_INLINE void __CFSocketSetValid(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 4, 4, 1);
}

CF_INLINE void __CFSocketUnsetValid(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 4, 4, 0);
}

CF_INLINE uint8_t __CFSocketCallBackTypes(CFSocketRef s) {
    return (uint8_t)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_info, 3, 0);
}

CF_INLINE uint8_t __CFSocketReadCallBackType(CFSocketRef s) {
    return (uint8_t)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_info, 1, 0);
}

CF_INLINE void __CFSocketSetCallBackTypes(CFSocketRef s, uint8_t types) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_info, 3, 0, types & 0xF);
}

CF_INLINE void __CFSocketLock(CFSocketRef s) {
    __CFSpinLock(&(s->_lock));
}

CF_INLINE void __CFSocketUnlock(CFSocketRef s) {
    __CFSpinUnlock(&(s->_lock));
}

CF_INLINE void __CFSocketWriteLock(CFSocketRef s) {
    __CFSpinLock(&(s->_writeLock));
}

CF_INLINE void __CFSocketWriteUnlock(CFSocketRef s) {
    __CFSpinUnlock(&(s->_writeLock));
}

CF_INLINE Boolean __CFSocketIsConnectionOriented(CFSocketRef s) {
    return (SOCK_STREAM == s->_socketType || SOCK_SEQPACKET == s->_socketType);
}

CF_INLINE Boolean __CFSocketIsScheduled(CFSocketRef s) {
    return (s->_socketSetCount > 0);
}

CF_INLINE void __CFSocketEstablishAddress(CFSocketRef s) {
    /* socket should already be locked */
    uint8_t name[MAX_SOCKADDR_LEN];
    int namelen = sizeof(name);
    if (__CFSocketIsValid(s) && NULL == s->_address && INVALID_SOCKET != s->_socket && 0 == getsockname(s->_socket, (struct sockaddr *)name, &namelen) && NULL != name && 0 < namelen) {
        s->_address = CFDataCreate(CFGetAllocator(s), name, namelen);
    }
}

CF_INLINE void __CFSocketEstablishPeerAddress(CFSocketRef s) {
    /* socket should already be locked */
    uint8_t name[MAX_SOCKADDR_LEN];
    int namelen = sizeof(name);
    if (__CFSocketIsValid(s) && NULL == s->_peerAddress && INVALID_SOCKET != s->_socket && 0 == getpeername(s->_socket, (struct sockaddr *)name, &namelen) && NULL != name && 0 < namelen) {
        s->_peerAddress = CFDataCreate(CFGetAllocator(s), name, namelen);
    }
}

CF_INLINE CFIndex __CFSocketFdGetSize(CFDataRef fdSet) {
#if defined(__WIN32__)
    fd_set* set = (fd_set*)CFDataGetBytePtr(fdSet);
    return set ? set->fd_count : 0;
#else
    return NBBY * CFDataGetLength(fdSet);
#endif
}

CF_INLINE int __CFSocketLastError(void) {
#if defined(__WIN32__)
    return WSAGetLastError();
#else
    return thread_errno();
#endif
}

static Boolean __CFNativeSocketIsValid(CFSocketNativeHandle sock) {
#if defined(__WIN32__)
    SInt32 errorCode = 0;
    int errorSize = sizeof(errorCode);
    return !(0 != getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&errorCode, &errorSize) && WSAGetLastError() == WSAENOTSOCK);
#else
    SInt32 flags = fcntl(sock, F_GETFL, 0);
    return !(0 > flags && EBADF == thread_errno());
#endif
}

CF_INLINE Boolean __CFSocketFdSet(CFSocketNativeHandle sock, CFMutableDataRef fdSet) {
    /* returns true if a change occurred, false otherwise */
    Boolean retval = false;
    if (INVALID_SOCKET != sock && 0 <= sock) {
#if defined(__WIN32__)
        fd_set* set = (fd_set*)CFDataGetMutableBytePtr(fdSet);
        if ((set->fd_count * sizeof(SOCKET) + sizeof(u_int)) >= CFDataGetLength(fdSet)) {
            CFDataIncreaseLength(fdSet, sizeof(SOCKET));
            set = (fd_set*)CFDataGetMutableBytePtr(fdSet);
        }
        if (!FD_ISSET(sock, set)) {
            retval = true;
            FD_SET(sock, set);
        }
#else
        CFIndex numFds = NBBY * CFDataGetLength(fdSet);
        fd_mask *fds_bits;
        if (sock >= numFds) {
            CFIndex oldSize = numFds / NFDBITS, newSize = (sock + NFDBITS) / NFDBITS, changeInBytes = (newSize - oldSize) * sizeof(fd_mask);
            CFDataIncreaseLength(fdSet, changeInBytes);
            fds_bits = (fd_mask *)CFDataGetMutableBytePtr(fdSet);
            memset(fds_bits + oldSize, 0, changeInBytes);
        } else {
            fds_bits = (fd_mask *)CFDataGetMutableBytePtr(fdSet);
        }
        if (!FD_ISSET(sock, (fd_set *)fds_bits)) {
            retval = true;
            FD_SET(sock, (fd_set *)fds_bits);
        }
#endif
    }
    return retval;
}

CF_INLINE Boolean __CFSocketFdClr(CFSocketNativeHandle sock, CFMutableDataRef fdSet) {
    /* returns true if a change occurred, false otherwise */
    Boolean retval = false;
    if (INVALID_SOCKET != sock && 0 <= sock) {
#if defined(__WIN32__)
        fd_set* set = (fd_set*)CFDataGetMutableBytePtr(fdSet);
        if (FD_ISSET(sock, set)) {
            retval = true;
            FD_CLR(sock, set);
        }
#else
        CFIndex numFds = NBBY * CFDataGetLength(fdSet);
        fd_mask *fds_bits;
        if (sock < numFds) {
            fds_bits = (fd_mask *)CFDataGetMutableBytePtr(fdSet);
            if (FD_ISSET(sock, (fd_set *)fds_bits)) {
                retval = true;
                FD_CLR(sock, (fd_set *)fds_bits);
            }
        }
#endif
    }
    return retval;
}

static SInt32 __CFSocketCreateWakeupSocketPair(void) {
#if defined(CFSOCKET_USE_SOCKETPAIR)
    return socketpair(PF_LOCAL, SOCK_DGRAM, 0, __CFWakeupSocketPair);
#else
    //??? should really use native Win32 facilities
    UInt32 i;
    SInt32 error = 0;
    struct sockaddr_in address[2];
    int namelen = sizeof(struct sockaddr_in);
    for (i = 0; i < 2; i++) {
        __CFWakeupSocketPair[i] = socket(PF_INET, SOCK_DGRAM, 0);
        memset(&(address[i]), 0, sizeof(struct sockaddr_in));
        address[i].sin_family = AF_INET;
        address[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (0 <= error) error = bind(__CFWakeupSocketPair[i], (struct sockaddr *)&(address[i]), sizeof(struct sockaddr_in));
        if (0 <= error) error = getsockname(__CFWakeupSocketPair[i], (struct sockaddr *)&(address[i]), &namelen);
        if (sizeof(struct sockaddr_in) != namelen) error = -1;
    }
    if (0 <= error) error = connect(__CFWakeupSocketPair[0], (struct sockaddr *)&(address[1]), sizeof(struct sockaddr_in));
    if (0 <= error) error = connect(__CFWakeupSocketPair[1], (struct sockaddr *)&(address[0]), sizeof(struct sockaddr_in));
    if (0 > error) {
        closesocket(__CFWakeupSocketPair[0]);
        closesocket(__CFWakeupSocketPair[1]);
        __CFWakeupSocketPair[0] = INVALID_SOCKET;
        __CFWakeupSocketPair[1] = INVALID_SOCKET;
    }
    return error;
#endif
}

#if defined(USE_V1_RUN_LOOP_SOURCE)
// Version 1 RunLoopSources set a mask in a Windows System Event to control what socket activity we
// hear about.  Because you can only set the mask as a whole, we must remember the previous value so
// set can make relative changes to it.  The way we enable/disable precludes calculating the whole
// mask from scratch from the current state we keep.

static Boolean __CFSocketSetWholeEventMask(CFSocketRef s, long newMask) {
    if (s->_oldEventMask != newMask) {
        int err;
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "calling WSAEventSelect for socket/event %d/%d with event flags 0x%lx\n", s->_socket, (int)s->_event, newMask);
#endif
        err = WSAEventSelect(s->_socket, s->_event, newMask);
        CFAssert2(0 == err, __kCFLogAssertion, "%s(): WSAEventSelect failed: %d", __PRETTY_FUNCTION__, WSAGetLastError());
        s->_oldEventMask = newMask;
        return TRUE;
    } else
        return FALSE;
}

CF_INLINE Boolean __CFSocketSetFDForRead(CFSocketRef s) {
    long bitToSet;
    // we assume that some read bits are set - all callers have checked this
    CFAssert1(0 != __CFSocketReadCallBackType(s), __kCFLogAssertion, "%s(): __CFSocketReadCallBackType is zero", __PRETTY_FUNCTION__);
    bitToSet = (__CFSocketReadCallBackType(s) == kCFSocketAcceptCallBack) ? FD_ACCEPT : FD_READ;
    return __CFSocketSetWholeEventMask(s, s->_oldEventMask | bitToSet);
}

CF_INLINE Boolean __CFSocketClearFDForRead(CFSocketRef s) {
    long bitToClear;
    // we assume that some read bits are set - all callers have checked this
    CFAssert1(0 != __CFSocketReadCallBackType(s), __kCFLogAssertion, "%s(): __CFSocketReadCallBackType is zero", __PRETTY_FUNCTION__);
    bitToClear = (__CFSocketReadCallBackType(s) == kCFSocketAcceptCallBack) ? FD_ACCEPT : FD_READ;
    return __CFSocketSetWholeEventMask(s, s->_oldEventMask & ~bitToClear);
}

#else  // !USE_V1_RUN_LOOP_SOURCE
// Version 0 RunLoopSources set a mask in an FD set to control what socket activity we hear about.
CF_INLINE Boolean __CFSocketSetFDForRead(CFSocketRef s) {
    return __CFSocketFdSet(s->_socket, __CFReadSocketsFds);
}

CF_INLINE Boolean __CFSocketClearFDForRead(CFSocketRef s) {
    return __CFSocketFdClr(s->_socket, __CFReadSocketsFds);
}
#endif

CF_INLINE Boolean __CFSocketSetFDForWrite(CFSocketRef s) {
    return __CFSocketFdSet(s->_socket, __CFWriteSocketsFds);
}

CF_INLINE Boolean __CFSocketClearFDForWrite(CFSocketRef s) {
    return __CFSocketFdClr(s->_socket, __CFWriteSocketsFds);
}

#if defined(USE_V1_RUN_LOOP_SOURCE)
static Boolean __CFSocketCanAcceptBytes(CFSocketRef s) {
    struct timeval timeout = {0, 0};
    fd_set set;
    int result;
    FD_ZERO(&set);
    FD_SET(s->_socket, &set);
    result = select(s->_socket + 1, NULL, &set, NULL, &timeout);
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "polling writability of %d yields %d\n", s->_socket, result);
#endif
    return result == 1;
}

static Boolean __CFSocketHasBytesToRead(CFSocketRef s) {
    unsigned long avail;
    int err = ioctlsocket(s->_socket, FIONREAD, &avail);
    CFAssert3(0 == err, __kCFLogAssertion, "%s(): unexpected error from ioctlsocket(%d, FIONREAD,...): %d", __PRETTY_FUNCTION__, s->_socket, WSAGetLastError());
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "polling readability of %d yields %ld\n", s->_socket, avail);
#endif
    return (0 == err) && avail > 0;
}
#endif

#if defined(__WIN32__)
static Boolean WinSockUsed = FALSE;

static void __CFSocketInitializeWinSock_Guts(void) {
    if (!WinSockUsed) {
        WinSockUsed = TRUE;
        WORD versionRequested = MAKEWORD(2, 0);
        WSADATA wsaData;
        int errorStatus = WSAStartup(versionRequested, &wsaData);
        if (errorStatus != 0 || LOBYTE(wsaData.wVersion) != LOBYTE(versionRequested) || HIBYTE(wsaData.wVersion) != HIBYTE(versionRequested)) {
            WSACleanup();
            CFLog(0, CFSTR("*** Could not initialize WinSock subsystem!!!"));
        }
    }
}

CF_EXPORT void __CFSocketInitializeWinSock(void) {
    __CFSpinLock(&__CFActiveSocketsLock);
    __CFSocketInitializeWinSock_Guts();
    __CFSpinUnlock(&__CFActiveSocketsLock);
}

__private_extern__ void __CFSocketCleanup(void) {
    __CFSpinLock(&__CFActiveSocketsLock);
    if (NULL != __CFReadSockets) {
        CFRelease(__CFWriteSockets);
        __CFWriteSockets = NULL;
        CFRelease(__CFReadSockets);
        __CFReadSockets = NULL;
        CFRelease(__CFWriteSocketsFds);
        __CFWriteSocketsFds = NULL;
        CFRelease(__CFReadSocketsFds);
        __CFReadSocketsFds = NULL;
        CFRelease(__CFExceptSocketsFds);
        __CFExceptSocketsFds = NULL;
        CFRelease(zeroLengthData);
        zeroLengthData = NULL;
    }
    if (NULL != __CFAllSockets) {
        CFRelease(__CFAllSockets);
        __CFAllSockets = NULL;
    }
    if (INVALID_SOCKET != __CFWakeupSocketPair[0]) {
        closesocket(__CFWakeupSocketPair[0]);
        __CFWakeupSocketPair[0] = INVALID_SOCKET;
    }
    if (INVALID_SOCKET != __CFWakeupSocketPair[1]) {
        closesocket(__CFWakeupSocketPair[1]);
        __CFWakeupSocketPair[1] = INVALID_SOCKET;
    }
    if (WinSockUsed) {
        WSACleanup();
    }
    __CFSpinUnlock(&__CFActiveSocketsLock);
}
#endif

// CFNetwork needs to call this, especially for Win32 to get WSAStartup
static void __CFSocketInitializeSockets(void) {
    __CFWriteSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    __CFReadSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    __CFWriteSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    __CFReadSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    zeroLengthData = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
#if defined(__WIN32__)
    __CFSocketInitializeWinSock_Guts();
    // make sure we have space for the count field and the first socket
    CFDataIncreaseLength(__CFWriteSocketsFds, sizeof(u_int) + sizeof(SOCKET));
    CFDataIncreaseLength(__CFReadSocketsFds, sizeof(u_int) + sizeof(SOCKET));
    __CFExceptSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    CFDataIncreaseLength(__CFExceptSocketsFds, sizeof(u_int) + sizeof(SOCKET));
#endif
    if (0 > __CFSocketCreateWakeupSocketPair()) {
        CFLog(0, CFSTR("*** Could not create wakeup socket pair for CFSocket!!!"));
    } else {
        UInt32 yes = 1;
        /* wakeup sockets must be non-blocking */
        ioctlsocket(__CFWakeupSocketPair[0], FIONBIO, &yes);
        ioctlsocket(__CFWakeupSocketPair[1], FIONBIO, &yes);
        __CFSocketFdSet(__CFWakeupSocketPair[1], __CFReadSocketsFds);
    }
}

static CFRunLoopRef __CFSocketCopyRunLoopToWakeUp(CFSocketRef s) {
    CFRunLoopRef rl = NULL;
    SInt32 idx, cnt = CFArrayGetCount(s->_runLoops);
    if (0 < cnt) {
        rl = (CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, 0);
        for (idx = 1; NULL != rl && idx < cnt; idx++) {
            CFRunLoopRef value = (CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, idx);
            if (value != rl) rl = NULL;
        }
        if (NULL == rl) {	/* more than one different rl, so we must pick one */
            /* ideally, this would be a run loop which isn't also in a
            * signaled state for this or another source, but that's tricky;
            * we pick one that is running in an appropriate mode for this
            * source, and from those if possible one that is waiting; then
            * we move this run loop to the end of the list to scramble them
            * a bit, and always search from the front */
            Boolean foundIt = false, foundBackup = false;
            SInt32 foundIdx = 0;
            for (idx = 0; !foundIt && idx < cnt; idx++) {
                CFRunLoopRef value = (CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, idx);
                CFStringRef currentMode = CFRunLoopCopyCurrentMode(value);
                if (NULL != currentMode) {
                    if (CFRunLoopContainsSource(value, s->_source0, currentMode)) {
                        if (CFRunLoopIsWaiting(value)) {
                            foundIdx = idx;
                            foundIt = true;
                        } else if (!foundBackup) {
                            foundIdx = idx;
                            foundBackup = true;
                        }
                    }
                    CFRelease(currentMode);
                }
            }
            rl = (CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, foundIdx);
            CFRetain(rl);
            CFArrayRemoveValueAtIndex(s->_runLoops, foundIdx);
            CFArrayAppendValue(s->_runLoops, rl);
        } else {
            CFRetain(rl);
        }
    }
    return rl;
}

// If callBackNow, we immediately do client callbacks, else we have to signal a v0 RunLoopSource so the
// callbacks can happen in another thread.
static void __CFSocketHandleWrite(CFSocketRef s, Boolean callBackNow) {
    SInt32 errorCode = 0;
    int errorSize = sizeof(errorCode);
    CFOptionFlags writeCallBacksAvailable;
    
    if (!CFSocketIsValid(s)) return;
    if (0 != getsockopt(s->_socket, SOL_SOCKET, SO_ERROR, (void *)&errorCode, &errorSize)) errorCode = 0;	// cast for WinSock bad API
#if defined(LOG_CFSOCKET)
    if (errorCode) fprintf(stdout, "error %ld on socket %d\n", errorCode, s->_socket);
#endif
    __CFSocketLock(s);
    writeCallBacksAvailable = __CFSocketCallBackTypes(s) & (kCFSocketWriteCallBack | kCFSocketConnectCallBack);
    if ((s->_f.client & kCFSocketConnectCallBack) != 0) writeCallBacksAvailable &= ~kCFSocketConnectCallBack;
    if (!__CFSocketIsValid(s) || ((s->_f.disabled & writeCallBacksAvailable) == writeCallBacksAvailable)) {
        __CFSocketUnlock(s);
        return;
    }
    s->_errorCode = errorCode;
    __CFSocketSetWriteSignalled(s);
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "write signaling source for socket %d\n", s->_socket);
#endif
    if (callBackNow) {
        __CFSocketDoCallback(s, NULL, NULL, 0);
    } else {
        CFRunLoopRef rl;
        CFRunLoopSourceSignal(s->_source0);
        rl = __CFSocketCopyRunLoopToWakeUp(s);
        __CFSocketUnlock(s);
        if (NULL != rl) {
            CFRunLoopWakeUp(rl);
            CFRelease(rl);
        }
    }
}

static void __CFSocketHandleRead(CFSocketRef s) {
    CFDataRef data = NULL, address = NULL;
    CFSocketNativeHandle sock = INVALID_SOCKET;
    if (!CFSocketIsValid(s)) return;
    if (__CFSocketReadCallBackType(s) == kCFSocketDataCallBack) {
        uint8_t buffer[MAX_DATA_SIZE];
        uint8_t name[MAX_SOCKADDR_LEN];
        int namelen = sizeof(name);
        SInt32 recvlen = recvfrom(s->_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)name, &namelen);
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "read %ld bytes on socket %d\n", recvlen, s->_socket);
#endif
        if (0 >= recvlen) {
            //??? should return error if <0
            /* zero-length data is the signal for perform to invalidate */
            data = CFRetain(zeroLengthData);
        } else {
            data = CFDataCreate(CFGetAllocator(s), buffer, recvlen);
        }
        __CFSocketLock(s);
        if (!__CFSocketIsValid(s)) {
            CFRelease(data);
            __CFSocketUnlock(s);
            return;
        }
        __CFSocketSetReadSignalled(s);
        if (NULL != name && 0 < namelen) {
            //??? possible optimizations:  uniquing; storing last value
            address = CFDataCreate(CFGetAllocator(s), name, namelen);
        } else if (__CFSocketIsConnectionOriented(s)) {
            if (NULL == s->_peerAddress) __CFSocketEstablishPeerAddress(s);
            if (NULL != s->_peerAddress) address = CFRetain(s->_peerAddress);
        }
        if (NULL == address) {
            address = CFRetain(zeroLengthData);
        }
#if !defined(USE_V1_RUN_LOOP_SOURCE)
        if (NULL == s->_dataQueue) {
            s->_dataQueue = CFArrayCreateMutable(CFGetAllocator(s), 0, &kCFTypeArrayCallBacks);
        }
        if (NULL == s->_addressQueue) {
            s->_addressQueue = CFArrayCreateMutable(CFGetAllocator(s), 0, &kCFTypeArrayCallBacks);
        }
        CFArrayAppendValue(s->_dataQueue, data);
        CFRelease(data);
        CFArrayAppendValue(s->_addressQueue, address);
        CFRelease(address);
#endif // !USE_V1_RUN_LOOP_SOURCE
        if (0 < recvlen
            && (s->_f.client & kCFSocketDataCallBack) != 0 && (s->_f.disabled & kCFSocketDataCallBack) == 0
            && __CFSocketIsScheduled(s)
#if !defined(USE_V1_RUN_LOOP_SOURCE)
            && (0 == s->_maxQueueLen || CFArrayGetCount(s->_dataQueue) < s->_maxQueueLen)
#endif // !USE_V1_RUN_LOOP_SOURCE
        ) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            __CFSocketSetFDForRead(s);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    } else if (__CFSocketReadCallBackType(s) == kCFSocketAcceptCallBack) {
        uint8_t name[MAX_SOCKADDR_LEN];
        int namelen = sizeof(name);
        sock = accept(s->_socket, (struct sockaddr *)name, &namelen);
        if (INVALID_SOCKET == sock) {
            //??? should return error
            return;
        }
        if (NULL != name && 0 < namelen) {
            address = CFDataCreate(CFGetAllocator(s), name, namelen);
        } else {
            address = CFRetain(zeroLengthData);
        }
        __CFSocketLock(s);
        if (!__CFSocketIsValid(s)) {
            closesocket(sock);
            CFRelease(address);
            __CFSocketUnlock(s);
            return;
        }
        __CFSocketSetReadSignalled(s);
#if !defined(USE_V1_RUN_LOOP_SOURCE)
        if (NULL == s->_dataQueue) {
            s->_dataQueue = CFArrayCreateMutable(CFGetAllocator(s), 0, NULL);
        }
        if (NULL == s->_addressQueue) {
            s->_addressQueue = CFArrayCreateMutable(CFGetAllocator(s), 0, &kCFTypeArrayCallBacks);
        }
        CFArrayAppendValue(s->_dataQueue, (void *)sock);
        CFArrayAppendValue(s->_addressQueue, address);
        CFRelease(address);
#endif // !USE_V1_RUN_LOOP_SOURCE
        if ((s->_f.client & kCFSocketAcceptCallBack) != 0 && (s->_f.disabled & kCFSocketAcceptCallBack) == 0
            && __CFSocketIsScheduled(s)
#if !defined(USE_V1_RUN_LOOP_SOURCE)
            && (0 == s->_maxQueueLen || CFArrayGetCount(s->_dataQueue) < s->_maxQueueLen)
#endif // !USE_V1_RUN_LOOP_SOURCE
        ) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            __CFSocketSetFDForRead(s);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    } else {
        __CFSocketLock(s);
        if (!__CFSocketIsValid(s) || (s->_f.disabled & kCFSocketReadCallBack) != 0) {
            __CFSocketUnlock(s);
            return;
        }
        __CFSocketSetReadSignalled(s);
    }
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "read signaling source for socket %d\n", s->_socket);
#endif
#if defined(USE_V1_RUN_LOOP_SOURCE)
    // since in the v0 case data and sock come from the same queue, only one could be set
    CFAssert1(NULL == data || 0 == sock, __kCFLogAssertion, "%s(): both data and sock are set", __PRETTY_FUNCTION__);
    __CFSocketDoCallback(s, data, address, sock);	// does __CFSocketUnlock(s)
    if (NULL != data) CFRelease(data);
    if (NULL != address) CFRelease(address);
#else    
    CFRunLoopSourceSignal(s->_source0);
    CFRunLoopRef rl = __CFSocketCopyRunLoopToWakeUp(s);
    __CFSocketUnlock(s);
    if (NULL != rl) {
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
#endif // !USE_V1_RUN_LOOP_SOURCE
}

#if defined(LOG_CFSOCKET)
static void __CFSocketWriteSocketList(CFArrayRef sockets, CFDataRef fdSet, Boolean onlyIfSet) {
    fd_set *tempfds = (fd_set *)CFDataGetBytePtr(fdSet);
    SInt32 idx, cnt;
    for (idx = 0, cnt = CFArrayGetCount(sockets); idx < cnt; idx++) {
        CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(sockets, idx);
        if (FD_ISSET(s->_socket, tempfds)) {
            fprintf(stdout, "%d ", s->_socket);
        } else if (!onlyIfSet) {
            fprintf(stdout, "(%d) ", s->_socket);
        }
    }
}
#endif

#ifdef __GNUC__
__attribute__ ((noreturn))	// mostly interesting for shutting up a warning
#endif
static void __CFSocketManager(void * arg)
{
    SInt32 nrfds, maxnrfds, fdentries = 1;
    SInt32 rfds, wfds;
#if defined(__WIN32__)
    fd_set *exceptfds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
    fd_set *writefds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
    fd_set *readfds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
#else
    fd_set *exceptfds = NULL;
    fd_set *writefds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(fd_mask), 0);
    fd_set *readfds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(fd_mask), 0);
#endif
    fd_set *tempfds;
    SInt32 idx, cnt;
    uint8_t buffer[256];
    CFMutableArrayRef selectedWriteSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef selectedReadSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    
    for (;;) {       
        __CFSpinLock(&__CFActiveSocketsLock);
        __CFSocketManagerIteration++;
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "socket manager iteration %lu looking at read sockets ", __CFSocketManagerIteration);
        __CFSocketWriteSocketList(__CFReadSockets, __CFReadSocketsFds, FALSE);
        if (0 < CFArrayGetCount(__CFWriteSockets)) {
            fprintf(stdout, " and write sockets ");
            __CFSocketWriteSocketList(__CFWriteSockets, __CFWriteSocketsFds, FALSE);
#if defined(__WIN32__)
            fprintf(stdout, " and except sockets ");
            __CFSocketWriteSocketList(__CFWriteSockets, __CFExceptSocketsFds, TRUE);
#endif
        }
        fprintf(stdout, "\n");
#endif
        rfds = __CFSocketFdGetSize(__CFReadSocketsFds);
        wfds = __CFSocketFdGetSize(__CFWriteSocketsFds);
        maxnrfds = __CFMax(rfds, wfds);
#if defined(__WIN32__)
        if (maxnrfds > fdentries) {
            fdentries = maxnrfds;
            exceptfds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, exceptfds, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
            writefds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, writefds, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
            readfds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, readfds, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
        }
        memset(exceptfds, 0, fdentries * sizeof(SOCKET) + sizeof(u_int));
        memset(writefds, 0, fdentries * sizeof(SOCKET) + sizeof(u_int));
        memset(readfds, 0, fdentries * sizeof(SOCKET) + sizeof(u_int));
        CFDataGetBytes(__CFExceptSocketsFds, CFRangeMake(0, __CFSocketFdGetSize(__CFExceptSocketsFds) * sizeof(SOCKET) + sizeof(u_int)), (UInt8 *)exceptfds);
        CFDataGetBytes(__CFWriteSocketsFds, CFRangeMake(0, wfds * sizeof(SOCKET) + sizeof(u_int)), (UInt8 *)writefds);
        CFDataGetBytes(__CFReadSocketsFds, CFRangeMake(0, rfds * sizeof(SOCKET) + sizeof(u_int)), (UInt8 *)readfds); 
#else
        if (maxnrfds > fdentries * (int)NFDBITS) {
            fdentries = (maxnrfds + NFDBITS - 1) / NFDBITS;
            writefds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, writefds, fdentries * sizeof(fd_mask), 0);
            readfds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, readfds, fdentries * sizeof(fd_mask), 0);
        }
        memset(writefds, 0, fdentries * sizeof(fd_mask)); 
        memset(readfds, 0, fdentries * sizeof(fd_mask));
        CFDataGetBytes(__CFWriteSocketsFds, CFRangeMake(0, CFDataGetLength(__CFWriteSocketsFds)), (UInt8 *)writefds);
        CFDataGetBytes(__CFReadSocketsFds, CFRangeMake(0, CFDataGetLength(__CFReadSocketsFds)), (UInt8 *)readfds); 
#endif
        __CFSpinUnlock(&__CFActiveSocketsLock);
    
        nrfds = select(maxnrfds, readfds, writefds, exceptfds, NULL);
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "socket manager woke from select, ret=%ld\n", nrfds);
#endif
        if (0 == nrfds) continue;
        if (0 > nrfds) {
            SInt32 selectError = __CFSocketLastError();
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager received error %ld from select\n", selectError);
#endif
            if (EBADF == selectError) {
                CFMutableArrayRef invalidSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
                __CFSpinLock(&__CFActiveSocketsLock);
                cnt = CFArrayGetCount(__CFWriteSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
                    if (!__CFNativeSocketIsValid(s->_socket)) {
#if defined(LOG_CFSOCKET)
                        fprintf(stdout, "socket manager found write socket %d invalid\n", s->_socket);
#endif
                        CFArrayAppendValue(invalidSockets, s);
                    }
                }
                cnt = CFArrayGetCount(__CFReadSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
                    if (!__CFNativeSocketIsValid(s->_socket)) {
#if defined(LOG_CFSOCKET)
                        fprintf(stdout, "socket manager found read socket %d invalid\n", s->_socket);
#endif
                        CFArrayAppendValue(invalidSockets, s);
                    }
                }
                __CFSpinUnlock(&__CFActiveSocketsLock);
        
                cnt = CFArrayGetCount(invalidSockets);
                for (idx = 0; idx < cnt; idx++) {
                    __CFSocketInvalidate(((CFSocketRef)CFArrayGetValueAtIndex(invalidSockets, idx)), false);
                }
                CFRelease(invalidSockets);
            }
            continue;
        }
        if (FD_ISSET(__CFWakeupSocketPair[1], readfds)) {
            recv(__CFWakeupSocketPair[1], buffer, sizeof(buffer), 0);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager received %c on wakeup socket\n", buffer[0]);
#endif
        }
        __CFSpinLock(&__CFActiveSocketsLock);
        tempfds = NULL;
        cnt = CFArrayGetCount(__CFWriteSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
            CFSocketNativeHandle sock = s->_socket;
#if !defined(__WIN32__)
            // We might have an new element in __CFWriteSockets that we weren't listening to,
            // in which case we must be sure not to test a bit in the fdset that is
            // outside our mask size.
            Boolean sockInBounds = (0 <= sock && sock < maxnrfds);
#else
            // fdset's are arrays, so we don't have that issue above
            Boolean sockInBounds = true;
#endif
            if (INVALID_SOCKET != sock && sockInBounds) {
                if (FD_ISSET(sock, writefds)) {
                    CFArrayAppendValue(selectedWriteSockets, s);
                    /* socket is removed from fds here, restored by CFSocketReschedule */
                    if (!tempfds) tempfds = (fd_set *)CFDataGetMutableBytePtr(__CFWriteSocketsFds);
                    FD_CLR(sock, tempfds);
#if defined(__WIN32__)
                    fd_set *exfds = (fd_set *)CFDataGetMutableBytePtr(__CFExceptSocketsFds);
                    FD_CLR(sock, exfds);
#endif
                }
#if defined(__WIN32__)
                else if (FD_ISSET(sock, exceptfds)) {
                    // On Win32 connect errors come in on exceptFDs.  We treat these as if
                    // they had come on writeFDs, since the rest of our Unix-based code
                    // expects that.
                    CFArrayAppendValue(selectedWriteSockets, s);
                    fd_set *exfds = (fd_set *)CFDataGetMutableBytePtr(__CFExceptSocketsFds);
                    FD_CLR(sock, exfds);
                }
#endif
            }
        }
        tempfds = NULL;
        cnt = CFArrayGetCount(__CFReadSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
            CFSocketNativeHandle sock = s->_socket;
#if !defined(__WIN32__)
            // We might have an new element in __CFReadSockets that we weren't listening to,
            // in which case we must be sure not to test a bit in the fdset that is
            // outside our mask size.
            Boolean sockInBounds = (0 <= sock && sock < maxnrfds);
#else
            // fdset's are arrays, so we don't have that issue above
            Boolean sockInBounds = true;
#endif
            if (INVALID_SOCKET != sock && sockInBounds && FD_ISSET(sock, readfds)) {
                CFArrayAppendValue(selectedReadSockets, s);
                /* socket is removed from fds here, will be restored in read handling or in perform function */
                if (!tempfds) tempfds = (fd_set *)CFDataGetMutableBytePtr(__CFReadSocketsFds);
                FD_CLR(sock, tempfds);
            }
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
        
        cnt = CFArrayGetCount(selectedWriteSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(selectedWriteSockets, idx);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager signaling socket %d for write\n", s->_socket);
#endif
            __CFSocketHandleWrite(s, FALSE);
        }
        if (0 < cnt) CFArrayRemoveAllValues(selectedWriteSockets);
        cnt = CFArrayGetCount(selectedReadSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(selectedReadSockets, idx);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager signaling socket %d for read\n", s->_socket);
#endif
            __CFSocketHandleRead(s);
        }
        if (0 < cnt) CFArrayRemoveAllValues(selectedReadSockets);
    }
}

static CFStringRef __CFSocketCopyDescription(CFTypeRef cf) {
    CFSocketRef s = (CFSocketRef)cf;
    CFMutableStringRef result;
    CFStringRef contextDesc = NULL;
    void *contextInfo = NULL;
    CFStringRef (*contextCopyDescription)(const void *info) = NULL;
    result = CFStringCreateMutable(CFGetAllocator(s), 0);
    __CFSocketLock(s);
    CFStringAppendFormat(result, NULL, CFSTR("<CFSocket %p [%p]>{valid = %s, type = %d, socket = %d, socket set count = %ld\n    callback types = 0x%x, callout = %x, source = %p,\n    run loops = %@,\n    context = "), cf, CFGetAllocator(s), (__CFSocketIsValid(s) ? "Yes" : "No"), s->_socketType, s->_socket, s->_socketSetCount, __CFSocketCallBackTypes(s), s->_callout, s->_source0, s->_runLoops);
    contextInfo = s->_context.info;
    contextCopyDescription = s->_context.copyDescription;
    __CFSocketUnlock(s);
    if (NULL != contextInfo && NULL != contextCopyDescription) {
        contextDesc = (CFStringRef)contextCopyDescription(contextInfo);
    }
    if (NULL == contextDesc) {
        contextDesc = CFStringCreateWithFormat(CFGetAllocator(s), NULL, CFSTR("<CFSocket context %p>"), contextInfo);
    }
    CFStringAppend(result, contextDesc);
    CFRelease(contextDesc);
    return result;
}

static void __CFSocketDeallocate(CFTypeRef cf) {
    /* Since CFSockets are cached, we can only get here sometime after being invalidated */
    CFSocketRef s = (CFSocketRef)cf;
    if (NULL != s->_address) {
        CFRelease(s->_address);
        s->_address = NULL;
    }
}

static const CFRuntimeClass __CFSocketClass = {
    0,
    "CFSocket",
    NULL,      // init
    NULL,      // copy
    __CFSocketDeallocate,
    NULL,      // equal
    NULL,      // hash
    NULL,      // 
    __CFSocketCopyDescription
};

__private_extern__ void __CFSocketInitialize(void) {
    __kCFSocketTypeID = _CFRuntimeRegisterClass(&__CFSocketClass);
}

CFTypeID CFSocketGetTypeID(void) {
    return __kCFSocketTypeID;
}

CFSocketError CFSocketSetAddress(CFSocketRef s, CFDataRef address) {
    const uint8_t *name;
    SInt32 namelen, result = 0;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    if (NULL == address) return -1;
    name = CFDataGetBytePtr(address);
    namelen = CFDataGetLength(address);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && INVALID_SOCKET != s->_socket && NULL != name && 0 < namelen) {
        result = bind(s->_socket, (struct sockaddr *)name, namelen);
        if (0 == result) {
            __CFSocketEstablishAddress(s);
            listen(s->_socket, 256);
        }
    }
    if (NULL == s->_address && NULL != name && 0 < namelen && 0 == result) {
        s->_address = CFDataCreateCopy(CFGetAllocator(s), address);
    }   
    __CFSocketUnlock(s);
    //??? should return errno
    return result;
}

__private_extern__ void CFSocketSetAcceptBacklog(CFSocketRef s, CFIndex limit) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && INVALID_SOCKET != s->_socket) {
        listen(s->_socket, limit);
    }
    __CFSocketUnlock(s);
}

__private_extern__ void CFSocketSetMaximumQueueLength(CFSocketRef s, CFIndex length) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
#if !defined(USE_V1_RUN_LOOP_SOURCE)
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) {
        s->_maxQueueLen = length;
    }
    __CFSocketUnlock(s);
#endif
}

CFSocketError CFSocketConnectToAddress(CFSocketRef s, CFDataRef address, CFTimeInterval timeout) {
    //??? need error handling, retries
    const uint8_t *name;
    SInt32 namelen, result = -1, connect_err = 0, select_err = 0;
    UInt32 yes = 1, no = 0;
    Boolean wasBlocking = true;

    __CFGenericValidateType(s, __kCFSocketTypeID);
    name = CFDataGetBytePtr(address);
    namelen = CFDataGetLength(address);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && INVALID_SOCKET != s->_socket && NULL != name && 0 < namelen) {
#if !defined(__WIN32__)
        SInt32 flags = fcntl(s->_socket, F_GETFL, 0);
        if (flags >= 0) wasBlocking = ((flags & O_NONBLOCK) == 0);
        if (wasBlocking && (timeout > 0.0 || timeout < 0.0)) ioctlsocket(s->_socket, FIONBIO, &yes);
#else
        // You can set but not get this flag in WIN32, so assume it was in non-blocking mode.
        // The downside is that when we leave this routine we'll leave it non-blocking,
        // whether it started that way or not.
        if (timeout > 0.0 || timeout < 0.0) ioctlsocket(s->_socket, FIONBIO, &yes);
        wasBlocking = false;
#endif
        result = connect(s->_socket, (struct sockaddr *)name, namelen);
        if (result != 0) {
            connect_err = __CFSocketLastError();
#if defined(__WIN32__)
            if (connect_err == WSAEWOULDBLOCK) connect_err = EINPROGRESS;
#endif
        }
#if defined(LOG_CFSOCKET)
#if !defined(__WIN32__)
        fprintf(stdout, "connection attempt returns %ld error %ld on socket %d (flags 0x%lx blocking %d)\n", result, connect_err, s->_socket, flags, wasBlocking);
#else
        fprintf(stdout, "connection attempt returns %ld error %ld on socket %d\n", result, connect_err, s->_socket);
#endif
#endif
        if (EINPROGRESS == connect_err && timeout >= 0.0) {
            /* select on socket */
            SInt32 nrfds;
            int error_size = sizeof(select_err);
            struct timeval tv;
            CFMutableDataRef fds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
#if defined(__WIN32__)
            CFDataIncreaseLength(fds , sizeof(u_int) + sizeof(SOCKET));
#endif
            __CFSocketFdSet(s->_socket, fds);
            tv.tv_sec = (0 >= timeout || INT_MAX <= timeout) ? INT_MAX : (int)(float)floor(timeout);
            tv.tv_usec = (int)((timeout - floor(timeout)) * 1.0E6);
            nrfds = select(__CFSocketFdGetSize(fds), NULL, (fd_set *)CFDataGetMutableBytePtr(fds), NULL, &tv);
            if (nrfds < 0) {
                select_err = __CFSocketLastError();
                result = -1;
            } else if (nrfds == 0) {
                result = -2;
            } else {
                if (0 != getsockopt(s->_socket, SOL_SOCKET, SO_ERROR, (void *)&select_err, &error_size)) select_err = 0;
                result = (select_err == 0) ? 0 : -1;
            }
            CFRelease(fds);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "timed connection attempt %s on socket %d, result %ld, select returns %ld error %ld\n", (result == 0) ? "succeeds" : "fails", s->_socket, result, nrfds, select_err);
#endif
        }
        if (wasBlocking && (timeout > 0.0 || timeout < 0.0)) ioctlsocket(s->_socket, FIONBIO, &no);
        if (0 == result) {
            __CFSocketEstablishPeerAddress(s);
            if (NULL == s->_peerAddress && NULL != name && 0 < namelen && __CFSocketIsConnectionOriented(s)) {
                s->_peerAddress = CFDataCreateCopy(CFGetAllocator(s), address);
            }
        }
        if (EINPROGRESS == connect_err && timeout < 0.0) {
            result = 0;
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "connection attempt continues in background on socket %d\n", s->_socket);
#endif
        }
    }
    __CFSocketUnlock(s);
    //??? should return errno
    return result;
}

CFSocketRef CFSocketCreate(CFAllocatorRef allocator, SInt32 protocolFamily, SInt32 socketType, SInt32 protocol, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context) {
    CFSocketNativeHandle sock = INVALID_SOCKET;
    CFSocketRef s = NULL;
    if (0 >= protocolFamily) protocolFamily = PF_INET;
    if (PF_INET == protocolFamily) {
        if (0 >= socketType) socketType = SOCK_STREAM;
        if (0 >= protocol && SOCK_STREAM == socketType) protocol = IPPROTO_TCP;
        if (0 >= protocol && SOCK_DGRAM == socketType) protocol = IPPROTO_UDP;
    }
#if !defined(__WIN32__)
    if (PF_LOCAL == protocolFamily && 0 >= socketType) socketType = SOCK_STREAM;
#else
    /* WinSock needs to be initialized at this point for Windows, before socket() */
    __CFSocketInitializeWinSock();
#endif
    sock = socket(protocolFamily, socketType, protocol);
    if (INVALID_SOCKET != sock) {
        s = CFSocketCreateWithNative(allocator, sock, callBackTypes, callout, context);
    }
    return s;
}

CFSocketRef CFSocketCreateWithNative(CFAllocatorRef allocator, CFSocketNativeHandle sock, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context) {
    CFSocketRef memory;
    int typeSize = sizeof(memory->_socketType);
    __CFSpinLock(&__CFActiveSocketsLock);
    if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
    __CFSpinUnlock(&__CFActiveSocketsLock);
    __CFSpinLock(&__CFAllSocketsLock);
    if (NULL == __CFAllSockets) {
        __CFAllSockets = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    }
    if (INVALID_SOCKET != sock && CFDictionaryGetValueIfPresent(__CFAllSockets, (void *)sock, (const void **)&memory)) {
        __CFSpinUnlock(&__CFAllSocketsLock);
        CFRetain(memory);
        return memory;
    }
    memory = (CFSocketRef)_CFRuntimeCreateInstance(allocator, __kCFSocketTypeID, sizeof(struct __CFSocket) - sizeof(CFRuntimeBase), NULL);
    if (NULL == memory) {
        __CFSpinUnlock(&__CFAllSocketsLock);
        return NULL;
    }
    __CFSocketSetCallBackTypes(memory, callBackTypes);
    if (INVALID_SOCKET != sock) __CFSocketSetValid(memory);
    __CFSocketUnsetWriteSignalled(memory);
    __CFSocketUnsetReadSignalled(memory);
    memory->_f.client = ((callBackTypes & (~kCFSocketConnectCallBack)) & (~kCFSocketWriteCallBack)) | kCFSocketCloseOnInvalidate;
    memory->_f.disabled = 0;
    memory->_f.connected = FALSE;
    memory->_f.writableHint = FALSE;
    memory->_f.closeSignaled = FALSE;
    memory->_lock = 0;
    memory->_writeLock = 0;
    memory->_socket = sock;
    if (INVALID_SOCKET == sock || 0 != getsockopt(sock, SOL_SOCKET, SO_TYPE, (void *)&(memory->_socketType), &typeSize)) memory->_socketType = 0;		// cast for WinSock bad API
    memory->_errorCode = 0;
    memory->_address = NULL;
    memory->_peerAddress = NULL;
    memory->_socketSetCount = 0;
    memory->_source0 = NULL;
    if (INVALID_SOCKET != sock) {
        memory->_runLoops = CFArrayCreateMutable(allocator, 0, NULL);
    } else {
        memory->_runLoops = NULL;
    }
    memory->_callout = callout;
#if defined(USE_V1_RUN_LOOP_SOURCE)
    memory->_event = CreateEvent(NULL, true, false, NULL);
    CFAssert1(NULL != memory->_event, __kCFLogAssertion, "%s(): could not create event", __PRETTY_FUNCTION__);
    memory->_oldEventMask = 0;
    __CFSocketSetWholeEventMask(memory, FD_CLOSE|FD_CONNECT);	// always listen for closes, connects
    memory->_source1 = NULL;
#else // !USE_V1_RUN_LOOP_SOURCE
    memory->_dataQueue = NULL;
    memory->_addressQueue = NULL;
    memory->_maxQueueLen = 0;
#endif // !USE_V1_RUN_LOOP_SOURCE
    memory->_context.info = 0;
    memory->_context.retain = 0;
    memory->_context.release = 0;
    memory->_context.copyDescription = 0;
    if (INVALID_SOCKET != sock) CFDictionaryAddValue(__CFAllSockets, (void *)sock, memory);
    __CFSpinUnlock(&__CFAllSocketsLock);
    if (NULL != context) {
        void *contextInfo = context->retain ? (void *)context->retain(context->info) : context->info;
        __CFSocketLock(memory);
        memory->_context.retain = context->retain;
        memory->_context.release = context->release;
        memory->_context.copyDescription = context->copyDescription;
        memory->_context.info = contextInfo;
        __CFSocketUnlock(memory);
    }
    return memory;
}

CFSocketRef CFSocketCreateWithSocketSignature(CFAllocatorRef allocator, const CFSocketSignature *signature, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context) {
    CFSocketRef s = CFSocketCreate(allocator, signature->protocolFamily, signature->socketType, signature->protocol, callBackTypes, callout, context);
    if (NULL != s && (!CFSocketIsValid(s) || kCFSocketSuccess != CFSocketSetAddress(s, signature->address))) {
        CFSocketInvalidate(s);
        CFRelease(s);
        s = NULL;
    }
    return s;
}

CFSocketRef CFSocketCreateConnectedToSocketSignature(CFAllocatorRef allocator, const CFSocketSignature *signature, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context, CFTimeInterval timeout) {
    CFSocketRef s = CFSocketCreate(allocator, signature->protocolFamily, signature->socketType, signature->protocol, callBackTypes, callout, context);
    if (NULL != s && (!CFSocketIsValid(s) || kCFSocketSuccess != CFSocketConnectToAddress(s, signature->address, timeout))) {
        CFSocketInvalidate(s);
        CFRelease(s);
        s = NULL;
    }
    return s;
}

static void __CFSocketInvalidate(CFSocketRef s, Boolean wakeup) {
    UInt32 previousSocketManagerIteration;
    __CFGenericValidateType(s, __kCFSocketTypeID);
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "invalidating socket %d with flags 0x%x disabled 0x%x connected 0x%x wakeup %d\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected, wakeup);
#endif
    CFRetain(s);
    __CFSpinLock(&__CFAllSocketsLock);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) {
        SInt32 idx;
        CFRunLoopSourceRef source0;
        void *contextInfo = NULL;
        void (*contextRelease)(const void *info) = NULL;
        __CFSocketUnsetValid(s);
        __CFSocketUnsetWriteSignalled(s);
        __CFSocketUnsetReadSignalled(s);
        __CFSpinLock(&__CFActiveSocketsLock);
        idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFWriteSockets, idx);
            __CFSocketClearFDForWrite(s);
#if defined(__WIN32__)
            __CFSocketFdClr(s->_socket, __CFExceptSocketsFds);
#endif
        }
#if !defined(USE_V1_RUN_LOOP_SOURCE)
        // No need to clear FD's for V1 sources, since we'll just throw the whole event away
        idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFReadSockets, idx);
            __CFSocketClearFDForRead(s);
        }
#endif  // !USE_V1_RUN_LOOP_SOURCE
        previousSocketManagerIteration = __CFSocketManagerIteration;
        __CFSpinUnlock(&__CFActiveSocketsLock);
#if 0
        if (wakeup && __CFSocketManagerThread) {
            Boolean doneWaiting = false;
            uint8_t c = 'i';
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "invalidation wants socket iteration to change from %lu\n", previousSocketManagerIteration);
#endif
            send(__CFWakeupSocketPair[0], &c, sizeof(c), 0);
#if !defined(__WIN32__)
            while (!doneWaiting) {
                __CFSpinLock(&__CFActiveSocketsLock);
                if (previousSocketManagerIteration != __CFSocketManagerIteration) doneWaiting = true;
#if defined(LOG_CFSOCKET)
                fprintf(stdout, "invalidation comparing socket iteration %lu to previous %lu\n", __CFSocketManagerIteration, previousSocketManagerIteration);
#endif
                __CFSpinUnlock(&__CFActiveSocketsLock);
                if (!doneWaiting) {
                    struct timespec ts = {0, 1};
                    // ??? depress priority
                    nanosleep(&ts, NULL);
                }
            }
#endif
        }
#endif
        CFDictionaryRemoveValue(__CFAllSockets, (void *)(s->_socket));
        if ((s->_f.client & kCFSocketCloseOnInvalidate) != 0) closesocket(s->_socket);
        s->_socket = INVALID_SOCKET;
        if (NULL != s->_peerAddress) {
            CFRelease(s->_peerAddress);
            s->_peerAddress = NULL;
        }
#if !defined(USE_V1_RUN_LOOP_SOURCE)
        if (NULL != s->_dataQueue) {
            CFRelease(s->_dataQueue);
            s->_dataQueue = NULL;
        }
        if (NULL != s->_addressQueue) {
            CFRelease(s->_addressQueue);
            s->_addressQueue = NULL;
        }
        s->_socketSetCount = 0;
#endif  // !USE_V1_RUN_LOOP_SOURCE
        for (idx = CFArrayGetCount(s->_runLoops); idx--;) {
            CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, idx));
        }
        CFRelease(s->_runLoops);
        s->_runLoops = NULL;
        source0 = s->_source0;
        s->_source0 = NULL;
        contextInfo = s->_context.info;
        contextRelease = s->_context.release;
        s->_context.info = 0;
        s->_context.retain = 0;
        s->_context.release = 0;
        s->_context.copyDescription = 0;
        __CFSocketUnlock(s);
        if (NULL != contextRelease) {
            contextRelease(contextInfo);
        }
        if (NULL != source0) {
            CFRunLoopSourceInvalidate(source0);
            CFRelease(source0);
        }
#if defined(USE_V1_RUN_LOOP_SOURCE)
        // Important to do the v1 source after the v0 source, since cancelling the v0 source
        // references the v1 source (since the v1 source is added/removed from RunLoops as a
        // side-effect of the v0 source being added/removed).
        if (NULL != s->_source1) {
            CFRunLoopSourceInvalidate(s->_source1);
            CFRelease(s->_source1);
            s->_source1 = NULL;
        }
        CloseHandle(s->_event);
#endif
    } else {
        __CFSocketUnlock(s);
    }
    __CFSpinUnlock(&__CFAllSocketsLock);
    CFRelease(s);
}

void CFSocketInvalidate(CFSocketRef s) {__CFSocketInvalidate(s, true);}

Boolean CFSocketIsValid(CFSocketRef s) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    return __CFSocketIsValid(s);
}

CFSocketNativeHandle CFSocketGetNative(CFSocketRef s) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    return s->_socket;
}

CFDataRef CFSocketCopyAddress(CFSocketRef s) {
    CFDataRef result = NULL;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    __CFSocketEstablishAddress(s);
    if (NULL != s->_address) {
        result = CFRetain(s->_address);
    }
    __CFSocketUnlock(s);
    return result;
}

CFDataRef CFSocketCopyPeerAddress(CFSocketRef s) {
    CFDataRef result = NULL;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    __CFSocketEstablishPeerAddress(s);
    if (NULL != s->_peerAddress) {
        result = CFRetain(s->_peerAddress);
    }
    __CFSocketUnlock(s);
    return result;
}

void CFSocketGetContext(CFSocketRef s, CFSocketContext *context) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = s->_context;
}

__private_extern__ void CFSocketReschedule(CFSocketRef s) {
    CFSocketEnableCallBacks(s, __CFSocketCallBackTypes(s));
}

CFOptionFlags CFSocketGetSocketFlags(CFSocketRef s) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    return s->_f.client;
}

void CFSocketSetSocketFlags(CFSocketRef s, CFOptionFlags flags) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "setting flags for socket %d, from 0x%x to 0x%lx\n", s->_socket, s->_f.client, flags);
#endif
    s->_f.client = flags;
    __CFSocketUnlock(s);
}

void CFSocketDisableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes) {
    Boolean wakeup = false;
    uint8_t readCallBackType;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && __CFSocketIsScheduled(s)) {
        callBackTypes &= __CFSocketCallBackTypes(s);
        readCallBackType = __CFSocketReadCallBackType(s);
        s->_f.disabled |= callBackTypes;
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "unscheduling socket %d with flags 0x%x disabled 0x%x connected 0x%x for types 0x%lx\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected, callBackTypes);
#endif
        __CFSpinLock(&__CFActiveSocketsLock);
        if ((readCallBackType == kCFSocketAcceptCallBack) || !__CFSocketIsConnectionOriented(s)) s->_f.connected = TRUE;
        if (((callBackTypes & kCFSocketWriteCallBack) != 0) || (((callBackTypes & kCFSocketConnectCallBack) != 0) && !s->_f.connected)) {
            if (__CFSocketClearFDForWrite(s)) {
                // do not wake up the socket manager thread if all relevant write callbacks are disabled
                CFOptionFlags writeCallBacksAvailable = __CFSocketCallBackTypes(s) & (kCFSocketWriteCallBack | kCFSocketConnectCallBack);
                if (s->_f.connected) writeCallBacksAvailable &= ~kCFSocketConnectCallBack;
                if ((s->_f.disabled & writeCallBacksAvailable) != writeCallBacksAvailable) wakeup = true;
#if defined(__WIN32__)
                __CFSocketFdClr(s->_socket, __CFExceptSocketsFds);
#endif
            }
        }
        if (readCallBackType != kCFSocketNoCallBack && (callBackTypes & readCallBackType) != 0) {
            if (__CFSocketClearFDForRead(s)) {
#if !defined(USE_V1_RUN_LOOP_SOURCE)
                // do not wake up the socket manager thread if callback type is read
                if (readCallBackType != kCFSocketReadCallBack) wakeup = true;
#endif
            }
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }
    __CFSocketUnlock(s);
    if (wakeup && __CFSocketManagerThread) {
        uint8_t c = 'u';
        send(__CFWakeupSocketPair[0], &c, sizeof(c), 0);
    }
}

// "force" means to clear the disabled bits set by DisableCallBacks and always reenable.
// if (!force) we respect those bits, meaning they may stop us from enabling.
// In addition, if !force we assume that the sockets have already been added to the
// __CFReadSockets and __CFWriteSockets arrays.  This is true because the callbacks start
// enabled when the CFSocket is created (at which time we enable with force).
// Called with SocketLock held, returns with it released!
void __CFSocketEnableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes, Boolean force, uint8_t wakeupChar) {
    Boolean wakeup = FALSE;
    if (!callBackTypes) {
        __CFSocketUnlock(s);
        return;
    }
    if (__CFSocketIsValid(s) && __CFSocketIsScheduled(s)) {
        Boolean turnOnWrite = FALSE, turnOnConnect = FALSE, turnOnRead = FALSE;
        uint8_t readCallBackType = __CFSocketReadCallBackType(s);        
        callBackTypes &= __CFSocketCallBackTypes(s);
        if (force) s->_f.disabled &= ~callBackTypes;
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "rescheduling socket %d with flags 0x%x disabled 0x%x connected 0x%x for types 0x%lx\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected, callBackTypes);
#endif
        /* We will wait for connection only for connection-oriented, non-rendezvous sockets that are not already connected.  Mark others as already connected. */
        if ((readCallBackType == kCFSocketAcceptCallBack) || !__CFSocketIsConnectionOriented(s)) s->_f.connected = TRUE;

        // First figure out what to turn on
        if (s->_f.connected || (callBackTypes & kCFSocketConnectCallBack) == 0) {
            // if we want write callbacks and they're not disabled...
            if ((callBackTypes & kCFSocketWriteCallBack) != 0 && (s->_f.disabled & kCFSocketWriteCallBack) == 0) turnOnWrite = TRUE;
        } else {
            // if we want connect callbacks and they're not disabled...
            if ((callBackTypes & kCFSocketConnectCallBack) != 0 && (s->_f.disabled & kCFSocketConnectCallBack) == 0) turnOnConnect = TRUE;
        }
        // if we want read callbacks and they're not disabled...
        if (readCallBackType != kCFSocketNoCallBack && (callBackTypes & readCallBackType) != 0 && (s->_f.disabled & kCFSocketReadCallBack) == 0) turnOnRead = TRUE;

        // Now turn on the callbacks we've determined that we want on
        if (turnOnRead || turnOnWrite || turnOnConnect) {
            __CFSpinLock(&__CFActiveSocketsLock);
#if defined(USE_V1_RUN_LOOP_SOURCE)
            if (turnOnWrite) {
                if (force) {
                    SInt32 idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
                    if (kCFNotFound == idx) CFArrayAppendValue(__CFWriteSockets, s);
                }
                // If we can tell the socket is writable, just reschedule the v1 source.  Else we need
                // a real notification from the OS, so enable the SocketManager's listening.
                if (__CFSocketCanAcceptBytes(s)) {
                    SetEvent(s->_event);
                    s->_f.writableHint = TRUE;
                } else {
                    if (__CFSocketSetFDForWrite(s)) wakeup = true;
                }
            }
            if (turnOnConnect) __CFSocketSetWholeEventMask(s, s->_oldEventMask | FD_CONNECT);
            if (turnOnRead) __CFSocketSetFDForRead(s);
#else  // !USE_V1_RUN_LOOP_SOURCE
            if (turnOnWrite || turnOnConnect) {
                if (force) {
                    SInt32 idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
                    if (kCFNotFound == idx) CFArrayAppendValue(__CFWriteSockets, s);
                }
                if (__CFSocketSetFDForWrite(s)) wakeup = true;
#if defined(__WIN32__)
                if ((callBackTypes & kCFSocketConnectCallBack) != 0 && !s->_f.connected) __CFSocketFdSet(s->_socket, __CFExceptSocketsFds);
#endif
            }
            if (turnOnRead) {
                if (force) {
                    SInt32 idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
                    if (kCFNotFound == idx) CFArrayAppendValue(__CFReadSockets, s);
                }
                if (__CFSocketSetFDForRead(s)) wakeup = true;
            }
#endif
            if (wakeup && NULL == __CFSocketManagerThread) __CFSocketManagerThread = __CFStartSimpleThread(__CFSocketManager, 0);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    }
    __CFSocketUnlock(s);
    if (wakeup) send(__CFWakeupSocketPair[0], &wakeupChar, sizeof(wakeupChar), 0);
}

void CFSocketEnableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    __CFSocketEnableCallBacks(s, callBackTypes, TRUE, 'r');
}

static void __CFSocketSchedule(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFSocketRef s = info;
    __CFSocketLock(s);
    //??? also need to arrange delivery of all pending data
    if (__CFSocketIsValid(s)) {
        CFArrayAppendValue(s->_runLoops, rl);
        s->_socketSetCount++;
        // Since the v0 source is listened to on the SocketMgr thread, no matter how many modes it
        // is added to we just need to enable it there once (and _socketSetCount gives us a refCount
        // to know when we can finally disable it).
        if (1 == s->_socketSetCount) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "scheduling socket %d\n", s->_socket);
#endif
            __CFSocketEnableCallBacks(s, __CFSocketCallBackTypes(s), TRUE, 's');  // unlocks s
        } else
            __CFSocketUnlock(s);
#if defined(USE_V1_RUN_LOOP_SOURCE)
        // Since the v1 source is listened to in rl on this thread, we need to add it to all modes
        // the v0 source is added to.
        CFRunLoopAddSource(rl, s->_source1, mode);
        CFRunLoopWakeUp(rl);
#endif
    } else
        __CFSocketUnlock(s);
}

static void __CFSocketCancel(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFSocketRef s = info;
    SInt32 idx;
    __CFSocketLock(s);
    s->_socketSetCount--;
    if (0 == s->_socketSetCount) {
        __CFSpinLock(&__CFActiveSocketsLock);
        idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFWriteSockets, idx);
            __CFSocketClearFDForWrite(s);
#if defined(__WIN32__)
            __CFSocketFdClr(s->_socket, __CFExceptSocketsFds);
#endif
        }
#if !defined(USE_V1_RUN_LOOP_SOURCE)
        idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFReadSockets, idx);
            __CFSocketClearFDForRead(s);
        }
#endif
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }
#if defined(USE_V1_RUN_LOOP_SOURCE)
    CFRunLoopRemoveSource(rl, s->_source1, mode);
    CFRunLoopWakeUp(rl);
    if (0 == s->_socketSetCount && s->_socket != INVALID_SOCKET) {
        __CFSocketSetWholeEventMask(s, FD_CLOSE|FD_CONNECT);
    }
#endif
    if (NULL != s->_runLoops) {
        idx = CFArrayGetFirstIndexOfValue(s->_runLoops, CFRangeMake(0, CFArrayGetCount(s->_runLoops)), rl);
        if (0 <= idx) CFArrayRemoveValueAtIndex(s->_runLoops, idx);
    }
    __CFSocketUnlock(s);
}

// Note:  must be called with socket lock held, then returns with it released
// Used by both the v0 and v1 RunLoopSource perform routines
static void __CFSocketDoCallback(CFSocketRef s, CFDataRef data, CFDataRef address, CFSocketNativeHandle sock) {
    CFSocketCallBack callout = NULL;
    void *contextInfo = NULL;
    SInt32 errorCode = 0;
    Boolean readSignalled = false, writeSignalled = false, connectSignalled = false, calledOut = false;
    uint8_t readCallBackType, callBackTypes;
    
    callBackTypes = __CFSocketCallBackTypes(s);
    readCallBackType = __CFSocketReadCallBackType(s);
    readSignalled = __CFSocketIsReadSignalled(s);
    writeSignalled = __CFSocketIsWriteSignalled(s);
    connectSignalled = writeSignalled && !s->_f.connected;
    __CFSocketUnsetReadSignalled(s);
    __CFSocketUnsetWriteSignalled(s);
    callout = s->_callout;
    contextInfo = s->_context.info;
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "entering perform for socket %d with read signalled %d write signalled %d connect signalled %d callback types %d\n", s->_socket, readSignalled, writeSignalled, connectSignalled, callBackTypes);
#endif
    if (writeSignalled) {
        errorCode = s->_errorCode;
        s->_f.connected = TRUE;
    }
    __CFSocketUnlock(s);
    if ((callBackTypes & kCFSocketConnectCallBack) != 0) {
        if (connectSignalled && (!calledOut || CFSocketIsValid(s))) {
            if (errorCode) {
#if defined(LOG_CFSOCKET)
                fprintf(stdout, "perform calling out error %ld to socket %d\n", errorCode, s->_socket);
#endif
                if (callout) callout(s, kCFSocketConnectCallBack, NULL, &errorCode, contextInfo);
                calledOut = true;
            } else {
#if defined(LOG_CFSOCKET)
                fprintf(stdout, "perform calling out connect to socket %d\n", s->_socket);
#endif
                if (callout) callout(s, kCFSocketConnectCallBack, NULL, NULL, contextInfo);
                calledOut = true;
            }
        }
    }
    if (kCFSocketDataCallBack == readCallBackType) {
        if (NULL != data && (!calledOut || CFSocketIsValid(s))) {
            SInt32 datalen = CFDataGetLength(data);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out data of length %ld to socket %d\n", datalen, s->_socket);
#endif
            if (callout) callout(s, kCFSocketDataCallBack, address, data, contextInfo);
            calledOut = true;
            if (0 == datalen) __CFSocketInvalidate(s, true);
        }
    } else if (kCFSocketAcceptCallBack == readCallBackType) {
        if (INVALID_SOCKET != sock && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out accept of socket %d to socket %d\n", sock, s->_socket);
#endif
            if (callout) callout(s, kCFSocketAcceptCallBack, address, &sock, contextInfo);
            calledOut = true;
        }
    } else if (kCFSocketReadCallBack == readCallBackType) {
        if (readSignalled && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out read to socket %d\n", s->_socket);
#endif
            if (callout) callout(s, kCFSocketReadCallBack, NULL, NULL, contextInfo);
            calledOut = true;
        }
    }
    if ((callBackTypes & kCFSocketWriteCallBack) != 0) {
        if (writeSignalled && !errorCode && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out write to socket %d\n", s->_socket);
#endif
            if (callout) callout(s, kCFSocketWriteCallBack, NULL, NULL, contextInfo);
            calledOut = true;
        }
    }
}

#if defined(USE_V1_RUN_LOOP_SOURCE)
static HANDLE __CFSocketGetPort(void *info) {
    CFSocketRef s = info;
    return s->_event;
}

static void __CFSocketPerformV1(void *info) {
    CFSocketRef s = info;
    WSANETWORKEVENTS eventsTranspired;
    uint8_t readCallBackType = __CFSocketReadCallBackType(s);
    CFOptionFlags callBacksSignalled = 0;

    int err = WSAEnumNetworkEvents(s->_socket, s->_event, &eventsTranspired);
    CFAssert2(0 == err, __kCFLogAssertion, "%s(): WSAEnumNetworkEvents failed: %d", __PRETTY_FUNCTION__, WSAGetLastError());
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "socket %d with flags 0x%x disabled 0x%x connected 0x%x received NetworkEvents 0x%lx\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected, eventsTranspired.lNetworkEvents);
#endif
    // Get these bits cleared before any callouts, just as the SocketMgr thread used to
    if (eventsTranspired.lNetworkEvents & FD_READ) {
        __CFSpinLock(&__CFActiveSocketsLock);
        __CFSocketClearFDForRead(s);
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }

    if (eventsTranspired.lNetworkEvents & FD_READ || eventsTranspired.lNetworkEvents & FD_ACCEPT) callBacksSignalled |= readCallBackType;
    if (eventsTranspired.lNetworkEvents & FD_CONNECT || s->_f.writableHint) callBacksSignalled |= kCFSocketWriteCallBack;
    s->_f.writableHint = FALSE;
    CFAssert2(0 == (eventsTranspired.lNetworkEvents & FD_WRITE), __kCFLogAssertion, "%s(): WSAEnumNetworkEvents returned unexpected events: %lx", __PRETTY_FUNCTION__, eventsTranspired.lNetworkEvents);

#if defined(LOG_CFSOCKET)
    // I believe all these errors will be re-found in __CFSocketHandleRead and __CFSocketHandleWrite
    // so we don't need to check for them here.
    if (eventsTranspired.lNetworkEvents & FD_READ && eventsTranspired.iErrorCode[FD_READ_BIT] != 0)
        fprintf(stdout, "socket %d has error %d for FD_READ\n", s->_socket, eventsTranspired.iErrorCode[FD_READ_BIT]);
    if (eventsTranspired.lNetworkEvents & FD_WRITE && eventsTranspired.iErrorCode[FD_WRITE_BIT] != 0)
        fprintf(stdout, "socket %d has error %d for FD_WRITE\n", s->_socket, eventsTranspired.iErrorCode[FD_WRITE_BIT]);
    if (eventsTranspired.lNetworkEvents & FD_CLOSE && eventsTranspired.iErrorCode[FD_CLOSE_BIT] != 0)
        fprintf(stdout, "socket %d has error %d for FD_CLOSE\n", s->_socket, eventsTranspired.iErrorCode[FD_CLOSE_BIT]);
    if (eventsTranspired.lNetworkEvents & FD_CONNECT && eventsTranspired.iErrorCode[FD_CONNECT_BIT] != 0)
        fprintf(stdout, "socket %d has error %d for FD_CONNECT\n", s->_socket, eventsTranspired.iErrorCode[FD_CONNECT_BIT]);
#endif

    if (0 != (eventsTranspired.lNetworkEvents & FD_CLOSE)) s->_f.closeSignaled = TRUE;
    if (0 != (callBacksSignalled & readCallBackType)) __CFSocketHandleRead(s);
    if (0 != (callBacksSignalled & kCFSocketWriteCallBack)) __CFSocketHandleWrite(s, TRUE);
    // FD_CLOSE is edge triggered (sent once).  FD_READ is level triggered (sent as long as there are
    // bytes).  Event after we get FD_CLOSE, if there are still bytes to be read we'll keep getting
    // FD_READ until the pipe is drained.  However, an EOF condition on the socket will -not-
    // trigger an FD_READ, so we must be careful not to stall out after the last bytes are read.
    // Finally, a client may have already noticed the EOF in the Read callout just done, so we don't
    // call him again if the socket has been invalidated.
    // All this implies that once we have seen FD_CLOSE, we need to keep checking for EOF on the read
    // side to give the client one last callback for that case.
    if (__CFSocketIsValid(s) && (eventsTranspired.lNetworkEvents == FD_CLOSE || (s->_f.closeSignaled && !__CFSocketHasBytesToRead(s)))) {
        if (readCallBackType != kCFSocketNoCallBack) {
            __CFSocketHandleRead(s);
        } else if ((__CFSocketCallBackTypes(s) & kCFSocketWriteCallBack) != 0) {
            __CFSocketHandleWrite(s, TRUE);
        }
    }

    // Only reenable callbacks that are auto-reenabled
    __CFSocketLock(s);
    __CFSocketEnableCallBacks(s, callBacksSignalled & s->_f.client, FALSE, 'P');  // unlocks s
}
#endif  // USE_V1_RUN_LOOP_SOURCE

static void __CFSocketPerformV0(void *info) {
    CFSocketRef s = info;
    CFDataRef data = NULL;
    CFDataRef address = NULL;
    CFSocketNativeHandle sock = INVALID_SOCKET;
    uint8_t readCallBackType, callBackTypes;
    CFRunLoopRef rl = NULL;

    __CFSocketLock(s);
    if (!__CFSocketIsValid(s)) {
        __CFSocketUnlock(s);
        return;
    }
    callBackTypes = __CFSocketCallBackTypes(s);
    readCallBackType = __CFSocketReadCallBackType(s);
    CFOptionFlags callBacksSignalled = 0;
    if (__CFSocketIsReadSignalled(s)) callBacksSignalled |= readCallBackType;
    if (__CFSocketIsWriteSignalled(s)) callBacksSignalled |= kCFSocketWriteCallBack;

#if !defined(USE_V1_RUN_LOOP_SOURCE)
    if (kCFSocketDataCallBack == readCallBackType) {
        if (NULL != s->_dataQueue && 0 < CFArrayGetCount(s->_dataQueue)) {
            data = CFArrayGetValueAtIndex(s->_dataQueue, 0);
            CFRetain(data);
            CFArrayRemoveValueAtIndex(s->_dataQueue, 0);
            address = CFArrayGetValueAtIndex(s->_addressQueue, 0);
            CFRetain(address);
            CFArrayRemoveValueAtIndex(s->_addressQueue, 0);
        }
    } else if (kCFSocketAcceptCallBack == readCallBackType) {
        if (NULL != s->_dataQueue && 0 < CFArrayGetCount(s->_dataQueue)) {
            sock = (CFSocketNativeHandle)CFArrayGetValueAtIndex(s->_dataQueue, 0);
            CFArrayRemoveValueAtIndex(s->_dataQueue, 0);
            address = CFArrayGetValueAtIndex(s->_addressQueue, 0);
            CFRetain(address);
            CFArrayRemoveValueAtIndex(s->_addressQueue, 0);
        }
    }
#endif

    __CFSocketDoCallback(s, data, address, sock);	// does __CFSocketUnlock(s)
    if (NULL != data) CFRelease(data);
    if (NULL != address) CFRelease(address);

    __CFSocketLock(s);
#if !defined(USE_V1_RUN_LOOP_SOURCE)
    if (__CFSocketIsValid(s) && kCFSocketNoCallBack != readCallBackType) {
        // if there's still more data, we want to wake back up right away
        if ((kCFSocketDataCallBack == readCallBackType || kCFSocketAcceptCallBack == readCallBackType) && NULL != s->_dataQueue && 0 < CFArrayGetCount(s->_dataQueue)) {
            CFRunLoopSourceSignal(s->_source0);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform short-circuit signaling source for socket %d with flags 0x%x disabled 0x%x connected 0x%x\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected);
#endif
            rl = __CFSocketCopyRunLoopToWakeUp(s);
        }
    }
#endif
    // Only reenable callbacks that are auto-reenabled
    __CFSocketEnableCallBacks(s, callBacksSignalled & s->_f.client, FALSE, 'p');  // unlocks s

    if (NULL != rl) {
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
}

CFRunLoopSourceRef CFSocketCreateRunLoopSource(CFAllocatorRef allocator, CFSocketRef s, CFIndex order) {
    CFRunLoopSourceRef result = NULL;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) {
        if (NULL == s->_source0) {
            CFRunLoopSourceContext context;
#if defined(USE_V1_RUN_LOOP_SOURCE)
            CFRunLoopSourceContext1 context1;
            context1.version = 1;
            context1.info = s;
            context1.retain = CFRetain;
            context1.release = CFRelease;
            context1.copyDescription = CFCopyDescription;
            context1.equal = CFEqual;
            context1.hash = CFHash;
            context1.getPort = __CFSocketGetPort;
            context1.perform = __CFSocketPerformV1;
            s->_source1 = CFRunLoopSourceCreate(allocator, order, (CFRunLoopSourceContext*)&context1);
#endif
            context.version = 0;
            context.info = s;
            context.retain = CFRetain;
            context.release = CFRelease;
            context.copyDescription = CFCopyDescription;
            context.equal = CFEqual;
            context.hash = CFHash;
            context.schedule = __CFSocketSchedule;
            context.cancel = __CFSocketCancel;
            context.perform = __CFSocketPerformV0;
            s->_source0 = CFRunLoopSourceCreate(allocator, order, &context);
        }
        CFRetain(s->_source0);        /* This retain is for the receiver */
        result = s->_source0;
    }
    __CFSocketUnlock(s);
    return result;
}

//??? need timeout, error handling, retries
CFSocketError CFSocketSendData(CFSocketRef s, CFDataRef address, CFDataRef data, CFTimeInterval timeout) {
    const uint8_t *dataptr, *addrptr = NULL;
    SInt32 datalen, addrlen = 0, size = 0;
    CFSocketNativeHandle sock = INVALID_SOCKET;
    struct timeval tv;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    if (address) {
        addrptr = CFDataGetBytePtr(address);
        addrlen = CFDataGetLength(address);
    }
    dataptr = CFDataGetBytePtr(data);
    datalen = CFDataGetLength(data);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) sock = s->_socket;
    __CFSocketUnlock(s);
    if (INVALID_SOCKET != sock) {
        CFRetain(s);
        __CFSocketWriteLock(s);
        tv.tv_sec = (0 >= timeout || INT_MAX <= timeout) ? INT_MAX : (int)(float)floor(timeout);
        tv.tv_usec = (int)((timeout - floor(timeout)) * 1.0E6);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (void *)&tv, sizeof(tv));	// cast for WinSock bad API
        if (NULL != addrptr && 0 < addrlen) {
            size = sendto(sock, dataptr, datalen, 0, (struct sockaddr *)addrptr, addrlen);
        } else {
            size = send(sock, dataptr, datalen, 0);
        }
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "wrote %ld bytes to socket %d\n", size, s->_socket);
#endif
        __CFSocketWriteUnlock(s);
        CFRelease(s);
    }
    return (size > 0) ? kCFSocketSuccess : kCFSocketError;
}

typedef struct {
    CFSocketError *error;
    CFPropertyListRef *value;
    CFDataRef *address;
} __CFSocketNameRegistryResponse;

static void __CFSocketHandleNameRegistryReply(CFSocketRef s, CFSocketCallBackType type, CFDataRef address, const void *data, void *info) {
    CFDataRef replyData = (CFDataRef)data;
    __CFSocketNameRegistryResponse *response = (__CFSocketNameRegistryResponse *)info;
    CFDictionaryRef replyDictionary = NULL;
    CFPropertyListRef value;
    replyDictionary = CFPropertyListCreateFromXMLData(NULL, replyData, kCFPropertyListImmutable, NULL);
    if (NULL != response->error) *(response->error) = kCFSocketError;
    if (NULL != replyDictionary) {
        if (CFGetTypeID((CFTypeRef)replyDictionary) == CFDictionaryGetTypeID() && NULL != (value = CFDictionaryGetValue(replyDictionary, kCFSocketResultKey))) {
            if (NULL != response->error) *(response->error) = kCFSocketSuccess;
            if (NULL != response->value) *(response->value) = CFRetain(value);
            if (NULL != response->address) *(response->address) = address ? CFDataCreateCopy(NULL, address) : NULL;
        }
        CFRelease(replyDictionary);
    }
    CFSocketInvalidate(s);
}

static void __CFSocketSendNameRegistryRequest(CFSocketSignature *signature, CFDictionaryRef requestDictionary, __CFSocketNameRegistryResponse *response, CFTimeInterval timeout) {
    CFDataRef requestData = NULL;
    CFSocketContext context = {0, response, NULL, NULL, NULL};
    CFSocketRef s = NULL;
    CFRunLoopSourceRef source = NULL;
    if (NULL != response->error) *(response->error) = kCFSocketError;
    requestData = CFPropertyListCreateXMLData(NULL, requestDictionary);
    if (NULL != requestData) {
        if (NULL != response->error) *(response->error) = kCFSocketTimeout;
        s = CFSocketCreateConnectedToSocketSignature(NULL, signature, kCFSocketDataCallBack, __CFSocketHandleNameRegistryReply, &context, timeout);
        if (NULL != s) {
            if (kCFSocketSuccess == CFSocketSendData(s, NULL, requestData, timeout)) {
                source = CFSocketCreateRunLoopSource(NULL, s, 0);
                CFRunLoopAddSource(CFRunLoopGetCurrent(), source, __kCFSocketRegistryRequestRunLoopMode);
                CFRunLoopRunInMode(__kCFSocketRegistryRequestRunLoopMode, timeout, false);
                CFRelease(source);
            }
            CFSocketInvalidate(s);
            CFRelease(s);
        }
        CFRelease(requestData);
    }
}

static void __CFSocketValidateSignature(const CFSocketSignature *providedSignature, CFSocketSignature *signature, uint16_t defaultPortNumber) {
    struct sockaddr_in sain, *sainp;
    memset(&sain, 0, sizeof(sain));
#if !defined(__WIN32__)
    sain.sin_len = sizeof(sain);
#endif
    sain.sin_family = AF_INET;
    sain.sin_port = htons(__CFSocketDefaultNameRegistryPortNumber);
    sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (NULL == providedSignature) {
        signature->protocolFamily = PF_INET;
        signature->socketType = SOCK_STREAM;
        signature->protocol = IPPROTO_TCP;
        signature->address = CFDataCreate(NULL, (uint8_t *)&sain, sizeof(sain));
    } else {
        signature->protocolFamily = providedSignature->protocolFamily;
        signature->socketType = providedSignature->socketType;
        signature->protocol = providedSignature->protocol;
        if (0 >= signature->protocolFamily) signature->protocolFamily = PF_INET;
        if (PF_INET == signature->protocolFamily) {
            if (0 >= signature->socketType) signature->socketType = SOCK_STREAM;
            if (0 >= signature->protocol && SOCK_STREAM == signature->socketType) signature->protocol = IPPROTO_TCP;
            if (0 >= signature->protocol && SOCK_DGRAM == signature->socketType) signature->protocol = IPPROTO_UDP;
        }
        if (NULL == providedSignature->address) {
            signature->address = CFDataCreate(NULL, (uint8_t *)&sain, sizeof(sain));
        } else {
            sainp = (struct sockaddr_in *)CFDataGetBytePtr(providedSignature->address);
            if ((int)sizeof(struct sockaddr_in) <= CFDataGetLength(providedSignature->address) && (AF_INET == sainp->sin_family || 0 == sainp->sin_family)) {
#if !defined(__WIN32__)
                sain.sin_len = sizeof(sain);
#endif
                sain.sin_family = AF_INET;
                sain.sin_port = sainp->sin_port;
                if (0 == sain.sin_port) sain.sin_port = htons(defaultPortNumber);
                sain.sin_addr.s_addr = sainp->sin_addr.s_addr;
                if (htonl(INADDR_ANY) == sain.sin_addr.s_addr) sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                signature->address = CFDataCreate(NULL, (uint8_t *)&sain, sizeof(sain));
            } else {
                signature->address = CFRetain(providedSignature->address);
            }
        }
    }
}

CFSocketError CFSocketRegisterValue(const CFSocketSignature *nameServerSignature, CFTimeInterval timeout, CFStringRef name, CFPropertyListRef value) {
    CFSocketSignature signature;
    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(NULL, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFSocketError retval = kCFSocketError;
    __CFSocketNameRegistryResponse response = {&retval, NULL, NULL};
    CFDictionaryAddValue(dictionary, kCFSocketCommandKey, kCFSocketRegisterCommand);
    CFDictionaryAddValue(dictionary, kCFSocketNameKey, name);
    if (NULL != value) CFDictionaryAddValue(dictionary, kCFSocketValueKey, value);
    __CFSocketValidateSignature(nameServerSignature, &signature, __CFSocketDefaultNameRegistryPortNumber);
    __CFSocketSendNameRegistryRequest(&signature, dictionary, &response, timeout);
    CFRelease(dictionary);
    CFRelease(signature.address);
    return retval;
}

CFSocketError CFSocketCopyRegisteredValue(const CFSocketSignature *nameServerSignature, CFTimeInterval timeout, CFStringRef name, CFPropertyListRef *value, CFDataRef *serverAddress) {
    CFSocketSignature signature;
    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(NULL, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFSocketError retval = kCFSocketError;
    __CFSocketNameRegistryResponse response = {&retval, value, serverAddress};
    CFDictionaryAddValue(dictionary, kCFSocketCommandKey, kCFSocketRetrieveCommand);
    CFDictionaryAddValue(dictionary, kCFSocketNameKey, name);
    __CFSocketValidateSignature(nameServerSignature, &signature, __CFSocketDefaultNameRegistryPortNumber);
    __CFSocketSendNameRegistryRequest(&signature, dictionary, &response, timeout);
    CFRelease(dictionary);
    CFRelease(signature.address);
    return retval;
}

CFSocketError CFSocketRegisterSocketSignature(const CFSocketSignature *nameServerSignature, CFTimeInterval timeout, CFStringRef name, const CFSocketSignature *signature) {
    CFSocketSignature validatedSignature;
    CFMutableDataRef data = NULL;
    CFSocketError retval;
    CFIndex length;
    uint8_t bytes[4];
    if (NULL == signature) {
        retval = CFSocketUnregister(nameServerSignature, timeout, name);
    } else {
        __CFSocketValidateSignature(signature, &validatedSignature, 0);
        if (NULL == validatedSignature.address || 0 > validatedSignature.protocolFamily || 255 < validatedSignature.protocolFamily || 0 > validatedSignature.socketType || 255 < validatedSignature.socketType || 0 > validatedSignature.protocol || 255 < validatedSignature.protocol || 0 >= (length = CFDataGetLength(validatedSignature.address)) || 255 < length) {
            retval = kCFSocketError;
        } else {
            data = CFDataCreateMutable(NULL, sizeof(bytes) + length);
            bytes[0] = validatedSignature.protocolFamily;
            bytes[1] = validatedSignature.socketType;
            bytes[2] = validatedSignature.protocol;
            bytes[3] = length;
            CFDataAppendBytes(data, bytes, sizeof(bytes));
            CFDataAppendBytes(data, CFDataGetBytePtr(validatedSignature.address), length);
            retval = CFSocketRegisterValue(nameServerSignature, timeout, name, data);
            CFRelease(data);
        }
        CFRelease(validatedSignature.address);
    }
    return retval;
}

CFSocketError CFSocketCopyRegisteredSocketSignature(const CFSocketSignature *nameServerSignature, CFTimeInterval timeout, CFStringRef name, CFSocketSignature *signature, CFDataRef *nameServerAddress) {
    CFDataRef data = NULL;
    CFSocketSignature returnedSignature;
    const uint8_t *ptr = NULL, *aptr = NULL;
    uint8_t *mptr;
    CFIndex length = 0;
    CFDataRef serverAddress = NULL;
    CFSocketError retval = CFSocketCopyRegisteredValue(nameServerSignature, timeout, name, (CFPropertyListRef *)&data, &serverAddress);
    if (NULL == data || CFGetTypeID(data) != CFDataGetTypeID() || NULL == (ptr = CFDataGetBytePtr(data)) || (length = CFDataGetLength(data)) < 4) retval = kCFSocketError;
    if (kCFSocketSuccess == retval && NULL != signature) {
        returnedSignature.protocolFamily = (SInt32)*ptr++;
        returnedSignature.socketType = (SInt32)*ptr++;
        returnedSignature.protocol = (SInt32)*ptr++;
        ptr++;
        returnedSignature.address = CFDataCreate(NULL, ptr, length - 4);
        __CFSocketValidateSignature(&returnedSignature, signature, 0);
        CFRelease(returnedSignature.address);
        ptr = CFDataGetBytePtr(signature->address);
        if (CFDataGetLength(signature->address) >= (int)sizeof(struct sockaddr_in) && AF_INET == ((struct sockaddr *)ptr)->sa_family && NULL != serverAddress && CFDataGetLength(serverAddress) >= (int)sizeof(struct sockaddr_in) && NULL != (aptr = CFDataGetBytePtr(serverAddress)) && AF_INET == ((struct sockaddr *)aptr)->sa_family) {
            CFMutableDataRef address = CFDataCreateMutableCopy(NULL, CFDataGetLength(signature->address), signature->address);
            mptr = CFDataGetMutableBytePtr(address);
            ((struct sockaddr_in *)mptr)->sin_addr = ((struct sockaddr_in *)aptr)->sin_addr;
            CFRelease(signature->address);
            signature->address = address;
        }
        if (NULL != nameServerAddress) *nameServerAddress = serverAddress ? CFRetain(serverAddress) : NULL;
    }
    if (NULL != data) CFRelease(data);
    if (NULL != serverAddress) CFRelease(serverAddress);
    return retval;
}

CFSocketError CFSocketUnregister(const CFSocketSignature *nameServerSignature, CFTimeInterval timeout, CFStringRef name) {
    return CFSocketRegisterValue(nameServerSignature, timeout, name, NULL);
}

CF_EXPORT void CFSocketSetDefaultNameRegistryPortNumber(uint16_t port) {
    __CFSocketDefaultNameRegistryPortNumber = port;
}

CF_EXPORT uint16_t CFSocketGetDefaultNameRegistryPortNumber(void) {
    return __CFSocketDefaultNameRegistryPortNumber;
}
