
default:: bytecount

clean::
	rm -f bytecount

bytecount: bytecount.c
	gcc -Wall -g $? -o $@
