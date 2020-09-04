#! /bin/bash



action=$1

if [ -z "${action}" ]
then
	echo "This shellscipt for Infiniswap pre-configuration."
	echo "Run it with sudo or root"
	echo ""
	echo "Pleaes slect what to do:"
	echo "semeru : Prepare the semeru memory pool to run."
	echo "	1.1 close current Swap partition"
	echo "	1.2 load_semeru"
	echo "	1.3 mount_semeru"
	echo "load_semeru : load semeru module for Disk Driver DEBUG"
	echo "	2.1 close current Swap Partition."
	echo "	2.2 Instll semeru to manage the old swap partition.	"
	echo "mount_semeru : Format & Mount the remote memory partition"	
	echo "close_semeru : Close the remote memory partition && Remove the Semeru module "	
	
	read action 

fi

if [ -z "${HOME}"  ]
then 
	echo " Warning :HOME is null."
fi

## self defined function

close_swap_partition () {

  echo "Close current Swap Partition"
  swap_bd=$(swapon -s | grep "dev" | cut -d " " -f 1 )
  
  if [ -z "${swap_bd}" ]
  then
    echo "Nothing to close."
  else
    echo "Swap Partition to close :${swap_bd} "
    sudo swapoff "${swap_bd}"  
  fi 

	#check
	echo "Current swap partition:"
	swapon -s
}


if [ "${action}" = "semeru"  ]
then
	# Prepare the semeru module to run

	# 1. Close current swap partition
	echo "Close current swap partition"
	close_swap_partition

	# 2. load semeru module 
	echo "insmod ~/linux-4.11-rc8/semeru/semeru_cpu.ko"
	sudo insmod ./semeru_cpu.ko

	# 3. Mount Semeru memory pool as Swap partition
	echo "Format & Mount semeru(rmempool) as Swap parititon"
	sudo mkswap /dev/rmempool
	sudo swapon /dev/rmempool

	# 4. check Swap partition
	swapon -s

elif [ "${action}" = "load_semeru"  ]
then
	# 1
	close_swap_partition

	# 2, mound semeru
	echo "insmod ~/linux-4.11-rc8/semeru/semeru_cpu.ko"
	sudo insmod ./semeru_cpu.ko

	# 3, check
	lsblk
elif  [ "${action}" = "mount_semeru"  ]
then
	echo "Format rmempool to swap parititon"
	sudo mkswap /dev/rmempool

	echo "Mount /de/rmempool as swap partition"
	sudo swapon /dev/rmempool

	swapon -s

elif [	"${action}" = "close_semeru"	]
then
	
	#1
	close_swap_partition

	#2 remove the semeru moduels
	echo "Remove the semeru_cpu modeule"
	sudo rmmod ./semeru_cpu.ko

else
	echo "!! Wrong choice : ${action}"
fi




