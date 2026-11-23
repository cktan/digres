# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# ty -- the operations of the SQL types, for sm and digres to call.
#
#   make -j4         build build/libty.a and the test binary
#   make build/libty-release.a   the library, optimized: what digres's
#                    release build links
#   make test        build, and run the tests in test/
#   make tsan        the tests under the thread sanitizer
#   make clean       remove build/
#   make format      clang-format src/ and test/
#
# The library is build/libty.a, from src/ and include/. It is built
# from nothing outside this directory. Setting DEBUG=1 in the
# environment builds -O0 -g, as sm's levels do; otherwise -O3
# -DNDEBUG. The flags are recorded in build/flags, so changing them
# rebuilds everything. include/ty.h and ty_error.h are aligned by
# hand and are not formatted.

ifeq ($(DEBUG),1)
OPTFLAGS := -O0 -g
else
OPTFLAGS := -O3 -DNDEBUG
endif

CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -Isrc -MMD -MP
# doctest, for the tests (../ext/doctest).
CXXFLAGS += -I../ext/doctest
LDLIBS   += -lpthread

BUILD := build

# The compiler and flags every object is built with. Rewriting the
# file when they change makes it newer than the objects, so each one
# is rebuilt below.
FLAGS      := $(CXX) $(CXXFLAGS)
FLAGS_FILE := $(BUILD)/flags
$(shell mkdir -p $(BUILD); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS)' ] || \
        echo '$(FLAGS)' > $(FLAGS_FILE))

LIB_SRCS  := $(wildcard src/*.cc)
LIB_OBJS  := $(LIB_SRCS:src/%.cc=$(BUILD)/src/%.o)
# PostgreSQL's float printers (src/ryu/, which has its own notice), compiled
# as C++ with -DFRONTEND, which makes them include their own shim.
RYU_SRCS  := $(wildcard src/ryu/*.c)
RYU_OBJS  := $(RYU_SRCS:src/%.c=$(BUILD)/src/%.o)
RYU_FLAGS := -w -DFRONTEND -Isrc/ryu
LIB_OBJS  += $(RYU_OBJS)
TEST_SRCS := $(wildcard test/*.cc)
TEST_OBJS := $(TEST_SRCS:test/%.cc=$(BUILD)/test/%.o)

LIB      := $(BUILD)/libty.a
TEST_BIN := $(BUILD)/test_ty

.PHONY: all test tsan format clean

all: $(LIB) $(TEST_BIN)

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDLIBS)

$(BUILD)/src/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/src/ryu/%.o: src/ryu/%.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RYU_FLAGS) -x c++ -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	$(TEST_BIN) -E

# The library again, optimized, its objects apart in build/rel: what
# digres's release build links, as it links sq's.
REL_OBJS     := $(LIB_OBJS:$(BUILD)/%=$(BUILD)/rel/%)
REL_CXXFLAGS := -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -Isrc -MMD -MP \
                -O2 -DNDEBUG

$(BUILD)/libty-release.a: $(REL_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/rel/src/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(REL_CXXFLAGS) -c -o $@ $<

$(BUILD)/rel/src/ryu/%.o: src/ryu/%.c
	@mkdir -p $(dir $@)
	$(CXX) $(REL_CXXFLAGS) $(RYU_FLAGS) -x c++ -c -o $@ $<

# The tests again, with the library built into them under the thread
# sanitizer. setarch -R stops the kernel's randomisation from moving
# the sanitizer's shadow memory.
$(BUILD)/test_ty_tsan: $(LIB_SRCS) $(RYU_SRCS) $(TEST_SRCS)
	$(CXX) -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -Isrc \
	    -I../ext/doctest -O1 -g -w \
	    $(RYU_FLAGS) -x c++ -fsanitize=thread -o $@ $^ $(LDLIBS)

tsan: $(BUILD)/test_ty_tsan
	setarch $$(uname -m) -R $(BUILD)/test_ty_tsan -E

clean:
	$(RM) -r $(BUILD)

format:
	clang-format -i src/*.cc src/*.h test/*.cc include/*.h

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(REL_OBJS:.o=.d)
