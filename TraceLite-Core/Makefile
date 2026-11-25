# Makefile de TraceLite-Core.
#
# Salidas:
#   lib/libtracelite.a  biblioteca estatica de instrumentacion
#   bin/collector       proceso Collector Lite
#
# Las fuentes se descubren con wildcard. La biblioteca estatica y el Collector
# Lite estan implementados. El emisor de pruebas y el test de biblioteca se
# compilan como ejecutables separados porque tienen su propio main().

CC ?= cc
AR ?= ar

CFLAGS   ?= -O2 -g
override CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic
override CPPFLAGS += -Iinclude -I../third_party/cjson -D_POSIX_C_SOURCE=200809L

BUILD_DIR := build
LIB_DIR   := lib
BIN_DIR   := bin
LIB         := $(LIB_DIR)/libtracelite.a
COLLECTOR   := $(BIN_DIR)/collector
TEST_SENDER := $(BIN_DIR)/collector_test_sender
TEST_LIB    := $(BIN_DIR)/test_library

LIB_SRCS        := $(wildcard src/lib/*.c)
COLLECTOR_SRCS  := $(filter-out src/collector/collector_test_sender.c,$(wildcard src/collector/*.c))
TEST_SENDER_SRC := src/collector/collector_test_sender.c
TEST_LIB_SRC    := tests/test_library.c

LIB_OBJS         := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
COLLECTOR_OBJS   := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COLLECTOR_SRCS))
TEST_SENDER_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(TEST_SENDER_SRC))
TEST_LIB_OBJS    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_LIB_SRC))
CJSON_OBJ        := $(BUILD_DIR)/third_party/cjson.o

DEPS := $(LIB_OBJS:.o=.d) $(COLLECTOR_OBJS:.o=.d) \
        $(TEST_SENDER_OBJS:.o=.d) $(TEST_LIB_OBJS:.o=.d) $(CJSON_OBJ:.o=.d)

.PHONY: all lib collector test-sender test-collector test-library check \
        clean distclean help

all: lib collector

ifeq ($(strip $(LIB_SRCS)),)
lib:
	@echo "TraceLite-Core: biblioteca pendiente; se implementara en una etapa posterior."
else
lib: $(LIB)
endif

ifeq ($(strip $(COLLECTOR_SRCS)),)
collector:
	@echo "TraceLite-Core: collector pendiente; no hay fuentes en src/collector."
else
collector: $(COLLECTOR)
endif

test-sender: $(TEST_SENDER)

test-collector: $(COLLECTOR) $(TEST_SENDER)
	@$(SHELL) scripts/test_collector.sh

test-library: $(TEST_LIB)
	@$(TEST_LIB)

ifneq ($(strip $(LIB_SRCS)),)
$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
endif

ifneq ($(strip $(COLLECTOR_SRCS)),)
$(COLLECTOR): $(COLLECTOR_OBJS) $(CJSON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(COLLECTOR_OBJS) $(CJSON_OBJ) $(LDLIBS)
endif

ifneq ($(wildcard $(TEST_SENDER_SRC)),)
$(TEST_SENDER): $(TEST_SENDER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(TEST_SENDER_OBJS) $(LDLIBS)
endif

$(TEST_LIB): $(TEST_LIB_OBJS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(TEST_LIB_OBJS) $(LIB) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(CJSON_OBJ): ../third_party/cjson/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

check:
	@set -eu; \
	for header in include/tracelite/*.h; do \
		[ -e "$$header" ] || continue; \
		name=$${header#include/}; \
		printf '#include <%s>\nint tl_header_check;\n' "$$name" | \
			$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only -x c -; \
	done; \
	echo "TraceLite-Core: encabezados OK"

clean:
	rm -rf $(BUILD_DIR) $(LIB) $(COLLECTOR) $(TEST_SENDER) $(TEST_LIB)

distclean: clean
	rm -rf $(LIB_DIR) $(BIN_DIR)

help:
	@echo "TraceLite-Core - objetivos disponibles"
	@echo "  make                Construye libtracelite.a y collector"
	@echo "  make lib            Construye solo lib/libtracelite.a"
	@echo "  make collector      Construye solo bin/collector"
	@echo "  make test-sender    Construye bin/collector_test_sender"
	@echo "  make test-collector Construye y ejecuta la prueba del Collector"
	@echo "  make test-library   Construye y ejecuta la prueba de libtracelite.a"
	@echo "  make check          Valida los encabezados publicos"
	@echo "  make clean          Elimina build/ y los ejecutables generados"
	@echo "  make distclean      Elimina tambien lib/ y bin/"

-include $(DEPS)
