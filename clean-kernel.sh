cp .config .config.bkp;
make ARCH=arm CROSS_COMPILE=android-toolchain/bin/arm-eabi- mrproper;
cp .config.bkp .config;
make clean;
