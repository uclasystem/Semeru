#! /bin/bash


## Build openjdk in SLOW-DEBUG mode
## Add RDMA dependency

mode=$1

if [ -z "${mode}"  ]
then
	echo " Please choose the operation : slowdebug, fastdebug, release"
	read mode
fi

## Saint check
if [ -z "$HOME"  ]
then
	echo "HOME is null. Please configure it."
	exit
fi

home_dir="$HOME"

# Both available cores && number of jobs
num_core=16
# in MB, 32GB as default
build_mem="32768"
boot_jdk="${home_dir}/jdk-12.0.2"


## Do the action

if [ "${mode}" = "slowdebug"  ]
then
	./configure --prefix=${home_dir}/jdk12u-self-build  --with-debug-level=slowdebug   --with-extra-cxxflags="-lrdmacm -libverbs" --with-extra-ldflags="-lrdmacm -libverbs"  --with-num-cores=${num_core} --with-jobs=${num_core} --with-memory-size=${build_mem} --with-boot-jdk=${boot_jdk} 

elif [ "${mode}" = "fastdebug"  ]
then
	./configure --prefix=${home_dir}/jdk12u-self-build  --with-debug-level=fastdebug   --with-extra-cxxflags="-lrdmacm -libverbs" --with-extra-ldflags="-lrdmacm -libverbs"  --with-num-cores=${num_core} --with-jobs=${num_core} --with-memory-size=${build_mem} --with-boot-jdk=${boot_jdk} 

elif [ "${mode}" = "release"  ]
then
	./configure --prefix=${home_dir}/jdk12u-self-build  --with-debug-level=release   --with-extra-cxxflags="-lrdmacm -libverbs" --with-extra-ldflags="-lrdmacm -libverbs"  --with-num-cores=${num_core} --with-jobs=${num_core} --with-memory-size=${build_mem} --with-boot-jdk=${boot_jdk} 

elif [ "${mode}" = "build"  ]
then
	# The job number is decided in configuration by --with-boot-jdk=${boot_jdk}
	make

elif [ "${mode}" = "install"  ]
then
	make install

else

	echo " !! Wrong build mode slected. !!"
	exit 
fi



