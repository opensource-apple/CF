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
#include <winsock.h>
#define EINPROGRESS 36
//#include <errno.h>
#elif defined(__MACH__)
#include <libc.h>
#else
#include <sys/filio.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
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
static CFSpinLock_t __CFActiveSocketsLock = 0; /* controls __CFRead/WriteSockets and __CFRead/WriteSocketsFds */
static CFMutableArrayRef __CFWriteSockets = NULL;
static CFMutableArrayRef __CFReadSockets = NULL;
static CFMutableDataRef __CFWriteSocketsFds = NULL;
static CFMutableDataRef __CFReadSocketsFds = NULL;

static CFSocketNativeHandle __CFWakeupSocketPair[2] = {0, 0};
static void *__CFSocketManagerThread = NULL;

#if defined(__WIN32__)
static Boolean __CFSocketWinSockInitialized = false;
#else
#define CFSOCKET_USE_SOCKETPAIR
#define closesocket(a) close((a))
#define ioctlsocket(a,b,c) ioctl((a),(b),(c))
#endif

static CFTypeID __kCFSocketTypeID = _kCFRuntimeNotATypeID;

struct __CFSocket {
    CFRuntimeBase _base;
    uint32_t _flags;
    CFSpinLock_t _lock;
    CFSpinLock_t _writeLock;
    CFSocketNativeHandle _socket;	/* immutable */
    SInt32 _socketType;
    SInt32 _errorCode;
    CFDataRef _address;
    CFDataRef _peerAddress;
    SInt32 _socketSetCount;
    CFRunLoopSourceRef _source;
    CFMutableArrayRef _runLoops;
    CFSocketCallBack _callout;		/* immutable */
    CFSocketContext _context;		/* immutable */
    CFIndex _maxQueueLen;
    CFMutableArrayRef _dataQueue;
    CFMutableArrayRef _addressQueue;
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
    fd_set* set = CFDataGetBytePtr(fdSet);
    return set ? set->fd_count : 0;
#else
    return NBBY * CFDataGetLength(fdSet);
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
    }
    return error;
#endif
}

static void __CFSocketInitializeSockets(void) {
    UInt32 yes = 1;
#if defined(__WIN32__)
    if (!__CFSocketWinSockInitialized) {
        WORD versionRequested = MAKEWORD(1, 1);
        WSADATA wsaData;
        int errorStatus = WSAStartup(versionRequested, &wsaData);
        if (errorStatus != 0 || LOBYTE(wsaData.wVersion) != LOBYTE(versionRequested) || HIBYTE(wsaData.wVersion) != HIBYTE(versionRequested)) {
            WSACleanup();
            CFLog(0, CFSTR("*** Could not initialize WinSock subsystem!!!"));
        }
        __CFSocketWinSockInitialized = true;
    }
#endif
    __CFWriteSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    __CFReadSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    __CFWriteSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    __CFReadSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
#if defined(__WIN32__)
    CFDataIncreaseLength(__CFWriteSocketsFds, sizeof(u_int) + sizeof(SOCKET));
    CFDataIncreaseLength(__CFReadSocketsFds, sizeof(u_int) + sizeof(SOCKET));
#endif
    if (0 > __CFSocketCreateWakeupSocketPair()) {
        CFLog(0, CFSTR("*** Could not create wakeup socket pair for CFSocket!!!"));
    } else {
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
            for (idx = 0; idx < cnt; idx++) {
                CFRunLoopRef value = (CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, idx);
                CFStringRef currentMode = CFRunLoopCopyCurrentMode(value);
                if (NULL != currentMode && CFRunLoopIsWaiting(value) && CFRunLoopContainsSource(value, s->_source, currentMode)) {
                    CFRelease(currentMode);
                    /* ideally, this would be a run loop which isn't also in a
                    * signaled state for this or another source, but that's tricky;
                    * we move this run loop to the end of the list to scramble them
                    * a bit, and always search from the front */
                    CFArrayRemoveValueAtIndex(s->_runLoops, idx);
                    rl = value;
                    break;
                }
                if (NULL != currentMode) CFRelease(currentMode);
            }
            if (NULL == rl) {	/* didn't choose one above, so choose first */
                rl = (CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, 0);
                CFArrayRemoveValueAtIndex(s->_runLoops, 0);
            }
            CFArrayAppendValue(s->_runLoops, rl);
        }
    }
    if (NULL != rl) CFRetain(rl);
    return rl;
}

