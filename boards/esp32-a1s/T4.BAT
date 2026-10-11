REM Run 4 of 4: run 1 again with the radio switched off, which is the
REM whole question.  The browser console dies with the radio - that is
REM expected; the amplifier keeps playing and the wire still reads it.
REM Start this one and play; nothing else is needed.
wifi off
run c:\STOMP.AXE jcm800 os 4 cabms 46 cab c:\CAB.WAV inmix out pcm0
