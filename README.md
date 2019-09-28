# termvideo
Proof of concept command line video player written with libav/Ffmpeg and libao.

# Requirements
## Building
+ libao
+ Ffmpeg/libav (codec, format, swscale and swresample)
+ [my fork of termbox](https://github.com/Kimplul/termbox)
+ Linux (for now at least)

## Running
+ A terminal emulator that supports unicode and true color. Xterm seems to perform the best, gnome-terminal and derivatives are a bit slow.

# Building
I've included a simple makefile, just write ```make``` in the same directory and you'll be golden. An executable named ```vtview``` should appear.

# Usage
Type ```./vtview PATH/TO/VIDEO```. Press ```ESC``` or ```Q``` to exit once the video is playing.

# TODO/Flaws/etc.
+ Audio is a bit choppy. Some kind of queuing mechanism should be added but that'd need a somewhat sizeable rewrite. Might do it in the future.
+ The video's height and width should be larger than the terminal's. If the terminal is larger, you get some undefined behaviour.
+ Libao might be muted on some systems, change ```/etc/libao.conf``` to fix. Remove ```muted``` if present, and change default driver if necessary.
+ Probably lots of other stuff, but hey, first time using libav, I'm still hecking proud.

# Thanks
+ [https://github.com/leandromoreira/ffmpeg-libav-tutorial](https://github.com/leandromoreira/ffmpeg-libav-tutorial)
+ [Steven Dranger's "How to write a video player in less than 1000 lines"](dranger.com/ffmpeg/)

