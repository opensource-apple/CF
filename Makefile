# Simple makefile for building CoreFoundation on Darwin
#
# These make variables (or environment variables) are used
# if defined:
#	SRCROOT		path location of root of source hierarchy;
#			defaults to ".", but must be set to a
#			destination path for installsrc target.
#	OBJROOT		path location where .o files will be put;
#			defaults to "/tmp/CoreFoundation.obj".
#	SYMROOT		path location where build products will be
#			put; defaults to "/tmp/CoreFoundation.sym".
#	DSTROOT		path location where installed products will
#			be put; defaults to "/tmp/CoreFoundation.dst".
# OBJROOT and SYMROOT should not be directories shared with other
# built projects, and should not be the same directory.
#	PLATFORM	name of platform being built on
#	USER		name of user building the project
#	ARCHS		list of archs for which to build
#	RC_ARCHS	more archs for which to build (build system)
#	OTHER_CFLAGS	other flags to be passed to compiler
#	RC_CFLAGS	more flags to be passed to compiler (build system)
#	OTHER_LFLAGS	other flags to be passed to the link stage
#

CURRENT_PROJECT_VERSION = 299.31

# First figure out the platform if not specified, so we can use it in the
# rest of this file.  Currently defined values: Darwin, Linux, FreeBSD, WIN32
ifeq "$(PLATFORM)" ""
PLATFORM := $(shell uname)
endif

ifeq "$(PLATFORM)" "Darwin"
PLATFORM_CFLAGS = -D__MACH__=1 -fconstant-cfstrings
endif

ifeq "$(PLATFORM)" "Linux"
PLATFORM_CFLAGS = -D__LINUX__=1
endif

ifeq "$(PLATFORM)" "FreeBSD"
PLATFORM_CFLAGS = -D__FREEBSD__=1
endif

ifeq "$(PLATFORM)" "WIN32"
PLATFORM_CFLAGS = -D__WIN32__=1
OBJROOT = CoreFoundation.obj
SYMROOT = CoreFoundation.sym
DSTROOT = CoreFoundation.dst
endif

ifndef SRCROOT
SRCROOT = .
endif

ifndef OBJROOT
OBJROOT = /tmp/CoreFoundation.obj
endif

ifndef SYMROOT
SYMROOT = /tmp/CoreFoundation.sym
endif

ifndef DSTROOT
DSTROOT = /tmp/CoreFoundation.dst
endif

SILENT = @
ifeq "$(PLATFORM)" "WIN32"
CC = gcc
ECHO = echo
MKDIRS = mkdir -p
COPY = cp
COPY_RECUR = cp -r
REMOVE = rm
REMOVE_RECUR = rm -rf
SYMLINK = ln -s
CHMOD = chmod
CHOWN = chown
TAR = tar
STRIP = strip
DLLTOOL = dlltool
else
ifeq "$(PLATFORM)" "Darwin"
CC = /usr/bin/cc
else
CC = /usr/bin/gcc
endif
ECHO = /bin/echo
MKDIRS = /bin/mkdir -p
COPY = /bin/cp
COPY_RECUR = /bin/cp -r
REMOVE = /bin/rm
REMOVE_RECUR = /bin/rm -rf
SYMLINK = /bin/ln -s
CHMOD = /bin/chmod
CHOWN = /usr/sbin/chown
TAR = /usr/bin/tar
STRIP = /usr/bin/strip
endif

ifeq "$(PLATFORM)" "Darwin"
WARNING_FLAGS = -Wno-precomp -Wno-four-char-constants
endif

ifeq "$(PLATFORM)" "Darwin"
ifneq "$(ARCHS)" ""
ARCH_FLAGS = $(foreach A, $(ARCHS), $(addprefix -arch , $(A)))
else
ifneq "$(RC_ARCHS)" ""
ARCH_FLAGS = $(foreach A, $(RC_ARCHS), $(addprefix -arch , $(A)))
else
ARCH_FLAGS = -arch ppc
endif
endif
endif

