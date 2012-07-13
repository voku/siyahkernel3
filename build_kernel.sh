#!/bin/sh

# location
export KERNELDIR=`readlink -f .`
export PARENT_DIR=`readlink -f ..`
export INITRAMFS_SOURCE=`readlink -f $KERNELDIR/../initramfs3`

# kernel
export ARCH=arm
export EXTRA_AFLAGS=-mfpu=neon
export USE_SEC_FIPS_MODE=true

# compiler
# gcc 4.5.2
#export CROSS_COMPILE=$PARENT_DIR/toolchain/bin/arm-none-eabi-
# gcc 4.4.3 (CM9)
#export CROSS_COMPILE=/media/Source-Code/android/system/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-
# gcc 4.7 (Linaro 12.04)
#export CROSS_COMPILE=$PARENT_DIR/linaro/bin/arm-eabi-
# gcc 4.6 (Linaro 12.06)
export CROSS_COMPILE=android-toolchain/bin/arm-eabi-

# build script
export USER=`whoami`

NAMBEROFCPUS=`grep 'processor' /proc/cpuinfo | wc -l`
INITRAMFS_TMP="/tmp/initramfs-source"

if [ "${1}" != "" ];
then
	export KERNELDIR=`readlink -f ${1}`
fi

if [ ! -f $KERNELDIR/.config ]; then
	cp $KERNELDIR/arch/arm/configs/dorimanx_defconfig .config
	make dorimanx_defconfig
fi

. $KERNELDIR/.config

# remove previous zImage files
if [ -e $KERNELDIR/zImage ]; then
	rm $KERNELDIR/zImage
fi

if [ -e $KERNELDIR/arch/arm/boot/zImage ]; then
	rm $KERNELDIR/arch/arm/boot/zImage
fi

# remove all old modules before compile
cd $KERNELDIR
OLDMODULES=`find -name *.ko`
for i in $OLDMODULES; do
	rm -f $i
done

# remove previous initramfs files
if [ -e $INITRAMFS_TMP ]; then
	echo "removing old temp iniramfs"
	rm -rf $INITRAMFS_TMP
fi

if [ -f /tmp/cpio* ]; then
	echo "removing old temp iniramfs_tmp.cpio"
	rm -rf /tmp/cpio*
fi

# clean initramfs old compile data
rm -f usr/initramfs_data.cpio
rm -f usr/initramfs_data.o

cd $KERNELDIR/
if [ $USER != "root" ]; then
	make -j$NAMBEROFCPUS modules || exit 1
else
	nice -n 10 make -j$NAMBEROFCPUS modules || exit 1
fi

# copy initramfs files to tmp directory
cp -ax $INITRAMFS_SOURCE $INITRAMFS_TMP
# clear git repositories in initramfs
if [ -e /tmp/initramfs-source/.git ]; then
	rm -rf /tmp/initramfs-source/.git
fi
# remove empty directory placeholders
find $INITRAMFS_TMP -name EMPTY_DIRECTORY -exec rm -rf {} \;
# remove mercurial repository
if [ -d $INITRAMFS_TMP/.hg ]; then
	rm -rf $INITRAMFS_TMP/.hg
fi

#For now remove the VLM binary from initramfs till it's will be used for something.
if [ -e $INITRAMFS_TMP/sbin/lvm ]; then
	rm -f $INITRAMFS_TMP/sbin/lvm
fi

# copy modules into initramfs
mkdir -p $INITRAMFS/lib/modules
mkdir -p $INITRAMFS_TMP/lib/modules
find -name '*.ko' -exec cp -av {} $INITRAMFS_TMP/lib/modules/ \;
${CROSS_COMPILE}strip --strip-debug $INITRAMFS_TMP/lib/modules/*.ko
chmod 755 $INITRAMFS_TMP/lib/modules/*
if [ $USER != "root" ]; then
	make -j$NAMBEROFCPUS zImage CONFIG_INITRAMFS_SOURCE="$INITRAMFS_TMP" || exit 1
else
	nice -n 10 make -j$NAMBEROFCPUS zImage CONFIG_INITRAMFS_SOURCE="$INITRAMFS_TMP" || exit 1
fi

if [ -e $KERNELDIR/arch/arm/boot/zImage ]; then
	$KERNELDIR/mkshbootimg.py $KERNELDIR/zImage $KERNELDIR/arch/arm/boot/zImage $KERNELDIR/payload.tar $KERNELDIR/recovery.tar.xz

	# copy all needed to ready kernel folder.
	cp $KERNELDIR/.config $KERNELDIR/arch/arm/configs/dorimanx_defconfig
	cp $KERNELDIR/.config $KERNELDIR/READY/
	rm $KERNELDIR/READY/boot/zImage
	rm $KERNELDIR/READY/Kernel_Dorimanx-SGII-ICS*
	stat $KERNELDIR/zImage
	GETVER=`grep 'Siyah-Dorimanx-V' arch/arm/configs/dorimanx_defconfig | cut -c 38-42`
	cp $KERNELDIR/zImage /$KERNELDIR/READY/boot/
	cd $KERNELDIR/READY/
	zip -r Kernel_Dorimanx-SGII-ICS-$GETVER-`date +"T-%H-%M-D-%d-%m"`.zip .
else
	echo "Kernel STUCK in BUILD! no zImage exist"
fi
