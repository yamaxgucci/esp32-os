REM The amplifier at four times oversampling, which it now reaches.
REM
REM   run c:\a.bat        jcm800 at 4x, the whole 46 ms cabinet
REM   run c:\a2.bat       slo at 2x, 5.8 ms a block - the short one
REM
REM Measured at the converter with a phone watching the panel:
REM 22049 frames a second out of 22050.  The gear in the top corner
REM changes both the block and the oversampling while it plays.
run c:\STOMP.AXE jcm800 os 4 cabms 46 cab c:\CAB.WAV inmix out pcm0