ifeq "$(PLATFORM)" "FreeBSD"
ARCH_FLAGS = -march=i386
endif

ifeq "$(PLATFORM)" "Linux"
ARCH_FLAGS = 
endif

ifeq "$(USER)" ""
USER = unknown
endif

CFLAGS = -DCF_BUILDING_CF=1 -g -fno-common -pipe $(PLATFORM_CFLAGS) \
	$(WARNING_FLAGS) -I$(SYMROOT)/ProjectHeaders -I.

ifeq "$(PLATFORM)" "WIN32"
LFLAGS = -lmsvcrt -lnetapi32 -lobjc -lole32 -lws2_32
else
LFLAGS =
endif

ifeq "$(wildcard /System/Library/Frameworks)" ""
LIBRARY_STYLE = Library
LIBRARY_EXT = .so
ifeq "$(PLATFORM)" "Linux"
LIBRARY_EXT = .a
endif
ifeq "$(PLATFORM)" "WIN32"
LIBRARY_EXT = .dll
endif
HEADER_INSTALLDIR = /usr/local/include/CoreFoundation
INSTALLDIR = /usr/local/lib
CHARACTERSETS_INSTALLDIR = /usr/local/share/CoreFoundation
else
LIBRARY_STYLE = Framework
INSTALLDIR = /System/Library/Frameworks
FRAMEWORK_DIR = /System/Library/Frameworks/CoreFoundation.framework
CHARACTERSETS_INSTALLDIR = /System/Library/CoreServices
endif

ifeq "$(PLATFORM)" "Darwin"
CFLAGS += $(ARCH_FLAGS) -F$(SYMROOT)
LFLAGS += $(ARCH_FLAGS) -dynamiclib -dynamic -compatibility_version 150 \
	-current_version $(CURRENT_PROJECT_VERSION) -Wl,-init,___CFInitialize
endif

ifeq "$(PLATFORM)" "FreeBSD"
LFLAGS += -shared
endif

ifeq "$(PLATFORM)" "Linux"
LFLAGS += -lpthread
endif

CFLAGS += $(OTHER_CFLAGS) $(RC_CFLAGS)
LFLAGS += $(OTHER_LFLAGS)


SUBPROJECTS = AppServices Base Collections Locale NumberDate Parsing PlugIn \
		RunLoop String StringEncodings URL

AppServices_PUBHEADERS = CFUserNotification.h
AppServices_SOURCES = CFUserNotification.c
Base_PROJHEADERS = CFPriv.h CFInternal.h ForFoundationOnly.h CFRuntime.h \
		CFUtilities.h
Base_PUBHEADERS = CFBase.h CFByteOrder.h CoreFoundation.h CFUUID.h
Base_SOURCES = CFBase.c CFUtilities.c CFSortFunctions.c \
		CFRuntime.c CFFileUtilities.c CFPlatform.c CFUUID.c uuid.c
Collections_PROJHEADERS = CFStorage.h
Collections_PUBHEADERS = CFArray.h CFBag.h CFBinaryHeap.h CFBitVector.h \
		CFData.h CFDictionary.h CFSet.h CFTree.h
Collections_SOURCES = CFArray.c CFBag.c CFBinaryHeap.c CFBitVector.c \
		CFData.c CFDictionary.c CFSet.c CFStorage.c CFTree.c
Locale_PUBHEADERS = CFLocale.h
NumberDate_PROJHEADERS = 
NumberDate_PUBHEADERS = CFDate.h CFNumber.h CFTimeZone.h 
NumberDate_SOURCES = CFDate.c CFNumber.c CFTimeZone.c
Parsing_PROJHEADERS = CFXMLInputStream.h
Parsing_PUBHEADERS = CFPropertyList.h CFXMLParser.h CFXMLNode.h
Parsing_SOURCES = CFBinaryPList.c CFPropertyList.c CFXMLParser.c \
		CFXMLInputStream.c CFXMLNode.c CFXMLTree.c
