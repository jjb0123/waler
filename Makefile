all:
	@mkdir -p build
	cmake -S . -B build
	cmake --build build -j
	cp build/compile_commands.json compile_commands.json
