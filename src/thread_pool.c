#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    tp_work_fn fn;
    void* arg;
} TPTask;

struct ThreadPool {
    pthread_t* threads;
    size_t nthreads;

    TPTask* queue;
    size_t q_cap;
    size_t q_count;

    int stop;

    size_t active;  /* tareas en ejecución */

    pthread_mutex_t mtx;
    pthread_cond_t  cv_has_work;
    pthread_cond_t  cv_done;
};

static void* tp_worker_main(void* arg) {
    ThreadPool* tp = (ThreadPool*)arg;

    for (;;) {
        pthread_mutex_lock(&tp->mtx);

        while (!tp->stop && tp->q_count == 0) {
            pthread_cond_wait(&tp->cv_has_work, &tp->mtx);
        }

        if (tp->stop && tp->q_count == 0) {
            pthread_mutex_unlock(&tp->mtx);
            break;
        }

        /* sacar tarea de la cola (FIFO simple) */
        TPTask task = tp->queue[0];
        memmove(tp->queue, tp->queue + 1, (tp->q_count - 1) * sizeof(TPTask));
        tp->q_count--;
        tp->active++;

        pthread_mutex_unlock(&tp->mtx);

        /* ejecutar fuera del candado */
        if (task.fn) {
            task.fn(task.arg);
        }

        pthread_mutex_lock(&tp->mtx);
        tp->active--;
        if (tp->q_count == 0 && tp->active == 0) {
            pthread_cond_broadcast(&tp->cv_done);
        }
        pthread_mutex_unlock(&tp->mtx);
    }
    return NULL;
}

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

void tp_wait(ThreadPool* tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mtx);
    while (tp->q_count > 0 || tp->active > 0) {
        pthread_cond_wait(&tp->cv_done, &tp->mtx);
    }
    pthread_mutex_unlock(&tp->mtx);
}

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
