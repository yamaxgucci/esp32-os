REM Run 1 of 4: the base point.  jcm800 at 4x, the whole 46 ms cabinet.
REM This is the setting my own numbers were taken on, so it is the one
REM everything else is compared against.  Play two or three minutes.
REM The transmitter at 2 dBm first: at 19.5 it costs the analogue input
REM 20 dB of noise floor, and that is heard as the amplifier being dirty.
REM The stored ap.txpower is not applied when the point starts - a bug of
REM its own - so every run that uses the radio sets it here.
wifi power 2
run c:\STOMP.AXE jcm800 os 4 cabms 46 cab c:\CAB.WAV inmix out pcm0
