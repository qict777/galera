// Copyright (C) 2007 Codership Oy <info@codership.com>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "galera_job_queue.h"

/*
 * @brief release all jobs, which waited for this worker
 *        Will send signal for waiting workers
 *        Must hold queue mutex, when calling this
 *
 * @param: queue the job queue
 * @param: worker worker ending
 */
static void release_my_waiters (
    struct job_queue *queue, struct job_worker *worker
) {
    unsigned short i;
    /* if some job is depending on me, release it to continue */
    for (i=0; i<queue->registered_workers; i++) {
        if (queue->jobs[worker->id].waiters[i]) {
	    gu_debug ("job queue signal for: %d", i);
            assert(JOB_WAIT_FOR_JOB == queue->jobs[i].state);

            gu_cond_signal(&queue->jobs[i].cond);
        }
    }
}

static void init_worker(
    struct job_worker *worker, unsigned short id, unsigned short workers
) {
    unsigned short i;
    worker->ident = IDENT_job_worker;
    worker->state = JOB_VOID;
    worker->ctx   = NULL;
    worker->id    = id;

    for (i=0; i<workers; i++) {
        worker->waiters[i] = 0;
    }

    gu_cond_init(&(worker->cond), NULL);
}

struct job_queue *job_queue_create(
     unsigned short max_workers, job_queue_conflict_fun conflict_test, 
     job_queue_cmp_fun cmp_order
) {
    struct job_queue *queue;
    unsigned short i;

    MAKE_OBJ(queue, job_queue);

    queue->active_workers         = 0;
    queue->registered_workers     = 0;
    queue->max_concurrent_workers = (max_workers < MAX_WORKERS) ?
      max_workers :  MAX_WORKERS;
    queue->conflict_test          = conflict_test;
    queue->job_cmp_order          = cmp_order;

    gu_mutex_init(&queue->mutex, NULL);

    for (i=0; i<MAX_WORKERS; i++) {
        init_worker(&(queue->jobs[i]), i, MAX_WORKERS);
    }

    return queue;
}

int job_queue_destroy(struct job_queue *queue) {

    gu_free(queue);
    return WSDB_OK;
}


struct job_worker *job_queue_new_worker(
    struct job_queue *queue, enum job_type type
) {
    struct job_worker *worker = NULL;
    int i=0;

    CHECK_OBJ(queue, job_queue);
    gu_mutex_lock(&(queue->mutex));

    if (queue->registered_workers == MAX_WORKERS) {
        gu_mutex_unlock(&(queue->mutex));
        gu_warn(
             "job queue full, type: %d, workers: %d", 
             type, queue->registered_workers
        );
        return NULL;
    }
    
    while (i<MAX_WORKERS && !worker) {
        if (queue->jobs[i].state == JOB_VOID) {
            worker = &(queue->jobs[i]);
        }
        i++;
    }
    if (!worker) {
        gu_mutex_unlock(&(queue->mutex));
        gu_warn(
             "no free job queue worker found for type: %d, workers: %d", 
             type, queue->registered_workers
        );

        return NULL;
    }

    queue->registered_workers++;
    worker->state = JOB_IDLE;
    worker->type  = type;
    gu_mutex_unlock(&(queue->mutex));

    gu_debug(
         "new job queue worker, type: %d, id: %d, workers: %d", 
         worker->type, worker->id, queue->registered_workers
    );

    return (worker);
}
void job_queue_remove_worker(
    struct job_queue *queue, struct job_worker *worker
) {
    CHECK_OBJ(queue, job_queue);
    CHECK_OBJ(worker, job_worker);

    gu_mutex_lock(&(queue->mutex));

    /* job can be registered, but never started */
    if (JOB_REGISTERED == worker->state) {
        gu_debug("registered job removed, id: %d", worker->id); 
        worker->state = JOB_IDLE;
        worker->ctx   = NULL;

        /* registered job may have waiters piled up, release them now */
        release_my_waiters(queue, worker);
    }

    assert(JOB_IDLE == worker->state);

    worker->state = JOB_VOID;
    queue->registered_workers--;

    gu_debug("job queue released, workers now: %d", queue->registered_workers);

    gu_mutex_unlock(&(queue->mutex));
}

