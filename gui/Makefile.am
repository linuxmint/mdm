NULL =

SUBDIRS = . greeter modules faces

## Process this file with automake to produce makefile.in
DEFS = @DEFS@ \
	-DPREFIX=\"@prefix@\" 				\
	-DAUTHDIR=\"@authdir@\" 			\
	-DBINDIR=\"@bindir@\" 				\
	-DDATADIR=\"@datadir@\" 			\
	-DDMCONFDIR=\"@dmconfdir@\" 			\
	-DMDMCONFDIR=\"@mdmconfdir@\"			\
	-DMDMLOCALEDIR=\"@mdmlocaledir@\" 		\
	-DMDM_GLADE_DIR=\"@datadir@/mdm\" 		\
	-DLIBDIR=\"@libdir@\" 				\
	-DLIBEXECDIR=\"@libexecdir@\" 			\
	-DLOGDIR=\"@logdir@\"				\
	-DATSPIDIR=\"@atspidir@\"				\
	-DPIXMAPDIR=\"@pixmapdir@\" 			\
	-DSBINDIR=\"@sbindir@\"				\
	$(NULL)

INCLUDES = \
	-I.						\
	-I..						\
	-I$(top_srcdir)/daemon				\
	-I$(top_srcdir)/common				\
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\" 	\
	$(GUI_CFLAGS) \
	$(WEBKIT_CFLAGS) 

#
#	-DG_DISABLE_DEPRECATED				\
#	-DGDK_DISABLE_DEPRECATED			\
#	-DGDK_PIXBUF_DISABLE_DEPRECATED			\
#	-DGTK_DISABLE_DEPRECATED			\
#	-DGNOME_DISABLE_DEPRECATED			\
#

noinst_LIBRARIES = libmdmwm.a libmdmcommon.a libmdmgreeter.a

libexec_PROGRAMS = \
	mdmlogin			\
	mdmwebkit

sbin_PROGRAMS = \
	mdmsetup

bin_PROGRAMS = \
	mdmflexiserver

mdmflexiserver_SOURCES = \
	mdmflexiserver.c

libmdmwm_a_SOURCES = \
	mdmwm.c			\
	mdmwm.h

libmdmgreeter_a_SOURCES = \
	mdmgreeter.c		\
	mdmgreeter.h		\
	mdmlanguages.c		\
	mdmlanguages.h		\
	mdmsession.c		\
	mdmsession.h		\
	mdmuser.c		\
	mdmuser.h		\
	$(NULL)

libmdmcommon_a_SOURCES = \
	misc.c			\
	misc.h			\
	mdmcomm.c		\
	mdmcomm.h		\
	mdmconfig.c		\
	mdmconfig.h		\
	mdmcommon.c		\
	mdmcommon.h		\
	$(NULL)

mdmlogin_SOURCES = \
	mdmlogin.c

mdmwebkit_SOURCES = \
	mdmwebkit.c

mdmsetup_SOURCES = \
	mdmsession.c		\
	mdmsession.h		\
	mdmuser.c		\
	mdmuser.h		\
	mdmsetup.c

mdmlogin_LDADD = \
	$(EXTRA_GREETER_LIBS)	\
	libmdmwm.a		\
	libmdmcommon.a		\
	libmdmgreeter.a		\
	$(GUING_LIBS)		\
	$(INTLLIBS)		\
	$(GLIB_LIBS)		\
	$(GOBJECT_LIBS)		\
	$(GDK_LIBS)		\
	$(PANGO_LIBS)		\
	$(GDKPIXBUF_LIBS)	\
	$(top_builddir)/common/libmdmcommon.a \
	$(X_EXTRA_LIBS)		\
	$(XINERAMA_LIBS)	\
	$(X_LIBS)		\
	-lX11			\
	-lXau			\
	-lm				\
	$(NULL)

mdmwebkit_LDADD = \
	$(EXTRA_GREETER_LIBS)	\
	libmdmwm.a		\
	libmdmcommon.a		\
	libmdmgreeter.a		\
	$(GUING_LIBS)		\
	$(WEBKIT_LIBS)		\
	$(INTLLIBS)		\
	$(GLIB_LIBS)		\
	$(GOBJECT_LIBS)		\
	$(GDK_LIBS)		\
	$(PANGO_LIBS)		\
	$(GDKPIXBUF_LIBS)	\
	$(top_builddir)/common/libmdmcommon.a \
	$(X_EXTRA_LIBS)		\
	$(XINERAMA_LIBS)	\
	$(X_LIBS)		\
	-lX11			\
	-lXau			\
	-lm				\
	$(NULL)

# LIBGNOMEUI should be removed from mdmsetup at some point.
# It is needed because it uses gnome_help_display_uri(), which
# should be replaced at some point.
#
mdmsetup_CFLAGS = $(LIBGNOMEUI_CFLAGS)
mdmsetup_LDFLAGS = -export-dynamic
mdmsetup_LDADD = \
	libmdmcommon.a		\
	$(GUI_LIBS)		\
	$(INTLLIBS)		\
	$(GLIB_LIBS)		\
	$(GOBJECT_LIBS)		\
	$(PANGO_LIBS)		\
	$(GDK_LIBS)		\
	$(GDKPIXBUF_LIBS)	\
	$(EXTRA_SOCKET_LIB)	\
	$(EXTRA_NSL_LIB)	\
	$(LIBGNOMEUI_LIBS)	\
	$(top_builddir)/common/libmdmcommon.a \
	$(X_LIBS)		\
	-lXau			\
	-lX11			\
	$(NULL)

mdmflexiserver_LDADD = \
	libmdmcommon.a		\
	$(GUI_LIBS)		\
	$(INTLLIBS)		\
	$(GLIB_LIBS)		\
	$(GOBJECT_LIBS)		\
	$(GDK_LIBS)		\
	$(top_builddir)/common/libmdmcommon.a \
	$(EXTRA_SOCKET_LIB)	\
	$(EXTRA_NSL_LIB)	\
	$(X_LIBS)		\
	-lX11			\
	-lXau			\
	$(NULL)

Systemdir = $(datadir)/mdm/applications
System_files = \
	mdmsetup.desktop	\
	mdmflexiserver.desktop
System_in_files = \
	mdmsetup.desktop.in.in			\
	mdmflexiserver.desktop.in	
System_DATA = $(System_files)

CLEANFILES = mdmsetup.desktop mdmsetup.desktop.in mdmflexiserver.desktop

mdmsetup.desktop.in: $(srcdir)/mdmsetup.desktop.in.in
	sed -e 's,[@]sbindir[@],$(sbindir),g' <$(srcdir)/mdmsetup.desktop.in.in \
		>mdmsetup.desktop.in

@INTLTOOL_DESKTOP_RULE@

Gladedir = $(datadir)/mdm
Glade_DATA = mdmsetup.glade

EXTRA_DIST = \
	$(Glade_DATA)		\
	$(System_DATA)		\
	$(System_in_files)

clean-local:
	rm -f $(System_files)

