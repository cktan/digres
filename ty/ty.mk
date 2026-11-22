# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# ty.mk -- where ty keeps its header and its library, for the Makefiles
# that call ty or link a library that does (sm, aa and digres). Include
# it by its path from the including Makefile:
#
#   include ../../ty/ty.mk
#
# The library is built by ty/Makefile, which says whether it is up to
# date: DEBUG=1 builds -O0, as the other parts do.

TY := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TY_INC := $(TY)/include
TY_LIB := $(TY)/build/libty.a

# The rule that hands the library to ty/Makefile, in every Makefile but
# that one. It keeps the including Makefile's default goal.
ifneq ($(abspath $(CURDIR)),$(TY))
ty_goal := $(.DEFAULT_GOAL)
.PHONY: $(TY_LIB)
$(TY_LIB):
	$(MAKE) -C $(TY) build/libty.a
.DEFAULT_GOAL := $(ty_goal)
endif