static void __CFSocketHandleWrite(CFSocketRef s) {
    SInt32 errorCode = 0;
    int errorSize = sizeof(errorCode);
    CFRunLoopRef rl = NULL;
    CFOptionFlags writeCallBacksAvailable;
    
    if (!CFSocketIsValid(s)) return;
    if (0 != getsockopt(s->_socket, SOL_SOCKET, SO_ERROR, &errorCode, &errorSize)) errorCode = 0;
#if defined(LOG_CFSOCKET)
    if (errorCode) printf("error %d on socket %d\n", errorCode, s->_socket);
#endif
    __CFSocketLock(s);
    writeCallBacksAvailable = __CFSocketCallBackTypes(s) & (kCFSocketWriteCallBack | kCFSocketConnectCallBack);
    if ((s->_flags & kCFSocketConnectCallBack) != 0) writeCallBacksAvailable &= ~kCFSocketConnectCallBack;
    if (!__CFSocketIsValid(s) || (((s->_flags >> 24) & writeCallBacksAvailable) == writeCallBacksAvailable)) {
        __CFSocketUnlock(s);
        return;
    }
    s->_errorCode = errorCode;
    __CFSocketSetWriteSignalled(s);
    CFRunLoopSourceSignal(s->_source);
#if defined(LOG_CFSOCKET)
    printf("write signaling source for socket %d\n", s->_socket);
#endif
    rl = __CFSocketCopyRunLoopToWakeUp(s);
    __CFSocketUnlock(s);
    if (NULL != rl) {
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
}

static void __CFSocketHandleRead(CFSocketRef s) {
    static CFDataRef zeroLengthData = NULL;
    CFRunLoopRef rl = NULL;
    if (NULL == zeroLengthData) zeroLengthData = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    if (!CFSocketIsValid(s)) return;
    if (__CFSocketReadCallBackType(s) == kCFSocketDataCallBack) {
        uint8_t buffer[MAX_DATA_SIZE];
#if !defined(__WIN32__)
        uint8_t name[MAX_SOCKADDR_LEN];
        int namelen = sizeof(name);
#else
        struct sockaddr* name = NULL;
        int namelen = 0;
#endif
        SInt32 recvlen = recvfrom(s->_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)name, &namelen);
        CFDataRef data, address = NULL;
#if defined(LOG_CFSOCKET)
        printf("read %d bytes on socket %d\n", recvlen, s->_socket);
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
        if (0 < recvlen && 0 < s->_socketSetCount && (s->_flags & kCFSocketDataCallBack) != 0 && ((s->_flags >> 24) & kCFSocketDataCallBack) == 0 && (0 == s->_maxQueueLen || CFArrayGetCount(s->_dataQueue) < s->_maxQueueLen)) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            __CFSocketFdSet(s->_socket, __CFReadSocketsFds);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    } else if (__CFSocketReadCallBackType(s) == kCFSocketAcceptCallBack) {
        uint8_t name[MAX_SOCKADDR_LEN];
        int namelen = sizeof(name);
        CFSocketNativeHandle sock = accept(s->_socket, (struct sockaddr *)name, &namelen);
        CFDataRef address;
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
        if (NULL == s->_dataQueue) {
            s->_dataQueue = CFArrayCreateMutable(CFGetAllocator(s), 0, NULL);
        }
        if (NULL == s->_addressQueue) {
            s->_addressQueue = CFArrayCreateMutable(CFGetAllocator(s), 0, &kCFTypeArrayCallBacks);
        }
        CFArrayAppendValue(s->_dataQueue, (void *)sock);
        CFArrayAppendValue(s->_addressQueue, address);
        CFRelease(address);
        if (0 < s->_socketSetCount && (s->_flags & kCFSocketAcceptCallBack) != 0 && ((s->_flags >> 24) & kCFSocketAcceptCallBack) == 0 && (0 == s->_maxQueueLen || CFArrayGetCount(s->_dataQueue) < s->_maxQueueLen)) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            __CFSocketFdSet(s->_socket, __CFReadSocketsFds);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    } else {
        __CFSocketLock(s);
        if (!__CFSocketIsValid(s) || ((s->_flags >> 24) & kCFSocketReadCallBack) != 0) {
            __CFSocketUnlock(s);
            return;
        }
        __CFSocketSetReadSignalled(s);
    }
    CFRunLoopSourceSignal(s->_source);
#if defined(LOG_CFSOCKET)
    printf("read signaling source for socket %d\n", s->_socket);
#endif
    rl = __CFSocketCopyRunLoopToWakeUp(s);
    __CFSocketUnlock(s);
    if (NULL != rl) {
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
}

static void * __CFSocketManager(void * arg) {
    SInt32 nrfds, maxnrfds, fdentries = 1;
#if defined(__WIN32__)
    fd_set *writefds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
    fd_set *readfds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
#else
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
#if defined(LOG_CFSOCKET)
        printf("socket manager looking at read sockets ");
        tempfds = (fd_set *)CFDataGetBytePtr(__CFReadSocketsFds);
        for (idx = 0, cnt = CFArrayGetCount(__CFReadSockets); idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
            if (FD_ISSET(s->_socket, tempfds)) {
                printf("%d ", s->_socket);
            } else {
                printf("(%d) ", s->_socket);
            }
        }
        if (0 < CFArrayGetCount(__CFWriteSockets)) printf(" and write sockets ");
        tempfds = (fd_set *)CFDataGetBytePtr(__CFWriteSocketsFds);
        for (idx = 0, cnt = CFArrayGetCount(__CFWriteSockets); idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
            if (FD_ISSET(s->_socket, tempfds)) {
                printf("%d ", s->_socket);
            } else {
                printf("(%d) ", s->_socket);
            }
        }
        printf("\n");
#endif
        maxnrfds = MAX(__CFSocketFdGetSize(__CFWriteSocketsFds), __CFSocketFdGetSize(__CFReadSocketsFds));
#if defined(__WIN32__)
        if (maxnrfds > fdentries) {
            fdentries = maxnrfds;
            writefds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, writefds, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
            readfds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, readfds, fdentries * sizeof(SOCKET) + sizeof(u_int), 0);
        }
        memset(writefds, 0, fdentries * sizeof(SOCKET) + sizeof(u_int)); 
        memset(readfds, 0, fdentries * sizeof(SOCKET) + sizeof(u_int)); 
#else
        if (maxnrfds > fdentries * (int)NFDBITS) {
            fdentries = (maxnrfds + NFDBITS - 1) / NFDBITS;
            writefds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, writefds, fdentries * sizeof(fd_mask), 0);
            readfds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, readfds, fdentries * sizeof(fd_mask), 0);
        }
        memset(writefds, 0, fdentries * sizeof(fd_mask)); 
        memset(readfds, 0, fdentries * sizeof(fd_mask)); 
