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
/*	CFSocket.c
	Copyright (c) 1999-2007 Apple Inc.  All rights reserved.
	Responsibility: Doug Davidson
*/

#define _DARWIN_UNLIMITED_SELECT 1

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
#include <libc.h>
#include <dlfcn.h>
#include "auto_stubs.h"

// On Mach we use a v0 RunLoopSource to make client callbacks.  That source is signalled by a
// separate SocketManager thread who uses select() to watch the sockets' fds.

//#define LOG_CFSOCKET

#define INVALID_SOCKET (CFSocketNativeHandle)(-1)

enum {
    kCFSocketLeaveErrors = 64   // candidate for publicization in future
};

static uint16_t __CFSocketDefaultNameRegistryPortNumber = 2454;

CONST_STRING_DECL(kCFSocketCommandKey, "Command")
CONST_STRING_DECL(kCFSocketNameKey, "Name")
CONST_STRING_DECL(kCFSocketValueKey, "Value")
CONST_STRING_DECL(kCFSocketResultKey, "Result")
CONST_STRING_DECL(kCFSocketErrorKey, "Error")
CONST_STRING_DECL(kCFSocketRegisterCommand, "Register")
CONST_STRING_DECL(kCFSocketRetrieveCommand, "Retrieve")
CONST_STRING_DECL(__kCFSocketRegistryRequestRunLoopMode, "CFSocketRegistryRequest")

#define closesocket(a) close((a))
#define ioctlsocket(a,b,c) ioctl((a),(b),(c))

CF_INLINE int __CFSocketLastError(void) {
    return thread_errno();
}

CF_INLINE CFIndex __CFSocketFdGetSize(CFDataRef fdSet) {
    return NBBY * CFDataGetLength(fdSet);
}

CF_INLINE Boolean __CFSocketFdSet(CFSocketNativeHandle sock, CFMutableDataRef fdSet) {
    /* returns true if a change occurred, false otherwise */
    Boolean retval = false;
    if (INVALID_SOCKET != sock && 0 <= sock) {
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
    }
    return retval;
}


#define NEW_SOCKET 0
#if NEW_SOCKET

__private_extern__ void __CFSocketInitialize(void) {}

#else

#define MAX_SOCKADDR_LEN 256
#define MAX_DATA_SIZE 65535
#define MAX_CONNECTION_ORIENTED_DATA_SIZE 32768

/* locks are to be acquired in the following order:
   (1) __CFAllSocketsLock
   (2) an individual CFSocket's lock
   (3) __CFActiveSocketsLock
*/
static CFSpinLock_t __CFAllSocketsLock = CFSpinLockInit; /* controls __CFAllSockets */
static CFMutableDictionaryRef __CFAllSockets = NULL;
static CFSpinLock_t __CFActiveSocketsLock = CFSpinLockInit; /* controls __CFRead/WriteSockets, __CFRead/WriteSocketsFds, __CFSocketManagerThread, and __CFSocketManagerIteration */
static volatile UInt32 __CFSocketManagerIteration = 0;
static CFMutableArrayRef __CFWriteSockets = NULL;
static CFMutableArrayRef __CFReadSockets = NULL;
static CFMutableDataRef __CFWriteSocketsFds = NULL;
static CFMutableDataRef __CFReadSocketsFds = NULL;
static CFDataRef zeroLengthData = NULL;
static Boolean __CFReadSocketsTimeoutInvalid = true;  /* rebuild the timeout value before calling select */

static CFSocketNativeHandle __CFWakeupSocketPair[2] = {INVALID_SOCKET, INVALID_SOCKET};
static void *__CFSocketManagerThread = NULL;

static CFTypeID __kCFSocketTypeID = _kCFRuntimeNotATypeID;
static void __CFSocketDoCallback(CFSocketRef s, CFDataRef data, CFDataRef address, CFSocketNativeHandle sock);

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
    CFMutableArrayRef _dataQueue;	// queues to pass data from SocketMgr thread
    CFMutableArrayRef _addressQueue;
	
	struct timeval _readBufferTimeout;
	CFMutableDataRef _readBuffer;
	CFIndex _bytesToBuffer;			/* is length of _readBuffer */
	CFIndex _bytesToBufferPos;		/* where the next _CFSocketRead starts from */
	CFIndex _bytesToBufferReadPos;	/* Where the buffer will next be read into (always after _bytesToBufferPos, but less than _bytesToBuffer) */
	Boolean _atEOF;
    int _bufferedReadError;
	
	CFMutableDataRef _leftoverBytes;
};

/* Bit 6 in the base reserved bits is used for write-signalled state (mutable) */
/* Bit 5 in the base reserved bits is used for read-signalled state (mutable) */
/* Bit 4 in the base reserved bits is used for invalid state (mutable) */
/* Bits 0-3 in the base reserved bits are used for callback types (immutable) */
/* Of this, bits 0-1 are used for the read callback type. */

CF_INLINE Boolean __CFSocketIsWriteSignalled(CFSocketRef s) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 6, 6);
}

CF_INLINE void __CFSocketSetWriteSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 6, 6, 1);
}

CF_INLINE void __CFSocketUnsetWriteSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 6, 6, 0);
}

CF_INLINE Boolean __CFSocketIsReadSignalled(CFSocketRef s) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 5, 5);
}

CF_INLINE void __CFSocketSetReadSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 5, 5, 1);
}

CF_INLINE void __CFSocketUnsetReadSignalled(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 5, 5, 0);
}

CF_INLINE Boolean __CFSocketIsValid(CFSocketRef s) {
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 4, 4);
}

CF_INLINE void __CFSocketSetValid(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 4, 4, 1);
}

CF_INLINE void __CFSocketUnsetValid(CFSocketRef s) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 4, 4, 0);
}

CF_INLINE uint8_t __CFSocketCallBackTypes(CFSocketRef s) {
    return (uint8_t)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 3, 0);
}

