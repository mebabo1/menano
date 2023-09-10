#!/bin/bash

# Preparing

echo " system update"
apt update &>/dev/null
echo " install sudo nano wget"
apt install sudo nano wget -y &>/dev/null
echo " install language"
apt install language-pack-ko language-pack-ja language-pack-en fonts-nanum fonts-nanum-coding fonts-nanum-extra -y &>/dev/null

echo "
deb http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse

deb http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse

deb http://ports.ubuntu.com/ubuntu-ports jammy-backports main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy-backports main restricted universe multiverse

deb http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
" > /etc/apt/sources.list

dpkg --add-architecture armhf
echo " system update"
apt update &>/dev/null
echo " pkg install arm64"
sudo apt install -y libxrender1 libxfixes3 libxrandr2 libxcomposite1 libxi6 libxcursor1
sudo apt install -y libgnutls30 libxext-dev libasound2 libvulkan1 libfontconfig-dev libfreetype6-dev libpulse0 libasound2-plugins
echo " pkg install armhf"
sudo apt install -y libxrender1:armhf libxfixes3:armhf libxrandr2:armhf libxcomposite1:armhf libxi6:armhf libxcursor1:armhf
sudo apt install -y libgnutls30:armhf libxext-dev:armhf libasound2:armhf libvulkan1:armhf libfontconfig-dev:armhf libfreetype6-dev:armhf libpulse0:armhf libasound2-plugins:armhf



wget https://github.com/mebabo1/menano/releases/download/bininstall/usr.tar.gz
tar xf usr.tar.gz
cp -r usr /
rm -rf usr.tar.gz usr

rm -rf install04
