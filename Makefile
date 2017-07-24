.PHONY: all clean default

default: all

sio: sio.c
	gcc sio.c -lpthread -lrt -o sio

mc_proxy: mc_proxy.c
	gcc mc_proxy.c -o mc_proxy

ipc: ipc.c
	gcc ipc.c -lpthread -lrt -o ipc

all: sio mc_proxy ipc


clean:
	rm -f sio mc_proxy ipc

