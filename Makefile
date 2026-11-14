# Makefile raiz de TraceLite.
#
# Orquesta los dos proyectos independientes:
#   - TraceLite-Core: libtracelite.a y collector
#   - Services:       process_manager, order_service, inventory_service
#
# El core se construye antes que Services, que enlaza libtracelite.a.

SERVICES_DIR := Services
CORE_DIR     := TraceLite-Core

.PHONY: all core services run test test-collector test-library test-tracing \
        check clean distclean help

all: core services

core:
	$(MAKE) -C $(CORE_DIR)

services: core
	$(MAKE) -C $(SERVICES_DIR)

run: all
	./$(SERVICES_DIR)/bin/process_manager

test-collector:
	$(MAKE) -C $(CORE_DIR) test-collector

test-library:
	$(MAKE) -C $(CORE_DIR) test-library

test-tracing: all
	@$(SHELL) scripts/test_tracing.sh

test: test-collector test-library test-tracing

check:
	$(MAKE) -C $(CORE_DIR) check
	$(MAKE) -C $(SERVICES_DIR) check

clean:
	$(MAKE) -C $(CORE_DIR) clean
	$(MAKE) -C $(SERVICES_DIR) clean

distclean:
	$(MAKE) -C $(CORE_DIR) distclean
	$(MAKE) -C $(SERVICES_DIR) distclean

help:
	@echo "TraceLite - objetivos disponibles"
	@echo "  make              Construye TraceLite-Core y Services"
	@echo "  make core         Construye solo TraceLite-Core"
	@echo "  make services     Construye solo Services (usa encabezados del core)"
	@echo "  make run          Compila y ejecuta process_manager"
	@echo "  make test         Ejecuta todas las pruebas (collector, biblioteca, tracing)"
	@echo "  make test-collector  Compila y prueba el Collector Lite"
	@echo "  make test-library    Compila y prueba libtracelite.a"
	@echo "  make test-tracing    Prueba end-to-end con los servicios instrumentados"
	@echo "  make check        Valida los encabezados de ambos proyectos"
	@echo "  make clean        Elimina artefactos de compilacion"
	@echo "  make distclean    Elimina artefactos y directorios de salida"
