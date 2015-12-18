
default:: bytecount bytelog bytelog2 test smoother

clean::
	rm -f bytecount bytelog
	make -C `pwd`/test clean

test::
	make -C `pwd`/test 

bytecount: bytecount.c
	gcc -Wall -g $? -o $@

bytelog: bytelog.c
	gcc -Wall -g $? -o $@

bytelog2: bytelog2.c
	gcc -Wall -g $? -o $@

smoother: smoother.c
	gcc -Wall -g $? -o $@