#endif
        CFDataGetBytes(__CFWriteSocketsFds, CFRangeMake(0, CFDataGetLength(__CFWriteSocketsFds)), (UInt8 *)writefds); 
        CFDataGetBytes(__CFReadSocketsFds, CFRangeMake(0, CFDataGetLength(__CFReadSocketsFds)), (UInt8 *)readfds); 
        __CFSpinUnlock(&__CFActiveSocketsLock);
    
        nrfds = select(maxnrfds, readfds, writefds, NULL, NULL);
        if (0 == nrfds) continue;
        if (0 > nrfds) {
            SInt32 selectError = thread_errno();
#if defined(LOG_CFSOCKET)
            printf("socket manager received error %d from select\n", selectError);
#endif
            if (EBADF == selectError) {
                CFMutableArrayRef invalidSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
                __CFSpinLock(&__CFActiveSocketsLock);
                cnt = CFArrayGetCount(__CFWriteSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
                    SInt32 flags = fcntl(s->_socket, F_GETFL, 0);
                    if (0 > flags && EBADF == thread_errno()) {
#if defined(LOG_CFSOCKET)
                        printf("socket manager found write socket %d invalid\n", s->_socket);
#endif
                        CFArrayAppendValue(invalidSockets, s);
                    }
                }
                cnt = CFArrayGetCount(__CFReadSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
                    SInt32 flags = fcntl(s->_socket, F_GETFL, 0);
                    if (0 > flags && EBADF == thread_errno()) {
#if defined(LOG_CFSOCKET)
                        printf("socket manager found read socket %d invalid\n", s->_socket);
#endif
                        CFArrayAppendValue(invalidSockets, s);
                    }
                }
                __CFSpinUnlock(&__CFActiveSocketsLock);
        
                cnt = CFArrayGetCount(invalidSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketInvalidate((CFSocketRef)CFArrayGetValueAtIndex(invalidSockets, idx));
                }
                CFRelease(invalidSockets);
            }
            continue;
        }
        if (FD_ISSET(__CFWakeupSocketPair[1], readfds)) {
            recv(__CFWakeupSocketPair[1], buffer, sizeof(buffer), 0);
#if defined(LOG_CFSOCKET)
            printf("socket manager received %c on wakeup socket\n", buffer[0]);
#endif
        }
        __CFSpinLock(&__CFActiveSocketsLock);
        tempfds = NULL;
        cnt = CFArrayGetCount(__CFWriteSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
            CFSocketNativeHandle sock = s->_socket;
            if (INVALID_SOCKET != sock && 0 <= sock && sock < maxnrfds && FD_ISSET(sock, writefds)) {
                CFArrayAppendValue(selectedWriteSockets, s);
                /* socket is removed from fds here, restored by CFSocketReschedule */
                if (!tempfds) tempfds = (fd_set *)CFDataGetMutableBytePtr(__CFWriteSocketsFds);
                FD_CLR(sock, tempfds);
            }    
        }
        tempfds = NULL;
        cnt = CFArrayGetCount(__CFReadSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
            CFSocketNativeHandle sock = s->_socket;
            if (INVALID_SOCKET != sock && 0 <= sock && sock < maxnrfds && FD_ISSET(sock, readfds)) {
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
            printf("socket manager signaling socket %d for write\n", s->_socket);
#endif
            __CFSocketHandleWrite(s);
        }
        if (0 < cnt) CFArrayRemoveAllValues(selectedWriteSockets);
        cnt = CFArrayGetCount(selectedReadSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(selectedReadSockets, idx);
#if defined(LOG_CFSOCKET)
            printf("socket manager signaling socket %d for read\n", s->_socket);
#endif
            __CFSocketHandleRead(s);
        }
        if (0 < cnt) CFArrayRemoveAllValues(selectedReadSockets);
    }
    return (void *)0;
}