CF_INLINE uint8_t __CFSocketReadCallBackType(CFSocketRef s) {
    return (uint8_t)__CFBitfieldGetValue(((const CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 1, 0);
}

CF_INLINE void __CFSocketSetCallBackTypes(CFSocketRef s, uint8_t types) {
    __CFBitfieldSetValue(((CFRuntimeBase *)s)->_cfinfo[CF_INFO_BITS], 3, 0, types & 0xF);
}

CF_INLINE void __CFSocketLock(CFSocketRef s) {
    __CFSpinLock(&(s->_lock));
}

CF_INLINE void __CFSocketUnlock(CFSocketRef s) {
    __CFSpinUnlock(&(s->_lock));
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
    if (__CFSocketIsValid(s) && NULL == s->_address && INVALID_SOCKET != s->_socket && 0 == getsockname(s->_socket, (struct sockaddr *)name, (socklen_t *)&namelen) && NULL != name && 0 < namelen) {
        s->_address = CFDataCreate(CFGetAllocator(s), name, namelen);
    }
}

CF_INLINE void __CFSocketEstablishPeerAddress(CFSocketRef s) {
    /* socket should already be locked */
    uint8_t name[MAX_SOCKADDR_LEN];
    int namelen = sizeof(name);
    if (__CFSocketIsValid(s) && NULL == s->_peerAddress && INVALID_SOCKET != s->_socket && 0 == getpeername(s->_socket, (struct sockaddr *)name, (socklen_t *)&namelen) && NULL != name && 0 < namelen) {
        s->_peerAddress = CFDataCreate(CFGetAllocator(s), name, namelen);
    }
}

static Boolean __CFNativeSocketIsValid(CFSocketNativeHandle sock) {
    SInt32 flags = fcntl(sock, F_GETFL, 0);
    return !(0 > flags && EBADF == thread_errno());
}

CF_INLINE Boolean __CFSocketFdClr(CFSocketNativeHandle sock, CFMutableDataRef fdSet) {
    /* returns true if a change occurred, false otherwise */
    Boolean retval = false;
    if (INVALID_SOCKET != sock && 0 <= sock) {
        CFIndex numFds = NBBY * CFDataGetLength(fdSet);
        fd_mask *fds_bits;
        if (sock < numFds) {
            fds_bits = (fd_mask *)CFDataGetMutableBytePtr(fdSet);
            if (FD_ISSET(sock, (fd_set *)fds_bits)) {
                retval = true;
                FD_CLR(sock, (fd_set *)fds_bits);
            }
        }
    }
    return retval;
}

static SInt32 __CFSocketCreateWakeupSocketPair(void) {
    return socketpair(PF_LOCAL, SOCK_DGRAM, 0, __CFWakeupSocketPair);
}


// Version 0 RunLoopSources set a mask in an FD set to control what socket activity we hear about.
CF_INLINE Boolean __CFSocketSetFDForRead(CFSocketRef s) {
    return __CFSocketFdSet(s->_socket, __CFReadSocketsFds);
}

CF_INLINE Boolean __CFSocketClearFDForRead(CFSocketRef s) {
    return __CFSocketFdClr(s->_socket, __CFReadSocketsFds);
}

CF_INLINE Boolean __CFSocketSetFDForWrite(CFSocketRef s) {
    return __CFSocketFdSet(s->_socket, __CFWriteSocketsFds);
}

CF_INLINE Boolean __CFSocketClearFDForWrite(CFSocketRef s) {
    return __CFSocketFdClr(s->_socket, __CFWriteSocketsFds);
}


// CFNetwork needs to call this, especially for Win32 to get WSAStartup
static void __CFSocketInitializeSockets(void) {
    __CFWriteSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    __CFReadSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    __CFWriteSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    __CFReadSocketsFds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    zeroLengthData = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
    if (0 > __CFSocketCreateWakeupSocketPair()) {
        CFLog(kCFLogLevelWarning, CFSTR("*** Could not create wakeup socket pair for CFSocket!!!"));
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
    if (0 != (s->_f.client & kCFSocketLeaveErrors) || 0 != getsockopt(s->_socket, SOL_SOCKET, SO_ERROR, (void *)&errorCode, (socklen_t *)&errorSize)) errorCode = 0;	// cast for WinSock bad API
#if defined(LOG_CFSOCKET)
    if (errorCode) fprintf(stdout, "error %ld on socket %d\n", errorCode, s->_socket);
#endif /* LOG_CFSOCKET */
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
#endif /* LOG_CFSOCKET */
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

static void __CFSocketHandleRead(CFSocketRef s, Boolean causedByTimeout)
{
    CFDataRef data = NULL, address = NULL;
    CFSocketNativeHandle sock = INVALID_SOCKET;
    if (!CFSocketIsValid(s)) return;
    if (__CFSocketReadCallBackType(s) == kCFSocketDataCallBack) {
        uint8_t bufferArray[MAX_CONNECTION_ORIENTED_DATA_SIZE], *buffer;
        uint8_t name[MAX_SOCKADDR_LEN];
        int namelen = sizeof(name);
        SInt32 recvlen = 0;
        if (__CFSocketIsConnectionOriented(s)) {
            buffer = bufferArray;
            recvlen = recvfrom(s->_socket, buffer, MAX_CONNECTION_ORIENTED_DATA_SIZE, 0, (struct sockaddr *)name, (socklen_t *)&namelen);
        } else {
            buffer = malloc(MAX_DATA_SIZE);
            if (buffer) recvlen = recvfrom(s->_socket, buffer, MAX_DATA_SIZE, 0, (struct sockaddr *)name, (socklen_t *)&namelen);
        }
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "read %ld bytes on socket %d\n", recvlen, s->_socket);
#endif /* LOG_CFSOCKET */
        if (0 >= recvlen) {
            //??? should return error if <0
            /* zero-length data is the signal for perform to invalidate */
            data = CFRetain(zeroLengthData);
        } else {
            data = CFDataCreate(CFGetAllocator(s), buffer, recvlen);
        }
        if (buffer && buffer != bufferArray) free(buffer);
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
        if (0 < recvlen
            && (s->_f.client & kCFSocketDataCallBack) != 0 && (s->_f.disabled & kCFSocketDataCallBack) == 0
            && __CFSocketIsScheduled(s)
        ) {
            __CFSpinLock(&__CFActiveSocketsLock);
            /* restore socket to fds */
            __CFSocketSetFDForRead(s);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    } else if (__CFSocketReadCallBackType(s) == kCFSocketAcceptCallBack) {
        uint8_t name[MAX_SOCKADDR_LEN];
        int namelen = sizeof(name);
        sock = accept(s->_socket, (struct sockaddr *)name, (socklen_t *)&namelen);
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
        CFArrayAppendValue(s->_dataQueue, (void *)(uintptr_t)sock);
        CFArrayAppendValue(s->_addressQueue, address);
        CFRelease(address);
        if ((s->_f.client & kCFSocketAcceptCallBack) != 0 && (s->_f.disabled & kCFSocketAcceptCallBack) == 0
            && __CFSocketIsScheduled(s)
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
		
		if (causedByTimeout) {
#if defined(LOG_CFSOCKET)
			fprintf(stdout, "TIMEOUT RECEIVED - WILL SIGNAL IMMEDIATELY TO FLUSH (%d buffered)\n", s->_bytesToBufferPos);
#endif /* LOG_CFSOCKET */
            /* we've got a timeout, but no bytes read.  Ignore the timeout. */
            if (s->_bytesToBufferPos == 0) {
#if defined(LOG_CFSOCKET)
                fprintf(stdout, "TIMEOUT - but no bytes, restoring to active set\n");
                fflush(stdout);
#endif

                __CFSpinLock(&__CFActiveSocketsLock);
                /* restore socket to fds */
                __CFSocketSetFDForRead(s);
                __CFSpinUnlock(&__CFActiveSocketsLock);
                __CFSocketUnlock(s);
                return;
            }
		} else if (s->_bytesToBuffer != 0 && ! s->_atEOF) {
			UInt8* base;
			CFIndex ctRead;
			CFIndex ctRemaining = s->_bytesToBuffer - s->_bytesToBufferPos;

			/* if our buffer has room, we go ahead and buffer */
			if (ctRemaining > 0) {
				base = CFDataGetMutableBytePtr(s->_readBuffer);
			
				do {
					ctRead = read(CFSocketGetNative(s), &base[s->_bytesToBufferPos], ctRemaining);
				} while (ctRead == -1 && errno == EAGAIN);

				switch (ctRead) {
				case -1:
					s->_bufferedReadError = errno;
					s->_atEOF = true;
#if defined(LOG_CFSOCKET)
					fprintf(stderr, "BUFFERED READ GOT ERROR %d\n", errno);
#endif /* LOG_CFSOCKET */
					break;

				case 0:
	#if defined(LOG_CFSOCKET)
					fprintf(stdout, "DONE READING (EOF) - GOING TO SIGNAL\n");
	#endif /* LOG_CFSOCKET */
					s->_atEOF = true;
					break;
			
				default:
					s->_bytesToBufferPos += ctRead;
					if (s->_bytesToBuffer != s->_bytesToBufferPos) {
	#if defined(LOG_CFSOCKET)
						fprintf(stdout, "READ %d - need %d MORE - GOING BACK FOR MORE\n", ctRead, s->_bytesToBuffer - s->_bytesToBufferPos);
	#endif /* LOG_CFSOCKET */
						__CFSpinLock(&__CFActiveSocketsLock);
						/* restore socket to fds */
						__CFSocketSetFDForRead(s);
						__CFSpinUnlock(&__CFActiveSocketsLock);
						__CFSocketUnlock(s);
						return;
					} else {
	#if defined(LOG_CFSOCKET)
						fprintf(stdout, "DONE READING (read %d bytes) - GOING TO SIGNAL\n", ctRead);
	#endif /* LOG_CFSOCKET */
					}
				}
			}
		}

		__CFSocketSetReadSignalled(s);
    }
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "read signaling source for socket %d\n", s->_socket);
#endif /* LOG_CFSOCKET */
    CFRunLoopSourceSignal(s->_source0);
    CFRunLoopRef rl = __CFSocketCopyRunLoopToWakeUp(s);
    __CFSocketUnlock(s);
    if (NULL != rl) {
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
}

static struct timeval* intervalToTimeval(CFTimeInterval timeout, struct timeval* tv)
{
    if (timeout == 0.0)
        timerclear(tv);
    else {
        tv->tv_sec = (0 >= timeout || INT_MAX <= timeout) ? INT_MAX : (int)(float)floor(timeout);
        tv->tv_usec = (int)((timeout - floor(timeout)) * 1.0E6);
    }
    return tv;
}

/* note that this returns a pointer to the min value, which won't have changed during
 the dictionary apply, since we've got the active sockets lock held */
static void _calcMinTimeout_locked(const void* val, void* ctxt)
{
	CFSocketRef s = (CFSocketRef) val;
	struct timeval** minTime = (struct timeval**) ctxt;
	if (timerisset(&s->_readBufferTimeout) && (*minTime == NULL || timercmp(&s->_readBufferTimeout, *minTime, <)))
		*minTime = &s->_readBufferTimeout;
}

void __CFSocketSetReadBufferTimeout(CFSocketRef s, CFTimeInterval timeout)
{
    struct timeval timeoutVal;

    intervalToTimeval(timeout, &timeoutVal);

	/* lock ordering is socket lock, activesocketslock */
	/* activesocketslock protects our timeout calculation */
    __CFSocketLock(s);
	__CFSpinLock(&__CFActiveSocketsLock);
    if (timercmp(&s->_readBufferTimeout, &timeoutVal, !=)) {
        s->_readBufferTimeout = timeoutVal;
        __CFReadSocketsTimeoutInvalid = true;
    }
	__CFSpinUnlock(&__CFActiveSocketsLock);
    __CFSocketUnlock(s);
}

void __CFSocketSetReadBufferLength(CFSocketRef s, CFIndex length)
{
	__CFSocketLock(s);
	if (s->_bytesToBuffer != length) {
		if (s->_bytesToBufferPos != 0 && s->_bytesToBufferReadPos != 0) {
			/* As originally envisaged, you were supposed to be sure to drain the buffer before 
			 * issuing another request on the socket.  In practice, there seem to be times when we want to re-use 
			 * the stream (or perhaps, are on our way to closing it out) and this policy doesn't work so well.  
			 * So, if someone changes the buffer size while we have bytes already buffered, we put them 
			 * aside and use them to satisfy any subsequent reads. 
			 */
#if defined(DEBUG)
			fprintf(stderr, "%s(%d): WARNING: shouldn't set read buffer length while data is still in the read buffer\n\n", __FUNCTION__, __LINE__);
#endif
			if (s->_leftoverBytes == NULL)
				s->_leftoverBytes = CFDataCreateMutable(CFGetAllocator(s), 0);
				
			/* append the current buffered bytes over.  We'll keep draining _leftoverBytes while we have them... */
			CFDataAppendBytes(s->_leftoverBytes, CFDataGetBytePtr(s->_readBuffer) + s->_bytesToBufferPos, s->_bytesToBufferReadPos - s->_bytesToBufferPos);
			CFRelease(s->_readBuffer);
			s->_readBuffer = NULL;
				
			s->_bytesToBuffer = 0;
			s->_bytesToBufferPos = 0;
			s->_bytesToBufferReadPos = 0;
		}
		if (length == 0) {
			s->_bytesToBuffer = 0;
			s->_bytesToBufferPos = 0;
			s->_bytesToBufferReadPos = 0;
			if (s->_readBuffer) {
				CFRelease(s->_readBuffer);
				s->_readBuffer = NULL;
			}
		} else {
			/* if the buffer shrank, we can re-use the old one */
			if (length > s->_bytesToBuffer) {
				if (s->_readBuffer) {
					CFRelease(s->_readBuffer);
					s->_readBuffer = NULL;
				}
			}
			
			s->_bytesToBuffer = length;
			s->_bytesToBufferPos = 0;
			s->_bytesToBufferReadPos = 0;
			if (s->_readBuffer == NULL) {
				s->_readBuffer = CFDataCreateMutable(kCFAllocatorDefault, length);
				CFDataSetLength(s->_readBuffer, length);
			}
		}
	}
	__CFSocketUnlock(s);
	if (length == 0)
		__CFSocketSetReadBufferTimeout(s, 0.0);
}

CFIndex __CFSocketRead(CFSocketRef s, UInt8* buffer, CFIndex length, int* error)
{
#if defined(LOG_CFSOCKET)
	fprintf(stdout, "READING BYTES FOR SOCKET %d (%d buffered, out of %d desired, eof = %d, err = %d)\n", s->_socket, s->_bytesToBufferPos, s->_bytesToBuffer, s->_atEOF, s->_bufferedReadError);
#endif /* LOG_CFSOCKET */

    CFIndex result = -1;

    __CFSocketLock(s);

	*error = 0;
	
	/* Any leftover buffered bytes? */
	if (s->_leftoverBytes) {
		CFIndex ctBuffer = CFDataGetLength(s->_leftoverBytes);
#if defined(DEBUG)
		fprintf(stderr, "%s(%d): WARNING: Draining %d leftover bytes first\n\n", __FUNCTION__, __LINE__, ctBuffer);
#endif
		if (ctBuffer > length)
			ctBuffer = length;
		memcpy(buffer, CFDataGetBytePtr(s->_leftoverBytes), ctBuffer);
		if (ctBuffer < CFDataGetLength(s->_leftoverBytes))
			CFDataReplaceBytes(s->_leftoverBytes, CFRangeMake(0, ctBuffer), NULL, 0);
		else {
			CFRelease(s->_leftoverBytes);
			s->_leftoverBytes = NULL;
		}
		result = ctBuffer;
		goto unlock;
	}
	
	/* return whatever we've buffered */
	if (s->_bytesToBuffer != 0) {
		CFIndex ctBuffer = s->_bytesToBufferPos - s->_bytesToBufferReadPos;
		if (ctBuffer > 0) {
			/* drain our buffer first */
			if (ctBuffer > length)
				ctBuffer = length;
			memcpy(buffer, CFDataGetBytePtr(s->_readBuffer) + s->_bytesToBufferReadPos, ctBuffer);
			s->_bytesToBufferReadPos += ctBuffer;
			if (s->_bytesToBufferReadPos == s->_bytesToBufferPos) {
#if defined(LOG_CFSOCKET)
				fprintf(stdout, "DRAINED BUFFER - SHOULD START BUFFERING AGAIN!\n");
#endif /* LOG_CFSOCKET */
				s->_bytesToBufferPos = 0;
				s->_bytesToBufferReadPos = 0;
			}
			
#if defined(LOG_CFSOCKET)
			fprintf(stdout, "SLURPED %d BYTES FROM BUFFER %d LEFT TO READ!\n", ctBuffer, length);
#endif /* LOG_CFSOCKET */

			result = ctBuffer;
            goto unlock;
		}
	}
	/* nothing buffered, or no buffer selected */
	
	/* Did we get an error on a previous read (or buffered read)? */
	if (s->_bufferedReadError != 0) {
#if defined(LOG_CFSOCKET)
		fprintf(stdout, "RETURNING ERROR %d\n", s->_bufferedReadError);
#endif /* LOG_CFSOCKET */
		*error = s->_bufferedReadError;
        result = -1;
        goto unlock;
	}
	
	/* nothing buffered, if we've hit eof, don't bother reading any more */
	if (s->_atEOF) {
#if defined(LOG_CFSOCKET)
		fprintf(stdout, "RETURNING EOF\n");
#endif /* LOG_CFSOCKET */
		result = 0;
        goto unlock;
	}
	
	/* normal read */
	result = read(CFSocketGetNative(s), buffer, length);
#if defined(LOG_CFSOCKET)
	fprintf(stdout, "READ %d bytes", result);
#endif /* LOG_CFSOCKET */

    if (result == 0) {
        /* note that we hit EOF */
        s->_atEOF = true;
    } else if (result < 0) {
        *error = errno;

        /* if it wasn't EAGAIN, record it (although we shouldn't get called again) */
        if (*error != EAGAIN) {
            s->_bufferedReadError = *error;
        }
    }

unlock:
    __CFSocketUnlock(s);

    return result;
}

Boolean __CFSocketGetBytesAvailable(CFSocketRef s, CFIndex* ctBytesAvailable)
{
	CFIndex ctBuffer = s->_bytesToBufferPos - s->_bytesToBufferReadPos;
	if (ctBuffer != 0) {
		*ctBytesAvailable = ctBuffer;
		return true;
	} else {
		int result;
#if ! defined(__WIN32__)
	    int bytesAvailable, intLen = sizeof(bytesAvailable);
	    result = getsockopt(CFSocketGetNative(s), SOL_SOCKET, SO_NREAD, &bytesAvailable, (void *)&intLen);
#else
	    unsigned long bytesAvailable;
	    result = ioctlsocket(CFSocketGetNative(s), FIONREAD, &bytesAvailable);
#endif
		if (result < 0)
			return false;
		*ctBytesAvailable = (CFIndex) bytesAvailable;
		return true;
	}
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
#endif /* LOG_CFSOCKET */

#ifdef __GNUC__
__attribute__ ((noreturn))	// mostly interesting for shutting up a warning
#endif /* __GNUC__ */
static void __CFSocketManager(void * arg)
{
    if (objc_collecting_enabled()) auto_zone_register_thread(auto_zone());
    SInt32 nrfds, maxnrfds, fdentries = 1;
    SInt32 rfds, wfds;
    fd_set *exceptfds = NULL;
    fd_set *writefds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(fd_mask), 0);
    fd_set *readfds = (fd_set *)CFAllocatorAllocate(kCFAllocatorSystemDefault, fdentries * sizeof(fd_mask), 0);
    fd_set *tempfds;
    SInt32 idx, cnt;
    uint8_t buffer[256];
    CFMutableArrayRef selectedWriteSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef selectedReadSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFIndex selectedWriteSocketsIndex = 0, selectedReadSocketsIndex = 0;
    
	struct timeval tv;
	struct timeval* pTimeout = NULL;
	struct timeval timeBeforeSelect;
	
    for (;;) {       
        __CFSpinLock(&__CFActiveSocketsLock);
        __CFSocketManagerIteration++;
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "socket manager iteration %lu looking at read sockets ", __CFSocketManagerIteration);
        __CFSocketWriteSocketList(__CFReadSockets, __CFReadSocketsFds, FALSE);
        if (0 < CFArrayGetCount(__CFWriteSockets)) {
            fprintf(stdout, " and write sockets ");
            __CFSocketWriteSocketList(__CFWriteSockets, __CFWriteSocketsFds, FALSE);
        }
        fprintf(stdout, "\n");
#endif /* LOG_CFSOCKET */
        rfds = __CFSocketFdGetSize(__CFReadSocketsFds);
        wfds = __CFSocketFdGetSize(__CFWriteSocketsFds);
        maxnrfds = __CFMax(rfds, wfds);
        if (maxnrfds > fdentries * (int)NFDBITS) {
            fdentries = (maxnrfds + NFDBITS - 1) / NFDBITS;
            writefds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, writefds, fdentries * sizeof(fd_mask), 0);
            readfds = (fd_set *)CFAllocatorReallocate(kCFAllocatorSystemDefault, readfds, fdentries * sizeof(fd_mask), 0);
        }
        memset(writefds, 0, fdentries * sizeof(fd_mask)); 
        memset(readfds, 0, fdentries * sizeof(fd_mask));
        CFDataGetBytes(__CFWriteSocketsFds, CFRangeMake(0, CFDataGetLength(__CFWriteSocketsFds)), (UInt8 *)writefds);
        CFDataGetBytes(__CFReadSocketsFds, CFRangeMake(0, CFDataGetLength(__CFReadSocketsFds)), (UInt8 *)readfds); 
		
        if (__CFReadSocketsTimeoutInvalid) {
            struct timeval* minTimeout = NULL;
            __CFReadSocketsTimeoutInvalid = false;
#if defined(LOG_CFSOCKET)
			fprintf(stdout, "Figuring out which sockets have timeouts...\n");
#endif /* LOG_CFSOCKET */
            CFArrayApplyFunction(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), _calcMinTimeout_locked, (void*) &minTimeout);

            if (minTimeout == NULL) {
#if defined(LOG_CFSOCKET)
				fprintf(stdout, "No one wants a timeout!\n");
#endif /* LOG_CFSOCKET */
                pTimeout = NULL;
            } else {
#if defined(LOG_CFSOCKET)
				fprintf(stdout, "timeout will be %d, %d!\n", minTimeout->tv_sec, minTimeout->tv_usec);
#endif /* LOG_CFSOCKET */
                tv = *minTimeout;
                pTimeout = &tv;
            }
        }

        if (pTimeout) {
#if defined(LOG_CFSOCKET)
			fprintf(stdout, "select will have a %d, %d timeout\n", pTimeout->tv_sec, pTimeout->tv_usec);
#endif /* LOG_CFSOCKET */
            gettimeofday(&timeBeforeSelect, NULL);
        }
		
		__CFSpinUnlock(&__CFActiveSocketsLock);

        nrfds = select(maxnrfds, readfds, writefds, exceptfds, pTimeout);

#if defined(LOG_CFSOCKET)
		fprintf(stdout, "socket manager woke from select, ret=%ld\n", nrfds);
#endif /* LOG_CFSOCKET */

		/*
		 * select returned a timeout
		 */
        if (0 == nrfds) {
			struct timeval timeAfterSelect;
			struct timeval deltaTime;
			gettimeofday(&timeAfterSelect, NULL);
			/* timeBeforeSelect becomes the delta */
			timersub(&timeAfterSelect, &timeBeforeSelect, &deltaTime);
			
#if defined(LOG_CFSOCKET)
			fprintf(stdout, "Socket manager received timeout - kicking off expired reads (expired delta %d, %d)\n", deltaTime.tv_sec, deltaTime.tv_usec);
#endif /* LOG_CFSOCKET */
			
			__CFSpinLock(&__CFActiveSocketsLock);
			
			tempfds = NULL;
			cnt = CFArrayGetCount(__CFReadSockets);
			for (idx = 0; idx < cnt; idx++) {
				CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
				if (timerisset(&s->_readBufferTimeout)) {
					CFSocketNativeHandle sock = s->_socket;
					// We might have an new element in __CFReadSockets that we weren't listening to,
					// in which case we must be sure not to test a bit in the fdset that is
					// outside our mask size.
					Boolean sockInBounds = (0 <= sock && sock < maxnrfds);
					/* if this sockets timeout is less than or equal elapsed time, then signal it */
					if (INVALID_SOCKET != sock && sockInBounds && timercmp(&s->_readBufferTimeout, &deltaTime, <=)) {
#if defined(LOG_CFSOCKET)
						fprintf(stdout, "Expiring socket %d (delta %d, %d)\n", sock, s->_readBufferTimeout.tv_sec, s->_readBufferTimeout.tv_usec);
#endif /* LOG_CFSOCKET */
						CFArraySetValueAtIndex(selectedReadSockets, selectedReadSocketsIndex, s);
						selectedReadSocketsIndex++;
						/* socket is removed from fds here, will be restored in read handling or in perform function */
						if (!tempfds) tempfds = (fd_set *)CFDataGetMutableBytePtr(__CFReadSocketsFds);
						FD_CLR(sock, tempfds);
					}
				}
			}
			
			__CFSpinUnlock(&__CFActiveSocketsLock);
			
			/* and below, we dispatch through the normal read dispatch mechanism */
		} 
		
		if (0 > nrfds) {
            SInt32 selectError = __CFSocketLastError();
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager received error %ld from select\n", selectError);
#endif /* LOG_CFSOCKET */
            if (EBADF == selectError) {
                CFMutableArrayRef invalidSockets = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
                __CFSpinLock(&__CFActiveSocketsLock);
                cnt = CFArrayGetCount(__CFWriteSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
                    if (!__CFNativeSocketIsValid(s->_socket)) {
#if defined(LOG_CFSOCKET)
                        fprintf(stdout, "socket manager found write socket %d invalid\n", s->_socket);
#endif /* LOG_CFSOCKET */
                        CFArrayAppendValue(invalidSockets, s);
                    }
                }
                cnt = CFArrayGetCount(__CFReadSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
                    if (!__CFNativeSocketIsValid(s->_socket)) {
#if defined(LOG_CFSOCKET)
                        fprintf(stdout, "socket manager found read socket %d invalid\n", s->_socket);
#endif /* LOG_CFSOCKET */
                        CFArrayAppendValue(invalidSockets, s);
                    }
                }
                __CFSpinUnlock(&__CFActiveSocketsLock);
        
                cnt = CFArrayGetCount(invalidSockets);
                for (idx = 0; idx < cnt; idx++) {
                    CFSocketInvalidate(((CFSocketRef)CFArrayGetValueAtIndex(invalidSockets, idx)));
                }
                CFRelease(invalidSockets);
            }
            continue;
        }
        if (FD_ISSET(__CFWakeupSocketPair[1], readfds)) {
            recv(__CFWakeupSocketPair[1], buffer, sizeof(buffer), 0);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager received %c on wakeup socket\n", buffer[0]);
#endif /* LOG_CFSOCKET */
        }
        __CFSpinLock(&__CFActiveSocketsLock);
        tempfds = NULL;
        cnt = CFArrayGetCount(__CFWriteSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFWriteSockets, idx);
            CFSocketNativeHandle sock = s->_socket;
            // We might have an new element in __CFWriteSockets that we weren't listening to,
            // in which case we must be sure not to test a bit in the fdset that is
            // outside our mask size.
            Boolean sockInBounds = (0 <= sock && sock < maxnrfds);
            if (INVALID_SOCKET != sock && sockInBounds) {
                if (FD_ISSET(sock, writefds)) {
                    CFArraySetValueAtIndex(selectedWriteSockets, selectedWriteSocketsIndex, s);
                    selectedWriteSocketsIndex++;
                    /* socket is removed from fds here, restored by CFSocketReschedule */
                    if (!tempfds) tempfds = (fd_set *)CFDataGetMutableBytePtr(__CFWriteSocketsFds);
                    FD_CLR(sock, tempfds);
                }
            }
        }
        tempfds = NULL;
        cnt = CFArrayGetCount(__CFReadSockets);
        for (idx = 0; idx < cnt; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(__CFReadSockets, idx);
            CFSocketNativeHandle sock = s->_socket;
            // We might have an new element in __CFReadSockets that we weren't listening to,
            // in which case we must be sure not to test a bit in the fdset that is
            // outside our mask size.
            Boolean sockInBounds = (0 <= sock && sock < maxnrfds);
            if (INVALID_SOCKET != sock && sockInBounds && FD_ISSET(sock, readfds)) {
                CFArraySetValueAtIndex(selectedReadSockets, selectedReadSocketsIndex, s);
                selectedReadSocketsIndex++;
                /* socket is removed from fds here, will be restored in read handling or in perform function */
                if (!tempfds) tempfds = (fd_set *)CFDataGetMutableBytePtr(__CFReadSocketsFds);
                FD_CLR(sock, tempfds);
            }
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
        
        for (idx = 0; idx < selectedWriteSocketsIndex; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(selectedWriteSockets, idx);
            if (kCFNull == (CFNullRef)s) continue;
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager signaling socket %d for write\n", s->_socket);
#endif /* LOG_CFSOCKET */
            __CFSocketHandleWrite(s, FALSE);
            CFArraySetValueAtIndex(selectedWriteSockets, idx, kCFNull);
        }
        selectedWriteSocketsIndex = 0;
        
        for (idx = 0; idx < selectedReadSocketsIndex; idx++) {
            CFSocketRef s = (CFSocketRef)CFArrayGetValueAtIndex(selectedReadSockets, idx);
            if (kCFNull == (CFNullRef)s) continue;
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "socket manager signaling socket %d for read\n", s->_socket);
#endif /* LOG_CFSOCKET */
            __CFSocketHandleRead(s, nrfds == 0);
            CFArraySetValueAtIndex(selectedReadSockets, idx, kCFNull);
        }
        selectedReadSocketsIndex = 0;
    }
    if (objc_collecting_enabled()) auto_zone_unregister_thread(auto_zone());
}

static CFStringRef __CFSocketCopyDescription(CFTypeRef cf) {
    CFSocketRef s = (CFSocketRef)cf;
    CFMutableStringRef result;
    CFStringRef contextDesc = NULL;
    void *contextInfo = NULL;
    CFStringRef (*contextCopyDescription)(const void *info) = NULL;
    result = CFStringCreateMutable(CFGetAllocator(s), 0);
    __CFSocketLock(s);
    void *addr = s->_callout;
    Dl_info info;
    const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
    CFStringAppendFormat(result, NULL, CFSTR("<CFSocket %p [%p]>{valid = %s, type = %d, socket = %d, socket set count = %ld,\n    callback types = 0x%x, callout = %s (%p), source = %p,\n    run loops = %@,\n    context = "), cf, CFGetAllocator(s), (__CFSocketIsValid(s) ? "Yes" : "No"), s->_socketType, s->_socket, s->_socketSetCount, __CFSocketCallBackTypes(s), name, addr, s->_source0, s->_runLoops);
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
    CFStringAppend(result, CFSTR("}"));
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
    if (NULL != s->_readBuffer) {
        CFRelease(s->_readBuffer);
        s->_readBuffer = NULL;
    }
	if (NULL != s->_leftoverBytes) {
		CFRelease(s->_leftoverBytes);
		s->_leftoverBytes = NULL;
	}
    timerclear(&s->_readBufferTimeout);
    s->_bytesToBuffer = 0;
    s->_bytesToBufferPos = 0;
    s->_bytesToBufferReadPos = 0;
    s->_atEOF = true;
	s->_bufferedReadError = 0;
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
static CFSocketRef _CFSocketCreateWithNative(CFAllocatorRef allocator, CFSocketNativeHandle sock, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context, Boolean useExistingInstance) {
    CHECK_FOR_FORK();
    CFSocketRef memory;
    int typeSize = sizeof(memory->_socketType);
    __CFSpinLock(&__CFActiveSocketsLock);
    if (NULL == __CFReadSockets) __CFSocketInitializeSockets();
    __CFSpinUnlock(&__CFActiveSocketsLock);
    __CFSpinLock(&__CFAllSocketsLock);
    if (NULL == __CFAllSockets) {
        __CFAllSockets = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    }
    if (INVALID_SOCKET != sock && CFDictionaryGetValueIfPresent(__CFAllSockets, (void *)(uintptr_t)sock, (const void **)&memory)) {
        if (useExistingInstance) {
			__CFSpinUnlock(&__CFAllSocketsLock);
			CFRetain(memory);
			return memory;
		} else {
#if defined(LOG_CFSOCKET)
			fprintf(stdout, "useExistingInstance is FALSE, removing existing instance %d from __CFAllSockets\n", (int)memory);
#endif /* LOG_CFSOCKET */
			__CFSpinUnlock(&__CFAllSocketsLock);
			CFSocketInvalidate(memory);
			__CFSpinLock(&__CFAllSocketsLock);
		}
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
    memory->_lock = CFSpinLockInit;
    memory->_writeLock = CFSpinLockInit;
    memory->_socket = sock;
    if (INVALID_SOCKET == sock || 0 != getsockopt(sock, SOL_SOCKET, SO_TYPE, (void *)&(memory->_socketType), (socklen_t *)&typeSize)) memory->_socketType = 0;		// cast for WinSock bad API
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
    memory->_dataQueue = NULL;
    memory->_addressQueue = NULL;
    memory->_context.info = 0;
    memory->_context.retain = 0;
    memory->_context.release = 0;
    memory->_context.copyDescription = 0;
    timerclear(&memory->_readBufferTimeout);
	memory->_readBuffer = NULL;
	memory->_bytesToBuffer = 0;
	memory->_bytesToBufferPos = 0;
	memory->_bytesToBufferReadPos = 0;
	memory->_atEOF = false;
	memory->_bufferedReadError = 0;
	
    if (INVALID_SOCKET != sock) CFDictionaryAddValue(__CFAllSockets, (void *)(uintptr_t)sock, memory);
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

CFSocketRef CFSocketCreateWithNative(CFAllocatorRef allocator, CFSocketNativeHandle sock, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context) {
	return _CFSocketCreateWithNative(allocator, sock, callBackTypes, callout, context, TRUE);
}

void CFSocketInvalidate(CFSocketRef s) {
    CHECK_FOR_FORK();
    UInt32 previousSocketManagerIteration;
    __CFGenericValidateType(s, __kCFSocketTypeID);
#if defined(LOG_CFSOCKET)
    fprintf(stdout, "invalidating socket %d with flags 0x%x disabled 0x%x connected 0x%x\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected);
#endif /* LOG_CFSOCKET */
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
        }
        // No need to clear FD's for V1 sources, since we'll just throw the whole event away
        idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFReadSockets, idx);
            __CFSocketClearFDForRead(s);
        }
        previousSocketManagerIteration = __CFSocketManagerIteration;
        __CFSpinUnlock(&__CFActiveSocketsLock);
        CFDictionaryRemoveValue(__CFAllSockets, (void *)(uintptr_t)(s->_socket));
        if ((s->_f.client & kCFSocketCloseOnInvalidate) != 0) closesocket(s->_socket);
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
    } else {
        __CFSocketUnlock(s);
    }
    __CFSpinUnlock(&__CFAllSocketsLock);
    CFRelease(s);
}

Boolean CFSocketIsValid(CFSocketRef s) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(s, __kCFSocketTypeID);
    return __CFSocketIsValid(s);
}

