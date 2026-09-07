CXX = g++
CXXFLAGS = -std=c++20 -pthread -Iinclude -O3 -march=native -flto -DNDEBUG
LDFLAGS = -luring -flto
TLS_OBJS =

ifdef WITH_TLS
CXXFLAGS += -DXCDN_TLS_ENABLED
LDFLAGS += -lssl -lcrypto
TLS_OBJS = src/xcdn_tls.o
endif

TARGET = xcdn
SRCS = src/main.cpp src/xcdn_server.cpp src/xcdn_cache.cpp src/xcdn_http.cpp src/xcdn_config.cpp src/xcdn_metrics.cpp $(TLS_OBJS)
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
