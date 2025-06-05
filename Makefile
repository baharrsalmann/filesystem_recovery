CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

all: histext2fs

histext2fs: histext2fs.cpp ext2fs.o ext2fs_print.o
	$(CXX) $(CXXFLAGS) -o histext2fs histext2fs.cpp ext2fs.o ext2fs_print.o

ext2fs.o: ext2fs.cpp ext2fs.h
	$(CXX) $(CXXFLAGS) -c ext2fs.cpp

ext2fs_print.o: ext2fs_print.cpp ext2fs_print.h
	$(CXX) $(CXXFLAGS) -c ext2fs_print.cpp

clean:
	rm -f *.o histext2fs