static CFStringRef __CFSocketCopyDescription(CFTypeRef cf) {
    CFSocketRef s = (CFSocketRef)cf;
    CFMutableStringRef result;
    CFStringRef contextDesc = NULL;
    void *contextInfo = NULL;
    CFStringRef (*contextCopyDescription)(const void *info) = NULL;
    result = CFStringCreateMutable(CFGetAllocator(s), 0);
    __CFSocketLock(s);
    CFStringAppendFormat(result, NULL, CFSTR("<CFSocket %p [%p]>{valid = %s, type = %d, socket = %d, socket set count = %d\n    callback types = 0x%x, callout = %x, source = %p,\n    run loops = %@,\n    context = "), cf, CFGetAllocator(s), (__CFSocketIsValid(s) ? "Yes" : "No"), s->_socketType, s->_socket, s->_socketSetCount, __CFSocketCallBackTypes(s), s->_callout, s->_source, s->_runLoops);
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
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) {
        s->_maxQueueLen = length;
    }
    __CFSocketUnlock(s);
}

CFSocketError CFSocketConnectToAddress(CFSocketRef s, CFDataRef address, CFTimeInterval timeout) {
    //??? need error handling, retries
    const uint8_t *name;
    SInt32 namelen, result = 0, connect_err = 0, flags;
    UInt32 yes = 1, no = 0;
    Boolean wasBlocking = true;
#if !defined(__WIN32__)
    __CFGenericValidateType(s, __kCFSocketTypeID);
    name = CFDataGetBytePtr(address);
    namelen = CFDataGetLength(address);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && INVALID_SOCKET != s->_socket && NULL != name && 0 < namelen) {
#if !defined(__WIN32__)
        flags = fcntl(s->_socket, F_GETFL, 0);
        if (flags >= 0) wasBlocking = ((flags & O_NONBLOCK) == 0);
#endif
        if (wasBlocking && (timeout > 0.0 || timeout < 0.0)) ioctlsocket(s->_socket, FIONBIO, &yes);
        result = connect(s->_socket, (struct sockaddr *)name, namelen);
#if defined(__WIN32__)
        if (result != 0 && WSAGetLastError() == WSAEWOULDBLOCK) connect_err = EINPROGRESS;
#else
        if (result != 0) connect_err = thread_errno();
#endif
#if defined(LOG_CFSOCKET)
        printf("connection attempt returns %d error %d on socket %d (flags 0x%x blocking %d)\n", result, connect_err, s->_socket, flags, wasBlocking);
#endif
        if (EINPROGRESS == connect_err && timeout >= 0.0) {
            /* select on socket */
            CFMutableDataRef fds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
            SInt32 nrfds;
            struct timeval tv;
            __CFSocketFdSet(s->_socket, fds);
            tv.tv_sec = (0 >= timeout || INT_MAX <= timeout) ? INT_MAX : (int)(float)floor(timeout);
            tv.tv_usec = (int)((timeout - floor(timeout)) * 1.0E6);
            nrfds = select(__CFSocketFdGetSize(fds), NULL, (fd_set *)CFDataGetMutableBytePtr(fds), NULL, &tv);
            result = (nrfds > 0) ? 0 : -1;
            CFRelease(fds);
#if defined(LOG_CFSOCKET)
            printf("timed connection attempt %s on socket %d\n", (result == 0) ? "succeeds" : "fails", s->_socket);
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
            printf("connection attempt continues in background on socket %d\n", s->_socket);
#endif
        }
    }
    __CFSocketUnlock(s);
    //??? should return errno
