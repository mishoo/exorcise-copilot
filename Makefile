all: exorcise-copilot

exorcise-copilot: copilot.cpp
	g++ -I /usr/include/libevdev-1.0/ -l evdev -o $@ copilot.cpp

clean:
	@rm -v exorcise-copilot
