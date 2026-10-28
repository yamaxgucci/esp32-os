REM The setting with the most room there is: 2x, a 256-frame block, the whole
REM cabinet.  Measured at 64% of the block where T1 needs 103% of its own.
REM
REM If this one is clean, the artefacts are the margin and the work has to
REM come down.  If this one is dirty too, it is not the timing at all, and
REM everything I have measured today was the wrong question.
wifi power 2
run c:\STOMP.AXE jcm800 os 2 blk 256 cabms 46 cab c:\CAB.WAV inmix out pcm0
