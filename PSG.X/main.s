;
; PSG.X/main.s
; PSG
;
; This file contains the firmware for the PIC channel chips. It generates a
; waveform of the specified type, volume, and frequency; responds to events from
; an 8-bit data bus over A5/4 & D5-0; and can receive firmware updates from the
; 8-bit data bus and write them to flash.
;
; This code is licensed under the GPLv2 license.
; Copyright (c) 2022-2023 JackMacWindows.
;

PROCESSOR 16LF1613

CONFIG FOSC = INTOSC
CONFIG PWRTE = OFF
CONFIG MCLRE = ON
CONFIG CP = OFF
CONFIG BOREN = ON
CONFIG CLKOUTEN = OFF
    
CONFIG WRT = OFF
CONFIG ZCD = OFF
CONFIG PLLEN = ON
CONFIG STVREN = ON
CONFIG BORV = LO
CONFIG LPBOR = OFF
CONFIG LVP = ON
    
CONFIG WDTCPS = WDTCPS2
CONFIG WDTE = SWDTEN
CONFIG WDTCWS = WDTCWS100
CONFIG WDTCCS = LFINTOSC
    
; memory allocations:
; common memory:
; - 0x070: wave type
; - 0x071: volume
; - 0x072: increment high
; - 0x073: increment low
; - 0x074: position high
; - 0x075: position low
; - 0x076: volume adjustment to center
; - 0x078: square duty cycle
; - 0x079: if set on WDT reset, do full reset
; general purpose memory:
; - 0x020-0x025: temporary storage for frequency multiplication: product
; - 0x026-0x028: permanent storage for frequency multiplication: multiplier
; - 0x029: temporary storage for frequency multiplication: loop index
; - 0x120: scale operation result
; - 0x121: scale operation input
; - 0x122: filler loop index
; - 0x123-0x124: random buffer
    
; wave types: none, square, sawtooth up, sawtooth down, triangle, sine, noise, ?
    
psect intentry,global,class=CODE,delta=2
_interrupt:
    ; for some reason the assembler puts the code two words behind the actual IV
    nop
    nop
    ; enable WDT for interrupt
    movlb 14
    bsf 0x11, 0
    clrf 0x79
    ; wait for clock
    movlb 0
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    
    btfsc 0x0C, 5
    goto setClock
    btfsc 0x0C, 4
    goto setVolume
setWaveType:
    movf 0x0E, 0
    andlw 0x07
    movwf 0x70
    ; check if setting square wave
    xorlw 0x01
    btfss 0x03, 2
    goto done
    
    ; wait for next clock
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; set duty cycle
    movf 0x0C, 0
    andlw 0x30
    movwf 0x78
    lslf 0x78
    lslf 0x78
    movf 0x0E, 0
    iorwf 0x78
    goto done
    
setVolume:
    ; set volume
    movf 0x0E, 0
    movwf 0x71
    lslf 0x71
    lslf 0x71
    ; set adjustment
    movlw 0x7F
    movwf 0x76
    lsrf 0x71, 0
    subwf 0x76
    goto done
    
setClock:
    btfss 0x0C, 4
    goto setIncrement
    ; reset if cmd = 0xFF
    movf 0x0E, 0
    xorlw 0x3F
    btfsc 0x03, 2
    goto sysCommand
    ; calculate register value: (~C | 0x18) << 3
    comf 0x0E, 0
    iorlw 0x18
    lslf 0x09
    lslf 0x09
    lslf 0x09
    ; set clock frequency
    movlb 1
    movwf 0x19
    goto done
    
setIncrement:
    ; set high incr
    movf 0x0E, 0
    movwf 0x72
    
    ; wait for next clock
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; set low incr
    movf 0x0C, 0
    andlw 0x30
    movwf 0x73
    lslf 0x73
    lslf 0x73
    movf 0x0E, 0
    iorwf 0x73
    
done:
    ; Reset interrupt register, disable WDT and return
    movlb 14
    bcf 0x11, 0
    movlw 0x90
    movwf 0x0B
    clrf 0x79
    retfie
    
sysCommand:
    ; system commands:
    ; 0xFF [0x00]: reset
    ; 0xFF 0x01: enter bootloader
    ; others: ignore & exit
    bsf 0x79, 0 ; do full reset on no command
    ; wait for next clock
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; run command
    movf 0x0E, 0
    btfsc 0x03, 2 ; 0
    reset
    movwf 0x7A
    decfsz 0x7A ; 1
    bra 1
    goto _bootloader
    decfsz 0x7A ; 1
    bra 1
    goto _bootloader
    goto done ; others
    
