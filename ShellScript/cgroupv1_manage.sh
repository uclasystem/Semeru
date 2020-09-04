#! /bin/bash


# Create a memory cgroup for current ssh session and the process launched by it.
# The cgroup will be deleted after exit the ssh session or reboot ?

## Parameters
# 1st, operation type: create, delete 
# 2nd, size for cgroup limitation.

operation=$1

if [ -z ${operation} ]
then
	echo "Choose operation: create, delete "
	read operation
fi


if [ "${operation}" = "create"  ]
then

	mem_size=$2

	if [ -z "${mem_size}"  ]
	then
		echo "Used default memroy size limitations, 10g"
		mem_size="10g"
	else
		echo "Memory size limitations: ${mem_size}"
	fi


	user=`whoami`

	#1 Create 
	echo "sudo cgcreate -t ${user} -a ${user} -g memory:/memctl"
	sudo cgcreate -t ${user} -a ${user} -g memory:/memctl


	#2 Limi the memory size 
	echo "${mem_size} > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes"
	sudo echo ${mem_size} > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes

elif [	${operation} = "delete"	]
then 
	echo " sudo cgdelete memory:/memctl"
	sudo cgdelete memory:/memctl

else
	echo "!! Wrong operation !!"
	exit
fi

#3 Add current ssh session into it
# $$ is the pid of the shellscript, not the sheel.
#`sudo echo $$ >> /sys/fs/cgroup/memory/memctl/tasks`
