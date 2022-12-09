# JackMacWindows's Programmable Sound Generator
This repo contains the source code for my programmable sound generation board. [Read the article for more info.](https://mcjack123.github.io/PSG/)

![PSG image](images/image3.jpg)
![PSG image 2](part-2/images/image6.jpg)

## Contents
* `board.fzz` is a Fritzing project file for a basic 4-channel version of the board.
* `PSG-PCB` is a KiCad PCB layout for the complete 16-channel board.
* `PSG.X` contains an MPLAB X project with the wave generator code for the microcontrollers.
* `pico-sound-driver` contains a Raspberry Pi Pico project for managing the MCUs with a USB MIDI interface.
* `sound-pico.cpp` is a CraftOS-PC plugin for connecting to the board using the `sound` plugin interface.
* `programmer.cpp` is a program to quickly flash UF2, HEX, or BIN (concatenated HEX + UF2) firmware files to the PIC chips and the Pico. The current production firmware is available in `firmware.bin`.
