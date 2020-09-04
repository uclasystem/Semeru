#! /bin/bash

dest=$1

if [ -z "${dest}"  ]
then
	echo "Sync semeru folder to other servers."
	echo "Enter the destination server. e.g. server_home"
	read dest

fi



if [ "${dest}" = server_home  ]
then
	echo "scp -r ./semeru wcx@server_home:~/linux-4.11-rc8/"
	scp -r ./semeru wcx@server_home:~/linux-4.11-rc8/

elif [ "${dest}" = zion-1  ]
then
	echo "scp -r ./semeru wcx@zion-1:~/linux-4.11-rc8/"
	scp -r ./semeru wcx@zion-1:~/linux-4.11-rc8/

elif [ "${dest}" = "mac"  ]
then
	echo "Warning : Only copy the semeru_cpu.ko"
	echo "scp wcx@server_home:~/linux-4.11-rc8/semeru/semeru_cpu.ko  ./semeru/"	
	scp wcx@server_home:~/linux-4.11-rc8/semeru/semeru_cpu.ko  ./semeru/
	scp wcx@server_home:~/linux-4.11-rc8/semeru/rdma_client.o  ./semeru/
	scp wcx@server_home:~/linux-4.11-rc8/semeru/register_disk.o  ./semeru/
	scp wcx@server_home:~/linux-4.11-rc8/semeru/local_block_device.o  ./semeru/
	scp wcx@server_home:~/linux-4.11-rc8/vmlinux  ./

elif [ "${dest}" = "qemu"  ]
then
	echo "Warning : Only copy the folder of semeru "
	echo "scp -r ${HOME}/linux-4.11-rc8/semeru  qemu:~/linux-4.11-rc8/"
	scp -r ${HOME}/linux-4.11-rc8/semeru  qemu:~/linux-4.11-rc8/

else

	echo "!! Wrong destination : ${dest} !!"
	exit
fi
