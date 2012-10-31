
SUBDIRS = \
	16x16 24x24 32x32 48x48

pixmapdir = $(datadir)/pixmaps

BITMAPS = mdm-foot-logo.png mdm-mdm-nobody.png mdm-mdm-nohost.png

pixmap_DATA = $(BITMAPS)

EXTRA_DIST  = $(BITMAPS)

gtk_update_icon_cache = gtk-update-icon-cache -f -t $(datadir)/icons/hicolor

install-data-hook: update-icon-cache
uninstall-hook: update-icon-cache
update-icon-cache:
	@-if test -z "$(DESTDIR)"; then \
		echo "Updating Gtk icon cache."; \
		$(gtk_update_icon_cache); \
	else \
		echo "*** Icon cache not updated.  After (un)install, run this:"; \
		echo "***   $(gtk_update_icon_cache)"; \
	fi
