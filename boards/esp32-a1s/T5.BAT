REM T2 word for word, with the radio off - the control for the stalls.
REM On T2 the chain fits (5278 us of a 5804 us block) and 13% of blocks
REM still miss their deadline, the worst by 31 ms.  So something stops the
REM thread rather than overloading it.  The browser console goes with the
REM radio; the amplifier plays on and the wire still reads it.
wifi off
run c:\STOMP.AXE jcm800 os 2 blk 128 cabms 46 cab c:\CAB.WAV inmix out pcm0
