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
/*	CFPlugIn_PlugIn.c
	Copyright 1999-2002, Apple, Inc. All rights reserved.
	Responsibility: Doug Davidson
*/

#include "CFBundle_Internal.h"
#include "CFInternal.h"

static void _registerFactory(const void *key, const void *val, void *context) {
    CFStringRef factoryIDStr = (CFStringRef)key;
    CFStringRef factoryFuncStr = (CFStringRef)val;
    CFBundleRef bundle = (CFBundleRef)context;
    CFUUIDRef factoryID = (CFGetTypeID(factoryIDStr) == CFStringGetTypeID()) ? CFUUIDCreateFromString(NULL, factoryIDStr) : NULL;
    if (NULL == factoryID) factoryID = CFRetain(factoryIDStr);
    if (CFGetTypeID(factoryFuncStr) != CFStringGetTypeID() || CFStringGetLength(factoryFuncStr) <= 0) factoryFuncStr = NULL;
    CFPlugInRegisterFactoryFunctionByName(factoryID, bundle, factoryFuncStr);
    if (NULL != factoryID) CFRelease(factoryID);
}

static void _registerType(const void *key, const void *val, void *context) {
    CFStringRef typeIDStr = (CFStringRef)key;
    CFArrayRef factoryIDStrArray = (CFArrayRef)val;
    CFBundleRef bundle = (CFBundleRef)context;
    SInt32 i, c = (CFGetTypeID(factoryIDStrArray) == CFArrayGetTypeID()) ? CFArrayGetCount(factoryIDStrArray) : 0;
    CFStringRef curFactoryIDStr;
    CFUUIDRef typeID = (CFGetTypeID(typeIDStr) == CFStringGetTypeID()) ? CFUUIDCreateFromString(NULL, typeIDStr) : NULL;
    CFUUIDRef curFactoryID;
    if (NULL == typeID) typeID = CFRetain(typeIDStr);
    if (0 == c && (CFGetTypeID(factoryIDStrArray) != CFArrayGetTypeID())) {
        curFactoryIDStr = (CFStringRef)val;
        curFactoryID = (CFGetTypeID(curFactoryIDStr) == CFStringGetTypeID()) ? CFUUIDCreateFromString(CFGetAllocator(bundle), curFactoryIDStr) : NULL;
        if (NULL == curFactoryID) curFactoryID = CFRetain(curFactoryIDStr);
        CFPlugInRegisterPlugInType(curFactoryID, typeID);
        if (NULL != curFactoryID) CFRelease(curFactoryID);
    } else for (i=0; i<c; i++) {
        curFactoryIDStr = (CFStringRef)CFArrayGetValueAtIndex(factoryIDStrArray, i);
        curFactoryID = (CFGetTypeID(curFactoryIDStr) == CFStringGetTypeID()) ? CFUUIDCreateFromString(CFGetAllocator(bundle), curFactoryIDStr) : NULL;
        if (NULL == curFactoryID) curFactoryID = CFRetain(curFactoryIDStr);
        CFPlugInRegisterPlugInType(curFactoryID, typeID);
        if (NULL != curFactoryID) CFRelease(curFactoryID);
    }
    if (NULL != typeID) CFRelease(typeID);
}

