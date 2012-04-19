NULL =

DEFS = @DEFS@ -DMDM_CONFIG_FILE=\"$(mdmconfdir)/mdm.conf\" \
	-DMDM_FACTORY_CONFIG_FILE=\"$(mdmconfdir)/factory-mdm.conf\" \
	-DMDM_GLADE_DIR=\"@datadir@/mdm\"

INCLUDES = \
	-I.						\
	-I..						\
	-I$(top_srcdir)/daemon				\
	-I$(top_srcdir)/vicious-extensions		\
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\" 	\
	$(UTILS_CFLAGS)

edit = sed \
	-e 's|@sbindir[@]|$(sbindir)|g' \
	-e 's|@sysconfdir[@]|$(sysconfdir)|g' \
	-e 's|@localstatedir[@]|$(localstatedir)|g' \
	-e 's|@MDM_PID_FILE[@]|$(MDM_PID_FILE)|g'

mdm-stop: mdm-stop.in Makefile
	$(edit) $< >$@
mdm-restart: mdm-restart.in Makefile
	$(edit) $< >$@
mdm-safe-restart: mdm-safe-restart.in Makefile
	$(edit) $< >$@
mdmsetup: mdmsetup-security.in
	$(edit) <mdmsetup-security.in >mdmsetup


sbin_SCRIPTS = 			\
	mdm-stop		\
	mdm-restart		\
	mdm-safe-restart	\
	$(NULL)

EXTRA_DIST = 			\
	mdm-ssh-session		\
	mdm-stop.in		\
	mdm-restart.in		\
	mdm-safe-restart.in	\
	mdmsetup-pam		\
	mdmsetup-security.in	\
	$(NULL)

libexec_SCRIPTS = \
	@MDMSSHSESSION@

libexec_PROGRAMS = \
	@MDMASKPASS@	\
	@MDMOPEN@	\
	@MDMPREFETCH@	\
	mdmtranslate

if DMX_SUPPORT
bin_PROGRAMS = mdm-dmx-reconnect-proxy
endif

EXTRA_SCRIPTS = mdm-ssh-session
EXTRA_PROGRAMS = mdmaskpass mdmopen mdmprefetch

mdmaskpass_SOURCES = \
	mdmaskpass.c

mdmtranslate_SOURCES = \
	mdmtranslate.c

mdmopen_SOURCES = \
	mdmopen.c

mdmprefetch_SOURCES = \
	mdmprefetch.c

mdmaskpass_LDADD = \
	$(INTLLIBS)		\
	-lpam			\
	$(EXTRA_MDMASKPASS_LIBS)

mdmopen_LDADD = \
	$(INTLLIBS)

mdmtranslate_LDADD = \
	$(INTLLIBS)

if DMX_SUPPORT
mdm_dmx_reconnect_proxy_SOURCES = \
	mdm-dmx-reconnect-proxy.c

mdm_dmx_reconnect_proxy_LDADD = \
	$(GLIB_LIBS) \
        $(X_EXTRA_LIBS) \
        $(X_LIBS) \
	-lX11 \
	$(DMX_LIBS)
endif

if CONSOLE_HELPER
cappsdir = $(PAM_PREFIX)/security/console.apps
capps_DATA = mdmsetup

install-data-local:
	$(mkinstalldirs) $(DESTDIR)$(PAM_PREFIX)/pam.d
	$(INSTALL_DATA) $(srcdir)/mdmsetup-pam $(DESTDIR)$(PAM_PREFIX)/pam.d/mdmsetup
else
install-data-local:
endif

CLEANFILES = 			\
	mdmsetup-security	\
	$(NULL)

DISTCLEANFILES = 		\
	mdmsetup		\
	mdm-stop		\
	mdm-restart		\
	mdm-safe-restart	\
	$(NULL)

