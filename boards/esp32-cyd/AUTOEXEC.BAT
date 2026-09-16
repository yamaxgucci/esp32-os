rem Bind the wire at boot.
rem
rem A board whose SYSTEM.CFG names it in the cluster and loads the wire
rem driver still has to be TOLD to bind, and the binding is lost on every
rem reset - so every test that resets a board loses it, and so does every
rem power cut in the field.  Until the kernel does this for itself, here.
link use node0
