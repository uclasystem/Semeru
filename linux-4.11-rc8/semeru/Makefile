# Kernel Module - Put into Kernel Source tree
# 
# The basic idea is to use the Makefile of Kernel Source to do the compilation.
# This module needs some external variable defined in kernel, or other kernel module.
#	So we need to use the Module.SYSVERS
#
#	This testcase is built as the C based Octopus platform.
#	For the client end,  it works like a normal swap partitions. 
#	All the I/O requst written  into the swap partition will be forwarded to remote memory pool.
#	The remote memory pool is C based now.
#	Finally, we need to replace the remote server as a Java based memory pool.
#





# obj-[m] means compile this module but not link it into kernel at startup.
obj-m += semeru_cpu_server.o

# a. the frontswap path
semeru_cpu_server-y	:= semeru_cpu.o
semeru_cpu_server-y	+= frontswap_ops.o
semeru_cpu_server-y	+= frontswap_rdma.o
semeru_cpu_server-y	+= local_dram.o

# b. the block layer path
#semeru_cpu_server-y := block_path_register_disk.o	# main entry
#semeru_cpu_server-y += block_path_rdma.o	# merged into .so
#semeru_cpu_server-y += local_block_device.o  # merged into .so







#kernel_rdma_module_dir=/lib/modules/$(shell uname -r)/extra/semeru_cpu


#ccflags-y := -v
#NOSTDINC_FLAGS += -I/usr/src/ofa_kernel/default/include


# If build agianst OFED, enable here && KBUILD_EXTRA_SYMBOLS="$(KSYMVERS)" 
# The Modules.sysvers listed here will be copy to bulding directory
#KSYMVERS := /usr/src/ofa_kernel/default/Module.symvers


# Debug
# Build the module separately
#all:
	# -C change to kernel source code, to use its Makefile 
	#  M is a hythen, return to $(PWD) after reading the Make in /lib/modules/$(shell uname -r)/build
	# make  -C /lib/modules/$(shell uname -r)/build  M=$(PWD)  KBUILD_EXTRA_SYMBOLS="$(KSYMVERS)"  modules
	#	make  -C /lib/modules/$(shell uname -r)/build  M=$(PWD)  modules
#clean:
#	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

# install:
# 	mkdir -p $(kernel_rdma_module_dir)
# 	/usr/bin/install -c -m 644 ./semeru_cpu.ko $(kernel_rdma_module_dir)

# uninstall:
# 	modprobe -r semeru_cpu
# 	rm $(kernel_rdma_module_dir)/semeru_cpu.ko




OFA_DIR ?= /usr/src/ofa_kernel/default
OFA_INCLUDE := $(OFA_DIR)/include
OFA_SYMVERS := $(OFA_DIR)/Module.symvers

ifneq ($(LINUXINCLUDE),)
# kbuild part of makefile

LINUXINCLUDE := \
  -I$(OFA_INCLUDE) \
  ${LINUXINCLUDE}

else
# normal makefile

export KBUILD_EXTRA_SYMBOLS=$(OFA_SYMVERS)
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
endif