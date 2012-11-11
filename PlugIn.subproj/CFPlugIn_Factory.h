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
/*	CFPlugIn_Factory.h
	Copyright (c) 1999-2003, Apple, Inc. All rights reserved.
*/

#if !defined(__COREFOUNDATION_CFPLUGIN_FACTORY__)
#define __COREFOUNDATION_CFPLUGIN_FACTORY__ 1

#include "CFBundle_Internal.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct __CFPFactory {
    CFAllocatorRef _allocator;

    CFUUIDRef _uuid;
    Boolean _enabled;
    char _padding[3];
    SInt32 _instanceCount;

    CFPlugInFactoryFunction _func;
    
    CFPlugInRef _plugIn;
    CFStringRef _funcName;

    CFMutableArrayRef _types;
} _CFPFactory;

extern _CFPFactory *_CFPFactoryCreate(CFAllocatorRef allocator, CFUUIDRef factoryID, CFPlugInFactoryFunction func);
extern _CFPFactory *_CFPFactoryCreateByName(CFAllocatorRef allocator, CFUUIDRef factoryID, CFPlugInRef plugIn, CFStringRef funcName);

extern _CFPFactory *_CFPFactoryFind(CFUUIDRef factoryID, Boolean enabled);

extern CFUUIDRef _CFPFactoryGetFactoryID(_CFPFactory *factory);
extern CFPlugInRef _CFPFactoryGetPlugIn(_CFPFactory *factory);

extern void *_CFPFactoryCreateInstance(CFAllocatorRef allocator, _CFPFactory *factory, CFUUIDRef typeID);
extern void _CFPFactoryDisable(_CFPFactory *factory);
extern Boolean _CFPFactoryIsEnabled(_CFPFactory *factory);

extern void _CFPFactoryFlushFunctionCache(_CFPFactory *factory);

extern void _CFPFactoryAddType(_CFPFactory *factory, CFUUIDRef typeID);
extern void _CFPFactoryRemoveType(_CFPFactory *factory, CFUUIDRef typeID);

extern Boolean _CFPFactorySupportsType(_CFPFactory *factory, CFUUIDRef typeID);
extern CFArrayRef _CFPFactoryFindForType(CFUUIDRef typeID);

/* These methods are called by CFPlugInInstance when an instance is created or destroyed.  If a factory's instance count goes to 0 and the factory has been disabled, the factory is destroyed. */
extern void _CFPFactoryAddInstance(_CFPFactory *factory);
extern void _CFPFactoryRemoveInstance(_CFPFactory *factory);

#if defined(__cplusplus)
}
#endif

#endif /* ! __COREFOUNDATION_CFPLUGIN_FACTORY__ */

