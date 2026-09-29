REM The amplifier, on the guitar, as the Audio Kit is meant to run it.
REM
REM   run c:\amp.bat        the guitar, live
REM   run c:\take.bat       the recorded take instead, for measuring
REM
REM os 2      two times oversampling.  4x costs 9300 us of an 11609 us block
REM           and does not fit on this chip whatever the cabinet is set to.
REM blk 64    64 frames a block: 2.9 ms instead of 11.6, because latency is
REM           what a guitarist feels.  It must be half of audio.dma_frames in
REM           BOARD.CFG or the converter runs dry between blocks.
REM cabms 23  23 ms of the impulse, which measures within half a decibel of 45
REM           in every octave band - and 507 taps is short enough to convolve
REM           directly, which is where the other 11.6 ms of delay went.
REM
REM All four models fit this at 2x, of 2902 us a block: ts9 about 1500,
REM jcm800 1777, bogner 2175, slo 2551.  Change the model by editing the word
REM after STOMP.AXE.
REM quiet     no periodic status.  Printing one costs longer than a 64-frame
REM           block, and the loop comes back to a clock that has moved: it was
REM           measured as one lost block every two seconds, which is how often
REM           the line was printed.  TAKE.BAT leaves it on, because a run being
REM           measured should say what it is doing.
REM inmix     both input channels added.  The board wires its line input to
REM           MIC2's pins, so a microphone is always in the signal; this is the
REM           attempt at cancelling it without a soldering iron.  Use `in` for
REM           the left channel alone.
run c:\STOMP.AXE jcm800 os 2 blk 64 cabms 23 cab c:\CAB.WAV inmix quiet out pcm0
