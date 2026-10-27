# Host-side unit tests for MorseLink's pure-C modules.
#   make -C test/host && ./test/host/run_tests [-v|-vv]
#
# Compiles the device's own source files (main/*.c that don't touch ESP-IDF)
# with the PC's compiler, together with the test programs.

SRC_DIR := ../../main
CFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -D_DEFAULT_SOURCE -I$(SRC_DIR)
SRCS    := test_main.c test_link.c test_phone.c test_ttest.c \
           $(SRC_DIR)/morse.c $(SRC_DIR)/hseq.c $(SRC_DIR)/calib.c $(SRC_DIR)/decoder.c \
           $(SRC_DIR)/proto.c $(SRC_DIR)/link.c $(SRC_DIR)/convo.c $(SRC_DIR)/disc.c \
           $(SRC_DIR)/phone.c $(SRC_DIR)/ttest.c $(SRC_DIR)/bench.c

run_tests: $(SRCS) $(wildcard $(SRC_DIR)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -f run_tests

.PHONY: clean
