# fbvnc #

patched framebuffer VNC client for embeded system like Raspberry Pi

## Introduction ##

There is already a framebuffer VNC client for Linux:

[http://pocketworkstation.org/fbvnc.html](http://pocketworkstation.org/fbvnc.html)

While this also comes with some bugs, and its size seems not suitable for tiny embeded system.

Then I found this fbvnc project:

[http://repo.or.cz/w/fbvnc.git](http://repo.or.cz/w/fbvnc.git)

Very lightweight, some function works fine, sadly still some bugs with this fbvnc.

So, I decide to make this fbvnc project better for embeded system.
These things should be done later:

* try to fix 16/32 bits color display bug (DONE)
* add VNC authentication support
* better mouse event support
* ...

## Notes about framebuffer depth (Raspbery Pi) ##


This fbvnc project assumes framebuffer color depth is 32bit.

If your framebuffer device has some bug for 32bit framebuffer (like Raspberry Pi)
or doesn't support 32bit framebuffer at all, 
then you need to change size of framebuffer depth in fbvnc.c

For Raspbery Pi (display well in 16bit framebuffer), you need to change:

typedef unsigned int fbval_t;

to:

typedef unsigned short fbval_t;