CFSocketNativeHandle CFSocketGetNative(CFSocketRef s) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(s, __kCFSocketTypeID);
    return s->_socket;
}

CFDataRef CFSocketCopyAddress(CFSocketRef s) {
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
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
    CHECK_FOR_FORK();
    __CFGenericValidateType(s, __kCFSocketTypeID);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = s->_context;
}

CFOptionFlags CFSocketGetSocketFlags(CFSocketRef s) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(s, __kCFSocketTypeID);
    return s->_f.client;
}

void CFSocketSetSocketFlags(CFSocketRef s, CFOptionFlags flags) {
    CHECK_FOR_FORK();
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "setting flags for socket %d, from 0x%x to 0x%lx\n", s->_socket, s->_f.client, flags);
#endif /* LOG_CFSOCKET */
    s->_f.client = flags;
    __CFSocketUnlock(s);
}

void CFSocketDisableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes) {
    CHECK_FOR_FORK();
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
#endif /* LOG_CFSOCKET */
        __CFSpinLock(&__CFActiveSocketsLock);
        if ((readCallBackType == kCFSocketAcceptCallBack) || !__CFSocketIsConnectionOriented(s)) s->_f.connected = TRUE;
        if (((callBackTypes & kCFSocketWriteCallBack) != 0) || (((callBackTypes & kCFSocketConnectCallBack) != 0) && !s->_f.connected)) {
            if (__CFSocketClearFDForWrite(s)) {
                // do not wake up the socket manager thread if all relevant write callbacks are disabled
                CFOptionFlags writeCallBacksAvailable = __CFSocketCallBackTypes(s) & (kCFSocketWriteCallBack | kCFSocketConnectCallBack);
                if (s->_f.connected) writeCallBacksAvailable &= ~kCFSocketConnectCallBack;
                if ((s->_f.disabled & writeCallBacksAvailable) != writeCallBacksAvailable) wakeup = true;
            }
        }
        if (readCallBackType != kCFSocketNoCallBack && (callBackTypes & readCallBackType) != 0) {
            if (__CFSocketClearFDForRead(s)) {
                // do not wake up the socket manager thread if callback type is read
                if (readCallBackType != kCFSocketReadCallBack) wakeup = true;
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
    CHECK_FOR_FORK();
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
#endif /* LOG_CFSOCKET */
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
            if (turnOnWrite || turnOnConnect) {
                if (force) {
                    SInt32 idx = CFArrayGetFirstIndexOfValue(__CFWriteSockets, CFRangeMake(0, CFArrayGetCount(__CFWriteSockets)), s);
                    if (kCFNotFound == idx) CFArrayAppendValue(__CFWriteSockets, s);
                }
                if (__CFSocketSetFDForWrite(s)) wakeup = true;
            }
            if (turnOnRead) {
                if (force) {
                    SInt32 idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
                    if (kCFNotFound == idx) CFArrayAppendValue(__CFReadSockets, s);
                }
                if (__CFSocketSetFDForRead(s)) wakeup = true;
            }
            if (wakeup && NULL == __CFSocketManagerThread) __CFSocketManagerThread = __CFStartSimpleThread(__CFSocketManager, 0);
            __CFSpinUnlock(&__CFActiveSocketsLock);
        }
    }
    __CFSocketUnlock(s);
    if (wakeup) send(__CFWakeupSocketPair[0], &wakeupChar, sizeof(wakeupChar), 0);
}

