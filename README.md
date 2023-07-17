# SataTo3DO
The goal of this project is to replace the physical drive of my NTSC FZ-1 with a low-cost ODE.

## Hardware Construction
In the `PCB` directory, the `SATA23DO_final.zip` gerbers for the PCB can be found and ordered from a fabricator such as PCBWay or JLCPCB.

For the BOM:
- 1x RP2040 module: RP2040-Plus from WaveShare: https://www.waveshare.com/wiki/RP2040-Plus
- 6x bi-directional level shifter modules: https://fr.aliexpress.com/item/32890488553.html
- 1x Octal bus tranceiver 74HC245N in DIP format
- 1x 2x6 connector 1.25mm (to be populated on POWER_1)
- 1x 2x15 connector 1.25mm (to be populated on CDROM)
Connectors can be found on aliexpress (https://m.fr.aliexpress.com/item/1005002262284714.html)
All other parts are not required to be populated. If you want to debug code, you can add a header on the UART port, and connect a UART to USB adapter (I am using minicom on Linux as a serial console to see the traces.)

Regarding connectors, take care to order the right one depending your 3DO's cable.

## Building Source Code
Use the attached source to compile a uf2 image using cmake, and copy it onto the RP2040 module (connected via USB.)

Note that `tinyusb` and `pico-sdk` come from git submodules. They can be cloned using the command:

```bash
git submodule update --init --recursive
```
In order to build:
```bash
sudo apt install cmake gcc-arm-none-eabi build-essential
mkdir build
cd build
cmake ../
make -j8
```
Per default, the setup is done for waveshare_rp2040_plus_4mb.
For a 16mb, you need to edit the CMakeLists.txt file and change the PICO_BOARD to waveshare_rp2040_plus_16mb

Otherwise, cmake will raise errors that it cannot find the necessary include files.

## Setup
Plug a USB-C usb flash drive key (CDROM support is preliminary, it requires extra power), do not use a SSD (consuming lot of power).

USB flash drive must be formatted as either the FAT32 or exFat file systems.

At the root of the usb key, copy the [boot.iso](https://github.com/fixelsan/3do-ode-firmware/blob/master/boot.iso) you can find on fixel's homepage (it is compatible).

Copy your desired game ISOs to the USB disk.

There is no sorting algorithm supported, so the order displayed on the 3DO will follow the FAT entry order. This means the first game you will transfer to the USB stick will be the first one in the list.

Do not hesitate to contribute with MR.
