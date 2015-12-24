default:: generator generator-clone

clean::
	rm -f generator generator-clone

generator: generator.c
	gcc -Wall -g $? -o $@

generator-clone: generator-clone.c
	gcc -Wall -g $? -o $@

