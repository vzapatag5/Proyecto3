#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tp_work_fn)(void *arg);

typedef struct ThreadPool ThreadPool;

/* Crea un pool con nthreads hilos. */
ThreadPool* tp_create(size_t nthreads);

/* Encola una tarea. Devuelve 0 si ok, -1 si error. */
int tp_submit(ThreadPool* tp, tp_work_fn fn, void* arg);

/* Espera a que se terminen TODAS las tareas encoladas. */
void tp_wait(ThreadPool* tp);

/* Apaga el pool y libera memoria. */
void tp_destroy(ThreadPool* tp);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_H */
