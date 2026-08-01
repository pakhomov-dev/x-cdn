CXX = g++
CXXFLAGS = -std=c++20 -pthread -Iinclude
LDFLAGS = -luring

TARGET = xcdn
SRCS = src/main.cpp src/xcdn_server.cpp src/xcdn_cache.cpp src/xcdn_http.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
