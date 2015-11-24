
default:: bytecount bytelog bytelog2

clean::
	rm -f bytecount bytelog

bytecount: bytecount.c
	gcc -Wall -g $? -o $@

bytelog: bytelog.c
	gcc -Wall -g $? -o $@

bytelog2: bytelog2.c
	gcc -Wall -g $? -o $@
