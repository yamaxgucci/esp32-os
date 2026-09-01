rem The RGB LED, and why two lines of shell rather than a driver.
rem
rem It is one WS2812 on GPIO 8: a serial protocol at 800 kHz, not three pins
rem with a level each.  It cannot be bit-banged through io->gpio_write - a zero
rem bit is 400 ns of high and the ABI call alone is longer than that - so there
rem is no driver for it yet, and GPIO 8 is a strapping pin besides, sampled at
rem reset.  Left alone the part latches whatever the strapping clocked past it
rem and sits there at full brightness, which is unpleasant on a desk.
rem
rem Parking the pin as a low output stops anything more being clocked in.  It
rem does not clear what is already latched - only losing power does - so this is
rem "off after the next power cycle, and off every boot from then on".  A real
rem WS2812.SYS over RMT is the fix, and it needs an RMT primitive in the port.
io 8 out
io 8 0
