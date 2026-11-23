CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude -MMD -MP

SRCS     = src/rw.cpp
OBJS     = $(SRCS:.cpp=.o)
DEPS     = $(SRCS:.cpp=.d)

.PHONY: all clean

all: rw

rw: $(OBJS)
	$(CXX) $(CXXFLAGS) -o rw $(OBJS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) rw