/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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

/*	CFPlugIn_Factory.c
	Copyright (c) 1999-2009, Apple Inc.  All rights reserved.
	Responsibility: Doug Davidson
*/

#include "CFBundle_Internal.h"
#include "CFInternal.h"

static CFSpinLock_t CFPlugInGlobalDataLock = CFSpinLockInit;
static CFMutableDictionaryRef _factoriesByFactoryID = NULL; /* Value is _CFPFactory */
static CFMutableDictionaryRef _factoriesByTypeID = NULL; /* Value is array of _CFPFactory */

static void _CFPFactoryAddToTable(_CFPFactory *factory) {
    __CFSpinLock(&CFPlugInGlobalDataLock);
    if (!_factoriesByFactoryID) {
        CFDictionaryValueCallBacks _factoryDictValueCallbacks = {0, NULL, NULL, NULL, NULL};
        _factoriesByFactoryID = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &_factoryDictValueCallbacks);
    }
    CFDictionarySetValue(_factoriesByFactoryID, factory->_uuid, factory);
    __CFSpinUnlock(&CFPlugInGlobalDataLock);
}

static void _CFPFactoryRemoveFromTable(_CFPFactory *factory) {
    __CFSpinLock(&CFPlugInGlobalDataLock);
    if (_factoriesByFactoryID) CFDictionaryRemoveValue(_factoriesByFactoryID, factory->_uuid);
    __CFSpinUnlock(&CFPlugInGlobalDataLock);
}

__private_extern__ _CFPFactory *_CFPFactoryFind(CFUUIDRef factoryID, Boolean enabled) {
    _CFPFactory *result = NULL;
    
    __CFSpinLock(&CFPlugInGlobalDataLock);
    if (_factoriesByFactoryID) {
        result = (_CFPFactory *)CFDictionaryGetValue(_factoriesByFactoryID, factoryID);
        if (result && result->_enabled != enabled) result = NULL;
    }
    __CFSpinUnlock(&CFPlugInGlobalDataLock);
    return result;
}

static void _CFPFactoryDeallocate(_CFPFactory *factory) {
    CFAllocatorRef allocator = factory->_allocator;
    SInt32 c;
    
    _CFPFactoryRemoveFromTable(factory);

    if (factory->_plugIn) _CFPlugInRemoveFactory(factory->_plugIn, factory);

    /* Remove all types for this factory. */
    c = CFArrayGetCount(factory->_types);
    while (c-- > 0) _CFPFactoryRemoveType(factory, (CFUUIDRef)CFArrayGetValueAtIndex(factory->_types, c));
    CFRelease(factory->_types);

    if (factory->_funcName) CFRelease(factory->_funcName);
    if (factory->_uuid) CFRelease(factory->_uuid);

    CFAllocatorDeallocate(allocator, factory);
    CFRelease(allocator);
}

static _CFPFactory *_CFPFactoryCommonCreate(CFAllocatorRef allocator, CFUUIDRef factoryID) {
    _CFPFactory *factory;
    UInt32 size;
    size = sizeof(_CFPFactory);
    allocator = (allocator ? (CFAllocatorRef)CFRetain(allocator) : (CFAllocatorRef)CFRetain(__CFGetDefaultAllocator()));
    factory = (_CFPFactory *)CFAllocatorAllocate(allocator, size, 0);
    if (!factory) {
        CFRelease(allocator);
        return NULL;
    }

    factory->_allocator = allocator;
    factory->_uuid = (CFUUIDRef)CFRetain(factoryID);
    factory->_enabled = true;
    factory->_instanceCount = 0;

    _CFPFactoryAddToTable(factory);

    factory->_types = CFArrayCreateMutable(allocator, 0, &kCFTypeArrayCallBacks);

    return factory;
}

__private_extern__ _CFPFactory *_CFPFactoryCreate(CFAllocatorRef allocator, CFUUIDRef factoryID, CFPlugInFactoryFunction func) {
    _CFPFactory *factory = _CFPFactoryCommonCreate(allocator, factoryID);

    factory->_func = func;
    factory->_plugIn = NULL;
    factory->_funcName = NULL;

    return factory;
}

__private_extern__ _CFPFactory *_CFPFactoryCreateByName(CFAllocatorRef allocator, CFUUIDRef factoryID, CFPlugInRef plugIn, CFStringRef funcName) {
    _CFPFactory *factory = _CFPFactoryCommonCreate(allocator, factoryID);

    factory->_func = NULL;
    factory->_plugIn = plugIn;
    if (plugIn) _CFPlugInAddFactory(plugIn, factory);
    factory->_funcName = (funcName ? (CFStringRef)CFStringCreateCopy(allocator, funcName) : NULL);

    return factory;
}

__private_extern__ CFUUIDRef _CFPFactoryGetFactoryID(_CFPFactory *factory) {
    return factory->_uuid;
}

__private_extern__ CFPlugInRef _CFPFactoryGetPlugIn(_CFPFactory *factory) {
    return factory->_plugIn;
}

