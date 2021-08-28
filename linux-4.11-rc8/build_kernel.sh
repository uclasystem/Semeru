#! /bin/bash


### Parameters

version="4.11.0-rc8"
#LocalVersion="Semeru"
# Or remove the suffix
LocalVersion=

num_cores=`nproc --all`

### Operations

op=$1


if [ -z "${op}"  ]
then
	echo "Please select the operation, e.g. build, install, replace, headers-install, update_grub"
	read op
fi

echo "Do the action ${op}"





# Detect Linux releases
OS_DISTRO=$( awk -F= '/^NAME/{print $2}' /etc/os-release | sed -e 's/^"//' -e 's/"$//' )
if [[ $OS_DISTRO == "CentOS Linux" ]]
then
	echo "Running on CentOS..."
elif [ $OS_DISTRO == "Ubuntu" ]
then
	echo "Running on Ubuntu..."
fi

## Functions
delete_old_kernel_contents () {
	if [[ $OS_DISTRO == "CentOS Linux" ]]
	then
		echo "sudo rm /boot/initramfs-${version}${LocalVersion}.img   /boot/System.map-${version}${LocalVersion}  /boot/vmlinuz-${version}${LocalVersion} "
		sleep 1
		sudo rm /boot/initramfs-${version}${LocalVersion}.img   /boot/System.map-${version}${LocalVersion}  /boot/vmlinuz-${version}${LocalVersion}
	elif [ $OS_DISTRO == "Ubuntu" ]
	then
		echo "sudo rm /boot/initrd.img-${version}*   /boot/System.map-${version}*  /boot/vmlinuz-${version}* "
		sleep 1
		sudo rm /boot/initrd.img-${version}*   /boot/System.map-${version}*  /boot/vmlinuz-${version}*
	fi
}


install_new_kernel_contents () {
	echo "install kernel modules"
	sleep 1
	sudo make modules_install

	echo "install kernel image"
	sleep 1
	sudo make install

}



update_grub_entries () {
  
  # The kernel boot version
  grub_boot_verion=4

	if [[ $OS_DISTRO == "CentOS Linux" ]]
	then
		# For CentOS, there maybe 2 grub entries
		echo "(MUST run with sudo)Delete old grub entry:"

		efi_grub="/boot/efi/EFI/centos/grub.cfg"
		if [[ -e /boot/efi/EFI/centos/grub.cfg ]]
		then
			echo " Delete EFI grub : sudo rm ${efi_grub}"
			sleep 1
			sudo rm ${efi_grub}

			echo " Rebuild EFI grub : sudo grub-mkconfig -o ${efi_grub}"
			sleep 1
			sudo grub2-mkconfig -o ${efi_grub}

		else
			echo "Delete /boot/grub/grub.cfg"
			sleep 1
			sudo rm /boot/grub/grub.cfg

			echo "Rebuild the grub.cfg"
			echo "grub-mkconfig -o /boot/grub/grub.cfg"
			sleep 1
			sudo grub2-mkconfig -o /boot/grub/grub.cfg
		fi
  
    echo "Set bootable kernel verion to ${version}"
	  echo "Set default entry to Item ${grub_boot_verion} (Please check if this is the expected ${version})"
	  sudo grub-set-default ${grub_boot_verion}

	  echo "Current grub entry"
	  sleep 1
	  sudo grub-editenv list

	elif [ $OS_DISTRO == "Ubuntu" ]
	then
		# # Ubuntu: to list grub entries
		# awk -F\' '/menuentry / {print $2}' /boot/grub/grub.cfg
		echo " Rebuild grub"
		sudo update-grub2

    echo "Warning : For Ubuntu, please edit /etc/default/grub to specify the right boot kernel verion"
    echo "Example : GRUB_DEFAULT='Advanced options for Ubuntu>Ubuntu, with Linux 4.11.0-rc8'"  

	fi
    
}



### Do the action

if [ "${op}" = "build" ]
then
	echo "make oldconfig"
	sleep 1
#	make oldconfig

	echo "make LOCALVERSION=${localVersion}  -j${num_cores}"
	sleep 1
	make LOCALVERSION="${localVersion}"  -j${num_cores}

elif [ "${op}" = "install" ]
then
	delete_old_kernel_contents
	sleep 1

	install_new_kernel_contents
	sleep 1

	update_grub_entries

elif [ "${op}" = "replace"  ]
then
	delete_old_kernel_contents
	sleep 1

	echo "Install kernel image only"
	sudo make install
	sleep 1

	update_grub_entries

elif [ "${op}" = "headers-install" ]
then
  echo "Warning - the kernel headers install may overwirite" 
  echo "the original headers installed by other libraries !!"
  echo "Must backup the headers first !!"
  echo "STOP the installation if you didn't back up the /usr/include !!"
  echo "3"
  sleep 1

  echo "2"
  sleep 1

  echo "1"  
  sleep
	echo "Install uapi kernel headers to /usr/include/linux/"
	sudo make headers_install INSTALL_HDR_PATH=/usr



elif [ "${op}" = "update_grub"  ]
then

	update_grub_entries

else
	echo "!! Wrong Operation - ${op} !!"
fi
