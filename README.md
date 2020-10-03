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
+ The video's height and width should be larger than the terminal's. If the terminal is larger, you get some undefined behaviour.
+ Slight overflow in certain videos, particularly letterboxed ones for some reason.

# Improvements made this year:
+ Changed rendering and audio playback. More work is now done in separate
  threads. In addition to that, I switched to PortAudio, libao caused stuttering
  and weird unwanted effects.
+ By default 256-color mode is selected, if you want to use truecolor give the
  command a third argument. For now it can be whatever, i.e. "./vtview VIDEO
  lol"

# Thanks
+ [https://github.com/leandromoreira/ffmpeg-libav-tutorial](https://github.com/leandromoreira/ffmpeg-libav-tutorial)
+ [Steven Dranger's "How to write a video player in less than 1000 lines"](dranger.com/ffmpeg/)
+ PortAudio as a whole

