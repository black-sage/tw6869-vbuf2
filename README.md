To build for NVidia Tegra TX1 (running R24.2.1):

1. Prepare the kernel headers:
  
  ```
  cd /usr/src/linux-headers-<kernel-version>/
  sudo make modules_prepare
  ```
  
1. Build the driver source:
  
  Do this from within the driver source directory (wherever you cloned this repo).
  
  ```	
  cd <source-dir>
  make jetson=y
  sudo make install
  sudo make load
  ```

1. Edit kernel boot params

  The kernel boot parameters are in ```/boot/extlinux/extlinux.conf```
  
  Add ```vmalloc=512M cma=64M coherent_pool=32M``` to the end of the last line. 
  The last line starts with ```APPEND``` and is part of the ```LABEL primary``` section of the config file. 
  It is a very long line and probably wraps several times in your text editor.
  It should look like this:
  
  ```
  LABEL primary
    MENU LABEL primary kernel
    LINUX /boot/Image
    ...
    ...
    APPEND fbcon:map0 console=tty0 ... ...
      ... vmalloc=512M cma=64M coherent_pool=32M
  ```
    
1. Reboot