psect init,class=CODE,delta=2 ; PIC10/12/16
global _main
_main:
    ; Disable WDT & check for reset
    movlb 14
    bcf 0x11, 0
    movlb 1
    btfsc 0x16, 4 ; PCON:~RWDT
    goto init
    btfsc 0x79, 0
    goto init
    clrf 0x79
    goto loop
init:
    ; Set initial state
    movlb 0
    clrf 0x70
    clrf 0x71
    clrf 0x72
    clrf 0x73
    clrf 0x74
    clrf 0x75
    clrf 0x78
    clrf 0x79
    ; Debugging: pre-set wave
    ;movlw 0x02
    ;movwf 0x70
    ;movlw 0xFF
    ;movwf 0x71
    ;movlw 0x00
    ;movwf 0x72
    ;movlw 0x3F
    ;movwf 0x73
    ;movlw 0x80
    ;movwf 0x78
    ; Set up I/O pins
    movlb 3
    clrf 0x0C ; ANSELA
    clrf 0x0E ; ANSELC
    movlb 4
    clrf 0x0C ; WPUA
    clrf 0x0E ; WPUC
    movlb 1
    movlw 0x36 ; TRISA5, TRISA4, TRISA2, TRISA1
    movwf 0x0C
    movlw 0x3F ; TRISC5, TRISC4, TRISC3, TRISC2, TRISC1, TRISC0
    movwf 0x0E
    ; Set rising edge interrupt
    movlw 0x40 ; INTEDG
    movwf 0x15
    ; Set clock frequency to 32MHz
    movlw 0xF8 ; SPLLEN, IRCF = 16 MHz, SCS = 0
    movwf 0x19
    ; Enable DAC
    movlb 2
    movlw 0xA0 ; DAC1EN, DAC1OE1, DAC1PSS = Vdd
    movwf 0x18
    ; Enable interrupts
    movlw 0x90 ; GIE, INTE
    movwf 0x0B
    
    ; We need to keep track of the clock cycles of each branch, and tune the
    ; others so that they all take the same amount of time. This will allow us
    ; to use the instruction count as a stable clock for sample timings.
    ; Base clock time: 22 clocks
    ; Base clock time + scaling: 60 clocks
    ; Clock time for each type: 10 clocks
    ; Clock time for each type + scaling: 48 clocks
    ; Total clock time required: 70 clocks
loop:
    ; Check volume + frequency for all 0s: 11 clocks
    movf 0x71
    btfsc 0x03, 2
    goto none
    movf 0x72
    btfss 0x03, 2
    goto ok ; 7
    movf 0x73
    btfsc 0x03, 2
    goto none
    goto ok2 ; 11

ok:
    ; filler to replace third check
    nop
    nop
    nop
    nop
ok2:
    ; Jump to handler: 5 clocks
    movf 0x70, 0
    brw ; jump ahead to correct handler
    goto none
    goto square
    goto sawtooth
    goto rsawtooth
    goto triangle
    goto sine
    goto noise
    goto none
    
none:
    ; Since there's no output, we can ignore clock timings and just loop back.
    movlw 0x7f
    movwf 0x19 ; reset DAC output
    ;sleep ; nothing else will happen, so wait for interrupt
    goto loop
    
square:
    ; Compare high position with duty cycle: 8 clocks
    movf 0x78, 0
    subwf 0x74, 0
    movf 0x03, 0
    andlw 0x01
    addlw 0xFF ; 0 => 255, 1 => 0 + carry
    andwf 0x71, 0 ; AND with volume
    addwf 0x76, 0 ; add adjustment
    movwf 0x19 ; write out
    ; Filler: 38 clocks
    ; Since the wait time is so high, use a loop
    movlw 12 ; calculate with (required - 1) // 3
    movwf 0x22
    decfsz 0x22
    bra -2
    nop ; calculate with (required - 1) % 3
    ; Jump: 2 clocks
    goto next_loop

sawtooth:
    ; Copy position to output: 2 clocks
    movf 0x74, 0
    movwf 0x21
    ; Filler: 6 clocks
    nop
    nop
    nop
    nop
    nop
    nop
    ; Jump: 2 clocks
    goto scale_output
    
rsawtooth:
    ; Subtract position from 0xFF: 3 clocks
    movf 0x74, 0
    sublw 0xFF
    movwf 0x21
    ; Filler: 5 clocks
    nop
    nop
    nop
    nop
    nop
    ; Jump: 2 clocks
    goto scale_output
    
