all: project3

project3: project3.cpp
	g++ -std=c++14 -o project3 project3.cpp

clean:
	rm -rf project3
