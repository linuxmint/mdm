pixmapdir = $(datadir)/pixmaps
confdir = $(mdmconfdir)
mdmlocaledir = $(mdmconfdir)
bisessdir = $(datadir)/mdm/BuiltInSessions
instsessdir = $(datadir)/xsessions
initdir = $(mdmconfdir)/Init
superinitdir = $(mdmconfdir)/SuperInit
superpostdir = $(mdmconfdir)/SuperPost
authdir = $(localstatedir)/mdm
postdir = $(mdmconfdir)/PostSession
predir = $(mdmconfdir)/PreSession
postlogindir = $(mdmconfdir)/PostLogin
# logdir is set by configure.ac

noinst_DATA = mdm.conf

DESKTOP_FILES = default.desktop @SSHDESKTOP@

EXTRA_DIST = \
	mdm.conf.in \
	mdm.conf-custom.in \
	$(DESKTOP_FILES) \
	default.desktop.in.in \
	ssh.desktop.in.in \
	Xsession.in \
	mdm \
	mdm-autologin \
	locale.alias \
	Init.in \
	SuperInit.in \
	SuperPost.in \
	PreSession.in \
	PostSession.in \
	PostLogin \
	XKeepsCrashing \
	gettextfoo.h \
	mdmprefetchlist.in \
	extract-shell.sh

CLEANFILES = Xsession mdm.conf mdm.conf-custom default.desktop ssh.desktop Init SuperInit SuperPost PreSession PostSession mdmprefetchlist

Xsession: $(srcdir)/Xsession.in
	sed	-e 's,[@]XSESSION_SHELL[@],$(XSESSION_SHELL),g' \
        -e 's,[@]libexecdir[@],$(libexecdir),g' \
		-e 's,[@]DATADIR[@],$(DATADIR),g' \
		<$(srcdir)/Xsession.in >Xsession

mdm.conf: $(srcdir)/mdm.conf.in
	sed	-e 's,[@]MDMPREFETCHCMD[@],$(MDMPREFETCHCMD),g' \
		-e 's,[@]MDM_CUSTOM_CONF[@],$(MDM_CUSTOM_CONF),g' \
		-e 's,[@]MDM_USER_PATH[@],$(MDM_USER_PATH),g' \
		-e 's,[@]HALT_COMMAND[@],$(HALT_COMMAND),g' \
		-e 's,[@]REBOOT_COMMAND[@],$(REBOOT_COMMAND),g' \
		-e 's,[@]SOUND_PROGRAM[@],$(SOUND_PROGRAM),g' \
		-e 's,[@]SUSPEND_COMMAND[@],$(SUSPEND_COMMAND),g' \
		-e 's,[@]XEVIE_OPTION[@],$(XEVIE_OPTION),g' \
		-e 's,[@]ALWAYS_RESTART_SERVER[@],$(ALWAYS_RESTART_SERVER),g' \
		-e 's,[@]UTMP_LINE_ATTACHED[@],$(UTMP_LINE_ATTACHED),g' \
		-e 's,[@]UTMP_PSEUDO_DEVICE[@],$(UTMP_PSEUDO_DEVICE),g' \
		-e 's,[@]X_CONFIG_OPTIONS[@],$(X_CONFIG_OPTIONS),g' \
		-e 's,[@]X_SERVER[@],$(X_SERVER),g' \		
		-e 's,[@]MDM_RBAC_SYSCMD_KEYS[@],$(MDM_RBAC_SYSCMD_KEYS),g' \
		-e 's,[@]authdir[@],$(authdir),g' \
		-e 's,[@]datadir[@],$(datadir),g' \
		-e 's,[@]dmconfdir[@],$(dmconfdir),g' \
		-e 's,[@]mdmconfdir[@],$(mdmconfdir),g' \
		-e 's,[@]libdir[@],$(libdir),g' \
		-e 's,[@]libexecdir[@],$(libexecdir),g' \
		-e 's,[@]mdmlocaledir[@],$(mdmlocaledir),g' \
		-e 's,[@]logdir[@],$(logdir),g' \
		-e 's,[@]pixmapdir[@],$(pixmapdir),g' \
		-e 's,[@]sbindir[@],$(sbindir),g' \
		<$(srcdir)/mdm.conf.in >mdm.conf

