.PHONY: clean cppcheck

CXX=g++

TARGETS= E3DC-Control

CXXFLAGS=-Wall -Wextra -fsanitize=address

all: $(TARGETS)

E3DC-Control: RscpExampleMain.cpp RscpProtocol.o AES.o SocketConnection.o
	$(CXX) $(CXXFLAGS) $^ -o $@

%.o: %.cpp %.h
	$(CXX) $(CXXFLAGS) $< -c -o $@

clean:
	rm -f $(TARGETS) *.o

cppcheck: $(TARGET)
	cppcheck --enable=all --language=c++ *.cpp
