## Process this file with automake to produce Makefile.in

NULL =

INCLUDES = \
	-I.					\
	-I..					\
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\" \
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
	$(GLIB_CFLAGS)

noinst_LIBRARIES = 		\
	libmdmcommon.a		\
	$(null)

libmdmcommon_a_SOURCES =	\
	mdm-common.h		\
	mdm-common.c		\
	mdm-common-config.h	\
	mdm-common-config.c	\
	mdm-config.h		\
	mdm-config.c		\
	mdm-log.h		\
	mdm-log.c		\
	ve-signal.h		\
	ve-signal.c		\
	$(NULL)

noinst_PROGRAMS = 		\
	test-config		\
	test-log		\
	$(NULL)

test_config_SOURCES = 		\
	$(top_builddir)/daemon/mdm-daemon-config-entries.h	\
	test-config.c	 	\
	$(NULL)

test_config_LDADD =		\
	libmdmcommon.a	\
	$(GLIB_LIBS)		\
	$(NULL)

test_log_SOURCES = 		\
	test-log.c	 	\
	$(NULL)

test_log_LDADD =		\
	libmdmcommon.a	\
	$(GLIB_LIBS)		\
	$(NULL)
