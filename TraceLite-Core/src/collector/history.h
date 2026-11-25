#ifndef TRACELITE_HISTORY_H
#define TRACELITE_HISTORY_H

#include <stdio.h>

int tl_history_list(const char *path, FILE *stream);
int tl_history_find(const char *path, const char *trace_id, FILE *stream);
int tl_history_anomalies(const char *path, FILE *stream);

#endif /* TRACELITE_HISTORY_H */
