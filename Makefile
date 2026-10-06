CC = gcc
CFLAGS = -Wall -Wextra -Werror -g -std=c11
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer

CLI = prun
TEST = prun_test
TEST_ASAN = prun_test_asan

all: $(CLI) $(TEST)

$(CLI): prun.c prun.h
	$(CC) $(CFLAGS) prun.c -o $(CLI)

$(TEST): prun.c prun_test.c prun.h
	$(CC) $(CFLAGS) -DTEST_BUILD prun.c prun_test.c -o $(TEST)

$(TEST_ASAN): prun.c prun_test.c prun.h
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -DTEST_BUILD prun.c prun_test.c -o $(TEST_ASAN)

test: $(TEST)
	./$(TEST)

test_asan: $(TEST_ASAN)
	./$(TEST_ASAN)

run_demo: $(CLI)
	./$(CLI) -j 4 sample_tasks.txt

clean:
	rm -f $(CLI) $(TEST) $(TEST_ASAN) *.o

.PHONY: all test test_asan run_demo clean
