;
; PSG.X/rocode.s
; PSG
;
; This file contains the read-only sections of the PIC firmware. It handles the
; bootloader code, as well as initial interrupt code to allow the bootloader to
; be activated even if the main firmware is corrupt. In addition, the sine table
; is stored here to save space. When programmed with code protection on the
; first quarter of flash, the chip will always be able to be reprogrammed with
; the bootloader through the Pico, avoiding the need to rip out the chips if a
; programming error occurs.
;
; This code is licensed under the GPLv2 license.
; Copyright (c) 2022-2023 JackMacWindows.
;
    
PROCESSOR 16LF1613
    
global _interrupt
global _sysCommand
global _main
    
psect intentry,global,class=CODE,delta=2,size=0x28
_ro_interrupt:
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
    
    ; check if the message is 0xFF (system command)
    btfss 0x0C, 5
    goto _interrupt
    btfss 0x0C, 4
    goto _interrupt
    movf 0x0E, 0
    xorlw 0x3F
    btfss 0x03, 2
    goto _interrupt
 
    ; read-only system commands:
    ; 0xFF [0x00]: reset
    ; 0xFF 0x01: enter bootloader
    ; others: jump to rewritable handler
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
    goto _sysCommand ; others
    
psect bootldr,class=CODE,delta=2,size=0x1E0
global _bootloader
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
    
psect sinetbl,class=CODE,delta=2,size=0x101
global _sine_table
_sine_table:
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
    
psect reset_vec,class=CODE,delta=2
    PAGESEL _main
    goto _main
psect end_init,class=CODE,delta=2
psect powerup,class=CODE,delta=2
psect cinit,class=CODE,delta=2
psect functab

end