clear
sysext-build zz-xdg-desktop-portal _build/
sysext-add zz-xdg-desktop-portal.sysext.raw
systemctl stop --user xdg-desktop-portal.service xdg-desktop-portal-gnome.service xdg-desktop-portal-gtk.service
/usr/libexec/xdg-desktop-portal --replace --verbose

