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
/*	CoreFoundation.h
	Copyright (c) 1998-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_COREFOUNDATION__)
#define __COREFOUNDATION_COREFOUNDATION__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFBag.h>
#include <CoreFoundation/CFCharacterSet.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFTree.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFXMLNode.h>
#include <CoreFoundation/CFXMLParser.h>

#include <stdarg.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__STDC_VERSION__) && (199901L <= __STDC_VERSION__)

#include <inttypes.h>
//#include <iso646.h>
#include <stdbool.h>
#include <stdint.h>

#endif

#include <CoreFoundation/CFBinaryHeap.h>
#include <CoreFoundation/CFBitVector.h>
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFByteOrder.h>
#include <CoreFoundation/CFPlugIn.h>
#include <CoreFoundation/CFURLAccess.h>
#include <CoreFoundation/CFUUID.h>

#if defined(__MACH__)
#include <CoreFoundation/CFUserNotification.h>
#endif

#if defined(__MACH__) || defined(__WIN32__)
#include <CoreFoundation/CFMachPort.h>
#include <CoreFoundation/CFMessagePort.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSocket.h>
#endif


#endif /* ! __COREFOUNDATION_COREFOUNDATION__ */

