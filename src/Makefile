CXX = g++
CXXFLAGS = -g --std=c++20
CXXFLAGS += -MMD -MP #의존성 작성
LDFLAGS =
LDLIBS = -lpthread -lnghttp2 -lssl -lcrypto
OBJS = $(patsubst %.cc, %.o, $(wildcard *.cc)) # 현재 디렉토리의 *.o 파일들
DEPS = $(OBJS:.o=.d)
TARGET = h2server

.PHONY : all
all : $(TARGET)

$(TARGET) : $(OBJS) 
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o : %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $< 

-include $(DEPS)

.PHONY : clean debug
clean :
	rm -rf $(OBJS) $(DEPS) $(TARGET)

debug :
	@echo $(OBJS)

