all:
	g++ -std=c++11 -o ge2dplayer main.cpp -L/usr/lib/aml_libs/ -lamcodec -lamadec -lamavutils -lasound -lpthread

clean:
	rm ge2dplayer

