ArgonOS KVM agent - Linux and macOS
===================================

Linux (X11 or XWayland; glibc 2.34+: Ubuntu 22.04, Debian 12, Fedora 35 or newer)
  cp /media/$USER/<the dongle>/kvmagent-linux ~/ && chmod +x ~/kvmagent-linux
  sudo cp /media/$USER/<the dongle>/99-argon-kvm.rules /etc/udev/rules.d/
  sudo udevadm control --reload        (then unplug and plug the dongle)
  ~/kvmagent-linux                     (--setup to change Wi-Fi/password/id)
  Without the udev rule it works under sudo.  A pure Wayland session (no X)
  has no screen it can read: log in to an "on Xorg" session.

macOS 10.13 or newer (Intel and Apple silicon)
  cp /Volumes/<the dongle>/kvmagent-mac ~/ && chmod +x ~/kvmagent-mac
  xattr -d com.apple.quarantine ~/kvmagent-mac 2>/dev/null
  ~/kvmagent-mac
  The first run asks for Screen Recording (System Settings > Privacy &
  Security): allow it for the Terminal, then run again.  Until then the
  picture is only the wallpaper.

Keyboard and mouse need nothing on any system - they are plain USB HID.