__private_extern__ void *_CFPFactoryCreateInstance(CFAllocatorRef allocator, _CFPFactory *factory, CFUUIDRef typeID) {
    void *result = NULL;
    if (factory->_enabled) {
        if (!factory->_func) {
            factory->_func = (CFPlugInFactoryFunction)CFBundleGetFunctionPointerForName(factory->_plugIn, factory->_funcName);
            if (!factory->_func) CFLog(__kCFLogPlugIn, CFSTR("Cannot find function pointer %@ for factory %@ in %@"), factory->_funcName, factory->_uuid, factory->_plugIn);
#if BINARY_SUPPORT_CFM
            if (factory->_func) {
                // return values from CFBundleGetFunctionPointerForName will always be dyld, but we must force-fault them because pointers to glue code do not fault correctly
                factory->_func = (void *)((uint32_t)(factory->_func) | 0x1);
            }
#endif /* BINARY_SUPPORT_CFM */
        }
        if (factory->_func) {
            // UPPGOOP
            FAULT_CALLBACK((void **)&(factory->_func));
            result = (void *)INVOKE_CALLBACK2(factory->_func, allocator, typeID);
        }
    } else {
        CFLog(__kCFLogPlugIn, CFSTR("Factory %@ is disabled"), factory->_uuid);
    }
    return result;
}

__private_extern__ void _CFPFactoryDisable(_CFPFactory *factory) {
    factory->_enabled = false;
    if (factory->_instanceCount == 0) _CFPFactoryDeallocate(factory);
}

__private_extern__ Boolean _CFPFactoryIsEnabled(_CFPFactory *factory) {
    return factory->_enabled;
}

__private_extern__ void _CFPFactoryFlushFunctionCache(_CFPFactory *factory) {
    /* MF:!!! Assert that this factory belongs to a plugIn. */
    /* This is called by the factory's plugIn when the plugIn unloads its code. */
    factory->_func = NULL;
}

__private_extern__ void _CFPFactoryAddType(_CFPFactory *factory, CFUUIDRef typeID) {
    CFMutableArrayRef array;
    
    /* Add the type to the factory's type list */
    CFArrayAppendValue(factory->_types, typeID);

    /* Add the factory to the type's array of factories */
    __CFSpinLock(&CFPlugInGlobalDataLock);
    if (!_factoriesByTypeID) _factoriesByTypeID = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    array = (CFMutableArrayRef)CFDictionaryGetValue(_factoriesByTypeID, typeID);
    if (!array) {
        CFArrayCallBacks _factoryArrayCallbacks = {0, NULL, NULL, NULL, NULL};
        // Create this from default allocator
        array = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &_factoryArrayCallbacks);
        CFDictionarySetValue(_factoriesByTypeID, typeID, array);
        CFRelease(array);
    }
    CFArrayAppendValue(array, factory);
    __CFSpinUnlock(&CFPlugInGlobalDataLock);
}

__private_extern__ void _CFPFactoryRemoveType(_CFPFactory *factory, CFUUIDRef typeID) {
    /* Remove it from the factory's type list */
    SInt32 idx;

    idx = CFArrayGetFirstIndexOfValue(factory->_types, CFRangeMake(0, CFArrayGetCount(factory->_types)), typeID);
    if (idx >= 0) CFArrayRemoveValueAtIndex(factory->_types, idx);

    /* Remove the factory from the type's list of factories */
    __CFSpinLock(&CFPlugInGlobalDataLock);
    if (_factoriesByTypeID) {
        CFMutableArrayRef array = (CFMutableArrayRef)CFDictionaryGetValue(_factoriesByTypeID, typeID);
        if (array) {
            idx = CFArrayGetFirstIndexOfValue(array, CFRangeMake(0, CFArrayGetCount(array)), factory);
            if (idx >= 0) {
                CFArrayRemoveValueAtIndex(array, idx);
                if (CFArrayGetCount(array) == 0) CFDictionaryRemoveValue(_factoriesByTypeID, typeID);
            }
        }
    }
    __CFSpinUnlock(&CFPlugInGlobalDataLock);
}

__private_extern__ Boolean _CFPFactorySupportsType(_CFPFactory *factory, CFUUIDRef typeID) {
    SInt32 idx;

    idx = CFArrayGetFirstIndexOfValue(factory->_types, CFRangeMake(0, CFArrayGetCount(factory->_types)), typeID);
    return (idx >= 0 ? true : false);
}

__private_extern__ CFArrayRef _CFPFactoryFindForType(CFUUIDRef typeID) {
    CFArrayRef result = NULL;

    __CFSpinLock(&CFPlugInGlobalDataLock);
    if (_factoriesByTypeID) result = (CFArrayRef)CFDictionaryGetValue(_factoriesByTypeID, typeID);
    __CFSpinUnlock(&CFPlugInGlobalDataLock);

    return result;
}

/* These methods are called by CFPlugInInstance when an instance is created or destroyed.  If a factory's instance count goes to 0 and the factory has been disabled, the factory is destroyed. */
__private_extern__ void _CFPFactoryAddInstance(_CFPFactory *factory) {
    /* MF:!!! Assert that factory is enabled. */
    factory->_instanceCount++;
    if (factory->_plugIn) _CFPlugInAddPlugInInstance(factory->_plugIn);
}

__private_extern__ void _CFPFactoryRemoveInstance(_CFPFactory *factory) {
    /* MF:!!! Assert that _instanceCount > 0. */
    factory->_instanceCount--;
    if (factory->_plugIn) _CFPlugInRemovePlugInInstance(factory->_plugIn);
    if (factory->_instanceCount == 0 && !factory->_enabled) _CFPFactoryDeallocate(factory);
}
