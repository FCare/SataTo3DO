# SataTo3DO
The goal of this project is to replace the physical drive of my NTSC FZ-1 with a low cost ODE
In PCB directory, the final PCB can be found and oredered in some companies like PCBWay or JLCPCB

For the BOM:
- 1 RP2040 moudle: RP2040-plus from waveshare: https://www.waveshare.com/wiki/RP2040-Plus
- 6 level bi-directional shifter: https://fr.aliexpress.com/item/32890488553.html
- 1 Octal bus tranceiver 74HC245N in DIP format
- 1 2x6 connector 1.5mm (to be populated on POWER_1)
- 1 2x15 connector 1.5mm (to be populated on CDROM)

All other parts are not required. If you want to debug code, you can add a header on uart port and plug a UAR to USB adapter (I am using minicom on linux to see the traces)

Regarding connectors, take care to order the right one depending your cable.

Use the attached source to compile a uf2 image and copy it on the Rp2040 module.
Plug a USB-C usb key (CDROM support is preliminary, it requires extra power), do not use a SSD (consuming lot of power).
USB key shall be formatted as FAT32 or exFat.
At the root of the usb key, copy the [boot.iso](https://github.com/fixelsan/3do-ode-firmware/blob/master/boot.iso) you can find on fixel's homepage (it is compatible).
Copy your game as you want on the USB disk.
There is no ordering algorithm supported, so the order will follow the FAT entry order. This means the first game you will transfer to the USB stick will be the first one in the list.

Do not hesitate to contribute with MR.
