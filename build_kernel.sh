#!/bin/bash

###############################################################################
# To all DEV around the world :)                                              #
# to build this kernel you need to be ROOT and to have bash as script loader  #
# do this:                                                                    #
# cd /bin                                                                     #
# rm -f sh                                                                    #
# ln -s bash sh                                                               #
# now go back to kernel folder and run:                                       #
# sh load_config.sh                                                           #
# sh clean_kernel.sh                                                          #
#                                                                             #
# Now you can build my kernel.                                                #
# using bash will make your life easy. so it's best that way.                 #
# Have fun and update me if something nice can be added to my source.         #
###############################################################################

# location
export KERNELDIR=`readlink -f .`;
export PARENT_DIR=`readlink -f ..`;
export INITRAMFS_SOURCE=`readlink -f ${KERNELDIR}/../initramfs3`;
export INITRAMFS_TMP="/tmp/initramfs-source";

# kernel
export ARCH=arm;
export USE_SEC_FIPS_MODE=true;
export KERNEL_CONFIG="dorimanx_defconfig";

# build script
export USER=`whoami`
export TMPFILE=$(mktemp -t)
export HOST_CHECK=`uname -n`;
export OLDMODULES=`find -name *.ko`;

# system compiler
# gcc x.x.x
# export CROSS_COMPILE=$PARENT_DIR/toolchain/bin/arm-none-eabi-;

# gcc 4.4.3 (CM9)
# export CROSS_COMPILE=/media/Source-Code/android/system/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-;

# gcc 4.7 (Linaro 12.04)
# export CROSS_COMPILE=$PARENT_DIR/linaro/bin/arm-eabi-;

# gcc 4.7.2 (Linaro 12.07)
export CROSS_COMPILE=${KERNELDIR}/android-toolchain/bin/arm-eabi-;

# importing PATCH for GCC depend on GCC version
GCCVERSION=`./scripts/gcc-version.sh ${CROSS_COMPILE}gcc`;

# check xml-config for "STweaks"-app
XML2CHECK="${INITRAMFS_SOURCE}/res/customconfig/customconfig.xml";
xmllint --noout ${XML2CHECK} || echo "xml-Error: ${XML2CHECK}" && exit 1;

if [ "a$GCCVERSION" == "a0404" ]; then
	cp ${KERNELDIR}/arch/arm/boot/compressed/Makefile_old_gcc ${KERNELDIR}/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.3.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0404" ]; then
	cp ${KERNELDIR}/arch/arm/boot/compressed/Makefile_old_gcc ${KERNELDIR}/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.4.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0405" ]; then
	cp ${KERNELDIR}/arch/arm/boot/compressed/Makefile_old_gcc ${KERNELDIR}/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.5.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0406" ]; then
	cp ${KERNELDIR}/arch/arm/boot/compressed/Makefile_linaro ${KERNELDIR}/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.6.X Compiler Detected, building";
elif [ "a$GCCVERSION" == "a0407" ]; then
	cp ${KERNELDIR}/arch/arm/boot/compressed/Makefile_linaro ${KERNELDIR}/arch/arm/boot/compressed/Makefile;
	echo "GCC 4.7.X Compiler Detected, building";
else
	echo "Compiler not recognized! please fix the 'build_kernel.sh'-script to match your compiler.";
	exit 0;
fi;

if [[ $HOST_CHECK == "dorimanx-virtual-machine" ]]; then
	NAMBEROFCPUS=32;
	echo "Dori power detected!";
else
	NAMBEROFCPUS=`grep 'processor' /proc/cpuinfo | wc -l`;
fi;

if [ "${1}" != "" ]; then
	export KERNELDIR=`readlink -f ${1}`;
fi;

if [ ! -f ${KERNELDIR}/.config ]; then
	cp ${KERNELDIR}/arch/arm/configs/${KERNEL_CONFIG} .config;
	make ${KERNEL_CONFIG};
fi;

. ${KERNELDIR}/.config;

# get version
GETVER=`grep 'Siyah-.*-V' ${KERNELDIR}/.config | sed 's/.*".//g' | sed 's/-J.*//g'`;

# remove previous zImage files
if [ -e ${KERNELDIR}/zImage ]; then
	rm ${KERNELDIR}/zImage;
fi;
if [ -e ${KERNELDIR}/arch/arm/boot/zImage ]; then
	rm ${KERNELDIR}/arch/arm/boot/zImage;
fi;

# remove all old modules before compile
cd ${KERNELDIR};
for i in ${OLDMODULES}; do
	rm -f $i;
done;

# remove previous initramfs files
if [ -d ${INITRAMFS_TMP} ]; then
	echo "removing old temp iniramfs";
	rm -rf ${INITRAMFS_TMP};
