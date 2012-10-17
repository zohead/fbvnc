fbvnc
=====

patched framebuffer VNC client for embeded system

There is already a framebuffer VNC client for Linux:
http://pocketworkstation.org/fbvnc.html

While this also comes with some bugs, and its size seems not suitable for tiny embeded system.

Then I found this fbvnc project:
http://repo.or.cz/w/fbvnc.git

Very lightweight, sadly still some bugs with this fbvnc.

So, I decide to make this fbvnc project better for embeded system.
These things should be done later:

1. try to fix 16/32 bits color display bug (DONE)
2. add VNC authentication support
3. better mouse event support
...