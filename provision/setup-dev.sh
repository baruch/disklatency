#!/bin/bash
# Assume run as root

set -e

# Upgrade to SID
echo "deb http://mirror.isoc.org.il/pub/debian/ sid main non-free contrib" > /etc/apt/sources.list
apt-get update
apt-get -y install
apt-get -y apt
apt-get -y install policykit-1
apt-get -y upgrade
apt-get -y dist-upgrade

# Add packages
apt-get -y install strace ltrace vim linux-image-amd64-dbg kdump-tools crash screen aptitude virtualbox-guest-dkms

# Setup kdump
sed -i \
        's/USE_KDUMP=.*/USE_KDUMP=1/' \
        /etc/default/kdump-tools

# Setup screen size
sed -i \
        's/GRUB_CMDLINE_LINUX_DEFAULT.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet vga=788 crashkernel=128M nmi_watchdog=1"/' \
        /etc/default/grub
update-grub
sync