#endif
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
#endif
    /* Needs to be initialized at this point for Windows, before socket() */
    if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
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
    memory->_flags = ((callBackTypes & (~kCFSocketConnectCallBack)) & (~kCFSocketWriteCallBack)) | kCFSocketCloseOnInvalidate;
    memory->_lock = 0;
    memory->_writeLock = 0;
    memory->_socket = sock;
    if (INVALID_SOCKET == sock || 0 != getsockopt(sock, SOL_SOCKET, SO_TYPE, &(memory->_socketType), &typeSize)) memory->_socketType = 0;
    memory->_errorCode = 0;
    memory->_address = NULL;
    memory->_peerAddress = NULL;
    memory->_socketSetCount = 0;
    memory->_source = NULL;
    if (INVALID_SOCKET != sock) {
        memory->_runLoops = CFArrayCreateMutable(allocator, 0, NULL);
    } else {
        memory->_runLoops = NULL;
    }
    memory->_callout = callout;
    memory->_dataQueue = NULL;
    memory->_addressQueue = NULL;
    memory->_maxQueueLen = 0;
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

void CFSocketInvalidate(CFSocketRef s) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    CFRetain(s);
    __CFSpinLock(&__CFAllSocketsLock);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) {
        SInt32 idx;
        CFRunLoopSourceRef source;
        void *contextInfo = NULL;
        void (*contextRelease)(const void *info) = NULL;
        __CFSocketUnsetValid(s);
        __CFSocketUnsetWriteSignalled(s);
        __CFSocketUnsetReadSignalled(s);
        __CFSpinLock(&__CFActiveSocketsLock);
        if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
        idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFWriteSockets, idx);
            __CFSocketFdClr(s->_socket, __CFWriteSocketsFds);
        }
        idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFReadSockets, idx);
            __CFSocketFdClr(s->_socket, __CFReadSocketsFds);
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
        CFDictionaryRemoveValue(__CFAllSockets, (void *)(s->_socket));
        if ((s->_flags & kCFSocketCloseOnInvalidate) != 0) closesocket(s->_socket);
        s->_socket = INVALID_SOCKET;
        if (NULL != s->_peerAddress) {
            CFRelease(s->_peerAddress);
            s->_peerAddress = NULL;
        }
        if (NULL != s->_dataQueue) {
            CFRelease(s->_dataQueue);
            s->_dataQueue = NULL;
        }
        if (NULL != s->_addressQueue) {
            CFRelease(s->_addressQueue);
            s->_addressQueue = NULL;
        }
        s->_socketSetCount = 0;
        for (idx = CFArrayGetCount(s->_runLoops); idx--;) {
            CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(s->_runLoops, idx));
        }
        CFRelease(s->_runLoops);
        s->_runLoops = NULL;
        source = s->_source;
        s->_source = NULL;
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
        if (NULL != source) {
            CFRunLoopSourceInvalidate(source);
            CFRelease(source);
        }
    } else {
        __CFSocketUnlock(s);
    }
    __CFSpinUnlock(&__CFAllSocketsLock);
    CFRelease(s);
}

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
    return (s->_flags & (~(kCFSocketConnectCallBack | (0xff << 24))));
}

void CFSocketSetSocketFlags(CFSocketRef s, CFOptionFlags flags) {
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    flags &= ~(kCFSocketConnectCallBack | (0xff << 24));
#if defined(LOG_CFSOCKET)
        printf("setting flags for socket %d with %x, flags going from %x to %x\n", s->_socket, flags, s->_flags, (s->_flags & (kCFSocketConnectCallBack | (0xff << 24))) | flags);
#endif
    s->_flags &= (kCFSocketConnectCallBack | (0xff << 24));
    s->_flags |= flags;
    __CFSocketUnlock(s);
}

void CFSocketDisableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes) {
    Boolean wakeup = false;
    uint8_t c = 'u', readCallBackType;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && 0 < s->_socketSetCount) {
        callBackTypes &= __CFSocketCallBackTypes(s);
        readCallBackType = __CFSocketReadCallBackType(s);
        s->_flags |= (callBackTypes << 24);
#if defined(LOG_CFSOCKET)
        printf("unscheduling socket %d with flags 0x%x for types 0x%x\n", s->_socket, s->_flags, callBackTypes);
#endif
        __CFSpinLock(&__CFActiveSocketsLock);
        if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
        if ((readCallBackType == kCFSocketAcceptCallBack) || !__CFSocketIsConnectionOriented(s)) s->_flags |= kCFSocketConnectCallBack;
        if (((callBackTypes & kCFSocketWriteCallBack) != 0) || (((callBackTypes & kCFSocketConnectCallBack) != 0) && ((s->_flags & kCFSocketConnectCallBack) == 0))) {
            if (__CFSocketFdClr(s->_socket, __CFWriteSocketsFds)) {
                // do not wake up the socket manager thread if all relevant write callbacks are disabled
                CFOptionFlags writeCallBacksAvailable = __CFSocketCallBackTypes(s) & (kCFSocketWriteCallBack | kCFSocketConnectCallBack);
                if ((s->_flags & kCFSocketConnectCallBack) != 0) writeCallBacksAvailable &= ~kCFSocketConnectCallBack;
                if (((s->_flags >> 24) & writeCallBacksAvailable) != writeCallBacksAvailable) wakeup = true;
            }
        }
        if (readCallBackType != kCFSocketNoCallBack && (callBackTypes & readCallBackType) != 0) {
            if (__CFSocketFdClr(s->_socket, __CFReadSocketsFds)) {
                // do not wake up the socket manager thread if callback type is read
                if (readCallBackType != kCFSocketReadCallBack) wakeup = true;
            }
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }
    __CFSocketUnlock(s);
    if (wakeup) {
        if (NULL == __CFSocketManagerThread) {
            __CFSocketManagerThread = __CFStartSimpleThread(__CFSocketManager, 0);
        }
        send(__CFWakeupSocketPair[0], &c, sizeof(c), 0);
    }
}

void CFSocketEnableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes) {
    Boolean wakeup = false;
    uint8_t c = 'r', readCallBackType;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && 0 < s->_socketSetCount) {
        callBackTypes &= __CFSocketCallBackTypes(s);
        readCallBackType = __CFSocketReadCallBackType(s);
        s->_flags &= ~(callBackTypes << 24);
#if defined(LOG_CFSOCKET)
        printf("rescheduling socket %d with flags 0x%x for types 0x%x\n", s->_socket, s->_flags, callBackTypes);
#endif
        __CFSpinLock(&__CFActiveSocketsLock);
        if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
        /* we will wait for connection only for connection-oriented, non-rendezvous sockets that are not already connected */
        if ((readCallBackType == kCFSocketAcceptCallBack) || !__CFSocketIsConnectionOriented(s)) s->_flags |= kCFSocketConnectCallBack;
        if (((callBackTypes & kCFSocketWriteCallBack) != 0) || (((callBackTypes & kCFSocketConnectCallBack) != 0) && ((s->_flags & kCFSocketConnectCallBack) == 0))) {
            SInt32 idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
            if (kCFNotFound == idx) CFArrayAppendValue(__CFWriteSockets, s);
            if (__CFSocketFdSet(s->_socket, __CFWriteSocketsFds)) wakeup = true;
        }
        if (readCallBackType != kCFSocketNoCallBack && (callBackTypes & readCallBackType) != 0) {
            SInt32 idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
            if (kCFNotFound == idx) CFArrayAppendValue(__CFReadSockets, s);
            if (__CFSocketFdSet(s->_socket, __CFReadSocketsFds)) wakeup = true;
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }
    __CFSocketUnlock(s);
    if (wakeup) {
        if (NULL == __CFSocketManagerThread) {
            __CFSocketManagerThread = __CFStartSimpleThread(__CFSocketManager, 0);
        }
        send(__CFWakeupSocketPair[0], &c, sizeof(c), 0);
    }
}

