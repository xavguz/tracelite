# Makefile raiz de TraceLite.
#
# Orquesta los dos proyectos independientes:
#   - TraceLite-Core: libtracelite.a y collector
#   - Services:       process_manager, order_service, inventory_service
#
# El core se construye antes que Services, que enlaza libtracelite.a.

SERVICES_DIR := Services
CORE_DIR     := TraceLite-Core

.PHONY: all core services service tracelite run run-integrated test test-collector test-library test-tracing test-resilience \
        check clean distclean help

all: core services service tracelite

core:
	$(MAKE) -C $(CORE_DIR)

services: core
	$(MAKE) -C $(SERVICES_DIR)

service: services
	ln -sfn $(SERVICES_DIR)/bin/process_manager service

tracelite: core
	ln -sfn $(CORE_DIR)/bin/collector tracelite

run: service
	./service

run-integrated: services
	./$(SERVICES_DIR)/bin/process_manager --integrated

test-collector:
	$(MAKE) -C $(CORE_DIR) test-collector

test-library:
	$(MAKE) -C $(CORE_DIR) test-library

test-tracing: all
	@$(SHELL) scripts/test_tracing.sh

test-resilience: all
	@$(SHELL) scripts/test_resilience.sh

test: test-collector test-library test-tracing test-resilience

check:
	$(MAKE) -C $(CORE_DIR) check
	$(MAKE) -C $(SERVICES_DIR) check

clean:
	$(MAKE) -C $(CORE_DIR) clean
	$(MAKE) -C $(SERVICES_DIR) clean
	rm -f service tracelite

distclean:
	$(MAKE) -C $(CORE_DIR) distclean
	$(MAKE) -C $(SERVICES_DIR) distclean

help:
	@echo "TraceLite - objetivos disponibles"
	@echo "  make              Construye TraceLite-Core y Services"
	@echo "  make core         Construye solo TraceLite-Core"
	@echo "  make services     Construye solo Services (usa encabezados del core)"
	@echo "  make service      Crea ./service independiente"
	@echo "  make tracelite    Crea ./tracelite (Collector independiente)"
	@echo "  make run          Compila y ejecuta ./service"
	@echo "  make run-integrated  Ejecuta el modo compatible con la primera entrega"
	@echo "  make test         Ejecuta todas las pruebas (collector, biblioteca, tracing)"
	@echo "  make test-collector  Compila y prueba el Collector Lite"
	@echo "  make test-library    Compila y prueba libtracelite.a"
	@echo "  make test-tracing    Prueba end-to-end con los servicios instrumentados"
	@echo "  make test-resilience Prueba Collector ausente, historial y JSON invalido"
	@echo "  make check        Valida los encabezados de ambos proyectos"
	@echo "  make clean        Elimina artefactos de compilacion"
	@echo "  make distclean    Elimina artefactos y directorios de salida"