__private_extern__ void _CFBundleInitPlugIn(CFBundleRef bundle) {
    CFArrayCallBacks _pluginFactoryArrayCallbacks = {0, NULL, NULL, NULL, NULL};
    Boolean doDynamicReg = false;
    CFDictionaryRef infoDict;
    CFDictionaryRef factoryDict;
    CFDictionaryRef typeDict;
    CFStringRef tempStr;

    infoDict = CFBundleGetInfoDictionary(bundle);
    if (infoDict == NULL) {
        return;
    }
    factoryDict = CFDictionaryGetValue(infoDict, kCFPlugInFactoriesKey);
    if (factoryDict != NULL && CFGetTypeID(factoryDict) != CFDictionaryGetTypeID()) factoryDict = NULL;
    tempStr = CFDictionaryGetValue(infoDict, kCFPlugInDynamicRegistrationKey);
    if (tempStr != NULL && CFGetTypeID(tempStr) == CFStringGetTypeID() && CFStringCompare(tempStr, CFSTR("YES"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        doDynamicReg = true;
    }
    if (!factoryDict && !doDynamicReg) {
        // This is not a plugIn.
        return;
    }

    /* loadOnDemand is true by default if the plugIn does not do dynamic registration.  It is false, by default if it does do dynamic registration.  The dynamic register function can set this. */
    __CFBundleGetPlugInData(bundle)->_isPlugIn = true;
    __CFBundleGetPlugInData(bundle)->_loadOnDemand = true;
    __CFBundleGetPlugInData(bundle)->_isDoingDynamicRegistration = false;
    __CFBundleGetPlugInData(bundle)->_instanceCount = 0;

    __CFBundleGetPlugInData(bundle)->_factories = CFArrayCreateMutable(CFGetAllocator(bundle), 0, &_pluginFactoryArrayCallbacks);

    /* Now do the registration */

    /* First do static registrations, if any. */
    if (factoryDict != NULL) {
        CFDictionaryApplyFunction(factoryDict, _registerFactory, bundle);
    }
    typeDict = CFDictionaryGetValue(infoDict, kCFPlugInTypesKey);
    if (typeDict != NULL && CFGetTypeID(typeDict) != CFDictionaryGetTypeID()) typeDict = NULL;
    if (typeDict != NULL) {
        CFDictionaryApplyFunction(typeDict, _registerType, bundle);
    }

    /* Now do dynamic registration if necessary */
    if (doDynamicReg) {
        tempStr = CFDictionaryGetValue(infoDict, kCFPlugInDynamicRegisterFunctionKey);
        if (tempStr == NULL || CFGetTypeID(tempStr) != CFStringGetTypeID() || CFStringGetLength(tempStr) <= 0) {
            tempStr = CFSTR("CFPlugInDynamicRegister");
        }
        __CFBundleGetPlugInData(bundle)->_loadOnDemand = false;

        if (CFBundleLoadExecutable(bundle)) {
            CFPlugInDynamicRegisterFunction func = NULL;

            __CFBundleGetPlugInData(bundle)->_isDoingDynamicRegistration = true;

            /* Find the symbol and call it. */
            func = (CFPlugInDynamicRegisterFunction)CFBundleGetFunctionPointerForName(bundle, tempStr);
            if (func) {
                func(bundle);
                // MF:!!! Unload function is never called.  Need to deal with this!
            }

            __CFBundleGetPlugInData(bundle)->_isDoingDynamicRegistration = false;
            if (__CFBundleGetPlugInData(bundle)->_loadOnDemand && (__CFBundleGetPlugInData(bundle)->_instanceCount == 0)) {
                /* Unload now if we can/should. */
                CFBundleUnloadExecutable(bundle);
            }
        }
    }
}

__private_extern__ void _CFBundleDeallocatePlugIn(CFBundleRef bundle) {
    if (__CFBundleGetPlugInData(bundle)->_isPlugIn) {
        SInt32 c;

        /* Go through factories disabling them.  Disabling these factories should cause them to dealloc since we wouldn't be deallocating if any of the factories had outstanding instances.  So go backwards. */
        c = CFArrayGetCount(__CFBundleGetPlugInData(bundle)->_factories);
        while (c--) {
            _CFPFactoryDisable((_CFPFactory *)CFArrayGetValueAtIndex(__CFBundleGetPlugInData(bundle)->_factories, c));
        }
        CFRelease(__CFBundleGetPlugInData(bundle)->_factories);

        __CFBundleGetPlugInData(bundle)->_isPlugIn = false;
    }
}

UInt32 CFPlugInGetTypeID(void) {
    return CFBundleGetTypeID();
}

CFPlugInRef CFPlugInCreate(CFAllocatorRef allocator, CFURLRef plugInURL) {
    return (CFPlugInRef)CFBundleCreate(allocator, plugInURL);
}

CFBundleRef CFPlugInGetBundle(CFPlugInRef plugIn) {
    return (CFBundleRef)plugIn;
}

void CFPlugInSetLoadOnDemand(CFPlugInRef plugIn, Boolean flag) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        __CFBundleGetPlugInData(plugIn)->_loadOnDemand = flag;
        if (__CFBundleGetPlugInData(plugIn)->_loadOnDemand && !__CFBundleGetPlugInData(plugIn)->_isDoingDynamicRegistration && (__CFBundleGetPlugInData(plugIn)->_instanceCount == 0)) {
            /* Unload now if we can/should. */
            /* If we are doing dynamic registration currently, do not unload.  The unloading will happen when dynamic registration is done, if necessary. */
            CFBundleUnloadExecutable(plugIn);
        } else if (!__CFBundleGetPlugInData(plugIn)->_loadOnDemand) {
            /* Make sure we're loaded now. */
            CFBundleLoadExecutable(plugIn);
        }
    }
}

