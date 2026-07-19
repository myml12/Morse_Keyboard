CXX := clang++
CXXFLAGS := -std=c++17 -O2 -fobjc-arc -Wall -Wextra
FRAMEWORKS := -framework Cocoa -framework AudioUnit -framework AudioToolbox \
	-framework CoreGraphics -framework CoreFoundation
TARGET := morse_simulator
OBJS := keyers.o main.o

.PHONY: all clean run

all: $(TARGET)

keyers.o: keyers.cpp keyers.h
	$(CXX) $(CXXFLAGS) -c keyers.cpp -o $@

main.o: main.mm keyers.h sidetone.hpp decoder.hpp
	$(CXX) $(CXXFLAGS) -c main.mm -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(FRAMEWORKS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)
