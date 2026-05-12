cmake: clean
	mkdir -p build && cd build && cmake  -DCMAKE_BUILD_TYPE=Debug ..

clean:
	rm -fr build

test:
	./build/server_test

debug:
	gdb -d ${PWD} ${PWD}/build/server_test