
all: httpd

clean:
	rm -f httpd

httpd: main.cc
	g++ -g -o httpd main.cc -std=c++11 -I /opt/homebrew/Cellar/curl/7.82.0/include -L /opt/homebrew/Cellar/curl/7.82.0/lib -lcurl