fi;
if [ -f "/tmp/cpio*" ]; then
	echo "removing old temp iniramfs_tmp.cpio";
	rm -rf /tmp/cpio*;
fi;

# clean initramfs old compile data
rm -f usr/initramfs_data.cpio;
rm -f usr/initramfs_data.o;

# copy new config
cd ${KERNELDIR}/;
cp .config arch/arm/configs/${KERNEL_CONFIG};

# make modules
mkdir -p ${INITRAMFS}/lib/modules
if [ $USER != "root" ]; then
	make -j${NAMBEROFCPUS} modules || exit 1;
else
	nice -n -15 make -j${NAMBEROFCPUS} modules || exit 1;
fi;

# copy initramfs files to tmp directory
cp -ax ${INITRAMFS_SOURCE} ${INITRAMFS_TMP}

# clear git repositories in initramfs
if [ -e ${INITRAMFS_TMP}/.git ]; then
	rm -rf /tmp/initramfs-source/.git;
fi;

# remove empty directory placeholders
find ${INITRAMFS_TMP} -name EMPTY_DIRECTORY -exec rm -rf {} \;

# remove mercurial repository
if [ -d ${INITRAMFS_TMP}/.hg ]; then
	rm -rf ${INITRAMFS_TMP}/.hg;
fi;

# remove more from tmp-dir ...
rm -f ${INITRAMFS_TMP}/compress-sql.sh;
rm -f ${INITRAMFS_TMP}/update*;

# copy modules into initramfs
mkdir -p ${INITRAMFS_TMP}/lib/modules;
find -name '*.ko' -exec cp -av {} ${INITRAMFS_TMP}/lib/modules/ \;
${CROSS_COMPILE}strip --strip-debug ${INITRAMFS_TMP}/lib/modules/*.ko;
chmod 755 ${INITRAMFS_TMP}/lib/modules/*;

read -p "create new kernel Image LOGO with version & date (y/n)?";
if [ "$REPLY" == "y" ]; then
	# create new image with version & date
	convert -ordered-dither threshold,32,64,32 -pointsize 17 -fill white -draw "text 70,770 \"${GETVER} [$(date "+%H:%M | %d.%m.%Y"| sed -e ' s/\"/\\\"/g' )]\"" ${INITRAMFS_TMP}/res/images/icon_clockwork.png ${INITRAMFS_TMP}/res/images/icon_clockwork.png;
	optipng -o7 ${INITRAMFS_TMP}/res/images/icon_clockwork.png;
fi;

# make kernel
if [ $USER != "root" ]; then
	time make -j${NAMBEROFCPUS} zImage CONFIG_INITRAMFS_SOURCE="${INITRAMFS_TMP}";
else
	time nice -n -15 make -j${NAMBEROFCPUS} zImage CONFIG_INITRAMFS_SOURCE="${INITRAMFS_TMP}";
fi;

# restore clean arch/arm/boot/compressed/Makefile_clean till next time
cp ${KERNELDIR}/arch/arm/boot/compressed/Makefile_clean ${KERNELDIR}/arch/arm/boot/compressed/Makefile;

if [ -e ${KERNELDIR}/arch/arm/boot/zImage ]; then
	${KERNELDIR}/mkshbootimg.py ${KERNELDIR}/zImage ${KERNELDIR}/arch/arm/boot/zImage ${KERNELDIR}/payload.tar.xz ${KERNELDIR}/recovery.tar.xz;

	# copy all needed to ready kernel folder
	cp ${KERNELDIR}/.config ${KERNELDIR}/arch/arm/configs/${KERNEL_CONFIG};
	cp ${KERNELDIR}/.config ${KERNELDIR}/READY-JB/;
	rm ${KERNELDIR}/READY-JB/boot/zImage;
	rm ${KERNELDIR}/READY-JB/Kernel_*;
	stat ${KERNELDIR}/zImage;
	cp ${KERNELDIR}/zImage /${KERNELDIR}/READY-JB/boot/;
	cd ${KERNELDIR}/READY-JB/;
	zip -r Kernel_${GETVER}-`date +"[%H-%M]-[%d-%m]-JB-CM-AOKP-SGII-PWR-CORE"`.zip .;

	STATUS=`adb get-state`;
	if [ "$STATUS" == "device" ]; then
		read -p "push kernel to android (y/n)?";
		if [ "$REPLY" == "y" ]; then
			adb push ${KERNELDIR}/READY-JB/Kernel_*JB*.zip /sdcard/;
		fi;
	fi;
else
	# with red-color
	echo -e "\e[1;31mKernel STUCK in BUILD! no zImage exist\e[m"
fi;