PlugIn_PROJHEADERS = CFBundlePriv.h CFBundle_BinaryTypes.h CFBundle_Internal.h \
		CFPlugIn_Factory.h
PlugIn_PUBHEADERS = CFBundle.h CFPlugIn.h CFPlugInCOM.h
PlugIn_SOURCES = CFBundle.c CFBundle_Resources.c CFPlugIn.c CFPlugIn_Factory.c \
		CFPlugIn_Instance.c CFPlugIn_PlugIn.c
ifeq "$(PLATFORM)" "Darwin"
RunLoop_PROJHEADERS = CFWindowsMessageQueue.h
RunLoop_PUBHEADERS = CFRunLoop.h CFSocket.h CFMachPort.h CFMessagePort.h
RunLoop_SOURCES = CFMachPort.c CFMessagePort.c CFRunLoop.c CFSocket.c \
		CFWindowsMessageQueue.c 
endif
ifeq "$(PLATFORM)" "WIN32"
RunLoop_PROJHEADERS =
RunLoop_PUBHEADERS = CFRunLoop.h CFMachPort.h CFMessagePort.h
RunLoop_SOURCES = CFMachPort.c CFMessagePort.c CFRunLoop.c
endif
String_PROJHEADERS = CFCharacterSetPriv.h
String_PUBHEADERS = CFCharacterSet.h CFString.h
String_SOURCES = CFCharacterSet.c CFString.c CFStringEncodings.c \
		CFStringScanner.c CFStringUtilities.c
StringEncodings_PROJHEADERS = CFStringEncodingConverter.h CFUniChar.h \
		CFStringEncodingConverterExt.h CFUniCharPriv.h \
		CFStringEncodingConverterPriv.h CFUnicodeDecomposition.h \
		CFUnicodePrecomposition.h
StringEncodings_PUBHEADERS = 
StringEncodings_SOURCES = CFStringEncodingConverter.c CFBuiltinConverters.c \
		CFUnicodeDecomposition.c CFUnicodePrecomposition.c CFUniChar.c
URL_PROJHEADERS = 
URL_PUBHEADERS = CFURL.h CFURLAccess.h
URL_SOURCES = CFURL.c CFURLAccess.c

OTHER_SOURCES = version.c Makefile APPLE_LICENSE PropertyList.dtd

default: build
all: build
build: prebuild actual-build postbuild

# These are the main targets:
#    build		builds the library to OBJROOT and SYMROOT
#    installsrc		copies the sources to SRCROOT
#    installhdrs	install only the headers to DSTROOT
#    install		build, then install the headers and library to DSTROOT
#    clean		removes build products in OBJROOT and SYMROOT

installsrc:
	$(SILENT) $(ECHO) "Installing source..."
ifeq "$(SRCROOT)" "."
	$(SILENT) $(ECHO) "SRCROOT must be defined to be the destination directory; it cannot be '.'"
	exit 1
endif
	$(SILENT) $(MKDIRS) $(SRCROOT)
	$(SILENT) $(MKDIRS) $(foreach S, $(SUBPROJECTS), $(SRCROOT)/$(S).subproj)
	-$(SILENT) $(foreach S, $(SUBPROJECTS), $(COPY) $(foreach F, $($(S)_SOURCES), $(S).subproj/$(F)) $(SRCROOT)/$(S).subproj;)
	-$(SILENT) $(foreach S, $(SUBPROJECTS), $(COPY) $(foreach F, $($(S)_PROJHEADERS), $(S).subproj/$(F)) $(SRCROOT)/$(S).subproj;)
	-$(SILENT) $(foreach S, $(SUBPROJECTS), $(COPY) $(foreach F, $($(S)_PUBHEADERS), $(S).subproj/$(F)) $(SRCROOT)/$(S).subproj;)
	$(SILENT) $(COPY) $(OTHER_SOURCES) $(SRCROOT)
	$(SILENT) $(COPY_RECUR) CharacterSets $(SRCROOT)
	$(SILENT) $(REMOVE_RECUR) $(SRCROOT)/CharacterSets/CVS

