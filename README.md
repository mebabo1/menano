# Smartbox64
---------------------------

First, install the two APKs on your smartphone use v0.118.0 [termux arm64](https://github.com/termux/termux-app) [termux-x11 arm64](https://github.com/xMeM/termux-x11/actions)

You can install Smartbox64 by running the termux app and executing five commands

You can run limited Windows games on your smartphone using termux

English, Korean, and Japanese are available in Smartbox64

Smartbox64 requires 6GB of space to install, including the wine installation

The library with the FFmpeg codec is ready on SmartBox64
Two codecs now work with wine9.18: GStreamer and FFmpeg
Therefore, you may not need to install K-Lite Codec Pack in wine9.18

In-game, GStreamer is suitable for playing videos If you want to watch more videos, install the [K-Lite Codec Pack](https://codecguide.com/download_kl.htm) After selecting the Basic version and clicking the Next button, we recommend that you do not adjust any options during installation.

```sh
PATH="$PATH:$HOME/bin"
```
```sh
curl -o install https://raw.githubusercontent.com/mebabo1/menano/File/install && chmod +x install && ./install
```
```sh
curl -o install02 https://raw.githubusercontent.com/mebabo1/menano/File/install02 && chmod +x install02 && ./install02
```
```sh
curl -o install03 https://raw.githubusercontent.com/mebabo1/menano/File/install03 && chmod +x install03 && ./install03
```
```sh
curl -o update https://raw.githubusercontent.com/mebabo1/menano/File/update && chmod +x update && ./update
```

[How do I work with exe files?](https://youtu.be/2_HRNpfYb4E?si=xfyPsoTDXvwhWlmM)

[How do I change my desktop image?](https://youtu.be/37OT0TS5n1Q?si=XWKP_RtksVkA_rUs)

[How do I enroll a CD-ROM drive?](https://youtu.be/-RGOKmRupRw?si=AqYzyfw9uGhoIwK4)

[Can I run the program quickly?](https://youtu.be/nofblx0pbA0?si=DT0d13iLas9IFel1)

[My keyboard doesn't work in Unity Games](https://youtu.be/3-Gvppin1wk?si=GJ6l86YN0kyBKv7L)

[If you want to change to a newer GLIBC GPU drive distributed by another site, do the following](https://youtu.be/MAQRe2DCh3I?si=DFaQ812HYG620Jly)

I've been playing my favorite game lately, and I can't stand the hassle.

[Guide1](https://youtu.be/zkExc3zT42w?si=rCkb0e-UxmX8E5_R)
[Guide2](https://youtu.be/ferTypykBg4?si=mMJ1DN7hnw0_z0bN)

## Smartbox64 was built using many sites open source
---------------------------
[Dxvk](https://github.com/doitsujin/dxvk)

[Wine](https://github.com/airidosas252/Wine-Builds)

[Wined3d](https://fdossena.com/?p=wined3d/index.frag)

[Termux](https://github.com/termux/termux-app)

[Termux-x11](https://github.com/termux/termux-x11/blob/master/README.md#running-graphical-applications)

[Glibc packages for termux](https://github.com/termux-pacman/glibc-packages)

[Box64](https://github.com/ptitSeb/box64)