triangle:
    ; Check rising or falling: 2/3 clocks
    btfsc 0x74, 7
    goto triangle_down
    
    ; Double and output: 3 clocks
    movf 0x74, 0
    addwf 0x74, 0
    movwf 0x21
    ; Filler: 3 clocks
    nop
    nop
    nop
    ; Jump: 2 clocks
    goto scale_output
    
triangle_down:
    ; Subtract position from 0xFF + double: 4 clocks
    movf 0x74, 0
    sublw 0xFF
    movwf 0x21
    addwf 0x21
    ; Filler: 1 clocks
    nop
    ; Jump: 2 clocks
    goto scale_output
    
sine:
    ; Get sine table entry: 8 clocks
    movf 0x74, 0
    call sine_table ; 6 clocks
    movwf 0x21
    ; Filler: 0 clocks
    ; Jump: 2 clocks
    goto scale_output
    
noise:
    ; Generate random sample: 13 clocks
    ; Uses NES noise algorithm
    ; 1. check if period has looped over (skip if not)
    btfss 0x03, 1
    goto skip_noise
    ; 2. calculate feedback: r[1] ^ r[0] => W[0]
    lsrf 0x24, 0
    xorwf 0x24, 0
    ; 3. shift right
    lsrf 0x23
    rrf 0x24
    ; 4. set high bit from W[0]
    btfsc 0x09, 0
    bsf 0x23, 6
    ; 5. write out r[0] * volume
    movlw 0x00
    btfss 0x24, 0
    movf 0x71, 0
    addwf 0x76, 0
    movwf 0x19 ; write out
    ; Filler: 33 clocks
    ; Since the wait time is so high, use a loop
    movlw 10 ; calculate with (required - 1) // 3
    movwf 0x22
    decfsz 0x22
    bra -2
    nop ; calculate with (required - 1) % 3
    nop
    ; Jump: 2 clocks
    goto next_loop
    
skip_noise:
    ; Filler for no noise change: 42 clocks
    ; Since the wait time is so high, use a loop
    movlw 13 ; calculate with (required - 1) // 3
    movwf 0x22
    decfsz 0x22
    bra -2
    nop ; calculate with (required - 1) % 3
    nop
    ; Jump: 2 clocks
    goto next_loop
    
scale_output:
    ; Multiply unscaled output by volume + output: 38 clocks
    ; from http://www.piclist.com/techref/microchip/math/mul/8x8.htm
mult    MACRO
    btfsc   0x03, 0
    addwf   0x20,F
    rrf     0x20,F
    rrf     0x21,F
ENDM

    movf 0x71, 0
    clrf    0x20                    ;* 1 cycle
    rrf     0x21,F                  ;* 1 cycle

    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    mult                            ;* 4 cycles
    movf 0x20, 0
    addwf 0x76, 0
    movwf 0x19
    
next_loop:
    ; Advance position & loop: 6 clocks
    movf 0x73, 0
    addwf 0x75
    movf 0x72, 0
    addwfc 0x74
    goto loop
    