void CFSocketEnableCallBacks(CFSocketRef s, CFOptionFlags callBackTypes) {
    CHECK_FOR_FORK();
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
#endif /* LOG_CFSOCKET */
            __CFSocketEnableCallBacks(s, __CFSocketCallBackTypes(s), TRUE, 's');  // unlocks s
        } else
            __CFSocketUnlock(s);
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
        }
        idx = CFArrayGetFirstIndexOfValue(__CFReadSockets, CFRangeMake(0, CFArrayGetCount(__CFReadSockets)), s);
        if (0 <= idx) {
            CFArrayRemoveValueAtIndex(__CFReadSockets, idx);
            __CFSocketClearFDForRead(s);
        }
        __CFSpinUnlock(&__CFActiveSocketsLock);
    }
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
#endif /* LOG_CFSOCKET */
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
#endif /* LOG_CFSOCKET */
                if (callout) callout(s, kCFSocketConnectCallBack, NULL, &errorCode, contextInfo);
                calledOut = true;
            } else {
#if defined(LOG_CFSOCKET)
                fprintf(stdout, "perform calling out connect to socket %d\n", s->_socket);
#endif /* LOG_CFSOCKET */
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
#endif /* LOG_CFSOCKET */
            if (callout) callout(s, kCFSocketDataCallBack, address, data, contextInfo);
            calledOut = true;
            if (0 == datalen) CFSocketInvalidate(s);
        }
    } else if (kCFSocketAcceptCallBack == readCallBackType) {
        if (INVALID_SOCKET != sock && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out accept of socket %d to socket %d\n", sock, s->_socket);
#endif /* LOG_CFSOCKET */
            if (callout) callout(s, kCFSocketAcceptCallBack, address, &sock, contextInfo);
            calledOut = true;
        }
    } else if (kCFSocketReadCallBack == readCallBackType) {
        if (readSignalled && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out read to socket %d\n", s->_socket);
#endif /* LOG_CFSOCKET */
            if (callout) callout(s, kCFSocketReadCallBack, NULL, NULL, contextInfo);
            calledOut = true;
        }
    }
    if ((callBackTypes & kCFSocketWriteCallBack) != 0) {
        if (writeSignalled && !errorCode && (!calledOut || CFSocketIsValid(s))) {
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform calling out write to socket %d\n", s->_socket);
#endif /* LOG_CFSOCKET */
            if (callout) callout(s, kCFSocketWriteCallBack, NULL, NULL, contextInfo);
            calledOut = true;
        }
    }
}

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
            sock = (CFSocketNativeHandle)(uintptr_t)CFArrayGetValueAtIndex(s->_dataQueue, 0);
            CFArrayRemoveValueAtIndex(s->_dataQueue, 0);
            address = CFArrayGetValueAtIndex(s->_addressQueue, 0);
            CFRetain(address);
            CFArrayRemoveValueAtIndex(s->_addressQueue, 0);
        }
    }

    __CFSocketDoCallback(s, data, address, sock);	// does __CFSocketUnlock(s)
    if (NULL != data) CFRelease(data);
    if (NULL != address) CFRelease(address);

    __CFSocketLock(s);
    if (__CFSocketIsValid(s) && kCFSocketNoCallBack != readCallBackType) {
        // if there's still more data, we want to wake back up right away
        if ((kCFSocketDataCallBack == readCallBackType || kCFSocketAcceptCallBack == readCallBackType) && NULL != s->_dataQueue && 0 < CFArrayGetCount(s->_dataQueue)) {
            CFRunLoopSourceSignal(s->_source0);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "perform short-circuit signaling source for socket %d with flags 0x%x disabled 0x%x connected 0x%x\n", s->_socket, s->_f.client, s->_f.disabled, s->_f.connected);
#endif /* LOG_CFSOCKET */
            rl = __CFSocketCopyRunLoopToWakeUp(s);
        }
    }
    // Only reenable callbacks that are auto-reenabled
    __CFSocketEnableCallBacks(s, callBacksSignalled & s->_f.client, FALSE, 'p');  // unlocks s

    if (NULL != rl) {
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
}

