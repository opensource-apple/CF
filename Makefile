#
# Define sets of files to build, other info specific to this project.
#

NAME = CoreFoundation

SUBPROJECTS = AppServices Base Collections Locale NumberDate Parsing PlugIn Preferences \
		RunLoop Stream String StringEncodings URL

AppServices_PUBHEADERS = CFUserNotification.h
AppServices_SOURCES = CFUserNotification.c
Base_PROJHEADERS = CFInternal.h ForFoundationOnly.h auto_stubs.h CFRuntime.h CFUtilities.h
Base_PRIVHEADERS = CFPriv.h CFRuntime.h CFUtilities.h CFUtilitiesPriv.h
Base_PUBHEADERS = CFBase.h CFByteOrder.h CoreFoundation.h CFUUID.h
Base_SOURCES = CFBase.c CFUtilities.c CFSortFunctions.c CFSystemDirectories.c \
		CFRuntime.c CFFileUtilities.c CFPlatform.c CFUUID.c uuid.c
Collections_PRIVHEADERS = CFStorage.h
Collections_PUBHEADERS = CFArray.h CFBag.h CFBinaryHeap.h CFBitVector.h \
		CFData.h CFDictionary.h CFSet.h CFStorage.h CFTree.h
Collections_SOURCES = CFArray.c CFBag.c CFBinaryHeap.c CFBitVector.c \
		CFData.c CFDictionary.c CFSet.c CFStorage.c CFTree.c
Locale_PUBHEADERS = CFLocale.h
NumberDate_PUBHEADERS = CFDate.h CFNumber.h CFTimeZone.h 
NumberDate_SOURCES = CFDate.c CFNumber.c CFTimeZone.c
Parsing_PROJHEADERS = CFXMLInputStream.h
Parsing_PUBHEADERS = CFPropertyList.h CFXMLParser.h CFXMLNode.h
Parsing_SOURCES = CFBinaryPList.c CFPropertyList.c CFXMLParser.c \
		CFXMLInputStream.c CFXMLNode.c CFXMLTree.c
PlugIn_PROJHEADERS = CFBundle_BinaryTypes.h CFBundle_Internal.h CFPlugIn_Factory.h
PlugIn_PRIVHEADERS = CFBundlePriv.h
PlugIn_PUBHEADERS = CFBundle.h CFPlugIn.h CFPlugInCOM.h
PlugIn_SOURCES = CFBundle.c CFBundle_Resources.c CFPlugIn.c CFPlugIn_Factory.c \
		CFPlugIn_Instance.c CFPlugIn_PlugIn.c
Preferences_PUBHEADERS = CFPreferences.h
Preferences_SOURCES = CFApplicationPreferences.c CFPreferences.c CFXMLPreferencesDomain.c
RunLoop_PUBHEADERS = CFMachPort.h CFMessagePort.h CFRunLoop.h CFSocket.h
RunLoop_PRIVHEADERS = CFRunLoopPriv.h
RunLoop_SOURCES = CFMachPort.c CFMessagePort.c CFRunLoop.c CFSocket.c
ifeq "$(PLATFORM)" "CYGWIN"
RunLoop_PUBHEADERS += CFWindowsMessageQueue.h
RunLoop_SOURCES += CFWindowsMessageQueue.c 
endif
Stream_PRIVHEADERS = CFStreamPriv.h CFStreamAbstract.h
Stream_PUBHEADERS = CFStream.h
Stream_SOURCES = CFStream.c CFConcreteStreams.c CFSocketStream.c
String_PRIVHEADERS = CFCharacterSetPriv.h CFStringDefaultEncoding.h
String_PUBHEADERS = CFCharacterSet.h CFString.h CFStringEncodingExt.h
String_SOURCES = CFCharacterSet.c CFString.c CFStringEncodings.c \
		CFStringScanner.c CFStringUtilities.c
StringEncodings_PROJHEADERS = CFUniCharPriv.h CFStringEncodingConverterPriv.h
StringEncodings_PRIVHEADERS = CFUniChar.h CFStringEncodingConverter.h \
		CFUnicodeDecomposition.h CFUnicodePrecomposition.h \
		CFStringEncodingConverterExt.h
StringEncodings_SOURCES = CFStringEncodingConverter.c CFBuiltinConverters.c \
		CFUnicodeDecomposition.c CFUnicodePrecomposition.c CFUniChar.c