sine_table:
    brw
    retlw 127
    retlw 130
    retlw 133
    retlw 136
    retlw 139
    retlw 143
    retlw 146
    retlw 149
    retlw 152
    retlw 155
    retlw 158
    retlw 161
    retlw 164
    retlw 167
    retlw 170
    retlw 173
    retlw 176
    retlw 179
    retlw 182
    retlw 184
    retlw 187
    retlw 190
    retlw 193
    retlw 195
    retlw 198
    retlw 200
    retlw 203
    retlw 205
    retlw 208
    retlw 210
    retlw 213
    retlw 215
    retlw 217
    retlw 219
    retlw 221
    retlw 224
    retlw 226
    retlw 228
    retlw 229
    retlw 231
    retlw 233
    retlw 235
    retlw 236
    retlw 238
    retlw 239
    retlw 241
    retlw 242
    retlw 244
    retlw 245
    retlw 246
    retlw 247
    retlw 248
    retlw 249
    retlw 250
    retlw 251
    retlw 251
    retlw 252
    retlw 253
    retlw 253
    retlw 254
    retlw 254
    retlw 254
    retlw 254
    retlw 254
    retlw 255
    retlw 254
    retlw 254
    retlw 254
    retlw 254
    retlw 254
    retlw 253
    retlw 253
    retlw 252
    retlw 251
    retlw 251
    retlw 250
    retlw 249
    retlw 248
    retlw 247
    retlw 246
    retlw 245
    retlw 244
    retlw 242
    retlw 241
    retlw 239
    retlw 238
    retlw 236
    retlw 235
    retlw 233
    retlw 231
    retlw 229
    retlw 228
    retlw 226
    retlw 224
    retlw 221
    retlw 219
    retlw 217
    retlw 215
    retlw 213
    retlw 210
    retlw 208
    retlw 205
    retlw 203
    retlw 200
    retlw 198
    retlw 195
    retlw 193
    retlw 190
    retlw 187
    retlw 184
    retlw 182
    retlw 179
    retlw 176
    retlw 173
    retlw 170
    retlw 167
    retlw 164
    retlw 161
    retlw 158
    retlw 155
    retlw 152
    retlw 149
    retlw 146
    retlw 143
    retlw 139
    retlw 136
    retlw 133
    retlw 130
    retlw 127
    retlw 124
    retlw 121
    retlw 118
    retlw 115
    retlw 111
    retlw 108
    retlw 105
    retlw 102
    retlw 99
    retlw 96
    retlw 93
    retlw 90
    retlw 87
    retlw 84
    retlw 81
    retlw 78
    retlw 75
    retlw 72
    retlw 70
    retlw 67
    retlw 64
    retlw 61
    retlw 59
    retlw 56
    retlw 54
    retlw 51
    retlw 49
    retlw 46
    retlw 44
    retlw 41
    retlw 39
    retlw 37
    retlw 35
    retlw 33
    retlw 30
    retlw 28
    retlw 26
    retlw 25
    retlw 23
    retlw 21
    retlw 19
    retlw 18
    retlw 16
    retlw 15
    retlw 13
    retlw 12
    retlw 10
    retlw 9
    retlw 8
    retlw 7
    retlw 6
    retlw 5
    retlw 4
    retlw 3
    retlw 3
    retlw 2
    retlw 1
    retlw 1
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 0
    retlw 1
    retlw 1
    retlw 2
    retlw 3
    retlw 3
    retlw 4
    retlw 5
    retlw 6
    retlw 7
    retlw 8
    retlw 9
    retlw 10
    retlw 12
    retlw 13
    retlw 15
    retlw 16
    retlw 18
    retlw 19
    retlw 21
    retlw 23
    retlw 25
    retlw 26
    retlw 28
    retlw 30
    retlw 33
    retlw 35
    retlw 37
    retlw 39
    retlw 41
    retlw 44
    retlw 46
    retlw 49
    retlw 51
    retlw 54
    retlw 56
    retlw 59
    retlw 61
    retlw 64
    retlw 67
    retlw 70
    retlw 72
    retlw 75
    retlw 78
    retlw 81
    retlw 84
    retlw 87
    retlw 90
    retlw 93
    retlw 96
    retlw 99
    retlw 102
    retlw 105
    retlw 108
    retlw 111
    retlw 115
    retlw 118
    retlw 121
    retlw 124
    
psect bootldr,class=CODE,delta=2,size=0x100
_bootloader:
    ; Bootloader routine for quick batch programming (one-way)
    ; Takes binary Intel HEX lines - 0x20 bytes per line (16 words)
    ;   Addresses must be aligned to 0x20
    ;   Less than 0x20 bytes = last bytes are erased
    ; Wait 2-5 ms after sending the last byte of each row before checksum!
    ; Wait the same amount of time after sending command 0x00 too!
    ;
    ; Memory allocation:
    ; - 0x0070: Data length in bytes
    ; - 0x0071: Temporary storage space, command type
    ; - 0x0072: Temporary storage space
    ; - 0x0073: Ignore write if set
    
    ; Disable WDT
    movlb 14
    bcf 0x11, 0
    ; Set clock frequency to 32MHz
    movlb 1
    movlw 0xF8 ; SPLLEN, IRCF = 16 MHz, SCS = 0
    movwf 0x19
    ; Disable DAC
    movlb 2
    clrf 0x18
    ; Disable interrupts
    movlb 0
    bcf 0x0B, 7
    bsf 0x0C, 0
    clrf 0x70
    clrf 0x71
    clrf 0x72
    clrf 0x73
    