CFRunLoopSourceRef CFSocketCreateRunLoopSource(CFAllocatorRef allocator, CFSocketRef s, CFIndex order) {
    CHECK_FOR_FORK();
    CFRunLoopSourceRef result = NULL;
    __CFGenericValidateType(s, __kCFSocketTypeID);
    __CFSocketLock(s);
    if (__CFSocketIsValid(s)) {
        if (NULL == s->_source0) {
            CFRunLoopSourceContext context;
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

#endif /* NEW_SOCKET */

static CFSpinLock_t __CFSocketWriteLock_ = CFSpinLockInit;
//#warning can only send on one socket at a time now

CF_INLINE void __CFSocketWriteLock(CFSocketRef s) {
    __CFSpinLock(& __CFSocketWriteLock_);
}

CF_INLINE void __CFSocketWriteUnlock(CFSocketRef s) {
    __CFSpinUnlock(& __CFSocketWriteLock_);
}

//??? need timeout, error handling, retries
CFSocketError CFSocketSendData(CFSocketRef s, CFDataRef address, CFDataRef data, CFTimeInterval timeout) {
    CHECK_FOR_FORK();
    const uint8_t *dataptr, *addrptr = NULL;
    SInt32 datalen, addrlen = 0, size = 0;
    CFSocketNativeHandle sock = INVALID_SOCKET;
    struct timeval tv;
    __CFGenericValidateType(s, CFSocketGetTypeID());
    if (address) {
        addrptr = CFDataGetBytePtr(address);
        addrlen = CFDataGetLength(address);
    }
    dataptr = CFDataGetBytePtr(data);
    datalen = CFDataGetLength(data);
    if (CFSocketIsValid(s)) sock = CFSocketGetNative(s);
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
        fprintf(stdout, "wrote %ld bytes to socket %d\n", size, sock);
#endif /* LOG_CFSOCKET */
        __CFSocketWriteUnlock(s);
        CFRelease(s);
    }
    return (size > 0) ? kCFSocketSuccess : kCFSocketError;
}

CFSocketError CFSocketSetAddress(CFSocketRef s, CFDataRef address) {
    CHECK_FOR_FORK();
    const uint8_t *name;
    SInt32 namelen, result = 0;
    __CFGenericValidateType(s, CFSocketGetTypeID());
    if (NULL == address) return -1;
    if (!CFSocketIsValid(s)) return 0;
    name = CFDataGetBytePtr(address);
    namelen = CFDataGetLength(address);
    if (!name || namelen <= 0) return 0;
    CFSocketNativeHandle sock = CFSocketGetNative(s);
        result = bind(sock, (struct sockaddr *)name, namelen);
        if (0 == result) {
            listen(sock, 256);
        }
    //??? should return errno
    return result;
}

CFSocketError CFSocketConnectToAddress(CFSocketRef s, CFDataRef address, CFTimeInterval timeout) {
    CHECK_FOR_FORK();
    //??? need error handling, retries
    const uint8_t *name;
    SInt32 namelen, result = -1, connect_err = 0, select_err = 0;
    UInt32 yes = 1, no = 0;
    Boolean wasBlocking = true;

    __CFGenericValidateType(s, CFSocketGetTypeID());
    if (!CFSocketIsValid(s)) return 0;
    name = CFDataGetBytePtr(address);
    namelen = CFDataGetLength(address);
    if (!name || namelen <= 0) return 0;
    CFSocketNativeHandle sock = CFSocketGetNative(s);
    {
        SInt32 flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) wasBlocking = ((flags & O_NONBLOCK) == 0);
        if (wasBlocking && (timeout > 0.0 || timeout < 0.0)) ioctlsocket(sock, FIONBIO, &yes);
        result = connect(sock, (struct sockaddr *)name, namelen);
        if (result != 0) {
            connect_err = __CFSocketLastError();
        }
#if defined(LOG_CFSOCKET)
        fprintf(stdout, "connection attempt returns %ld error %ld on socket %d (flags 0x%lx blocking %d)\n", result, connect_err, sock, flags, wasBlocking);
#endif /* LOG_CFSOCKET */
        if (EINPROGRESS == connect_err && timeout >= 0.0) {
            /* select on socket */
            SInt32 nrfds;
            int error_size = sizeof(select_err);
            struct timeval tv;
            CFMutableDataRef fds = CFDataCreateMutable(kCFAllocatorSystemDefault, 0);
            __CFSocketFdSet(sock, fds);
            tv.tv_sec = (0 >= timeout || INT_MAX <= timeout) ? INT_MAX : (int)(float)floor(timeout);
            tv.tv_usec = (int)((timeout - floor(timeout)) * 1.0E6);
            nrfds = select(__CFSocketFdGetSize(fds), NULL, (fd_set *)CFDataGetMutableBytePtr(fds), NULL, &tv);
            if (nrfds < 0) {
                select_err = __CFSocketLastError();
                result = -1;
            } else if (nrfds == 0) {
                result = -2;
            } else {
                if (0 != getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&select_err, (socklen_t *)&error_size)) select_err = 0;
                result = (select_err == 0) ? 0 : -1;
            }
            CFRelease(fds);
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "timed connection attempt %s on socket %d, result %ld, select returns %ld error %ld\n", (result == 0) ? "succeeds" : "fails", sock, result, nrfds, select_err);
#endif /* LOG_CFSOCKET */
        }
        if (wasBlocking && (timeout > 0.0 || timeout < 0.0)) ioctlsocket(sock, FIONBIO, &no);
        if (EINPROGRESS == connect_err && timeout < 0.0) {
            result = 0;
#if defined(LOG_CFSOCKET)
            fprintf(stdout, "connection attempt continues in background on socket %d\n", sock);
#endif /* LOG_CFSOCKET */
        }
    }
    //??? should return errno
    return result;
}

