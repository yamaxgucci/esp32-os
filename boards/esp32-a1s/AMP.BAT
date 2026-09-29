REM The amplifier, on the guitar, as the Audio Kit is meant to run it.
REM
REM   run c:\amp.bat        the guitar, live
REM   run c:\take.bat       the recorded take instead, for measuring
REM
REM os 2      two times oversampling.  4x costs 9300 us of an 11609 us block
REM           and does not fit on this chip whatever the cabinet is set to.
REM cabms 46  the impulse is truncated to 45 ms.  Its length is nearly free -
REM           23 ms costs 2606 us against 2696 for 45 - so keep the long one.
REM inmix     both input channels added.  The board wires its line input to
REM           MIC2's pins, so a microphone is always in the signal; this is the
REM           attempt at cancelling it without a soldering iron.  Use `in` for
REM           the left channel alone.
run c:\STOMP.AXE jcm800 os 2 cabms 46 cab c:\CAB.WAV inmix out pcm0
