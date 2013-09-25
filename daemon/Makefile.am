NULL =

# Note that we have cflags for just about everything even if
# we don't link to everything
INCLUDES = \
	-I.						\
	-I..						\
	-I$(top_srcdir)/common				\
	-DAUTHDIR=\"$(authdir)\"			\
	-DBINDIR=\"$(bindir)\"				\
	-DDATADIR=\"$(datadir)\"			\
	-DDMCONFDIR=\"$(dmconfdir)\"			\
	-DMDMCONFDIR=\"$(mdmconfdir)\"			\
	-DMDMLOCALEDIR=\"$(mdmlocaledir)\"		\
	-DLIBDIR=\"$(libdir)\"				\
	-DLIBEXECDIR=\"$(libexecdir)\"			\
	-DLOGDIR=\"$(logdir)\"				\
	-DPIXMAPDIR=\"$(pixmapdir)\"			\
	-DSBINDIR=\"$(sbindir)\"			\
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\"	\
	-DMDM_DEFAULTS_CONF=\"$(MDM_DEFAULTS_CONF)\"	\
	-DMDM_CUSTOM_CONF=\"$(MDM_CUSTOM_CONF)\"	\
	-DMDM_OLD_CONF=\"$(MDM_OLD_CONF)\"		\
	$(GUI_CFLAGS)					\
	$(DAEMON_CFLAGS)				\
	-DLANG_CONFIG_FILE=\"$(LANG_CONFIG_FILE)\"	\
	$(GNOME_INCLUDEDIR)				\
	-DGREETERTHEMEDIR=\""$(datadir)/mdm/themes"\"	\
	$(NULL)

sbin_PROGRAMS = mdm-binary		\
	$(NULL)

mdm_binary_SOURCES = \
	mdm.c				\
	mdm.h \
	mdm-daemon-config.c \
	mdm-daemon-config.h \
	mdm-daemon-config-entries.h \
	mdm-daemon-config-keys.h \
	mdm-socket-protocol.h \
	display.c \
	display.h \
	fstype.c \
	slave.c \
	slave.h \
	server.c \
	server.h \
	misc.c \
	misc.h \
	auth.c \
	auth.h \
	cookie.c \
	cookie.h \	
	filecheck.c \
	filecheck.h \
	md5.c \
	md5.h \
	@VRFY@.c \
	verify.h \
	errorgui.c \
	errorgui.h \
	mdm-net.c \
	mdm-net.h \
	getvt.c \
	getvt.h	\
	$(NULL)

EXTRA_mdm_binary_SOURCES = 	\
	verify-pam.c		\
	verify-crypt.c		\
	verify-shadow.c		\
	$(NULL)

CONSOLE_KIT_SOURCES = 		\
	mdmconsolekit.c		\
	mdmconsolekit.h		\
	$(NULL)

EXTRA_mdm_binary_SOURCES += $(CONSOLE_KIT_SOURCES)

# Note that these libs are in LDFLAGS because they should come before
# everything else on the link line as they may override stuff
mdm_binary_LDFLAGS =				\
	$(EXTRA_DAEMON_LIBS)			\
	$(NULL)

mdm_binary_LDADD = \
	$(DAEMON_LIBS)				\
	$(INTLLIBS)				\
	$(GLIB_LIBS)				\
	$(GOBJECT_LIBS)				\
	$(GDK_LIBS)				\
	$(top_builddir)/common/libmdmcommon.a	\
	$(X_LIBS)				\
	$(XINERAMA_LIBS)			\
	$(LIBWRAP_LIBS)				\
	-lXau					\
	-lX11					\
	-lXext					\
	$(NULL)

if WITH_CONSOLE_KIT
mdm_binary_SOURCES += $(CONSOLE_KIT_SOURCES)
mdm_binary_LDADD += $(DBUS_LIBS)
INCLUDES += $(DBUS_CFLAGS)
endif

sbin_SCRIPTS = mdm
CLEANFILES = mdm

mdm: $(srcdir)/mdm.in
	sed -e 's,[@]sbindir[@],$(sbindir),g' <$(srcdir)/mdm.in >mdm

EXTRA_DIST = mdm.in
