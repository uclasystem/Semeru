#! /bin/bash



###
# Macro define

# The swap file/partition size should be equal to the whole size of remote memory
SWAP_PARTITION_SIZE="32G"

# Cause of sudo, NOT use ${HOME}
home_dir="/mnt/ssd/wcx"
semeru_module_dir="${home_dir}/Semeru/linux-4.11-rc8/semeru"
swap_file="${home_dir}/swapfile" 



##
# Do the action

action=$1

if [ -z "${action}" ]
then
	echo "This shellscipt for Infiniswap pre-configuration."
	echo "Run it with sudo or root"
	echo ""
	echo "Pleaes slect what to do:"
	echo "semeru : Prepare the semeru memory pool to run."
	echo "	1.1 close current Swap partition"
	echo "	1.2 create swapfile as fake swap device"
	echo "	1.3 install semeru"
  echo "create_swap_file : Creat a swapfile under ${swap_file} with size ${SWAP_PARTITION_SIZE}"
	echo "load_semeru : load semeru module"
	echo "	2.1 Instll semeru for the frontswap path.	"
	echo "close_semeru : Close the remote memory partition && Remove the Semeru module "	
	
	read action 

fi

if [ -z "${home_dir}"  ]
then 
	echo " Warning : home_dir is null."
fi

## self defined function

function close_swap_partition () {

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



function create_swap_file () {

  if [ -e ${swap_file} ]
  then
    echo "Please confirm the size of swapfile match the expected ${SWAP_PARTITION_SIZE}" 
    cur_size=$(du -sh ${swap_file} | awk '{print $1;}' ) 
    if [ "${cur_size}"  != "${SWAP_PARTITION_SIZE}" ]
    then
      echo "Current ${swap_file} : ${cur_size} NOT equal to expected ${SWAP_PARTITION_SIZE}"    
      echo "Delete it"
      sudo rm ${swap_file}
      
      echo "Create a file, ~/swapfile, with size ${SWAP_PARTITION_SIZE} as swap device."
      sudo fallocate -l ${SWAP_PARTITION_SIZE} ${swap_file} 
      sudo chmod 600 ${swap_file}
    fi
  else 
    # not exit, create a swapfile
    echo "Create a file, ~/swapfile, with size ${SWAP_PARTITION_SIZE} as swap device."
    sudo fallocate -l ${SWAP_PARTITION_SIZE} ${swap_file} 
    sudo chmod 600 ${swap_file}
    du -sh ${swap_file}
  fi

  sleep 1
  echo "Mount the ${swap_file} as swap device"
  sudo mkswap ${swap_file}
  sudo swapon ${swap_file}

  # check
  swapon -s
}




if [ "${action}" = "semeru"  ]
then
	# Prepare the semeru module to run

	# 1. Close current swap partition
	echo "Close current swap partition"
	close_swap_partition

  # 2. Create a swapfile and mount it as swap device 
  create_swap_file

	# 2. load semeru module 
	echo "insmod ~/linux-4.11-rc8/semeru/semeru_cpu_server.ko"
	sudo insmod ${semeru_module_dir}/semeru_cpu_server.ko

elif [ "${action}" = "create_swap_file" ]
then
 create_swap_file 

elif [ "${action}" = "load_semeru"  ]
then

	# 1, mound semeru
	echo "insmod ~/linux-4.11-rc8/semeru/semeru_cpu_server.ko"
	sudo insmod ${semeru_module_dir}/semeru_cpu_server.ko

elif [	"${action}" = "close_semeru"	]
then
	
	#1
	close_swap_partition

	#2 remove the semeru moduels
	echo "Remove the semeru_cpu_server modeule"
	sudo rmmod ${semeru_module_dir}/semeru_cpu_server.ko

else
	echo "!! Wrong choice : ${action}"
fi




