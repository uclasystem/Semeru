#! /bin/bash


### Parameters

version="4.11.0-rc8"


### Operations

op=$1


if [ -z "${op}"  ]
then
	echo "Please select the operation, e.g. build, install, replace, update_grub"
	read op
fi

echo "Do the action ${op}"






## Functions
delete_old_kernel_contents () {
	echo "sudo rm /boot/initramfs-${version}*   /boot/System.map-${version}*  /boot/vmlinuz-${version}* "
	sleep 1
	sudo rm /boot/initramfs-${version}*   /boot/System.map-${version}*  /boot/vmlinuz-${version}*
}


install_new_kernel_contents () {
	echo "install kernel modules"
	sleep 1
	sudo make modules_install

	echo "install kernel image"
	sleep 1
	sudo make install
}



# For CentOS, there maybe 2 grab entries
efi_grab="/boot/efi/EFI/centos/grub.cfg"

update_grub_entries () {
	echo "(MUST run with sudo)Delete old grub entry:"

	if [ -e /boot/efi/EFI/centos/grub.cfg ]
	then
		echo " Delete EFI grab : sudo rm ${efi_grab}"
		sleep 1
		sudo rm ${efi_grab}

		echo " Rebuild EFI grab : sudo grub2-mkconfig -o ${efi_grab}"
		sleep 1
		sudo grub2-mkconfig -o ${efi_grab}	

	else
	
		echo "Delete /boot/grub2/grub.cfg"
		sleep 1
		sudo rm /boot/grub2/grub.cfg	

		echo "Rebuild the grub.cfg"
		echo "grub2-mkconfig -o /boot/grub2/grub.cfg"
		sleep 1
		sudo grub2-mkconfig -o /boot/grub2/grub.cfg	
	fi

	echo "Set default entry to Item 0"
	sudo grub2-set-default 0

	echo "Current grub entry"
	sleep 1
	sudo grub2-editenv list
}



### Do the action

if [ "${op}" = "build" ]
then
	echo "make oldconfig"
	sleep 1
	make oldconfig

	echo "make -j16"
	sleep 1
	make -j16

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

elif [ "${op}" = "update_grub"  ]
then

	update_grub_entries

else
	echo "!! Wrong Operation - ${op} !!"
fi





