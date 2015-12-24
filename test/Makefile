default:: generator generator2 generator-clone

clean::
	rm -f generator generator2 generator-clone

generator: generator.c
	gcc -Wall -g $? -o $@

generator2: generator2.c
	gcc -Wall -g $? -o $@

generator-clone: generator-clone.c
	gcc -Wall -g $? -o $@

