#!/bin/bash

# Preparing

echo "
deb http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse

deb http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse

deb http://ports.ubuntu.com/ubuntu-ports jammy-backports main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy-backports main restricted universe multiverse

deb http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
deb-src http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse"
> /etc/apt/sources.list

sudo dpkg --add-architecture armhf

sudo apt update

apt install sudo nano -y
apt install language-pack-ko language-pack-ja language-pack-en fonts-nanum fonts-nanum-coding fonts-nanum-extra -y

wget https://github.com/mebabo1/menano/releases/download/bininstall/usr.tar.gz
tar xf usr.tar.gz
cp -r usr /
rm -rf usr.tar.gz usr

rm -rf install04
