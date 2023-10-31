#!/bin/bash

# Preparing

echo " system update 10"
apt update &>/dev/null
echo " pkg install sudo nano wget 09"
apt install sudo nano wget -y &>/dev/null
echo " pkg install language 08"
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
echo " system update 07"
apt update &>/dev/null
echo " pkg install arm64 06"
sudo apt install -y libxrender1 libxfixes3 libxrandr2 libxcomposite1 libxi6 libxcursor1 libopenal1 libsdl2-2.0-0 x11-xserver-utils &>/dev/null
echo " pkg install arm64 05"
sudo apt install -y libgnutls30 libxext-dev libasound2 libvulkan1 libfontconfig-dev libfreetype6-dev libpulse0 libasound2-plugins &>/dev/null
echo " pkg install armhf 04"
sudo apt install -y libxrender1:armhf libxfixes3:armhf libxrandr2:armhf libxcomposite1:armhf libxi6:armhf libxcursor1:armhf libopenal1:armhf libsdl2-2.0-0:armhf &>/dev/null
echo " pkg install armhf 03"
sudo apt install -y libgnutls30:armhf libxext-dev:armhf libasound2:armhf libvulkan1:armhf libfontconfig-dev:armhf libfreetype6-dev:armhf libpulse0:armhf libasound2-plugins:armhf &>/dev/null
echo " install optFile 02"
sudo apt install ranger -y  &>/dev/null
cd .config/ranger &>/dev/null
ranger --copy-config=all &>/dev/null
cd
wget https://github.com/mebabo1/menano/releases/download/Box/ranger.tar.gz &>/dev/null
tar xf ranger.tar.gz
rm -rf ranger.tar.gz
wget https://github.com/mebabo1/menano/releases/download/bininstall/opt.tar.gz &>/dev/null
rm -rf .bashrc
tar xf opt.tar.gz
cp -r opt /
rm -rf opt.tar.gz opt
echo " install amd64File 01"
wget https://github.com/mebabo1/menano/releases/download/bininstall/usr.tar.gz &>/dev/null
tar xf usr.tar.gz
cp -r usr /
rm -rf usr.tar.gz usr
echo " install Box64Droid 00"
cd /sdcard/
wget https://github.com/mebabo1/menano/releases/download/Box/Box64Droid.tar.gz &>/dev/null
tar xf Box64Droid.tar.gz
rm -rf Box64Droid.tar.gz
cd
echo " exit and run x11"
rm install04.sh
