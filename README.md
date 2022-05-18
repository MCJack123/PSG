# JackMacWindows's Programmable Sound Generator
This repo contains the source code for my programmable sound generation board. [Read the article for more info.](https://mcjack123.github.io/PSG/)

![PSG image](images/image3.png)

* `board.fzz` is a Fritzing project file for a basic 4-channel version of the board.
* `PSG.X` contains an MPLAB X project with the wave generator code for the microcontrollers.
* `pico-sound-driver` contains a Raspberry Pi Pico project for managing the MCUs with a USB serial interface.
* `sound-pico.cpp` is a CraftOS-PC plugin for connecting to the board using the `sound` plugin interface.
