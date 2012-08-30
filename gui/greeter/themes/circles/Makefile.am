circlesdir = $(datadir)/mdm/themes/circles
circles_DATA = \
	GdmGreeterTheme.desktop \
	circles.xml \
	background.svg \
	screenshot.png

circles_in_files = \
	GdmGreeterTheme.desktop.in

@INTLTOOL_DESKTOP_RULE@

EXTRA_DIST = $(circles_DATA) $(circles_in_files)

clean-local:
	/bin/rm -f GdmGreeterTheme.desktop