int job_queue_start_job(
    struct job_queue *queue, struct job_worker *worker, void *ctx
) {
    unsigned short i;
    CHECK_OBJ(queue, job_queue);
    CHECK_OBJ(worker, job_worker);

    if (worker->state == JOB_RUNNING) {
        gu_debug ("job %d  already running", worker->id);
        return WSDB_OK;
    }

    gu_mutex_lock(&(queue->mutex));

    /* cannot start, if max concurrent worker count is reached */
    if (queue->active_workers == queue->max_concurrent_workers) {
        gu_warn ("job queue full for: %d", worker->id);
        worker->state = JOB_WAIT_QUEUE_ENTER;
        gu_cond_wait(&worker->cond, &queue->mutex);
        gu_warn ("job queue released for: %d", worker->id);
    }

    queue->active_workers++;
    worker->ctx   = ctx;

    /* Check against all jobs which have registered or are running.
     * Also waiting jobs are checked against.
     */

    for (i=0; i<queue->registered_workers; i++) {
        if (((queue->jobs[i].state == JOB_RUNNING)           || 
             (queue->jobs[i].state == JOB_WAIT_QUEUE_ENTER)  || 
             (queue->jobs[i].state == JOB_WAIT_FOR_JOB)      || 
             (queue->jobs[i].state == JOB_REGISTERED))       && 
            (queue->jobs[i].id != worker->id)
        ) {
            if (queue->conflict_test(ctx, queue->jobs[i].ctx)) {
                queue->jobs[i].waiters[worker->id] = 1;
                gu_debug ("job %d  waiting for: %d", worker->id, i);
                worker->state = JOB_WAIT_FOR_JOB;
                gu_cond_wait(&queue->jobs[worker->id].cond, &queue->mutex);
                gu_debug ("job queue released: %d", worker->id);
                queue->jobs[i].waiters[worker->id] = 0;
            }
        }
    }
    worker->state = JOB_RUNNING;

    gu_debug("job: %d starting", worker->id);
    gu_mutex_unlock(&(queue->mutex));
    return WSDB_OK;
}

int job_queue_register_job(
    struct job_queue *queue, struct job_worker *worker, void *ctx
) {
    CHECK_OBJ(queue, job_queue);
    CHECK_OBJ(worker, job_worker);

    if (worker->state == JOB_RUNNING) {
        gu_debug ("job %d  already running", worker->id);
        return WSDB_OK;
    }

    worker->ctx   = ctx;
    worker->state = JOB_REGISTERED;

    gu_debug("job: %d registered", worker->id);
    return WSDB_OK;
}

void * job_queue_end_job(struct job_queue *queue, struct job_worker *worker
) {
    unsigned short i;
    int            min_job = -1;
    void*          ctx;

    CHECK_OBJ(queue, job_queue);
    CHECK_OBJ(worker, job_worker);

    //assert(worker == queue->jobs[worker->id]);

    if (worker->state != JOB_RUNNING    &&
        worker->state != JOB_REGISTERED
    ) {
        gu_warn("job queue end, with bad state, id: %d, state: %d", 
                worker->id, worker->state);
        return NULL;
    }

    gu_mutex_lock(&queue->mutex);

    /* release jobs, which waited for me */
    release_my_waiters(queue, worker);

    if (JOB_RUNNING == worker->state) {
        queue->active_workers--;
    }

    ctx                           = queue->jobs[worker->id].ctx;
    queue->jobs[worker->id].state = JOB_IDLE;
    queue->jobs[worker->id].ctx   = NULL;
   
    /* if queue was full, find next in order to get in */
    for (i=0; i<queue->registered_workers; i++) {
        if (queue->jobs[i].state == JOB_WAIT_QUEUE_ENTER) {
            if (min_job > -1) {
                if (queue->job_cmp_order(
                     &queue->jobs[i].ctx, &queue->jobs[min_job].ctx) == -1) {
                      min_job = i;
                }
            } else {
                min_job = i;
            }
        }
    }

    if (min_job > -1) {
        gu_info ("job full queue signal for: %d", min_job);
        gu_cond_signal(&queue->jobs[min_job].cond);
    }

    gu_debug("job: %d complete", worker->id);
    gu_mutex_unlock(&(queue->mutex));

    return ctx;
}
