
default:: bytecount bytelog

clean::
	rm -f bytecount bytelog

bytecount: bytecount.c
	gcc -Wall -g $? -o $@

bytelog: bytelog.c
	gcc -Wall -g $? -o $@
