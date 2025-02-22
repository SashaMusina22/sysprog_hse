GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all:
	gcc $(GCC_FLAGS) libcoro.c corobus.c test.c ../utils/unit.c \
		-I ../utils -o test

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test