static void __CFSocketSchedule(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFSocketRef s = info;
    Boolean wakeup = false;
    uint8_t c = 's', readCallBackType, callBackTypes;
    __CFSocketLock(s);
    //??? also need to arrange delivery of all pending data
    if (__CFSocketIsValid(s)) {
        CFArrayAppendValue(s->_runLoops, rl);
        s->_socketSetCount++;
        if (1 == s->_socketSetCount) {
#if defined(LOG_CFSOCKET)
    printf("scheduling socket %d\n", s->_socket);
#endif
            callBackTypes = __CFSocketCallBackTypes(s);
            readCallBackType = __CFSocketReadCallBackType(s);
            __CFSpinLock(&__CFActiveSocketsLock);
            if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
            /* we will wait for connection only for connection-oriented, non-rendezvous sockets that are not already connected */
            if ((readCallBackType == kCFSocketAcceptCallBack) || !__CFSocketIsConnectionOriented(s)) s->_flags |= kCFSocketConnectCallBack;
            if (((callBackTypes & kCFSocketWriteCallBack) != 0) || (((callBackTypes & kCFSocketConnectCallBack) != 0) && ((s->_flags & kCFSocketConnectCallBack) == 0))) {
                SInt32 idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
                if (kCFNotFound == idx) CFArrayAppendValue(__CFWriteSockets, s);
                if (__CFSocketFdSet(s->_socket, __CFWriteSocketsFds)) wakeup = true;
            }
            if (readCallBackType != kCFSocketNoCallBack) {
                SInt32 idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
                if (kCFNotFound == idx) CFArrayAppendValue(__CFReadSockets, s);
                if (__CFSocketFdSet(s->_socket, __CFReadSocketsFds)) wakeup = true;
            }
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    }
    __CFSocketUnlock(s);
    if (wakeup) {
        if (NULL == __CFSocketManagerThread) {
            __CFSocketManagerThread = __CFStartSimpleThread(__CFSocketManager, 0);
        }
        send(__CFWakeupSocketPair[0], &c, sizeof(c), 0);
    }
}

static void __CFSocketCancel(void *info, CFRunLoopRef rl, CFStringRef mode) {
    CFSocketRef s = info;
    SInt32 idx;
    __CFSocketLock(s);
    s->_socketSetCount--;
    if (0 == s->_socketSetCount) {
        __CFSpinLock(&__CFActiveSocketsLock);
        if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
        idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFWriteSockets, idx);
            __CFSocketFdClr(s->_socket, __CFWriteSocketsFds);
        }
        idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFReadSockets, idx);
            __CFSocketFdClr(s->_socket, __CFReadSocketsFds);
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }
    if (NULL != s->_runLoops) {
        idx = CFArrayGetFirstIndexOfValue(s->_runLoops, CFRangeMake(0, CFArrayGetCount(s->_runLoops)), rl);
        if (0 <= idx) CFArrayRemoveValueAtIndex(s->_runLoops, idx);
    }
    __CFSocketUnlock(s);
}

