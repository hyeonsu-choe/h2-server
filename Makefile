CC = g++
#CXXFLAGS = -g --std=c++11 -I/usr/local/include -L/usr/local/lib
CXXFLAGS = -g --std=c++20
LDFLAGS =
LDLIBS = -lpthread -lnghttp2
OBJECTS = $(patsubst %.cc, %.o, $(wildcard *.cc)) # 현재 디렉토리의 *.o 파일들
TARGET = h2_server

.PHONY : all
all : $(TARGET)

$(TARGET) : $(OBJECTS) 
	$(CC) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

%.o : %.cc 
	$(CC) $(CXXFLAGS) -c -o $@ $< 

.PHONY : clean debug
clean :
	rm -rf $(OBJECTS)
	rm -rf $(TARGET)

debug :
	@echo $(OBJECTS)

