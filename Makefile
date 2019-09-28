all:
	g++ vtviewer.cpp -o vtview -lavutil -lavformat -lavcodec -lswresample -lswscale -ltermbox -lao -lm -lpthread -D__STDC_CONSTANT_MACROS
clean:
	rm vtview
