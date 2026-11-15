# Evidencias de ejecución

Este directorio contiene transcripciones **reales** de la compilación y de las
pruebas de TraceLite. No son resultados inventados ni editados.

| Archivo | Comando |
|---|---|
| `build.txt` | `make clean && make` |
| `check.txt` | `make check` |
| `test.txt` | `make test` |
| `run.txt` | `make run` |
| `cleanup.txt` | Comprobación de procesos, sockets y directorios residuales |

Los identificadores, rutas temporales y tiempos varían entre ejecuciones. Las
transcripciones se regeneraron en la validación final del repositorio.

Para reproducirlas:

```sh
make clean
make
make check
make test
make run
```