Boolean CFPlugInIsLoadOnDemand(CFPlugInRef plugIn) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        return __CFBundleGetPlugInData(plugIn)->_loadOnDemand;
    } else {
        return false;
    }
}

__private_extern__ void _CFPlugInWillUnload(CFPlugInRef plugIn) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        SInt32 c = CFArrayGetCount(__CFBundleGetPlugInData(plugIn)->_factories);
        /* First, flush all the function pointers that may be cached by our factories. */
        while (c--) {
            _CFPFactoryFlushFunctionCache((_CFPFactory *)CFArrayGetValueAtIndex(__CFBundleGetPlugInData(plugIn)->_factories, c));
        }
    }
}

__private_extern__ void _CFPlugInAddPlugInInstance(CFPlugInRef plugIn) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        if ((__CFBundleGetPlugInData(plugIn)->_instanceCount == 0) && (__CFBundleGetPlugInData(plugIn)->_loadOnDemand)) {
            /* Make sure we are not scheduled for unloading */
            _CFBundleUnscheduleForUnloading(CFPlugInGetBundle(plugIn));
        }
        __CFBundleGetPlugInData(plugIn)->_instanceCount++;
        /* Instances also retain the CFBundle */
        CFRetain(plugIn);
    }
}

__private_extern__ void _CFPlugInRemovePlugInInstance(CFPlugInRef plugIn) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        /* MF:!!! Assert that instanceCount > 0. */
        __CFBundleGetPlugInData(plugIn)->_instanceCount--;
        if ((__CFBundleGetPlugInData(plugIn)->_instanceCount == 0) && (__CFBundleGetPlugInData(plugIn)->_loadOnDemand)) {
            // We unload the code lazily because the code that caused this function to be called is probably code from the plugin itself.  If we unload now, we will hose things.
            //CFBundleUnloadExecutable(plugIn);
            _CFBundleScheduleForUnloading(CFPlugInGetBundle(plugIn));
        }
        /* Instances also retain the CFPlugIn */
        /* MF:!!! This will cause immediate unloading if it was the last ref on the plugin. */
        CFRelease(plugIn);
    }
}

__private_extern__ void _CFPlugInAddFactory(CFPlugInRef plugIn, _CFPFactory *factory) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        CFArrayAppendValue(__CFBundleGetPlugInData(plugIn)->_factories, factory);
    }
}

__private_extern__ void _CFPlugInRemoveFactory(CFPlugInRef plugIn, _CFPFactory *factory) {
    if (__CFBundleGetPlugInData(plugIn)->_isPlugIn) {
        SInt32 idx = CFArrayGetFirstIndexOfValue(__CFBundleGetPlugInData(plugIn)->_factories, CFRangeMake(0, CFArrayGetCount(__CFBundleGetPlugInData(plugIn)->_factories)), factory);
        if (idx >= 0) {
            CFArrayRemoveValueAtIndex(__CFBundleGetPlugInData(plugIn)->_factories, idx);
        }
    }
}
