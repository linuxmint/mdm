happygnomedir = $(datadir)/mdm/themes/happygnome
happygnome_DATA = \
	MdmGreeterTheme.desktop \
	happygnome.xml \
	background.svg \
	gnome-logo.svg  \
	screenshot.png

happygnome_in_files = \
	MdmGreeterTheme.desktop.in

@INTLTOOL_DESKTOP_RULE@

EXTRA_DIST = $(happygnome_DATA) $(happygnome_in_files)

clean-local:
	/bin/rm -f MdmGreeterTheme.desktop
