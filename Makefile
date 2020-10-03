all:
	g++ vtviewer.cpp -o vtview -O2 -lavutil -lavformat -lavcodec -lswresample -lswscale -ltermbox -lm -lpthread -lportaudio -D__STDC_CONSTANT_MACROS
clean:
	rm vtview