bootloader_start:
    ; wait for next clock
    movlb 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; set data length
    movf 0x0C, 0
    andlw 0x30
    movwf 0x70
    lslf 0x70
    lslf 0x70
    movf 0x0E, 0
    iorwf 0x70
    
    ; wait for next clock
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; read upper address
    movf 0x0C, 0
    andlw 0x30
    movwf 0x72
    lslf 0x72
    lslf 0x72
    movf 0x0E, 0
    iorwf 0x72
    
    ; wait for next clock
    movlb 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; read lower address
    movf 0x0C, 0
    andlw 0x30
    movlb 3
    movwf 0x71
    lslf 0x71
    lslf 0x71
    movlb 0
    movf 0x0E, 0
    movlb 3
    iorwf 0x71
    
    ; write address to registers
    bcf 0x03, 0 ; clear carry
    rrf 0x72
    rrf 0x71
    movlb 3
    movf 0x72, 0
    movwf 0x12
    movf 0x71, 0
    movwf 0x11
    
    ; wait for next clock
    movlb 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; read command type
    movf 0x0C, 0
    andlw 0x30
    movwf 0x71
    lslf 0x71
    lslf 0x71
    movf 0x0E, 0
    iorwf 0x71
    
    ; execute command
    btfsc 0x03, 2 ; 00
    goto bootloader_write
    decfsz 0x71   ; 01
    bra 1
    goto bootloader_end
    decfsz 0x71   ; 02
    bra 1
    goto bootloader_ignore
    decfsz 0x71   ; 03
    bra 1
    goto bootloader_ignore
    decfsz 0x71   ; 04
    goto err ; all other types - error
    goto bootloader_mode
    
bootloader_write:
    ; ignore config writes
    btfsc 0x73, 0
    goto bootloader_ignore
    ;movlw 1
    ;xorwf 0x0C
    ; erase row
    movlb 3
    movlw 0x14 ; FREE, WREN
    movwf 0x15 ; PMCON1
    movlw 0x55
    movwf 0x16
    movlw 0xAA
    movwf 0x16
    bsf 0x15, 1 ; execute erase
    nop ; 2-5ms pause
    nop
    movlw 0x24 ; LWLO, WREN
    movwf 0x15 ; PMCON1
    
bootloader_write_loop:
    ; wait for next clock
    movlb 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; read low byte
    movf 0x0C, 0
    andlw 0x30
    movwf 0x71
    lslf 0x71
    lslf 0x71
    movf 0x0E, 0
    iorwf 0x71, 0
    movlb 3
    movwf 0x13
    decf 0x70
    
    ; wait for next clock
    movlb 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; read high 6 bits
    movf 0x0E, 0
    movlb 3
    movwf 0x14
    decfsz 0x70 ; check if no more bytes to read
    bra 1
    goto bootloader_write_finish
    
    ; write to latch
    movlw 0x55
    movwf 0x16
    movlw 0xAA
    movwf 0x16
    bsf 0x15, 1 ; execute write
    nop
    nop
    incf 0x11 ; increment address
    goto bootloader_write_loop
    
bootloader_write_finish:
    bcf 0x15, 5 ; clear LWLO
    movlw 0x55
    movwf 0x16
    movlw 0xAA
    movwf 0x16
    bsf 0x15, 1 ; execute write
    nop ; 2-5ms pause
    nop
    bcf 0x15, 2
    goto bootloader_checksum
    
bootloader_end:
    reset
    
bootloader_mode:
    ; can't set config bits from software, so ignore attempts to write to it
    ; ignore high byte
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    ; set ignore if != 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    clrf 0x73
    movf 0x0E
    btfss 0x03, 2
    bsf 0x73, 0
    goto bootloader_checksum
    
bootloader_ignore:
    ; loop until all bytes are consumed
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    decfsz 0x70
    goto bootloader_ignore ; if > 0
    goto bootloader_checksum ; if = 0
    
bootloader_checksum:
    ; ignore for now
    movlb 0
    btfsc 0x0C, 1 ; loop while bit 1 is set
    bra -2
    btfss 0x0C, 1 ; loop while bit 1 is not set
    bra -2
    goto bootloader_start
    
err:
    ; Disable DAC
    movlb 2
    clrf 0x18
    movlb 0
err2:
    ; square wave of unknown frequency
    bsf 0x0C, 0
    decfsz 0x72
    bra -2
    decfsz 0x73
    bra -4
    bcf 0x0C, 0
    decfsz 0x72
    bra -2
    decfsz 0x73
    bra -4
    goto err2
    
psect reset_vec,class=CODE,delta=2
    PAGESEL _main
    goto _main
psect end_init,class=CODE,delta=2
psect powerup,class=CODE,delta=2
psect cinit,class=CODE,delta=2
psect functab
    
end _main
