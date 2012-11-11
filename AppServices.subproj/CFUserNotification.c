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
/*	CFUserNotification.c
	Copyright 2000-2002, Apple, Inc. All rights reserved.
	Responsibility: Doug Davidson
*/

#include <CoreFoundation/CFUserNotification.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFRunLoop.h>
#include "CFInternal.h"

#define __kCFLogUserNotification      20
#define CFUserNotificationLog(alertHeader, alertMessage) CFLog(__kCFLogUserNotification, CFSTR("%@:  %@"), alertHeader, alertMessage);

enum {
    kCFUserNotificationCancelFlag = (1 << 3),
    kCFUserNotificationUpdateFlag = (1 << 4)
};

CONST_STRING_DECL(kCFUserNotificationTokenKey, "Token")
CONST_STRING_DECL(kCFUserNotificationTimeoutKey, "Timeout")
CONST_STRING_DECL(kCFUserNotificationFlagsKey, "Flags")
CONST_STRING_DECL(kCFUserNotificationIconPathKey, "IconPath")
CONST_STRING_DECL(kCFUserNotificationSoundPathKey, "SoundPath")
CONST_STRING_DECL(kCFUserNotificationLocalizationPathKey, "LocalizationPath")
CONST_STRING_DECL(kCFUserNotificationAlertSourceKey, "AlertSource")
CONST_STRING_DECL(kCFUserNotificationTextFieldLabelsKey, "TextFieldTitles")
CONST_STRING_DECL(kCFUserNotificationCheckBoxLabelsKey, "CheckBoxTitles")
CONST_STRING_DECL(kCFUserNotificationIconURLKey, "IconURL")
CONST_STRING_DECL(kCFUserNotificationSoundURLKey, "SoundURL")
CONST_STRING_DECL(kCFUserNotificationLocalizationURLKey, "LocalizationURL")
CONST_STRING_DECL(kCFUserNotificationAlertHeaderKey, "AlertHeader")
CONST_STRING_DECL(kCFUserNotificationAlertMessageKey, "AlertMessage")
CONST_STRING_DECL(kCFUserNotificationDefaultButtonTitleKey, "DefaultButtonTitle")
CONST_STRING_DECL(kCFUserNotificationAlternateButtonTitleKey, "AlternateButtonTitle")
CONST_STRING_DECL(kCFUserNotificationOtherButtonTitleKey, "OtherButtonTitle")
CONST_STRING_DECL(kCFUserNotificationProgressIndicatorValueKey, "ProgressIndicatorValue")
CONST_STRING_DECL(kCFUserNotificationSessionIDKey, "SessionID")
CONST_STRING_DECL(kCFUserNotificationPopUpTitlesKey, "PopUpTitles")
CONST_STRING_DECL(kCFUserNotificationTextFieldTitlesKey, "TextFieldTitles")
CONST_STRING_DECL(kCFUserNotificationCheckBoxTitlesKey, "CheckBoxTitles")
CONST_STRING_DECL(kCFUserNotificationTextFieldValuesKey, "TextFieldValues")
CONST_STRING_DECL(kCFUserNotificationPopUpSelectionKey, "PopUpSelection")

static CFTypeID __kCFUserNotificationTypeID = _kCFRuntimeNotATypeID;

struct __CFUserNotification {
    CFRuntimeBase _base;
    SInt32 _replyPort;
    SInt32 _token;
    CFTimeInterval _timeout;
    CFOptionFlags _requestFlags;
    CFOptionFlags _responseFlags;
    CFStringRef _sessionID;
    CFDictionaryRef _responseDictionary;
    CFMachPortRef _machPort;
    CFUserNotificationCallBack _callout;
};

