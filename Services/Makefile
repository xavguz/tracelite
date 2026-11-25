# Makefile de Services.
#
# Salidas:
#   bin/process_manager
#   bin/order_service
#   bin/inventory_service
#
# Los tres ejecutables usan los encabezados publicos y libtracelite.a de
# TraceLite-Core. Si la biblioteca no existe, se construye invocando al
# Makefile del core.
#
# Las fuentes se descubren con wildcard. El objetivo all construye los tres
# ejecutables.

CC ?= cc

CFLAGS   ?= -O2 -g
override CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic
override CPPFLAGS += -Iinclude -I../TraceLite-Core/include -I../third_party/cjson -D_POSIX_C_SOURCE=200809L

BUILD_DIR := build
BIN_DIR   := bin

CORE_DIR := ../TraceLite-Core
CORE_LIB := $(CORE_DIR)/lib/libtracelite.a

# libtracelite.a es obligatoria desde la Etapa 3. La regla permite usar
# 'make -C Services' sin construir antes el core manualmente.
CORE_LINK := $(CORE_LIB)

PROGRAMS     := process_manager order_service inventory_service
PROGRAM_BINS := $(addprefix $(BIN_DIR)/,$(PROGRAMS))

COMMON_SRCS := $(wildcard src/common/*.c)
COMMON_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
CJSON_OBJ := $(BUILD_DIR)/third_party/cjson.o

process_manager_OBJS   := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(wildcard src/process_manager/*.c))
order_service_OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(wildcard src/order_service/*.c))
inventory_service_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(wildcard src/inventory_service/*.c))

SERVICE_OBJS := $(process_manager_OBJS) $(order_service_OBJS) $(inventory_service_OBJS)
DEPS         := $(COMMON_OBJS:.o=.d) $(SERVICE_OBJS:.o=.d) $(CJSON_OBJ:.o=.d)

.PHONY: all run run-integrated check clean distclean help

ifeq ($(strip $(SERVICE_OBJS)),)
all:
	@echo "Services: pendiente; no hay fuentes en src/ (Etapa 1)."
else
all: $(PROGRAM_BINS)
endif

run: all
	./bin/process_manager

run-integrated: all
	./bin/process_manager --integrated

$(BIN_DIR)/process_manager: $(process_manager_OBJS) $(COMMON_OBJS) $(CJSON_OBJ) $(CORE_LINK) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $(process_manager_OBJS) $(COMMON_OBJS) $(CJSON_OBJ) $(CORE_LINK) $(LDLIBS)

$(BIN_DIR)/order_service: $(order_service_OBJS) $(COMMON_OBJS) $(CJSON_OBJ) $(CORE_LINK) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $(order_service_OBJS) $(COMMON_OBJS) $(CJSON_OBJ) $(CORE_LINK) $(LDLIBS)

$(BIN_DIR)/inventory_service: $(inventory_service_OBJS) $(COMMON_OBJS) $(CJSON_OBJ) $(CORE_LINK) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $(inventory_service_OBJS) $(COMMON_OBJS) $(CJSON_OBJ) $(CORE_LINK) $(LDLIBS)

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR) lib

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(CJSON_OBJ): ../third_party/cjson/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR):
	@mkdir -p $@

check:
	@set -eu; \
	for header in include/*.h; do \
		[ -e "$$header" ] || continue; \
		name=$${header#include/}; \
		printf '#include <%s>\nint tl_header_check;\n' "$$name" | \
			$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only -x c -; \
	done; \
	echo "Services: encabezados OK"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

distclean: clean

help:
	@echo "Services - objetivos disponibles"
	@echo "  make              Construye los tres ejecutables"
	@echo "  make run          Ejecuta process_manager independiente"
	@echo "  make run-integrated  Ejecuta el modo legado con Collector hijo"
	@echo "  make check        Valida los encabezados de Services"
	@echo "  make clean        Elimina build/ y bin/"
	@echo "  make distclean    Equivalente a clean"
	@echo ""
	@echo "libtracelite.a se construye automaticamente si falta."

-include $(DEPS)