static void __CFSocketPerform(void *info) {
    CFSocketRef s = info;
    CFDataRef data = NULL;
    CFDataRef address = NULL;
    CFSocketNativeHandle sock = INVALID_SOCKET;
    CFSocketCallBack callout = NULL;
    void *contextInfo = NULL;
    SInt32 errorCode = 0;
    Boolean wakeup = false, readSignalled = false, writeSignalled = false, connectSignalled = false, calledOut = false;
    uint8_t c = 'p', readCallBackType, callBackTypes;
    CFRunLoopRef rl = NULL;
    
    __CFSocketLock(s);
    if (!__CFSocketIsValid(s)) {
        __CFSocketUnlock(s);
        return;
    }
    callBackTypes = __CFSocketCallBackTypes(s);
    readCallBackType = __CFSocketReadCallBackType(s);
    readSignalled = __CFSocketIsReadSignalled(s);
    writeSignalled = __CFSocketIsWriteSignalled(s);
    connectSignalled = writeSignalled && ((s->_flags & kCFSocketConnectCallBack) == 0);
    __CFSocketUnsetReadSignalled(s);
    __CFSocketUnsetWriteSignalled(s);
    callout = s->_callout;
    contextInfo = s->_context.info;
#if defined(LOG_CFSOCKET)
    printf("entering perform for socket %d with read signalled %d write signalled %d connect signalled %d callback types %d\n", s->_socket, readSignalled, writeSignalled, connectSignalled, callBackTypes);
#endif
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
    if (writeSignalled) {
        errorCode = s->_errorCode;
        s->_flags |= kCFSocketConnectCallBack;
    }
    __CFSocketUnlock(s);
    if ((callBackTypes & kCFSocketConnectCallBack) != 0) {
        if (connectSignalled && (!calledOut || CFSocketIsValid(s))) {
            if (errorCode) {
#if defined(LOG_CFSOCKET)
                printf("perform calling out error %d to socket %d\n", errorCode, s->_socket);
#endif
                if (callout) callout(s, kCFSocketConnectCallBack, NULL, &errorCode, contextInfo);
                calledOut = true;
            } else {
#if defined(LOG_CFSOCKET)
                printf("perform calling out connect to socket %d\n", s->_socket);
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
            printf("perform calling out data of length %d to socket %d\n", datalen, s->_socket);
#endif
            if (callout) callout(s, kCFSocketDataCallBack, address, data, contextInfo);
            calledOut = true;
            CFRelease(data);
            CFRelease(address);
            if (0 == datalen) CFSocketInvalidate(s);
        }
    } else if (kCFSocketAcceptCallBack == readCallBackType) {
        if (INVALID_SOCKET != sock && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            printf("perform calling out accept of socket %d to socket %d\n", sock, s->_socket);
#endif
            if (callout) callout(s, kCFSocketAcceptCallBack, address, &sock, contextInfo);
            calledOut = true;
            CFRelease(address);
        }
    } else if (kCFSocketReadCallBack == readCallBackType) {
        if (readSignalled && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            printf("perform calling out read to socket %d\n", s->_socket);
#endif
            if (callout) callout(s, kCFSocketReadCallBack, NULL, NULL, contextInfo);
            calledOut = true;
        }
    }
    if ((callBackTypes & kCFSocketWriteCallBack) != 0) {
        if (writeSignalled && !errorCode && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            printf("perform calling out write to socket %d\n", s->_socket);
#endif
            if (callout) callout(s, kCFSocketWriteCallBack, NULL, NULL, contextInfo);
            calledOut = true;
        }
    }
    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && kCFSocketNoCallBack != readCallBackType) {
        if ((kCFSocketDataCallBack == readCallBackType || kCFSocketAcceptCallBack == readCallBackType) && NULL != s->_dataQueue && 0 < CFArrayGetCount(s->_dataQueue)) {
            CFRunLoopSourceSignal(s->_source);
#if defined(LOG_CFSOCKET)
            printf("perform signaling source for socket %d with flags 0x%x\n", s->_socket, s->_flags);
#endif
            rl = __CFSocketCopyRunLoopToWakeUp(s);
        }
        if (readSignalled && 0 < s->_socketSetCount && (s->_flags & readCallBackType) != 0 && ((s->_flags >> 24) & readCallBackType) == 0) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            if (__CFSocketFdSet(s->_socket, __CFReadSocketsFds)) wakeup = true;
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    }
    if (__CFSocketIsValid(s) && (callBackTypes & kCFSocketWriteCallBack) != 0) {
        if (writeSignalled && 0 < s->_socketSetCount && (s->_flags & kCFSocketWriteCallBack) != 0 && ((s->_flags >> 24) & kCFSocketWriteCallBack) == 0) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            if (__CFSocketFdSet(s->_socket, __CFWriteSocketsFds)) wakeup = true;
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    }
    __CFSocketUnlock(s);
    if (wakeup) {
        if (NULL == __CFSocketManagerThread) {
            __CFSocketManagerThread = __CFStartSimpleThread(__CFSocketManager, 0);
        }
        send(__CFWakeupSocketPair[0], &c, sizeof(c), 0);
    }
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
        if (NULL == s->_source) {
            CFRunLoopSourceContext context;
            context.version = 0;
            context.info = (void *)s;
            context.retain = (const void *(*)(const void *))CFRetain;
            context.release = (void (*)(const void *))CFRelease;
            context.copyDescription = (CFStringRef (*)(const void *))CFCopyDescription;
            context.equal = (Boolean (*)(const void *, const void *))CFEqual;
            context.hash = (CFHashCode (*)(const void *))CFHash;
            context.schedule = __CFSocketSchedule;
            context.cancel = __CFSocketCancel;
            context.perform = __CFSocketPerform;
            s->_source = CFRunLoopSourceCreate(allocator, order, &context);
        }
        CFRetain(s->_source);        /* This retain is for the receiver */
        result = s->_source;
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
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (NULL != addrptr && 0 < addrlen) {
            size = sendto(sock, dataptr, datalen, 0, (struct sockaddr *)addrptr, addrlen);
        } else {
            size = send(sock, dataptr, datalen, 0);
        }
#if defined(LOG_CFSOCKET)
        printf("wrote %d bytes to socket %d\n", size, s->_socket);
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
    memset(&sin, 0, sizeof(sain));
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