CFSocketRef CFSocketCreate(CFAllocatorRef allocator, SInt32 protocolFamily, SInt32 socketType, SInt32 protocol, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context) {
    CHECK_FOR_FORK();
    CFSocketNativeHandle sock = INVALID_SOCKET;
    CFSocketRef s = NULL;
    if (0 >= protocolFamily) protocolFamily = PF_INET;
    if (PF_INET == protocolFamily) {
        if (0 >= socketType) socketType = SOCK_STREAM;
        if (0 >= protocol && SOCK_STREAM == socketType) protocol = IPPROTO_TCP;
        if (0 >= protocol && SOCK_DGRAM == socketType) protocol = IPPROTO_UDP;
    }
    if (PF_LOCAL == protocolFamily && 0 >= socketType) socketType = SOCK_STREAM;
    sock = socket(protocolFamily, socketType, protocol);
    if (INVALID_SOCKET != sock) {
        s = _CFSocketCreateWithNative(allocator, sock, callBackTypes, callout, context, FALSE);
    }
    return s;
}

CFSocketRef CFSocketCreateWithSocketSignature(CFAllocatorRef allocator, const CFSocketSignature *signature, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context) {
    CHECK_FOR_FORK();
    CFSocketRef s = CFSocketCreate(allocator, signature->protocolFamily, signature->socketType, signature->protocol, callBackTypes, callout, context);
    if (NULL != s && (!CFSocketIsValid(s) || kCFSocketSuccess != CFSocketSetAddress(s, signature->address))) {
        CFSocketInvalidate(s);
        CFRelease(s);
        s = NULL;
    }
    return s;
}