mdm.conf-custom: $(srcdir)/mdm.conf-custom.in
	sed	-e 's,[@]MDM_DEFAULTS_CONF[@],$(MDM_DEFAULTS_CONF),g' \
		<$(srcdir)/mdm.conf-custom.in >mdm.conf-custom

gettextfoo.h: XKeepsCrashing Xsession.in
	cat $^ | $(srcdir)/extract-shell.sh > gettextfoo.h

@INTLTOOL_DESKTOP_RULE@

clean-local:
	rm -f $(DESKTOP_FILES)

uninstall-hook:
	rm -f $(DESTDIR)$(bisessdir)/default.desktop \
	$(DESTDIR)$(instsessdir)/default.desktop \
	$(DESTDIR)$(instsessdir)/ssh.desktop \
	$(DESTDIR)$(MDM_DEFAULTS_CONF) \
	$(DESTDIR)$(MDM_CUSTOM_CONF) \
	`dirname $(DESTDIR)$(MDM_DEFAULTS_CONF)`/factory-`basename $(DESTDIR)$(MDM_DEFAULTS_CONF)` \
	$(DESTDIR)$(confdir)/XKeepsCrashing \
	$(DESTDIR)$(confdir)/Xsession \
	$(DESTDIR)$(confdir)/mdmprefetchlist \
	$(DESTDIR)$(mdmlocaledir)/locale.alias \
	$(DESTDIR)$(initdir)/Default \
	$(DESTDIR)$(superinitdir)/Default \
	$(DESTDIR)$(superpostdir)/Default \
	$(DESTDIR)$(postlogindir)/Default.sample \
	$(DESTDIR)$(predir)/Default \
	$(DESTDIR)$(postdir)/Default

