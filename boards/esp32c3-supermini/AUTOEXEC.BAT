rem THIS BOARD HAS NO CONSOLE ON THE DESK.
rem
rem It is the middle of the cluster and it sits on a power bank: its USB socket
rem carries power and nothing else, because there are two ports on the PC and
rem three boards.  So the only way to type anything at it is over the network.
rem
rem `telnet on` is the console over TCP, the same shell and the same journal the
rem UART would carry.  It is a login with no login - anyone who can reach port
rem 23 has the shell - which is why it is off by default and why this line is
rem here rather than in the image: a board on somebody else's network should not
rem answer on 23 because we found it convenient once.
rem
rem It needs the radio to have joined something.  `wifi connect <ssid> <pass>`
rem is remembered across reboots, so it is typed once, by hand, over USB, before
rem this board goes onto the power bank.
rem
rem And it is what makes `reboot /download` reachable: that is how this board is
rem handed to the S3-Zero to be flashed, and with no console there would be no
rem way to ask for it.  See docs/user/06-flashing-a-coprocessor.md.
telnet on

rem Binding the wire used to be a line in this file, with a note saying it
rem belonged in the kernel.  It does now: [link] bind in SYSTEM.CFG, done
rem before this file is read, so a board that is half of a cluster comes up
rem as half of a cluster - after a reset, after a power cut, and in every
rem test that resets a board.
