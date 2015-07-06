
default:: bytecount


bytecount: bytecount.c
	gcc -Wall -g $? -o $@
