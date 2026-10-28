REM The same amplifier at 256 frames a block - 11.6 ms of convolution delay,
REM about 58 ms round trip once the converter's rings are counted.
REM
REM   run c:\amp256.bat
REM
REM The three scripts differ in one word and nothing else, so what the ear
REM hears between them is the block and not a different chain.  Run one, play,
REM run the next; the sound stops while the slot swaps.
REM
REM Measured on this board, of a 11609 us block: valves 4350, cabinet
REM 3000 - 64% of the time there is.  A shorter block costs more
REM arithmetic for the same second of audio, because the same impulse becomes
REM more partitions of a smaller transform: the cabinet roughly doubles
REM between 256 and 64.  That is the trade being listened to.
REM
REM cabms 46 is the whole impulse, which is the point - at 23 ms it is short
REM enough to convolve directly and the block never mattered.
run c:\STOMP.AXE jcm800 os 2 blk 256 cabms 46 cab c:\CAB.WAV inmix quiet out pcm0
