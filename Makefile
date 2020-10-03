all:
	g++ vtviewer.cpp -o vtview -O3 -lavutil -lavformat -lavcodec -lswresample -lswscale -ltermbox -lm -lpthread -lportaudio -D__STDC_CONSTANT_MACROS -g
clean:
	rm vtview
