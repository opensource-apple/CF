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
/*
*/

#include <CoreFoundation/CFBase.h>
#include <sys/types.h>
#include <sys/event.h>

typedef struct {
    CFIndex	version;
    void *	info;
    const void *(*retain)(const void *info);
    void	(*release)(const void *info);
    CFStringRef	(*copyDescription)(const void *info);
    Boolean	(*equal)(const void *info1, const void *info2);
    CFHashCode	(*hash)(const void *info);
    void	(*perform)(const struct kevent *kev, void *info);
    struct kevent event;
} CFRunLoopSourceContext2;

// The bits in the flags field in the kevent structure are cleared except for EV_ONESHOT and EV_CLEAR.
// Do not use the udata field of the kevent structure -- that field is smashed by CFRunLoop.
// There is no way to EV_ENABLE or EV_DISABLE a kevent.
// The "autoinvalidation" of EV_ONESHOT is not handled properly by CFRunLoop yet.
// The "autoinvalidation" of EV_DELETE on the last close of a file descriptor is not handled properly by CFRunLoop yet.
// There is no way to reset the state in a kevent (such as clearing the EV_EOF state for fifos).

