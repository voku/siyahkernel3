#!/bin/bash

###############################################################################
# To all DEV around the world :)                                              #
#                                                                             #
# 1.) use the "bash"                                                          #
# chsh -s /bin/bash `whoami`                                                  #
#                                                                             #
# 2.) load the ".config"                                                      #
# ./load_config.sh                                                            #
#                                                                             #
# 3.) clean the sources                                                       #
# ./clean_kernel.sh                                                           #
#                                                                             #
# 4.) now you can build my kernel                                             #
# ./build_kernel.sh                                                           #
#                                                                             #
# Have fun and update me if something nice can be added to my source.         #
###############################################################################

# location
if [ "${1}" != "" ]; then
	export KERNELDIR=`readlink -f ${1}`;
else
	export KERNELDIR=`readlink -f .`;
fi;

export PARENT_DIR=`readlink -f ${KERNELDIR}/..`;
export INITRAMFS_SOURCE=`readlink -f ${KERNELDIR}/../initramfs3`;
export INITRAMFS_TMP=/tmp/initramfs_source;

# kernel
export ARCH=arm;
export USE_SEC_FIPS_MODE=true;
export KERNEL_CONFIG=dorimanx_defconfig;

# build script
export USER=`whoami`;
export HOST=`uname -n`;
export TMPFILE=`mktemp -t`;

# system compiler
# gcc x.x.x
# export CROSS_COMPILE=$PARENT_DIR/toolchain/bin/arm-none-eabi-;

# gcc 4.4.3 (CM9)
# export CROSS_COMPILE=/media/Source-Code/android/system/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-;

# gcc 4.7 (Linaro 12.04)
# export CROSS_COMPILE=$PARENT_DIR/linaro/bin/arm-eabi-;

# gcc 4.7.2 (Linaro 12.07)
export CROSS_COMPILE=$KERNELDIR/android-toolchain/bin/arm-eabi-;

# importing PATCH for GCC depend on GCC version
GCCVERSION=`./scripts/gcc-version.sh ${CROSS_COMPILE}gcc`;

# check xml-config for "STweaks"-app
XML2CHECK="${INITRAMFS_SOURCE}/res/customconfig/customconfig.xml";
xmllint --noout $XML2CHECK;
if [ $? == 1 ]; then
	echo "xml-Error: $XML2CHECK";
	exit 1;
fi;

# compiler detection
if [ "a$GCCVERSION" == "a0404" ]; then
	cp $KERNELDIR/arch/arm/boot/compressed/Makefile_old_gcc $KERNELDIR/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.3.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0404" ]; then
	cp $KERNELDIR/arch/arm/boot/compressed/Makefile_old_gcc $KERNELDIR/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.4.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0405" ]; then
	cp $KERNELDIR/arch/arm/boot/compressed/Makefile_old_gcc $KERNELDIR/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.5.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0406" ]; then
	cp $KERNELDIR/arch/arm/boot/compressed/Makefile_linaro $KERNELDIR/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.6.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0407" ]; then
	cp $KERNELDIR/arch/arm/boot/compressed/Makefile_linaro $KERNELDIR/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.7.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0408" ]; then
	cp $KERNELDIR/arch/arm/boot/compressed/Makefile_linaro $KERNELDIR/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.8.X Compiler Detected, building";
else
	echo "Compiler not recognized! please fix the 'build_kernel.sh'-script to match your compiler.";
	exit 0;
fi;

# dorimanx detection ;)
if [ $HOST == "dorimanx-virtual-machine" ]; then
	NAMBEROFCPUS=16;
	echo "Dori power detected!";
else
	NAMBEROFCPUS=`grep processor /proc/cpuinfo | wc -l`;
	if [ "$NAMBEROFCPUS" -lt "4" ]; then
		NAMBEROFCPUS=4;
		echo "slow system detected, setting 4 build treads"
	fi;
fi;

# copy config
if [ ! -f $KERNELDIR/.config ]; then
	cp $KERNELDIR/arch/arm/configs/$KERNEL_CONFIG $KERNELDIR/.config;
fi;

# read config
. $KERNELDIR/.config;

# get version from config
GETVER=`grep 'Siyah-.*-V' $KERNELDIR/.config | sed 's/.*".//g' | sed 's/-J.*//g'`;

# remove previous zImage files
if [ -e $KERNELDIR/zImage ]; then
	rm $KERNELDIR/zImage;
fi;
if [ -e $KERNELDIR/arch/arm/boot/zImage ]; then
	rm $KERNELDIR/arch/arm/boot/zImage;
fi;

# remove previous initramfs files
if [ -d $INITRAMFS_TMP ]; then
	echo "removing old temp iniramfs";
	rm -rf $INITRAMFS_TMP;
fi;