installhdrs:
	$(SILENT) $(ECHO) "Installing headers..."
ifeq "$(LIBRARY_STYLE)" "Framework"
	$(SILENT) $(REMOVE) -f $(DSTROOT)/$(FRAMEWORK_DIR)/Headers
	$(SILENT) $(REMOVE) -f $(DSTROOT)/$(FRAMEWORK_DIR)/PrivateHeaders
	$(SILENT) $(REMOVE) -f $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/Current
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/Headers
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/PrivateHeaders
	$(SILENT) $(SYMLINK) A $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/Current
	$(SILENT) $(SYMLINK) Versions/Current/Headers $(DSTROOT)/$(FRAMEWORK_DIR)/Headers
	$(SILENT) $(SYMLINK) Versions/Current/PrivateHeaders $(DSTROOT)/$(FRAMEWORK_DIR)/PrivateHeaders
	-$(SILENT) $(CHMOD) +w $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/Headers/*.h
	-$(SILENT) $(CHMOD) +w $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/PrivateHeaders/*.h
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PUBHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/Headers
	$(SILENT) $(COPY) Base.subproj/CFPriv.h Base.subproj/CFRuntime.h $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/PrivateHeaders
	$(SILENT) $(CHOWN) -R root:wheel $(DSTROOT)/$(FRAMEWORK_DIR)
	-$(SILENT) $(CHMOD) -w $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/Headers/*.h
	-$(SILENT) $(CHMOD) -w $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/PrivateHeaders/*.h
endif
ifeq "$(LIBRARY_STYLE)" "Library"
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(HEADER_INSTALLDIR)
	-$(SILENT) $(CHMOD) +w $(DSTROOT)/$(HEADER_INSTALLDIR)/*.h
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PUBHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(DSTROOT)/$(HEADER_INSTALLDIR)
	$(SILENT) $(CHMOD) -w $(DSTROOT)/$(HEADER_INSTALLDIR)/*.h
endif

install: build
	$(SILENT) $(ECHO) "Installing..."
ifeq "$(LIBRARY_STYLE)" "Framework"
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(FRAMEWORK_DIR)
	-$(SILENT) $(CHMOD) -R +w $(DSTROOT)/$(FRAMEWORK_DIR)
	$(SILENT) $(REMOVE_RECUR) $(DSTROOT)/$(FRAMEWORK_DIR)
	$(SILENT) (cd $(SYMROOT) && $(TAR) -cf - CoreFoundation.framework) | (cd $(DSTROOT)/$(INSTALLDIR) && $(TAR) -xf -)
	$(SILENT) $(STRIP) -S $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/CoreFoundation
	$(SILENT) $(STRIP) -S $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/CoreFoundation_debug
	$(SILENT) $(STRIP) -S $(DSTROOT)/$(FRAMEWORK_DIR)/Versions/A/CoreFoundation_profile
	$(SILENT) $(CHMOD) -R ugo-w $(DSTROOT)/$(FRAMEWORK_DIR)
	$(SILENT) $(CHMOD) -R o+rX $(DSTROOT)/$(FRAMEWORK_DIR)
	$(SILENT) $(CHOWN) -R root:wheel $(DSTROOT)/$(FRAMEWORK_DIR)
endif
ifeq "$(LIBRARY_STYLE)" "Library"
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(INSTALLDIR)
	-$(SILENT) $(CHMOD) +w $(DSTROOT)/$(INSTALLDIR)
	$(SILENT) $(REMOVE) -f $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation$(LIBRARY_EXT)
	$(SILENT) $(REMOVE) -f $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_debug$(LIBRARY_EXT)
	$(SILENT) $(REMOVE) -f $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_profile$(LIBRARY_EXT)
	$(SILENT) $(COPY) $(SYMROOT)/libCoreFoundation$(LIBRARY_EXT) $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation$(LIBRARY_EXT)
ifneq "$(LIBRARY_EXT)" ".a"
	$(SILENT) $(COPY) $(SYMROOT)/libCoreFoundation_debug$(LIBRARY_EXT) $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_debug$(LIBRARY_EXT)
	$(SILENT) $(COPY) $(SYMROOT)/libCoreFoundation_profile$(LIBRARY_EXT) $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_profile$(LIBRARY_EXT)
endif
	-$(SILENT) $(CHOWN) root:wheel $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation$(LIBRARY_EXT)
	-$(SILENT) $(CHOWN) root:wheel $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_debug$(LIBRARY_EXT)
	-$(SILENT) $(CHOWN) root:wheel $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_profile$(LIBRARY_EXT)
	$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation$(LIBRARY_EXT)
ifneq "$(LIBRARY_EXT)" ".a"
	$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_debug$(LIBRARY_EXT)
	$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(INSTALLDIR)/libCoreFoundation_profile$(LIBRARY_EXT)
endif
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(HEADER_INSTALLDIR)
	-$(SILENT) $(CHMOD) +w $(DSTROOT)/$(HEADER_INSTALLDIR)/*.h
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PUBHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(DSTROOT)/$(HEADER_INSTALLDIR)
	$(SILENT) $(CHMOD) -w $(DSTROOT)/$(HEADER_INSTALLDIR)/*.h
endif
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)
	-$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)
	$(SILENT) $(MKDIRS) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	-$(SILENT) $(CHMOD) -R +w $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	$(SILENT) $(REMOVE_RECUR) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	$(SILENT) $(COPY_RECUR) $(SRCROOT)/CharacterSets $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)
	$(SILENT) $(REMOVE_RECUR) $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets/CVS
	$(SILENT) $(CHOWN) -R root:wheel $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets
	$(SILENT) $(CHMOD) 444 $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets/*
	$(SILENT) $(CHMOD) 755 $(DSTROOT)/$(CHARACTERSETS_INSTALLDIR)/CharacterSets

clean:
	$(SILENT) $(ECHO) "Deleting build products..."
	$(SILENT) $(REMOVE_RECUR) $(SYMROOT)/ProjectHeaders
ifeq "$(LIBRARY_STYLE)" "Framework"
	$(SILENT) $(REMOVE_RECUR) $(SYMROOT)/CoreFoundation.framework
endif
ifeq "$(LIBRARY_STYLE)" "Library"
	$(SILENT) $(REMOVE) -f $(SYMROOT)/libCoreFoundation$(LIBRARY_EXT)
	$(SILENT) $(REMOVE) -f $(SYMROOT)/libCoreFoundation_debug$(LIBRARY_EXT)
	$(SILENT) $(REMOVE) -f $(SYMROOT)/libCoreFoundation_profile$(LIBRARY_EXT)
endif
ifeq "$(PLATFORM)" "WIN32"
	$(SILENT) $(REMOVE) -f $(SYMROOT)/CoreFoundation*$(LIBRARY_EXT)
	$(SILENT) $(REMOVE) -f $(SYMROOT)/CoreFoundation*.lib
endif
	$(SILENT) $(REMOVE) -f $(OBJROOT)/*.o

prebuild:
	$(SILENT) $(ECHO) "Prebuild-setup..."
ifeq "$(LIBRARY_STYLE)" "Framework"
	$(SILENT) $(MKDIRS) $(SYMROOT)
	$(SILENT) $(REMOVE_RECUR) $(SYMROOT)/ProjectHeaders
	$(SILENT) $(MKDIRS) $(SYMROOT)/ProjectHeaders
#	$(SILENT) $(REMOVE_RECUR) $(SYMROOT)/CoreFoundation.framework
	$(SILENT) $(REMOVE) -f $(SYMROOT)/CoreFoundation.framework/Versions/Current
	$(SILENT) $(REMOVE) -f $(SYMROOT)/CoreFoundation.framework/Headers
	$(SILENT) $(REMOVE) -f $(SYMROOT)/CoreFoundation.framework/PrivateHeaders
	$(SILENT) $(REMOVE) -f $(SYMROOT)/CoreFoundation.framework/Resources
	$(SILENT) $(MKDIRS) $(SYMROOT)/CoreFoundation.framework/Versions/A/Headers
	$(SILENT) $(MKDIRS) $(SYMROOT)/CoreFoundation.framework/Versions/A/PrivateHeaders
	$(SILENT) $(MKDIRS) $(SYMROOT)/CoreFoundation.framework/Versions/A/Resources
	$(SILENT) $(SYMLINK) A $(SYMROOT)/CoreFoundation.framework/Versions/Current
	$(SILENT) $(SYMLINK) Versions/Current/Headers $(SYMROOT)/CoreFoundation.framework/Headers
	$(SILENT) $(SYMLINK) Versions/Current/PrivateHeaders $(SYMROOT)/CoreFoundation.framework/PrivateHeaders
	$(SILENT) $(SYMLINK) Versions/Current/Resources $(SYMROOT)/CoreFoundation.framework/Resources
	$(SILENT) $(ECHO) "Copying headers..."
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PROJHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(SYMROOT)/ProjectHeaders
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PUBHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(SYMROOT)/CoreFoundation.framework/Versions/A/Headers
	$(SILENT) $(COPY) Base.subproj/CFPriv.h Base.subproj/CFRuntime.h $(SYMROOT)/CoreFoundation.framework/Versions/A/PrivateHeaders
endif
ifeq "$(LIBRARY_STYLE)" "Library"
	$(SILENT) $(MKDIRS) $(SYMROOT)
	$(SILENT) $(REMOVE_RECUR) $(SYMROOT)/ProjectHeaders
	$(SILENT) $(MKDIRS) $(SYMROOT)/ProjectHeaders
	$(SILENT) $(ECHO) "Copying headers..."
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PROJHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(SYMROOT)/ProjectHeaders
	$(SILENT) $(MKDIRS) $(SYMROOT)/ProjectHeaders/CoreFoundation
	$(SILENT) $(COPY) $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_PUBHEADERS), $(SRCROOT)/$(S).subproj/$(F))) $(SYMROOT)/ProjectHeaders/CoreFoundation
endif

actual-build:
	$(SILENT) $(ECHO) "Building..."
	$(SILENT) $(MKDIRS) $(OBJROOT)
	$(SILENT) for x in $(foreach S, $(SUBPROJECTS), $(foreach F, $($(S)_SOURCES), $(SRCROOT)/$(S).subproj/$(F))) ; do \
		if [ ! $(OBJROOT)/`basename $$x .c`.opt.o -nt $$x ] ; then \
		$(ECHO) "    ..." $$x " (optimized)" ; \
		$(CC) $(CFLAGS) $$x -O -c -o $(OBJROOT)/`basename $$x .c`.opt.o ; \
		fi ; \
		if [ ! $(OBJROOT)/`basename $$x .c`.debug.o -nt $$x ] ; then \
		$(ECHO) "    ..." $$x " (debug)" ; \
		$(CC) $(CFLAGS) $$x -DDEBUG -c -o $(OBJROOT)/`basename $$x .c`.debug.o ; \
		fi ; \
		if [ ! $(OBJROOT)/`basename $$x .c`.profile.o -nt $$x ] ; then \
		$(ECHO) "    ..." $$x " (profile)" ; \
		$(CC) $(CFLAGS) $$x -DPROFILE -pg -O -c -o $(OBJROOT)/`basename $$x .c`.profile.o ; \
		fi \
	done
	$(SILENT) $(CC) $(CFLAGS) $(SRCROOT)/version.c -DVERSION=$(CURRENT_PROJECT_VERSION) -DUSER=$(USER) -O -c -o $(OBJROOT)/version.opt.o
	$(SILENT) $(CC) $(CFLAGS) $(SRCROOT)/version.c -DVERSION=$(CURRENT_PROJECT_VERSION) -DUSER=$(USER) -DDEBUG -c -o $(OBJROOT)/version.debug.o
	$(SILENT) $(CC) $(CFLAGS) $(SRCROOT)/version.c -DVERSION=$(CURRENT_PROJECT_VERSION) -DUSER=$(USER) -DPROFILE -pg -O -c -o $(OBJROOT)/version.profile.o
	$(SILENT) $(ECHO) "Linking..."
ifeq "$(PLATFORM)" "Darwin"
	$(SILENT) $(CC) $(LFLAGS) -O -install_name $(FRAMEWORK_DIR)/Versions/A/CoreFoundation -o $(SYMROOT)/CoreFoundation.framework/Versions/A/CoreFoundation $(OBJROOT)/*.opt.o
	$(SILENT) $(CC) $(LFLAGS) -install_name $(FRAMEWORK_DIR)/Versions/A/CoreFoundation_debug -o $(SYMROOT)/CoreFoundation.framework/Versions/A/CoreFoundation_debug $(OBJROOT)/*.debug.o
	$(SILENT) $(CC) $(LFLAGS) -pg -O -install_name $(FRAMEWORK_DIR)/Versions/A/CoreFoundation_profile -o $(SYMROOT)/CoreFoundation.framework/Versions/A/CoreFoundation_profile $(OBJROOT)/*.profile.o
endif
ifeq "$(PLATFORM)" "Linux"
	$(SILENT) $(ECHO) "NOTE: Producing static libraries on Linux"
	$(SILENT) ar cr $(SYMROOT)/libCoreFoundation$(LIBRARY_EXT) $(OBJROOT)/*.opt.o
	$(SILENT) ar cr $(SYMROOT)/libCoreFoundation_debug$(LIBRARY_EXT) $(OBJROOT)/*.debug.o
	$(SILENT) ar cr $(SYMROOT)/libCoreFoundation_profile$(LIBRARY_EXT) $(OBJROOT)/*.profile.o
endif
ifeq "$(PLATFORM)" "FreeBSD"
	$(SILENT) $(CC) $(LFLAGS) -O -o $(SYMROOT)/libCoreFoundation$(LIBRARY_EXT) (OBJROOT)/*.opt.o
	$(SILENT) $(CC) $(LFLAGS) -o $(SYMROOT)/libCoreFoundation_debug$(LIBRARY_EXT) (OBJROOT)/*.debug.o
	$(SILENT) $(CC) $(LFLAGS) -pg -O -o $(SYMROOT)/libCoreFoundation_profile$(LIBRARY_EXT) (OBJROOT)/*.profile.o
endif
ifeq "$(PLATFORM)" "WIN32"
	$(SILENT) $(DLLTOOL) -e CoreFoundation -l CoreFoundation.lib $(OBJROOT)/*.opt.o
	$(SILENT) $(CC) -mdll $(OBJROOT)/*.opt.o CoreFoundation -o CoreFoundation$(LIBRARY_EXT) $(LFLAGS)
	$(SILENT) $(DLLTOOL) -e CoreFoundation_debug -l CoreFoundation_debug.lib $(OBJROOT)/*.debug.o
	$(SILENT) $(CC) -mdll $(OBJROOT)/*.debug.o CoreFoundation_debug -o CoreFoundation_debug$(LIBRARY_EXT) $(LFLAGS)
	$(SILENT) $(COPY) *.lib $(SYMROOT)
	$(SILENT) $(COPY) *.dll $(SYMROOT)
	$(SILENT) $(REMOVE) *.dll *.lib CoreFoundation CoreFoundation_debug
endif
	$(SILENT) $(ECHO) "Done!"

postbuild:
ifeq "$(LIBRARY_STYLE)" "Framework"
	-$(SILENT) $(SYMLINK) Versions/Current/CoreFoundation $(SYMROOT)/CoreFoundation.framework/CoreFoundation
	-$(SILENT) $(SYMLINK) Versions/Current/CoreFoundation_debug $(SYMROOT)/CoreFoundation.framework/CoreFoundation_debug
	-$(SILENT) $(SYMLINK) Versions/Current/CoreFoundation_profile $(SYMROOT)/CoreFoundation.framework/CoreFoundation_profile
endif
