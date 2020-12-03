Catweasel MK4 release 2 fix 2 core (november 27th, 2006)
change: write pulse length can now be set by software. Thanks to Tobias for the quick positive feedback. Since there could be more drives that want even longer write pulses, the length can now be set between 35ns and 1094ns. See the last paragraph of the Inside... file for details, because this core is NOT software-compatible with the previous one. Let's hope you all have the write enable procedure in a single place of your source code, then this change will be quick and easy...

--------------------------------------------------
Catweasel MK4 release 2 fix 1 core (november 26th, 2006)
fix: Made write pulse to floppy drive 105ns longer
fix: syncronized some signals that could have affected metastability
new feature: Auto-correction of Fifo idle cycles counter
----------------------------------------
Catweasel MK4 release 2 core (august 24th, 2006)

After 18 months: A new core! Only change: Pullup resistor enable bits in $d0 of the IO register bank. This will finally make the middle and right mouse button(s) work. Pullup resistors can be enabled separately for each joystick/mouse port. Since Pre29 core has been shipped with hundreds of controllers, it's the typical "beta that became a release". Therefore, I'm not calling this core "pre30", but "release 2". New filename is rel2.cw4 - upload it to the FPGA with your existing routines - no change necessary there. See the Inside_MK4 file for the bit descriptions. Also, check the Register_MK4 file for a precise description of the PCI bridge 8-bit port and some recommendations on how to clean up your source code. Unfortunately, my descriptions in the Inside_MK4 file have cause bad programming style, at least with the Windows driver programmer. The Windows driver is mostly cleaned up now, so switching between the onboard floppy controller and the Catweasel floppy controller is now possible. I'm hoping that this will not cause too much of a hassle for you...

Groepaz has done a great job in debugging the Windows driver in the past months. He also made me write down more documentation - now let's hope we finally get to the point where we can add features.

The rel2.cw4 core is *not* in the current beta driver for windows. I'd like to do some more testing before it'll be compiled into the new Windows driver.

----------------------------------------
Pre-release 29 core

fixed the "wrong PCI subsystem ID" bug on computer re-start without hard-reset. Also.. do the shutdown_notification stuff that I mentioned on the list!


floppy stuff (same as in core 23 readme - Jorge, did you ever try Async read???)
------------
To select the floppy/MK3 register bank, write 0x41 to register base+3.

The $f8 register has one bit that's still experimental, so I haven't moved the description to the other file yet.

bit 7:	ASync Read (experimental!)

If you set bit 7 of floppy register $f8, the read prescaler is not synced to the read pulses. This will hopefully improve (but not fix) the precision problems that Jorge experienced, because one problem can't be solved that easy: The state machine that writes to the memory is still syncronized to the read pulses (has to be, otherwise the data hold time to memory will be violated), so it's not certain how many count pulses are lost during the write-to-memory time. The value to add will depend on the prescale value itself: On "top speed", it'll be constant, and the 28Mhz/14Mhz settings will produce fractions to add to the read values in order to average out the statistical error.
This bit will switch on the 15-bit counter in future cores, so don't spend too much time playing with it.


Jens
