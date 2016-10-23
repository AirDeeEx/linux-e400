#!/bin/bash

kernel="Phoenix"
variant="v1"
toolchain=~/arm-eabi-4.4.3/bin
toolchain2="arm-eabi-"
config="Phoenix_defconfig"
kerneltype="zImage"
jobcount="-j$(grep -c ^processor /proc/cpuinfo)"

	export ARCH=arm
	export CROSS_COMPILE=$toolchain/"$toolchain2"

	read -p "Previous build found, clean working directory..(y/n)? : " cchoice
	case "$cchoice" in
		y|Y )
			rm -rf arch/arm/boot/"$kerneltype"
			make clean && make mrproper && rm -rf zip-creator/*.zip zip-creator/zImage zip-creator/modules/*.ko
			echo "Working directory cleaned...";;
		 n|N ) echo "continuing...";;
		* ) echo "invalid option"; sleep 2 ; build.sh;;
	esac

echo "now building the kernel"

START=$(date +%s)

	make "$config"
	make "$jobcount"

# the zip creation
if [ -f arch/arm/boot/"$kerneltype" ]; then

    rm -f zip-creator/$kerneltype

   cp arch/arm/boot/$kerneltype zip-creator/$kerneltype
	# changed antdking "now copy all created modules"
	# modules
	# (if you get issues with copying wireless drivers then it's your own fault for not cleaning)

	find . -name *.ko | xargs cp -a --target-directory=zip-creator/modules/

	zipfile="$kernel-$variant-$daytime.zip"
	cd zip-creator
	rm -f *.zip
	zip -r $zipfile * -x *kernel/.gitignore*

	echo "zip saved to zip-creator/$zipfile"
	
else # [ -f arch/arm/boot/"$kerneltype" ]
    echo "the build failed so a zip won't be created"
fi # [ -f arch/arm/boot/"$kerneltype" ]

END=$(date +%s)
BUILDTIME=$((END - START))
B_MIN=$((BUILDTIME / 60))
B_SEC=$((BUILDTIME - E_MIN * 60))
echo -ne "\033[32mBuildtime: "
[ $B_MIN != 0 ] && echo -ne "$B_MIN min(s) "
echo -e "$B_SEC sec(s)\033[0m"
