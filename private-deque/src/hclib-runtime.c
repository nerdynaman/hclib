/*
 * Copyright 2017 Rice University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hclib-internal.h"
#include <unistd.h>
pthread_key_t selfKey;
pthread_once_t selfKeyInitialized = PTHREAD_ONCE_INIT;

hclib_worker_state *workers;
int *worker_id;
int nb_workers;
int not_done;
static double user_specified_timer = 0;
static double benchmark_start_time_stats = 0;
#define MAILBOX_SIZE 8096
double mysecond()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + ((double)tv.tv_usec / 1000000);
}

// One global finish scope

static void initializeKey()
{
    pthread_key_create(&selfKey, NULL);
}

void set_current_worker(int wid)
{
    pthread_setspecific(selfKey, &workers[wid].id);
}

int hclib_current_worker()
{
    return *((int *)pthread_getspecific(selfKey));
}

int hclib_num_workers()
{
    return nb_workers;
}

// FWD declaration for pthread_create
void *worker_routine(void *args);

void setup()
{
    // Build queues
    not_done = 1;
    pthread_once(&selfKeyInitialized, initializeKey);
    workers = (hclib_worker_state *)malloc(sizeof(hclib_worker_state) * nb_workers);
    for (int i = 0; i < nb_workers; i++)
    {
        workers[i].deque = malloc(sizeof(deque_t));
        void *val = NULL;
        dequeInit(workers[i].deque, val);
        workers[i].current_finish = NULL;
        workers[i].id = i;
        workers[i].total_push = 0;
        workers[i].total_steals = 0;
        workers[i].thief_id = -1;
        workers[i].mailbox = malloc(sizeof(deque_t));
        dequeInit(workers[i].mailbox, val);
        workers[i].mailbox_size = 0;
    }
    // Start workers
    for (int i = 1; i < nb_workers; i++)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_create(&workers[i].tid, &attr, &worker_routine, &workers[i].id);
    }
    set_current_worker(0);
    // allocate root finish
    start_finish();
}

void check_in_finish(finish_t *finish)
{
    if (finish)
        hc_atomic_inc(&(finish->counter));
}

void check_out_finish(finish_t *finish)
{
    if (finish)
        hc_atomic_dec(&(finish->counter));
}

void hclib_init(int argc, char **argv)
{
    printf("---------HCLIB_RUNTIME_INFO-----------\n");
    printf(">>> HCLIB_WORKERS\t= %s\n", getenv("HCLIB_WORKERS"));
    printf("----------------------------------------\n");
    nb_workers =(getenv("HCLIB_WORKERS") != NULL) ? atoi(getenv("HCLIB_WORKERS")) : 1;
    setup();
    benchmark_start_time_stats = mysecond();
}

void execute_task(task_t *task)
{
    finish_t *current_finish = task->current_finish;
    int wid = hclib_current_worker();
    hclib_worker_state *ws = &workers[wid];
    ws->current_finish = current_finish;
    task->_fp((void *)task->args);
    check_out_finish(current_finish);
    free(task);
}

void checkStealRequest()
{
    // while push/pop/steal check if any thief has requested for a task
    // if yes, then put the task in their mailbox and signal them
    hclib_worker_state* ws = &workers[hclib_current_worker()];
    int thief_id = ws->thief_id;
    if (thief_id != -1)
    {
        // printf("request recieved Thief id: %d\n", thief_id);
        // some thief has requested for a task, if I have a task then put it in the mailbox otherwise signal the thief
        // task is supposed to be taken from the head of the deque
        // hc_mfence();
        void* task = NULL;
        if (ws->deque->tail - ws->deque->head > 0)
        {
            // printf("Task found in deque\n");
            task =(void*) ws->deque->data[ws->deque->head % INIT_DEQUE_CAPACITY];
            ws->deque->head = ws->deque->head + 1;
        }
    // hc_mfence();
        if (task)
        {
            // put the task in the mailbox
            dequePush(workers[thief_id].mailbox, task);
            // signal the thief
            workers[thief_id].mailbox_size =  workers[thief_id].mailbox_size + 1;
            workers[hclib_current_worker()].thief_id = -1;
            // printf("Signalling thief SUCESS %d\n",workers[thief_id].mailbox_size);
            // hc_signal(&workers[thief_id].mailbox_signal);
        }
        else
        {
            // signal the thief
            workers[thief_id].mailbox_size =  workers[thief_id].mailbox_size + 1;
            workers[hclib_current_worker()].thief_id = -1;
            // printf("Signalling thief FAILURE\n");
        }
    hc_mfence();
    }
}

void spawn(task_t *task)
{
    // get current worker
    int wid = hclib_current_worker();
    hclib_worker_state *ws = &workers[wid];
    check_in_finish(ws->current_finish);
    task->current_finish = ws->current_finish;
    // push on worker deq
    dequePush(ws->deque, task);
    // printf("Pushed task on worker deque\n");
    checkStealRequest();
    ws->total_push++;
}

void hclib_async(generic_frame_ptr fct_ptr, void *arg)
{
    task_t *task = malloc(sizeof(*task));
    *task = (task_t){
        ._fp = fct_ptr,
        .args = arg,
    };
    spawn(task);
}

void slave_worker_finishHelper_routine(finish_t *finish)
{
    int wid = hclib_current_worker();
    while (finish->counter > 0)
    {
        checkStealRequest(&workers[wid], workers[wid].deque);
        task_t *task = dequePop(workers[wid].deque);
        if (!task)
        {   
            // try to steal
            int i = 1;
            while (finish->counter > 0 && i < nb_workers)
            {
                if (workers[(wid + i) % (nb_workers)].deque->tail - workers[(wid + i) % (nb_workers)].deque->head > 0)
                {
                    // printf("Stealing from worker %d\n", (wid + i) % (nb_workers));
                }
                else
                {
                    i++;
                    continue;
                }
                int stealStatus = dequeStealMailbox(&workers[(wid + i) % (nb_workers)], wid);
                // task = dequeSteal(workers[(wid + i) % (nb_workers)].deque);
                hc_mfence();
                if (stealStatus)
                {
                    // printf("Steal status: %d\n", stealStatus);
                    // now wait for the victim to put the task in the mailbox
                    // when the victim puts the task in the mailbox, it will send a signal
                    while (1)
                    {
                        if (workers[wid].mailbox_size == 1)
                        {
                            break;
                        }
                        // wait for the victim to put the task in the mailbox
                        // sleep(0.01);
                        // printf("Waiting for victim to put task in mailbox\n");
                    }
                    task = dequePop(workers[wid].mailbox);
                    if (!task)
                    {
                        // printf("Task not found in mailbox\n");
                        i++;
                        workers[wid].mailbox_size = 0;
                        continue;
                    }
                    // printf("Task found in mailbox\n");
                    workers[wid].mailbox_size=0;
                    // now transfer rest of the task from the victim's mailbox to
                    workers[wid].total_steals+=1;
                    // printf("Steal success\n");
                    break;
                }
                hc_mfence();
                i++;
            }
        }
        if (task)
        {
            execute_task(task);
        }
    }
}

void start_finish()
{
    int wid = hclib_current_worker();
    hclib_worker_state *ws = &workers[wid];
    finish_t *finish = (finish_t *)malloc(sizeof(finish_t));
    finish->parent = ws->current_finish;
    check_in_finish(finish->parent);
    ws->current_finish = finish;
    finish->counter = 0;
}

void end_finish()
{
    int wid = hclib_current_worker();
    hclib_worker_state *ws = &workers[wid];
    finish_t *current_finish = ws->current_finish;
    if (current_finish->counter > 0)
    {
        slave_worker_finishHelper_routine(current_finish);
    }
    assert(current_finish->counter == 0);
    check_out_finish(current_finish->parent); // NULL check in check_out_finish
    ws->current_finish = current_finish->parent;
    free(current_finish);
}

void hclib_finalize()
{
    end_finish();
    not_done = 0;
    int i;
    int tpush = workers[0].total_push, tsteals = workers[0].total_steals;
    for (i = 1; i < nb_workers; i++)
    {
        pthread_join(workers[i].tid, NULL);
        tpush += workers[i].total_push;
        tsteals += workers[i].total_steals;
    }
    double duration = (mysecond() - benchmark_start_time_stats) * 1000;
    printf("============================ Tabulate Statistics ============================\n");
    printf("time.kernel\ttotalAsync\ttotalSteals\n");
    printf("%.3f\t%d\t%d\n", user_specified_timer, tpush, tsteals);
    printf("=============================================================================\n");
    printf("===== Total Time in %.f msec =====\n", duration);
    printf("===== Test PASSED in 0.0 msec =====\n");
}

void hclib_kernel(generic_frame_ptr fct_ptr, void *arg)
{
    double start = mysecond();
    fct_ptr(arg);
    user_specified_timer = (mysecond() - start) * 1000;
}

void hclib_finish(generic_frame_ptr fct_ptr, void *arg)
{
    start_finish();
    fct_ptr(arg);
    end_finish();
}

void* worker_routine(void * args) {
    int wid = *((int *) args);
   set_current_worker(wid);
   while(not_done) {
       task_t* task = dequePop(workers[wid].deque);
       if (!task) {
           // try to steal
           int i = 1;
           while (i < nb_workers) {
            // check if workers[(wid+i) % (nb_workers)] has a task in its deque
            if (workers[(wid+i) % (nb_workers)].deque->tail - workers[(wid+i) % (nb_workers)].deque->head > 0) {
                // printf("Stealing from worker %d\n", (wid+i) % (nb_workers));
            }
            else{
                i++;
                continue;
            }
            int stealStatus = dequeStealMailbox(&workers[(wid+i) % (nb_workers)], wid);
            // task = dequeSteal(workers[(wid + i) % (nb_workers)].deque);
            hc_mfence();
            if (stealStatus) {
                // printf("Steal status: %d\n", stealStatus);
                // now wait for the victim to put the task in the mailbox
                // when the victim puts the task in the mailbox, it will send a signal
                while(1) {
                    if (workers[wid].mailbox_size == 1) {
                        break;
                    }
                    // wait for the victim to put the task in the mailbox
                    // sleep(0.01);
                    // printf("Waiting for victim to put task in mailbox\n");
                }
                task = dequePop(workers[wid].mailbox);
                if (!task){
                    // printf("Task not found in mailbox\n");
                    i++;
                    workers[wid].mailbox_size=0;
                    continue;
                }
                workers[wid].mailbox_size=0;
                // now transfer rest of the task from the victim's mailbox to
                workers[wid].total_steals+=1;
                break;
            }
            hc_mfence();
            i++;
	   }

        }
        if(task) {
            execute_task(task);
        }
    }
    return NULL;
}
