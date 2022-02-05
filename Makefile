
httpproxy: main.cc
	g++ -g -o httpproxy main.cc -std=c++11 -I /usr/local/Cellar/curl/7.81.0/include/ -L /usr/local/Cellar/curl/7.81.0/lib/ -lcurl