# clean initramfs old compile data
rm -f $KERNELDIR/usr/initramfs_data.cpio;
rm -f $KERNELDIR/usr/initramfs_data.o;

# copy new config
cp $KERNELDIR/.config $KERNELDIR/arch/arm/configs/$KERNEL_CONFIG;

# remove all old modules before compile
find $KERNELDIR -name "*.ko" | parallel rm -rf {};

# make modules
mkdir -p $INITRAMFS/lib/modules
if [ $USER != "root" ]; then
	make -j $NAMBEROFCPUS modules || exit 1;
else
	nice -n -15 make -j $NAMBEROFCPUS modules || exit 1;
fi;

# copy initramfs files to tmp directory
cp -ax $INITRAMFS_SOURCE $INITRAMFS_TMP;

# create new image with version & date
read -t 3 -p "create new kernel Image LOGO with version & date, 3sec timeout (y/n)?";
echo "0" > $TMPFILE;
if [ "$REPLY" == "y" ]; then
	(
		convert -ordered-dither threshold,32,64,32 -pointsize 17 -fill white -draw "text 70,770 \"$GETVER \
			[`date "+%H:%M | %d.%m.%Y"| sed -e ' s/\"/\\\"/g' `]\"" \
			$INITRAMFS_TMP/res/images/icon_clockwork.png \
			$INITRAMFS_TMP/res/images/icon_clockwork.png;
		optipng -o7 -quiet $INITRAMFS_TMP/res/images/icon_clockwork.png;
		echo "1" > $TMPFILE;	
	)&
else
	echo "1" > $TMPFILE;
fi;    

# clear git repository from tmp-initramfs
if [ -d $INITRAMFS_TMP/.git ]; then
	rm -rf $INITRAMFS_TMP/.git;
fi;

# clear mercurial repository from tmp-initramfs
if [ -d $INITRAMFS_TMP/.hg ]; then
	rm -rf $INITRAMFS_TMP/.hg;
fi;

# remove empty directory placeholders from tmp-initramfs
find $INITRAMFS_TMP -name EMPTY_DIRECTORY | parallel rm -rf {};

# remove more from from tmp-initramfs ...
rm -f $INITRAMFS_TMP/compress-sql.sh;
rm -f $INITRAMFS_TMP/update*;

# copy modules into tmp-initramfs
mkdir -p $INITRAMFS_TMP/lib/modules;
find $KERNELDIR -name '*.ko' | parallel cp -av {} $INITRAMFS_TMP/lib/modules/;
find $INITRAMFS_TMP/lib/modules/ -name '*.ko' | parallel ${CROSS_COMPILE}strip --strip-debug {};
chmod 755 $INITRAMFS_TMP/lib/modules/*;

# wait for the boot-image
while [ $(cat ${TMPFILE}) == 0 ]; do
	sleep 2;
	echo "wait for image ...";
done;

# make kernel!!!
if [ "$USER" != "root" ]; then
	time make -j $NAMBEROFCPUS zImage CONFIG_INITRAMFS_SOURCE="$INITRAMFS_TMP";
else
	time nice -n -15 make -j $NAMBEROFCPUS zImage CONFIG_INITRAMFS_SOURCE="$INITRAMFS_TMP";
fi;

# restore clean arch/arm/boot/compressed/Makefile_clean till next time
cp $KERNELDIR/arch/arm/boot/compressed/Makefile_clean $KERNELDIR/arch/arm/boot/compressed/Makefile;

if [ -e $KERNELDIR/arch/arm/boot/zImage ]; then
	cp $KERNELDIR/arch/arm/boot/zImage $KERNELDIR/;
	cp $KERNELDIR/.config $KERNELDIR/arch/arm/configs/$KERNEL_CONFIG;

	# clean old files ...
	rm $KERNELDIR/READY-JB/boot/zImage;
	rm $KERNELDIR/READY-JB/Kernel_*;

	# copy all needed to ready kernel folder
	cp $KERNELDIR/.config $KERNELDIR/READY-JB/;
	stat $KERNELDIR/zImage || exit 1;
	cp $KERNELDIR/zImage /$KERNELDIR/READY-JB/boot/;

	# create zip-file
	cd $KERNELDIR/READY-JB/ && zip -r Kernel_${GETVER}-`date +"[%H-%M]-[%d-%m]-JB-CM-AOKP-SGII-PWR-CORE"`.zip .;

	# push to android
	ADB_STATUS=`adb get-state`;
	if [ "$ADB_STATUS" == "device" ]; then
		read -t 3 -p "push kernel to android, 3sec timeout (y/n)?";
		if [ "$REPLY" == "y" ]; then
			adb push $KERNELDIR/READY-JB/Kernel_*JB*.zip /sdcard/;
		fi;
	fi;
else
	# with red-color
	echo -e "\e[1;31mKernel STUCK in BUILD! no zImage exist\e[m"
fi;
