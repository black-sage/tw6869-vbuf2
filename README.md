To build for NVidia Tegra TX1 (running R24.2.1):

1. Prepare the kernel headers:

```
cd /usr/src/linux-headers-<kernel-version>/
sudo make modules_prepare
```

1. Make the driver

```
cd ~/sdk_812_1.1.11_linux/driver/vbuf2	
make jetson=y
sudo make install
sudo make load
```

1. Reboot



