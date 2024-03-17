#!/bin/bash
#
cd /root/kernel_v6.6

if [ ! -d linux ]; then
	branch="branch_pinenote_6-6-12"
	git clone --depth 1 --branch ${branch} https://github.com/m-weigand/linux
fi

cd linux

test -d pack && rm -r pack
mkdir pack

make clean
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- pinenote_defconfig
# build deb package with uncompressed Image
# make -j 2 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- all
make -j 2 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-pinenote-`date +%Y%m%d%H%M` KDEB_PKGVERSION="" KBUILD_IMAGE=arch/arm64/boot/Image deb-pkg
cd ..
ls
rm *dbg*.deb
# mv linux-image*.deb linux-image_with_uncompressed_image.deb
rename 's/.deb/_no_compression.deb/' linux-image*

mv *.deb linux/pack/
mv linux-upstream* linux/pack/

cd linux
# make -j 2 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-pinenote-`date +%Y%m%d%H%M` KDEB_PKGVERSION="" bindeb-pkg
# mv ../*.deb pack/

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH=${PWD}/pack modules_install
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_PATH=${PWD}/pack dtbs_install
cp ./arch/arm64/boot/dts/rockchip/rk3566-pinenote-v1.2.dtb pack/
cp ./arch/arm64/boot/Image pack/
# rename 's/.deb/_with_compression.deb/' linux-image*
cd pack
tar cvzf modules.tar.gz lib
rm -r lib
cd ../..

# extract the results from the Docker container
cp -r linux/pack /github/home/pack_v6.6
