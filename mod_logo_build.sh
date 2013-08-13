#!/bin/bash
GETVER=`grep 'Siyah-.*-V' .config |sed 's/Siyah-//g' | sed 's/.*".//g' | sed 's/-J.*//g'`;
boot_image=/tmp/initramfs_source/res/images/icon_clockwork.png;
convert -ordered-dither threshold,32,64,32 -pointsize 17 -fill white -draw "text 70,770 \"$GETVER [`date "+%H:%M | %d.%m.%Y"| sed -e ' s/\"/\\\"/g' `]\"" $boot_image $boot_image;
#optipng -o7 -quiet $boot_image; #strange but i kernel crush in phone if using this. disable for now.
echo "1" > $TMPFILE;

