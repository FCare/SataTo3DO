# SataTo3DO
The goal of this project is to replace the physical drive of my NTSC FZ-1 with a low cost ODE
In PCB directory, the final PCB can be found and oredered in some companies like PCBWay or JLCPCB

For the BOM:
- 1 RP2040 moudle: RP2040-plus from waveshare: https://www.waveshare.com/wiki/RP2040-Plus
- 6 level bi-directional shifter: https://fr.aliexpress.com/item/32890488553.html
- 1 Octal bus tranceiver 74HC245N in DIP format
- 1 2x6 connector 1.5mm
- 1 2x15 connector 1.5mm

All other parts are not required. If you want to debug code, you can add a header on uart port and plug a UAR to USB adapter (I am using minicom on linux to see thr traces)

Regarding connectors, take care to order the right one depending your cable.

Use the attached source to compile a uf2 image and copy it on the Rp2040 module.

Do not hesitate to contribute with MR.