install-data-hook: mdm.conf mdm.conf-custom Xsession Init SuperInit SuperPost PostSession PreSession $(DESKTOP_FILES) $(MDMPREFETCHLIST)
	if test '!' -d $(DESTDIR)$(confdir); then \
		$(mkinstalldirs) $(DESTDIR)$(confdir); \
		chmod 755 $(DESTDIR)$(confdir); \
	fi
	if test '!' -d $(DESTDIR)$(dmconfdir); then \
		$(mkinstalldirs) $(DESTDIR)$(dmconfdir); \
		chmod 755 $(DESTDIR)$(dmconfdir); \
	fi
	if test '!' -d `dirname $(DESTDIR)$(MDM_DEFAULTS_CONF)`; then \
		$(mkinstalldirs) `dirname $(DESTDIR)$(MDM_DEFAULTS_CONF)`; \
		chmod 755 `dirname $(DESTDIR)$(MDM_DEFAULTS_CONF)`; \
	fi
	if test '!' -d `dirname $(DESTDIR)$(MDM_CUSTOM_CONF)`; then \
		$(mkinstalldirs) `dirname $(DESTDIR)$(MDM_CUSTOM_CONF)`; \
		chmod 755 `dirname $(DESTDIR)$(MDM_CUSTOM_CONF)`; \
	fi
	if test -f $(DESTDIR)$(confdir)/mdm.conf; then \
	   if ! cmp -s $(DESTDIR)$(confdir)/mdm.conf $(DESTDIR)$(confdir)/factory-mdm.conf > /dev/null 2>&1 ; then \
		if test '!' -f $(DESTDIR)$(MDM_CUSTOM_CONF); then \
			mv -f $(DESTDIR)$(confdir)/mdm.conf $(DESTDIR)$(MDM_CUSTOM_CONF); \
		else \
			mv -f $(DESTDIR)$(confdir)/mdm.conf $(DESTDIR)$(MDM_DEFAULTS_CONF).org; \
		fi; \
	   fi; \
	fi
	$(INSTALL_DATA) mdm.conf $(DESTDIR)$(MDM_DEFAULTS_CONF)
	chmod 444 $(DESTDIR)$(MDM_DEFAULTS_CONF)
	if test '!' -f $(DESTDIR)$(MDM_CUSTOM_CONF); then \
		$(INSTALL_DATA) mdm.conf-custom $(DESTDIR)$(MDM_CUSTOM_CONF); \
		chmod 644 $(DESTDIR)$(MDM_CUSTOM_CONF); \
	fi
	$(INSTALL_DATA) mdm.conf `dirname $(DESTDIR)$(MDM_DEFAULTS_CONF)`/factory-`basename $(DESTDIR)$(MDM_DEFAULTS_CONF)`

	$(INSTALL_SCRIPT) $(srcdir)/XKeepsCrashing $(DESTDIR)$(confdir)/XKeepsCrashing
	$(INSTALL_SCRIPT) Xsession $(DESTDIR)$(confdir)/Xsession

	-if test -f $(DESTDIR)$(mdmlocaledir)/locale.alias; then \
		cp -f $(DESTDIR)$(mdmlocaledir)/locale.alias $(DESTDIR)$(mdmlocaledir)/locale.alias.orig; \
	fi
	$(INSTALL_DATA) $(srcdir)/locale.alias $(DESTDIR)$(mdmlocaledir)/locale.alias

	if test '!' -d $(DESTDIR)$(bisessdir); then \
		$(mkinstalldirs) $(DESTDIR)$(bisessdir); \
		chmod 755 $(DESTDIR)$(bisessdir); \
	fi

	if test '!' -d $(DESTDIR)$(instsessdir); then \
		$(mkinstalldirs) $(DESTDIR)$(instsessdir); \
		chmod 755 $(DESTDIR)$(instsessdir); \
	fi

	-if test -f $(DESTDIR)$(bisessdir)/Default.desktop; then \
		mv -f $(DESTDIR)$(bisessdir)/Default.desktop $(DESTDIR)$(bisessdir)/Default.desktop.orig; \
	fi
	-if test -f $(DESTDIR)$(bisessdir)/default.desktop; then \
		cp -f $(DESTDIR)$(bisessdir)/default.desktop $(DESTDIR)$(bisessdir)/default.desktop.orig; \
	fi
	$(INSTALL_DATA) default.desktop $(DESTDIR)$(bisessdir)/default.desktop
	chmod 644 $(DESTDIR)$(bisessdir)/default.desktop

	if test '!' -d $(DESTDIR)$(initdir); then \
		$(mkinstalldirs) $(DESTDIR)$(initdir); \
		chmod 755 $(DESTDIR)$(initdir); \
	fi
	-if test -f $(DESTDIR)$(initdir)/Default; then \
		cp -f $(DESTDIR)$(initdir)/Default $(DESTDIR)$(initdir)/Default.orig; \
	fi
	$(INSTALL_SCRIPT) Init $(DESTDIR)$(initdir)/Default

	if test '!' -d $(DESTDIR)$(superinitdir); then \
		$(mkinstalldirs) $(DESTDIR)$(superinitdir); \
		chmod 755 $(DESTDIR)$(superinitdir); \
	fi
	-if test -f $(DESTDIR)$(superinitdir)/Default; then \
		cp -f $(DESTDIR)$(superinitdir)/Default $(DESTDIR)$(superinitdir)/Default.orig; \
	fi
	$(INSTALL_SCRIPT) SuperInit $(DESTDIR)$(superinitdir)/Default

	if test '!' -d $(DESTDIR)$(superpostdir); then \
		$(mkinstalldirs) $(DESTDIR)$(superpostdir); \
		chmod 755 $(DESTDIR)$(superpostdir); \
	fi
	-if test -f $(DESTDIR)$(superpostdir)/Default; then \
		cp -f $(DESTDIR)$(superpostdir)/Default $(DESTDIR)$(superpostdir)/Default.orig; \
	fi
	$(INSTALL_SCRIPT) SuperPost $(DESTDIR)$(superpostdir)/Default

	if test '!' -d $(DESTDIR)$(postlogindir); then \
		$(mkinstalldirs) $(DESTDIR)$(postlogindir); \
		chmod 755 $(DESTDIR)$(postlogindir); \
	fi
	$(INSTALL_SCRIPT) $(srcdir)/PostLogin $(DESTDIR)$(postlogindir)/Default.sample

	if test '!' -d $(DESTDIR)$(predir); then \
		$(mkinstalldirs) $(DESTDIR)$(predir); \
		chmod 755 $(DESTDIR)$(predir); \
	fi
	-if test -f $(DESTDIR)$(predir)/Default; then \
		cp -f $(DESTDIR)$(predir)/Default $(DESTDIR)$(predir)/Default.orig; \
	fi
	$(INSTALL_SCRIPT) PreSession $(DESTDIR)$(predir)/Default

	if test '!' -d $(DESTDIR)$(postdir); then \
		$(mkinstalldirs) $(DESTDIR)$(postdir); \
		chmod 755 $(DESTDIR)$(postdir); \
	fi
	-if test -f $(DESTDIR)$(postdir)/Default; then \
		cp -f $(DESTDIR)$(postdir)/Default $(DESTDIR)$(postdir)/Default.orig; \
	fi
	$(INSTALL_SCRIPT) PostSession $(DESTDIR)$(postdir)/Default

	if test '!' -d $(DESTDIR)$(logdir); then \
		$(mkinstalldirs) $(DESTDIR)$(logdir); \
		chmod 755 $(DESTDIR)$(logdir); \
		chown root:root $(DESTDIR)$(logdir) || : ; \
	fi

	if test '!' -d $(DESTDIR)$(authdir); then \
		$(mkinstalldirs) $(DESTDIR)$(authdir); \
		chmod 1770 $(DESTDIR)$(authdir); \
		chown root:mdm $(DESTDIR)$(authdir) || : ; \
	fi

	system=`uname`; \
	if test -f /usr/include/security/pam_appl.h; then \
	  if test '!' -d $(DESTDIR)$(PAM_PREFIX)/pam.d; then \
		$(mkinstalldirs) $(DESTDIR)$(PAM_PREFIX)/pam.d; \
		chmod 755 $(DESTDIR)$(PAM_PREFIX)/pam.d; \
	   fi; \
	   if test $$system = Linux && test '!' -f $(DESTDIR)$(PAM_PREFIX)/pam.d/mdm; then \
		$(INSTALL_DATA) mdm $(DESTDIR)$(PAM_PREFIX)/pam.d/mdm; \
	   fi; \
	   if test $$system = Linux && test '!' -f $(DESTDIR)$(PAM_PREFIX)/pam.d/mdm-autologin; then \
		$(INSTALL_DATA) mdm-autologin $(DESTDIR)$(PAM_PREFIX)/pam.d/mdm-autologin; \
	   fi; \
	   if test $$system = SunOS; then \
		echo "Please add PAM authentication for mdm and mdm-autologin in $(PAM_PREFIX)/pam.conf!"; \
	   fi; \
	fi

	if test "x$(MDMPREFETCHLIST)" != "x"; then \
	   $(INSTALL_DATA) mdmprefetchlist $(DESTDIR)$(confdir)/mdmprefetchlist; \
	fi

	if test "x$(SSHDESKTOP)" != "x"; then \
	   if test -f $(DESTDIR)$(instsessdir)/ssh.desktop; then \
		cp -f $(DESTDIR)$(instsessdir)/ssh.desktop $(DESTDIR)$(instsessdir)/ssh.desktop.orig; \
	   fi; \
	   $(INSTALL_SCRIPT) ssh.desktop $(DESTDIR)$(instsessdir)/ssh.desktop; \
	   chmod 644 $(DESTDIR)$(instsessdir)/ssh.desktop; \
	fi

