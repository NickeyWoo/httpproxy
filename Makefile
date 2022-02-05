
all: httpd

clean:
	rm -f httpd

httpd: main.cc
	g++ -g -o httpd main.cc -std=c++11 -I /usr/local/Cellar/curl/7.81.0/include/ -L /usr/local/Cellar/curl/7.81.0/lib/ -lcurl

