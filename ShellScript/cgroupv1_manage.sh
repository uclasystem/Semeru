#! /bin/bash


# Create a memory cgroup for current ssh session and the process launched by it.
# The cgroup will be deleted after exit the ssh session or reboot ?

## Parameters
# 1st, operation type: create, delete 
# 2nd, size for cgroup limitation.
# 3rd, the name of the cgroup

echo " Help message"
echo "Parameters"
echo "  1st, operation type: create, delete"
echo "  2nd, the name of the cgroup. Default memctl"
echo "  3rd, size for cgroup limitation. Default 10g"

operation=$1

if [ -z ${operation} ]
then
	echo "Choose operation: create, delete "
	read operation
fi


if [ "${operation}" = "create"  ]
then

  cgroup_name=$2
  if [ -z "${cgroup_name}" ]
  then
    echo "Used default cgroup name, memctl"
    cgroup_name="memctl"
  else
    echo "Cgroup name : ${cgroupp_name}"
  fi


	mem_size=$3

	if [ -z "${mem_size}"  ]
	then
		echo "Used default memroy size limitations, 10g"
		mem_size="10g"
	else
		echo "Memory size limitations: ${mem_size}"
	fi


	user=`whoami`

	#1 Create 
	echo "sudo cgcreate -t ${user} -a ${user} -g memory:/${cgroup_name}"
	sudo cgcreate -t ${user} -a ${user} -g memory:/${cgroup_name}


	#2 Limi the memory size 
	echo "${mem_size} > /sys/fs/cgroup/memory/${cgroup_name}/memory.limit_in_bytes"
	sudo echo ${mem_size} > /sys/fs/cgroup/memory/${cgroup_name}/memory.limit_in_bytes

elif [	${operation} = "delete"	]
then 
  
  cgroup_name=$2
  if [ -z "${cgroup_name}" ]
  then
    echo "Used default cgroup name, memctl"
    cgroup_name="memctl"
  else
    echo "Cgroup name : ${cgroupp_name}"
  fi

	echo " sudo cgdelete memory:/${cgroup_name}"
	sudo cgdelete memory:/${cgroup_name}

else
	echo "!! Wrong operation !!"
	exit
fi

#3 Add current ssh session into it
# $$ is the pid of the shellscript, not the sheel.
#`sudo echo $$ >> /sys/fs/cgroup/memory/memctl/tasks`