CFSocketRef CFSocketCreateConnectedToSocketSignature(CFAllocatorRef allocator, const CFSocketSignature *signature, CFOptionFlags callBackTypes, CFSocketCallBack callout, const CFSocketContext *context, CFTimeInterval timeout) {
    CHECK_FOR_FORK();
    CFSocketRef s = CFSocketCreate(allocator, signature->protocolFamily, signature->socketType, signature->protocol, callBackTypes, callout, context);
    if (NULL != s && (!CFSocketIsValid(s) || kCFSocketSuccess != CFSocketConnectToAddress(s, signature->address, timeout))) {
        CFSocketInvalidate(s);
        CFRelease(s);
        s = NULL;
    }
    return s;
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
    replyDictionary = CFPropertyListCreateFromXMLData(kCFAllocatorSystemDefault, replyData, kCFPropertyListImmutable, NULL);
    if (NULL != response->error) *(response->error) = kCFSocketError;
    if (NULL != replyDictionary) {
        if (CFGetTypeID((CFTypeRef)replyDictionary) == CFDictionaryGetTypeID() && NULL != (value = CFDictionaryGetValue(replyDictionary, kCFSocketResultKey))) {
            if (NULL != response->error) *(response->error) = kCFSocketSuccess;
            if (NULL != response->value) *(response->value) = CFRetain(value);
            if (NULL != response->address) *(response->address) = address ? CFDataCreateCopy(kCFAllocatorSystemDefault, address) : NULL;
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
    requestData = CFPropertyListCreateXMLData(kCFAllocatorSystemDefault, requestDictionary);
    if (NULL != requestData) {
        if (NULL != response->error) *(response->error) = kCFSocketTimeout;
        s = CFSocketCreateConnectedToSocketSignature(kCFAllocatorSystemDefault, signature, kCFSocketDataCallBack, __CFSocketHandleNameRegistryReply, &context, timeout);
        if (NULL != s) {
            if (kCFSocketSuccess == CFSocketSendData(s, NULL, requestData, timeout)) {
                source = CFSocketCreateRunLoopSource(kCFAllocatorSystemDefault, s, 0);
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
    sain.sin_len = sizeof(sain);
    sain.sin_family = AF_INET;
    sain.sin_port = htons(__CFSocketDefaultNameRegistryPortNumber);
    sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (NULL == providedSignature) {
        signature->protocolFamily = PF_INET;
        signature->socketType = SOCK_STREAM;
        signature->protocol = IPPROTO_TCP;
        signature->address = CFDataCreate(kCFAllocatorSystemDefault, (uint8_t *)&sain, sizeof(sain));
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
            signature->address = CFDataCreate(kCFAllocatorSystemDefault, (uint8_t *)&sain, sizeof(sain));
        } else {
            sainp = (struct sockaddr_in *)CFDataGetBytePtr(providedSignature->address);
            if ((int)sizeof(struct sockaddr_in) <= CFDataGetLength(providedSignature->address) && (AF_INET == sainp->sin_family || 0 == sainp->sin_family)) {
                sain.sin_len = sizeof(sain);
                sain.sin_family = AF_INET;
                sain.sin_port = sainp->sin_port;
                if (0 == sain.sin_port) sain.sin_port = htons(defaultPortNumber);
                sain.sin_addr.s_addr = sainp->sin_addr.s_addr;
                if (htonl(INADDR_ANY) == sain.sin_addr.s_addr) sain.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                signature->address = CFDataCreate(kCFAllocatorSystemDefault, (uint8_t *)&sain, sizeof(sain));
            } else {
                signature->address = CFRetain(providedSignature->address);
            }
        }
    }
}

CFSocketError CFSocketRegisterValue(const CFSocketSignature *nameServerSignature, CFTimeInterval timeout, CFStringRef name, CFPropertyListRef value) {
    CFSocketSignature signature;
    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
            data = CFDataCreateMutable(kCFAllocatorSystemDefault, sizeof(bytes) + length);
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
        returnedSignature.address = CFDataCreate(kCFAllocatorSystemDefault, ptr, length - 4);
        __CFSocketValidateSignature(&returnedSignature, signature, 0);
        CFRelease(returnedSignature.address);
        ptr = CFDataGetBytePtr(signature->address);
        if (CFDataGetLength(signature->address) >= (int)sizeof(struct sockaddr_in) && AF_INET == ((struct sockaddr *)ptr)->sa_family && NULL != serverAddress && CFDataGetLength(serverAddress) >= (int)sizeof(struct sockaddr_in) && NULL != (aptr = CFDataGetBytePtr(serverAddress)) && AF_INET == ((struct sockaddr *)aptr)->sa_family) {
            CFMutableDataRef address = CFDataCreateMutableCopy(kCFAllocatorSystemDefault, CFDataGetLength(signature->address), signature->address);
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