static CFStringRef __CFUserNotificationCopyDescription(CFTypeRef cf) {
    CFMutableStringRef result;
    result = CFStringCreateMutable(CFGetAllocator(cf), 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFUserNotification 0x%x>"), (UInt32)cf);
    return result;
}

#if defined(__MACH__)

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <mach/mach.h>
#include <mach/error.h>
#include <servers/bootstrap.h>
#include <limits.h>
#include <errno.h>

#define MAX_STRING_LENGTH PATH_MAX
#define MAX_STRING_COUNT 16
#define MAX_PORT_NAME_LENGTH 63
#define NOTIFICATION_PORT_NAME "com.apple.UNCUserNotification"
#define NOTIFICATION_PORT_NAME_OLD "UNCUserNotification"
#define NOTIFICATION_PORT_NAME_SUFFIX ".session."
#define MESSAGE_TIMEOUT 100

static void __CFUserNotificationDeallocate(CFTypeRef cf) {
    CFUserNotificationRef userNotification = (CFUserNotificationRef)cf;
    if (userNotification->_machPort) {
        CFMachPortInvalidate(userNotification->_machPort);
        CFRelease(userNotification->_machPort);
    } else if (MACH_PORT_NULL != userNotification->_replyPort) {
        mach_port_destroy(mach_task_self(), userNotification->_replyPort);
    }
    if (userNotification->_sessionID) CFRelease(userNotification->_sessionID);
    if (userNotification->_responseDictionary) CFRelease(userNotification->_responseDictionary);
}

#endif /* __MACH__ */

static const CFRuntimeClass __CFUserNotificationClass = {
    0,
    "CFUserNotification",
    NULL,      // init
    NULL,      // copy
    __CFUserNotificationDeallocate,
    NULL,      // equal
    NULL,      // hash
    NULL,      // 
    __CFUserNotificationCopyDescription
};

__private_extern__ void __CFUserNotificationInitialize(void) {
    __kCFUserNotificationTypeID = _CFRuntimeRegisterClass(&__CFUserNotificationClass);
}

CFTypeID CFUserNotificationGetTypeID(void) {
    return __kCFUserNotificationTypeID;
}

#if defined(__MACH__)

static void _CFUserNotificationAddToDictionary(const void *key, const void *value, void *context) {
    if (CFGetTypeID(key) == CFStringGetTypeID()) CFDictionarySetValue((CFMutableDictionaryRef)context, key, value);
}

static CFDictionaryRef _CFUserNotificationModifiedDictionary(CFAllocatorRef allocator, CFDictionaryRef dictionary, SInt32 token, SInt32 timeout, CFStringRef source) {
    CFMutableDictionaryRef md = CFDictionaryCreateMutable(allocator, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef tokenNumber = CFNumberCreate(allocator, kCFNumberSInt32Type, &token);
    CFNumberRef timeoutNumber = CFNumberCreate(allocator, kCFNumberSInt32Type, &timeout);
    CFURLRef url = NULL;
    CFStringRef path = NULL;

    if (dictionary) CFDictionaryApplyFunction(dictionary, _CFUserNotificationAddToDictionary, md);
    if (source) CFDictionaryAddValue(md, kCFUserNotificationAlertSourceKey, source);
    if (tokenNumber) {
        CFDictionaryAddValue(md, kCFUserNotificationTokenKey, tokenNumber);
        CFRelease(tokenNumber);
    }
    if (timeoutNumber) {
        CFDictionaryAddValue(md, kCFUserNotificationTimeoutKey, timeoutNumber);
        CFRelease(timeoutNumber);
    }
    
    url = CFDictionaryGetValue(md, kCFUserNotificationIconURLKey);
    if (url && CFGetTypeID((CFTypeRef)url) == CFURLGetTypeID()) {
        url = CFURLCopyAbsoluteURL(url);
        CFDictionaryRemoveValue(md, kCFUserNotificationIconURLKey);
        path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFDictionaryAddValue(md, kCFUserNotificationIconPathKey, path);
        CFRelease(url);
        CFRelease(path);
    }
    url = CFDictionaryGetValue(md, kCFUserNotificationSoundURLKey);
    if (url && CFGetTypeID((CFTypeRef)url) == CFURLGetTypeID()) {
        url = CFURLCopyAbsoluteURL(url);
        CFDictionaryRemoveValue(md, kCFUserNotificationSoundURLKey);
        path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFDictionaryAddValue(md, kCFUserNotificationSoundPathKey, path);
        CFRelease(url);
        CFRelease(path);
    }
    url = CFDictionaryGetValue(md, kCFUserNotificationLocalizationURLKey);
    if (url && CFGetTypeID((CFTypeRef)url) == CFURLGetTypeID()) {
        url = CFURLCopyAbsoluteURL(url);
        CFDictionaryRemoveValue(md, kCFUserNotificationLocalizationURLKey);
        path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFDictionaryAddValue(md, kCFUserNotificationLocalizationPathKey, path);
        CFRelease(url);
        CFRelease(path);
    }
    return md;
}

static SInt32 _CFUserNotificationSendRequest(CFAllocatorRef allocator, CFStringRef sessionID, mach_port_t replyPort, SInt32 token, CFTimeInterval timeout, CFOptionFlags flags, CFDictionaryRef dictionary) {
    CFDictionaryRef modifiedDictionary = NULL;
    SInt32 retval = ERR_SUCCESS, itimeout = (timeout > 0.0 && timeout < INT_MAX) ? (SInt32)timeout : 0;
    CFDataRef data;
    mach_msg_base_t *msg = NULL;
    mach_port_t bootstrapPort = MACH_PORT_NULL, serverPort = MACH_PORT_NULL;
    CFIndex size;
    char namebuffer[MAX_PORT_NAME_LENGTH + 1], oldnamebuffer[MAX_PORT_NAME_LENGTH + 1];
    size_t namelen;

    strcpy(namebuffer, NOTIFICATION_PORT_NAME);
    strcpy(oldnamebuffer, NOTIFICATION_PORT_NAME_OLD);
    if (sessionID) {
        strcat(namebuffer, NOTIFICATION_PORT_NAME_SUFFIX);
        namelen = strlen(namebuffer);
        CFStringGetBytes(sessionID, CFRangeMake(0, CFStringGetLength(sessionID)), kCFStringEncodingUTF8, 0, false, namebuffer + namelen, MAX_PORT_NAME_LENGTH - namelen, &size);
        namebuffer[namelen + size] = '\0';
        
        strcat(oldnamebuffer, NOTIFICATION_PORT_NAME_SUFFIX);
        namelen = strlen(oldnamebuffer);
        CFStringGetBytes(sessionID, CFRangeMake(0, CFStringGetLength(sessionID)), kCFStringEncodingUTF8, 0, false, oldnamebuffer + namelen, MAX_PORT_NAME_LENGTH - namelen, &size);
        oldnamebuffer[namelen + size] = '\0';
    }

    retval = task_get_bootstrap_port(mach_task_self(), &bootstrapPort);
    if (ERR_SUCCESS == retval && MACH_PORT_NULL != bootstrapPort) retval = bootstrap_look_up(bootstrapPort, namebuffer, &serverPort);
    if (ERR_SUCCESS != retval || MACH_PORT_NULL == serverPort) retval = bootstrap_look_up(bootstrapPort, oldnamebuffer, &serverPort);
    if (ERR_SUCCESS == retval && MACH_PORT_NULL != serverPort) {
        modifiedDictionary = _CFUserNotificationModifiedDictionary(allocator, dictionary, token, itimeout, _CFProcessNameString());
        if (modifiedDictionary) {
            data = CFPropertyListCreateXMLData(allocator, modifiedDictionary);
            if (data) {
                size = sizeof(mach_msg_base_t) + ((CFDataGetLength(data) + 3) & (~0x3));
                msg = (mach_msg_base_t *)CFAllocatorAllocate(allocator, size, 0);
                if (__CFOASafe) __CFSetLastAllocationEventName(msg, "CFUserNotification (temp)");
                if (msg) {
                    memset(msg, 0, size);
                    msg->header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
                    msg->header.msgh_size = size;
                    msg->header.msgh_remote_port = serverPort;
                    msg->header.msgh_local_port = replyPort;
                    msg->header.msgh_id = flags;
                    msg->body.msgh_descriptor_count = 0;
                    CFDataGetBytes(data, CFRangeMake(0, CFDataGetLength(data)), (uint8_t *)msg + sizeof(mach_msg_base_t));
                    //CFShow(CFStringCreateWithBytes(NULL, (UInt8 *)msg + sizeof(mach_msg_base_t), CFDataGetLength(data), kCFStringEncodingUTF8, false));
                    retval = mach_msg((mach_msg_header_t *)msg, MACH_SEND_MSG|MACH_SEND_TIMEOUT, size, 0, MACH_PORT_NULL, MESSAGE_TIMEOUT, MACH_PORT_NULL);
                    CFAllocatorDeallocate(allocator, msg);
                } else {
                    retval = unix_err(ENOMEM);
                }
                CFRelease(data);
            } else {
                retval = unix_err(ENOMEM);
            }
            CFRelease(modifiedDictionary);
        } else {
            retval = unix_err(ENOMEM);
        }
    }
    return retval;
}

CFUserNotificationRef CFUserNotificationCreate(CFAllocatorRef allocator, CFTimeInterval timeout, CFOptionFlags flags, SInt32 *error, CFDictionaryRef dictionary) {
    CFUserNotificationRef userNotification = NULL;
    SInt32 retval = ERR_SUCCESS;
    static uint16_t tokenCounter = 0;
    SInt32 token = ((getpid()<<16) | (tokenCounter++));
    CFStringRef sessionID = (dictionary ? CFDictionaryGetValue(dictionary, kCFUserNotificationSessionIDKey) : NULL);
    mach_port_t replyPort = MACH_PORT_NULL;

    if (!allocator) allocator = __CFGetDefaultAllocator();

    retval = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort);
    if (ERR_SUCCESS == retval && MACH_PORT_NULL != replyPort) retval = _CFUserNotificationSendRequest(allocator, sessionID, replyPort, token, timeout, flags, dictionary);
    if (ERR_SUCCESS == retval) {
        userNotification = (CFUserNotificationRef)_CFRuntimeCreateInstance(allocator, __kCFUserNotificationTypeID, sizeof(struct __CFUserNotification) - sizeof(CFRuntimeBase), NULL);
        if (userNotification) {
            userNotification->_replyPort = replyPort;
            userNotification->_token = token;
            userNotification->_timeout = timeout;
            userNotification->_requestFlags = flags;
            userNotification->_responseFlags = 0;
            userNotification->_sessionID = NULL;
            userNotification->_responseDictionary = NULL;
            userNotification->_machPort = NULL;
            userNotification->_callout = NULL;
            if (sessionID) userNotification->_sessionID = CFStringCreateCopy(allocator, sessionID);
        } else {
            retval = unix_err(ENOMEM);
        }
    } else {
        if (dictionary) CFUserNotificationLog(CFDictionaryGetValue(dictionary, kCFUserNotificationAlertHeaderKey), CFDictionaryGetValue(dictionary, kCFUserNotificationAlertMessageKey));
    }
    if (ERR_SUCCESS != retval && MACH_PORT_NULL != replyPort) mach_port_destroy(mach_task_self(), replyPort);
    if (error) *error = retval;
    return userNotification;
}

static void _CFUserNotificationMachPortCallBack(CFMachPortRef port, void *m, CFIndex size, void *info) {
    CFUserNotificationRef userNotification = (CFUserNotificationRef)info;
    mach_msg_base_t *msg = (mach_msg_base_t *)m;
    CFOptionFlags responseFlags = msg->header.msgh_id;
    CFDataRef responseData = CFDataCreate(NULL, (uint8_t *)msg + sizeof(mach_msg_base_t), msg->header.msgh_size - sizeof(mach_msg_base_t));
    if (responseData) {
        userNotification->_responseDictionary = CFPropertyListCreateFromXMLData(NULL, responseData, kCFPropertyListImmutable, NULL);
        CFRelease(responseData);
    }
    CFMachPortInvalidate(userNotification->_machPort);
    CFRelease(userNotification->_machPort);
    userNotification->_machPort = NULL;
    mach_port_destroy(mach_task_self(), userNotification->_replyPort);
    userNotification->_replyPort = MACH_PORT_NULL;
    userNotification->_callout(userNotification, responseFlags);
}

SInt32 CFUserNotificationReceiveResponse(CFUserNotificationRef userNotification, CFTimeInterval timeout, CFOptionFlags *responseFlags) {
    SInt32 retval = ERR_SUCCESS;
    mach_msg_timeout_t msgtime = (timeout > 0.0 && 1000.0 * timeout < INT_MAX) ? (mach_msg_timeout_t)(1000.0 * timeout) : 0;
    mach_msg_base_t *msg = NULL;
    CFIndex size = MAX_STRING_COUNT * MAX_STRING_LENGTH;
    CFDataRef responseData;
    
    if (userNotification && MACH_PORT_NULL != userNotification->_replyPort) {
        msg = (mach_msg_base_t *)CFAllocatorAllocate(CFGetAllocator(userNotification), size, 0);
	if (__CFOASafe) __CFSetLastAllocationEventName(msg, "CFUserNotification (temp)");
        if (msg) {
            memset(msg, 0, size);
            msg->header.msgh_size = size;
            if (msgtime > 0) {
                retval = mach_msg((mach_msg_header_t *)msg, MACH_RCV_MSG|MACH_RCV_TIMEOUT, 0, size, userNotification->_replyPort, msgtime, MACH_PORT_NULL);
            } else {
                retval = mach_msg((mach_msg_header_t *)msg, MACH_RCV_MSG, 0, size, userNotification->_replyPort, 0, MACH_PORT_NULL);
            }
            if (ERR_SUCCESS == retval) {
                if (responseFlags) *responseFlags = msg->header.msgh_id;
                responseData = CFDataCreate(NULL, (uint8_t *)msg + sizeof(mach_msg_base_t), msg->header.msgh_size - sizeof(mach_msg_base_t));
                if (responseData) {
                    userNotification->_responseDictionary = CFPropertyListCreateFromXMLData(NULL, responseData, kCFPropertyListImmutable, NULL);
                    CFRelease(responseData);
                }
                if (userNotification->_machPort) {
                    CFMachPortInvalidate(userNotification->_machPort);
                    CFRelease(userNotification->_machPort);
                    userNotification->_machPort = NULL;
                }
                mach_port_destroy(mach_task_self(), userNotification->_replyPort);
                userNotification->_replyPort = MACH_PORT_NULL;
            }
            CFAllocatorDeallocate(CFGetAllocator(userNotification), msg);
        } else {
            retval = unix_err(ENOMEM);
        }
    }
    return retval;
}

CFStringRef CFUserNotificationGetResponseValue(CFUserNotificationRef userNotification, CFStringRef key, CFIndex idx) {
    CFStringRef retval = NULL;
    CFTypeRef value = NULL;
    if (userNotification && userNotification->_responseDictionary) {
        value = CFDictionaryGetValue(userNotification->_responseDictionary, key);
        if (CFGetTypeID(value) == CFStringGetTypeID()) {
            if (0 == idx) {
                retval = (CFStringRef)value;
            }
        } else if (CFGetTypeID(value) == CFArrayGetTypeID()) {
            if (0 <= idx && idx < CFArrayGetCount((CFArrayRef)value)) {
                retval = (CFStringRef)CFArrayGetValueAtIndex((CFArrayRef)value, idx);
            }
        }
    }
    return retval;
}

CFDictionaryRef CFUserNotificationGetResponseDictionary(CFUserNotificationRef userNotification) {
    return userNotification ? userNotification->_responseDictionary : NULL;
}

SInt32 CFUserNotificationUpdate(CFUserNotificationRef userNotification, CFTimeInterval timeout, CFOptionFlags flags, CFDictionaryRef dictionary) {
    SInt32 retval = ERR_SUCCESS;
    if (userNotification && MACH_PORT_NULL != userNotification->_replyPort) {
        retval = _CFUserNotificationSendRequest(CFGetAllocator(userNotification), userNotification->_sessionID, userNotification->_replyPort, userNotification->_token, timeout, flags|kCFUserNotificationUpdateFlag, dictionary);
    }
    return retval;
}

SInt32 CFUserNotificationCancel(CFUserNotificationRef userNotification) {
    SInt32 retval = ERR_SUCCESS;
    if (userNotification && MACH_PORT_NULL != userNotification->_replyPort) {
        retval = _CFUserNotificationSendRequest(CFGetAllocator(userNotification), userNotification->_sessionID, userNotification->_replyPort, userNotification->_token, 0, kCFUserNotificationCancelFlag, NULL);
    }
    return retval;
}

CFRunLoopSourceRef CFUserNotificationCreateRunLoopSource(CFAllocatorRef allocator, CFUserNotificationRef userNotification, CFUserNotificationCallBack callout, CFIndex order) {
    CFRunLoopSourceRef source = NULL;
    if (userNotification && callout && !userNotification->_machPort && MACH_PORT_NULL != userNotification->_replyPort) {
        CFMachPortContext context = {0, userNotification, NULL, NULL, NULL};
        userNotification->_machPort = CFMachPortCreateWithPort(CFGetAllocator(userNotification), (mach_port_t)userNotification->_replyPort, _CFUserNotificationMachPortCallBack, &context, false);
    }
    if (userNotification && userNotification->_machPort) {
        source = CFMachPortCreateRunLoopSource(allocator, userNotification->_machPort, order);
        userNotification->_callout = callout;
    }
    return source;
}

SInt32 CFUserNotificationDisplayNotice(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL, CFURLRef localizationURL, CFStringRef alertHeader, CFStringRef alertMessage, CFStringRef defaultButtonTitle) {
    CFUserNotificationRef userNotification;
    SInt32 retval = ERR_SUCCESS;
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (iconURL) CFDictionaryAddValue(dict, kCFUserNotificationIconURLKey, iconURL);
    if (soundURL) CFDictionaryAddValue(dict, kCFUserNotificationSoundURLKey, soundURL);
    if (localizationURL) CFDictionaryAddValue(dict, kCFUserNotificationLocalizationURLKey, localizationURL);
    if (alertHeader) CFDictionaryAddValue(dict, kCFUserNotificationAlertHeaderKey, alertHeader);
    if (alertMessage) CFDictionaryAddValue(dict, kCFUserNotificationAlertMessageKey, alertMessage);
    if (defaultButtonTitle) CFDictionaryAddValue(dict, kCFUserNotificationDefaultButtonTitleKey, defaultButtonTitle);
    userNotification = CFUserNotificationCreate(NULL, timeout, flags, &retval, dict);
    if (userNotification) CFRelease(userNotification);
    CFRelease(dict);
    return retval;
}

CF_EXPORT SInt32 CFUserNotificationDisplayAlert(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL, CFURLRef localizationURL, CFStringRef alertHeader, CFStringRef alertMessage, CFStringRef defaultButtonTitle, CFStringRef alternateButtonTitle, CFStringRef otherButtonTitle, CFOptionFlags *responseFlags) {
    CFUserNotificationRef userNotification;
    SInt32 retval = ERR_SUCCESS;
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (iconURL) CFDictionaryAddValue(dict, kCFUserNotificationIconURLKey, iconURL);
    if (soundURL) CFDictionaryAddValue(dict, kCFUserNotificationSoundURLKey, soundURL);
    if (localizationURL) CFDictionaryAddValue(dict, kCFUserNotificationLocalizationURLKey, localizationURL);
    if (alertHeader) CFDictionaryAddValue(dict, kCFUserNotificationAlertHeaderKey, alertHeader);
    if (alertMessage) CFDictionaryAddValue(dict, kCFUserNotificationAlertMessageKey, alertMessage);
    if (defaultButtonTitle) CFDictionaryAddValue(dict, kCFUserNotificationDefaultButtonTitleKey, defaultButtonTitle);
    if (alternateButtonTitle) CFDictionaryAddValue(dict, kCFUserNotificationAlternateButtonTitleKey, alternateButtonTitle);
    if (otherButtonTitle) CFDictionaryAddValue(dict, kCFUserNotificationOtherButtonTitleKey, otherButtonTitle);
    userNotification = CFUserNotificationCreate(NULL, timeout, flags, &retval, dict);
    if (userNotification) {
        retval = CFUserNotificationReceiveResponse(userNotification, timeout, responseFlags);
        if (MACH_RCV_TIMED_OUT == retval) {
            retval = CFUserNotificationCancel(userNotification);
            if (responseFlags) *responseFlags = kCFUserNotificationCancelResponse;
        }
        CFRelease(userNotification);
    }
    CFRelease(dict);
    return retval;
}

#else /* __MACH__ */

#warning CFUserNotification functions not fully implemented

void __CFUserNotificationDeallocate(CFTypeRef cf) {
}

CFUserNotificationRef CFUserNotificationCreate(CFAllocatorRef allocator, CFTimeInterval timeout, CFOptionFlags flags, SInt32 *error, CFDictionaryRef dictionary) {
    CFUserNotificationLog(CFDictionaryGetValue(dictionary, kCFUserNotificationAlertHeaderKey), CFDictionaryGetValue(dictionary, kCFUserNotificationAlertMessageKey));
    return NULL;
}

SInt32 CFUserNotificationReceiveResponse(CFUserNotificationRef userNotification, CFTimeInterval timeout, CFOptionFlags *responseFlags) {
    return -1;
}

CFDictionaryRef CFUserNotificationCopyResponseDictionary(CFUserNotificationRef userNotification) {
    return NULL;
}

SInt32 CFUserNotificationUpdate(CFUserNotificationRef userNotification, CFTimeInterval timeout, CFOptionFlags flags, CFDictionaryRef dictionary) {
    return -1;
}

SInt32 CFUserNotificationCancel(CFUserNotificationRef userNotification) {
    return -1;
}

CFRunLoopSourceRef CFUserNotificationCreateRunLoopSource(CFAllocatorRef allocator, CFUserNotificationRef userNotification, CFUserNotificationCallBack callout, CFIndex order) {
    return NULL;
}

SInt32 CFUserNotificationDisplayNotice(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL, CFURLRef localizationURL, CFStringRef alertHeader, CFStringRef alertMessage, CFStringRef defaultButtonTitle) {
    CFUserNotificationLog(alertHeader, alertMessage);
    return -1;
}

SInt32 CFUserNotificationDisplayAlert(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL, CFURLRef localizationURL, CFStringRef alertHeader, CFStringRef alertMessage, CFStringRef defaultButtonTitle, CFStringRef alternateButtonTitle, CFStringRef otherButtonTitle, CFOptionFlags *responseFlags) {
    CFUserNotificationLog(alertHeader, alertMessage);
    return -1;
}

#endif /* __MACH__ */

#undef __kCFLogUserNotification
#undef CFUserNotificationLog
#undef MAX_STRING_LENGTH
#undef MAX_STRING_COUNT
#undef NOTIFICATION_PORT_NAME
#undef NOTIFICATION_PORT_NAME_OLD
#undef MESSAGE_TIMEOUT

