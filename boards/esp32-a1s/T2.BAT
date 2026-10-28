REM Run 2 of 4: the same amplifier at a short block.  What a 128-frame
REM block costs - less delay under the fingers, more arithmetic per second.
REM The transmitter at 2 dBm first: at 19.5 it costs the analogue input
REM 20 dB of noise floor, and that is heard as the amplifier being dirty.
REM The stored ap.txpower is not applied when the point starts - a bug of
REM its own - so every run that uses the radio sets it here.
wifi power 2
run c:\STOMP.AXE jcm800 os 2 blk 128 cabms 46 cab c:\CAB.WAV inmix out pcm0