URL_PUBHEADERS = CFURL.h CFURLAccess.h
URL_SOURCES = CFURL.c CFURLAccess.c

OTHER_SOURCES = version.c Makefile APPLE_LICENSE PropertyList.dtd

# These are the actual vars that are used by framework.make
PUBLIC_HFILES = $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PUBHEADERS), $(SRCROOT)/$(S).subproj/$(F)))
PRIVATE_HFILES = $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PRIVHEADERS), $(SRCROOT)/$(S).subproj/$(F)))
PROJECT_HFILES = $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PROJHEADERS), $(SRCROOT)/$(S).subproj/$(F)))
CFILES = $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_SOURCES), $(SRCROOT)/$(S).subproj/$(F)))


-include nonOpenSource.make

include framework.make


#
# Misc additional options
#

CURRENT_PROJECT_VERSION = 368.27

# common items all build styles should be defining
CFLAGS += -DCF_BUILDING_CF=1
CPPFLAGS += -DCF_BUILDING_CF=1

# base addr is set to come before CFNetwork - use the rebase MS command to see the sizes
# more info at http://msdn.microsoft.com/library/en-us/tools/tools/rebase.asp
ifeq "$(PLATFORM)" "CYGWIN"
C_WARNING_FLAGS += -Wno-endif-labels
CPP_WARNING_FLAGS += -Wno-endif-labels
LIBS += -lole32 -lws2_32
LFLAGS += -Wl,--image-base=0x66000000
endif

ifeq "$(PLATFORM)" "Darwin"
CFLAGS += -F/System/Library/Frameworks/CoreServices.framework/Frameworks
CPPFLAGS += -F/System/Library/Frameworks/CoreServices.framework/Frameworks
LIBS += -licucore -lobjc
LFLAGS += -compatibility_version 150 -current_version $(CURRENT_PROJECT_VERSION) -Wl,-init,___CFInitialize
endif

ifeq "$(PLATFORM)" "FreeBSD"
LFLAGS += -shared
endif

ifeq "$(PLATFORM)" "Linux"
LIBS += -lpthread
endif

ifeq "$(LIBRARY_STYLE)" "Library"
CHARACTERSETS_INSTALLDIR = /usr/local/share/$(NAME)
else
CHARACTERSETS_INSTALLDIR = /System/Library/CoreServices
endif

#
# Additional steps we add to predefined targets
#

install_after::
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)
	-$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	-$(SILENT) $(CHMOD) -R +w $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	$(SILENT) $(REMOVE_RECUR) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	$(SILENT) $(COPY_RECUR) $(SRCROOT)/CharacterSets $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)
	$(SILENT) $(REMOVE_RECUR) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets/CVS
	$(SILENT) $(CHOWN) -R root:wheel $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	$(SILENT) $(CHMOD) 444 $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets/* #*/
	$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets

prebuild_after::
ifeq "$(LIBRARY_STYLE)" "Library"
	$(SILENT) $(COPY_RECUR) CharacterSets $(RESOURCE_DIR)
	$(SILENT) $(REMOVE_RECUR) $(RESOURCE_DIR)/CharacterSets/CVS
ifneq "$(PLATFORM)" "Darwin"
# All other platforms need the compatibility headers
	$(SILENT) $(COPY) OSXCompatibilityHeaders/*.h $(PUBLIC_HEADER_DIR)/.. #*/
	$(SILENT) $(MKDIRS) $(PUBLIC_HEADER_DIR)/../GNUCompatibility
	$(SILENT) $(COPY) OSXCompatibilityHeaders/GNUCompatibility/*.h $(PUBLIC_HEADER_DIR)/../GNUCompatibility #*/
endif
endif

ifeq "$(LIBRARY_STYLE)" "Library"
clean_after::
	$(REMOVE_RECUR) -f $(RESOURCE_DIR)/CharacterSets
endif

compile-after::
	$(SILENT) $(CC) $(CFLAGS) $(SRCROOT)/version.c -DVERSION=$(CURRENT_PROJECT_VERSION) -DUSER=$(USER) -c -o $(OFILE_DIR)/version.o

test:
	cd Tests; $(MAKE) test SYMROOT=$(SYMROOT) USE_OBJC=NO
