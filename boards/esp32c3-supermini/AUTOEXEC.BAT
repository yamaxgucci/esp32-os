rem Nothing to do here any more.
rem
rem Binding the wire used to be a line in this file, with a note saying it
rem belonged in the kernel.  It does now: [link] bind in SYSTEM.CFG, done
rem before this file is read, so a board that is half of a cluster comes up
rem as half of a cluster - after a reset, after a power cut, and in every
rem test that resets a board.
