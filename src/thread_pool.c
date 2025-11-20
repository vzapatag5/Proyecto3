/* =============================================================
 * thread_pool.c - Pool simple de hilos
 * -------------------------------------------------------------
 * Objetivo: ejecutar muchas tareas (función + argumento) usando
 * un conjunto fijo de hilos reutilizables en lugar de crear
 * y destruir hilos para cada trabajo.
 * Flujo básico:
 *   tp_create(n)  -> crea n hilos que esperan tareas.
 *   tp_submit(fn,arg) -> mete tarea en cola y despierta un hilo.
 *   tp_wait()     -> bloquea hasta que cola vacía y nada ejecutándose.
 *   tp_destroy()  -> señala "stop", une hilos y libera memoria.
 * Cola: FIFO sencilla. Se reubica con memmove (óptimo suficiente
 * para cargas pequeñas/medianas). Para más rendimiento podría usarse
 * índice circular.
 * ============================================================= */
#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* TPTask: representa una tarea pendiente (función + argumento). */
typedef struct {
    tp_work_fn fn;
    void* arg;
} TPTask;

/* ThreadPool: estado interno del pool.
 * - threads: arreglo de hilos trabajadores.
 * - queue/q_count/q_cap: cola dinámica de tareas pendientes.
 * - active: cuántas tareas se están ejecutando ahora.
 * - stop: bandera para terminar el bucle de cada hilo.
 * - mtx + condiciones: sincronización para acceso a la cola y espera.
 */
struct ThreadPool {
    pthread_t* threads;
    size_t nthreads;

    TPTask* queue;
    size_t q_cap;
    size_t q_count;

    int stop;
    size_t active;

    pthread_mutex_t mtx;
    pthread_cond_t  cv_has_work;
    pthread_cond_t  cv_done;
};

/* tp_worker_main: función que corre en cada hilo.
 * Espera trabajo; al recibirlo lo saca (FIFO), suelta el candado, ejecuta,
 * y al terminar actualiza 'active'. Si ya no queda nada (cola vacía y active=0)
 * avisa a quienes estén esperando en tp_wait().
 */
static void* tp_worker_main(void* arg) {
    ThreadPool* tp = (ThreadPool*)arg;
    for (;;) {
        pthread_mutex_lock(&tp->mtx);
        while (!tp->stop && tp->q_count == 0) {
            pthread_cond_wait(&tp->cv_has_work, &tp->mtx);
        }
        if (tp->stop && tp->q_count == 0) {
            pthread_mutex_unlock(&tp->mtx);
            break; /* salir del bucle */
        }
        TPTask task = tp->queue[0];
        memmove(tp->queue, tp->queue + 1, (tp->q_count - 1) * sizeof(TPTask));
        tp->q_count--;
        tp->active++;
        pthread_mutex_unlock(&tp->mtx);

        if (task.fn) task.fn(task.arg);

        pthread_mutex_lock(&tp->mtx);
        tp->active--;
        if (tp->q_count == 0 && tp->active == 0) {
            pthread_cond_broadcast(&tp->cv_done);
        }
        pthread_mutex_unlock(&tp->mtx);
    }
    return NULL;
}

/* tp_create: reserva estructuras y lanza 'nthreads' hilos trabajadores. */
ThreadPool* tp_create(size_t nthreads) {
    if (nthreads == 0) nthreads = 1;

    ThreadPool* tp = (ThreadPool*)calloc(1, sizeof(ThreadPool));
    if (!tp) return NULL;

    tp->nthreads = nthreads;
    tp->q_cap = 16;
    tp->queue = (TPTask*)calloc(tp->q_cap, sizeof(TPTask));
    if (!tp->queue) {
        free(tp);
        return NULL;
    }

    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->cv_has_work, NULL);
    pthread_cond_init(&tp->cv_done, NULL);

    tp->threads = (pthread_t*)calloc(nthreads, sizeof(pthread_t));
    if (!tp->threads) {
        free(tp->queue);
        pthread_mutex_destroy(&tp->mtx);
        pthread_cond_destroy(&tp->cv_has_work);
        pthread_cond_destroy(&tp->cv_done);
        free(tp);
        return NULL;
    }

    for (size_t i = 0; i < nthreads; ++i) {
        pthread_create(&tp->threads[i], NULL, tp_worker_main, tp);
    }

    return tp;
}

/* tp_submit: agrega una tarea (fn,arg) a la cola y despierta un hilo. */
int tp_submit(ThreadPool* tp, tp_work_fn fn, void* arg) {
    if (!tp || !fn) return -1;

    pthread_mutex_lock(&tp->mtx);

    if (tp->stop) {
        pthread_mutex_unlock(&tp->mtx);
        return -1;
    }

    if (tp->q_count == tp->q_cap) {
        size_t new_cap = tp->q_cap * 2;
        TPTask* nq = (TPTask*)realloc(tp->queue, new_cap * sizeof(TPTask));
        if (!nq) {
            pthread_mutex_unlock(&tp->mtx);
            return -1;
        }
        tp->queue = nq;
        tp->q_cap = new_cap;
    }

    tp->queue[tp->q_count].fn  = fn;
    tp->queue[tp->q_count].arg = arg;
    tp->q_count++;

    pthread_cond_signal(&tp->cv_has_work);
    pthread_mutex_unlock(&tp->mtx);
    return 0;
}

/* tp_wait: bloquea hasta que no queden tareas pendientes ni en ejecución. */
void tp_wait(ThreadPool* tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mtx);
    while (tp->q_count > 0 || tp->active > 0) {
        pthread_cond_wait(&tp->cv_done, &tp->mtx);
    }
    pthread_mutex_unlock(&tp->mtx);
}

/* tp_destroy: señala parada, une hilos y libera toda la memoria. */
void tp_destroy(ThreadPool* tp) {
    if (!tp) return;

    pthread_mutex_lock(&tp->mtx);
    tp->stop = 1;
    pthread_cond_broadcast(&tp->cv_has_work);
    pthread_mutex_unlock(&tp->mtx);

    for (size_t i = 0; i < tp->nthreads; ++i) {
        pthread_join(tp->threads[i], NULL);
    }

    free(tp->threads);
    free(tp->queue);
    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->cv_has_work);
    pthread_cond_destroy(&tp->cv_done);
    free(tp);
}
