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
CONFIG CP = ON
CONFIG BOREN = ON
CONFIG CLKOUTEN = OFF
    
CONFIG WRT = BOOT
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
    
global _sine_table
    
; memory allocations:
; common memory:
; - 0x070: wave type
; - 0x071: volume
; - 0x072: increment high
; - 0x073: increment low
; - 0x074: position high
; - 0x075: position low
; - 0x076: volume adjustment to center
; - 0x077: low pass alpha
; - 0x078: square duty cycle
; - 0x079: if set on WDT reset, do full reset
; general purpose memory:
; - 0x120: scale operation result
; - 0x121: scale operation input
; - 0x122: filler loop index
; - 0x123-0x124: random buffer
; - 0x125: last output value
; - 0x126: whether to flip LPF product
    
; wave types: none, square, sawtooth up, sawtooth down, triangle, sine, noise, ?
    
; DO NOT MODIFY THIS SECTION
psect roptrs,class=CODE,delta=2,size=4
global _main
_main:
    goto main
global _interrupt
_interrupt:
    goto interrupt
global _sysCommand
_sysCommand:
    goto sysCommand
    
; Read-write code begins here
psect init,class=CODE,delta=2 ; PIC10/12/16
interrupt:
    btfsc 0x0C, 5
    goto setLPF
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
    
setLPF:
    btfss 0x0C, 4
    goto setIncrement
    ; set LPF alpha
    movf 0x0E, 0
    addlw 0x01
    movwf 0x77
    lslf 0x77
    lslf 0x77
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
    ; no extra commands; exit
    goto done
    

main:
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
    clrf 0x76
    movlw 0xFC
    movwf 0x77
    clrf 0x78
    clrf 0x79
    ; Debugging: pre-set wave
    ;movlw 0x06
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
    movlw 1
    movwf 0x24
    ; Enable interrupts
    movlw 0x90 ; GIE, INTE
    movwf 0x0B
    
    ; We need to keep track of the clock cycles of each branch, and tune the
    ; others so that they all take the same amount of time. This will allow us
    ; to use the instruction count as a stable clock for sample timings.
    ; Base clock time: 22 clocks
    ; Clock time for each type: 13 clocks
    ; Clock time for filter + scaling: 93 clocks
    ; Total clock time required: 128 clocks
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
    ; Compare high position with duty cycle: 7 clocks
    movf 0x78, 0
    subwf 0x74, 0
    movf 0x03, 0
    andlw 0x01
    addlw 0xFF ; 0 => 255, 1 => 0 + carry
    movwf 0x21
    ; Filler: 4 clocks
    nop
    nop
    nop
    nop
    ; Jump: 2 clocks
    goto scale_output

sawtooth:
    ; Copy position to output: 2 clocks
    movf 0x74, 0
    movwf 0x21
    ; Filler: 9 clocks
    nop
    nop
    nop
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
    ; Filler: 8 clocks
    nop
    nop
    nop
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
    ; Filler: 6 clocks
    nop
    nop
    nop
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
    ; Filler: 4 clocks
    nop
    nop
    nop
    nop
    ; Jump: 2 clocks
    goto scale_output
    
sine:
    ; Get sine table entry: 8 clocks
    movf 0x74, 0
    call _sine_table ; 6 clocks
    movwf 0x21
    ; Filler: 3 clocks
    nop
    nop
    nop
    ; Jump: 2 clocks
    goto scale_output
    
noise:
    ; Generate random sample: 11 clocks
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
    clrf 0x21
    btfss 0x24, 0
    decf 0x21
    ; Jump: 2 clocks
    goto scale_output
    
skip_noise:
    ; Filler: 5 clocks
    nop
    nop
    nop
    nop
    nop
    ; 5. write out r[0] * volume
    clrf 0x21
    btfss 0x24, 0
    decf 0x21
    ; Jump: 2 clocks
    goto scale_output
    
scale_output:
    ; Multiply unscaled output by low pass: 55 clocks
    ; from http://www.piclist.com/techref/microchip/math/mul/8x8.htm
mult    MACRO
    btfsc   0x03, 0
    addwf   0x20,F
    rrf     0x20,F
    rrf     0x21,F
ENDM

    movf 0x25, 0
    subwf 0x21
    btfsc 0x03, 0 ; check borrow
    goto low_pass_skip
    comf 0x21
    incf 0x21
    movlw 0xFF
    movwf 0x26
    goto low_pass_continue
low_pass_skip:
    nop
    nop
    nop
    nop
    clrf 0x26
low_pass_continue:
    movf 0x77, 0
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
    btfsc 0x21, 7
    incf 0x20
    movf 0x20, 0
    btfsc 0x26, 0
    goto low_pass_sub
    addwf 0x25, 0
    goto low_pass_continue2
low_pass_sub:
    nop
    subwf 0x25, 0
low_pass_continue2:
    movwf 0x25
    movwf 0x21
    
    ; Scale volume + output: 38 clocks
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
    
end _main
