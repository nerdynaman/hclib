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
#define daemonN 1
pthread_key_t selfKey;
pthread_once_t selfKeyInitialized = PTHREAD_ONCE_INIT;

hclib_worker_state* workers;
int * worker_id;
int nb_workers;
int not_done;
static double user_specified_timer = 0;
static double benchmark_start_time_stats = 0;
double kernel_energy, likwid_jpi = 0;
int shutdown = 0;
int *sleepCounterArr;
pthread_cond_t *condArr;
pthread_mutex_t *mutexArr;

double mysecond() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + ((double) tv.tv_usec / 1000000);
}

// One global finish scope

void configure_DOP(double JPI_prev, double JPI_curr, int *threadsSleptArr)
{
    double delta_JPI = JPI_curr - JPI_prev;
    if (delta_JPI > 0.0)
    {
        // sleep daemonN number of threads
        for (int i = 0; i < daemonN; i++)
        {
            int randomIndex = (rand() % (nb_workers-1)) + 1;
            if (threadsSleptArr[randomIndex] == -1)
            {
                // pthread_mutex_lock(&mutexArr[randomIndex]);
                sleepCounterArr[randomIndex] = 1;
                threadsSleptArr[randomIndex] = 1;
            }
            else
            {
                i--;
            }
        }
    }
    else if (delta_JPI < 0.0)
    {
        // wake up daemonN number of threads
        for (int i = 0; i < daemonN; i++)
        {
            int randomIndex = (rand() % (nb_workers-1)) + 1;
            if (threadsSleptArr[randomIndex] == 1)
            {
                sleepCounterArr[randomIndex] = 0;
                threadsSleptArr[randomIndex] = -1;
                pthread_cond_signal(&condArr[randomIndex]);
                // pthread_mutex_unlock(&mutexArr[randomIndex]);
            }
            else
            {
                i--;
            }
        }
    }
}


void daemon_profiler()
{                                  // a dedicated pthread (not part of HClib work-stealing)
    const int fixed_interval = 20; // some value that you find experimentally
    double JPI_prev = 0;           // JPI is Joules per Instructions Retired
    int *threadsSleptArr = (int *)malloc(sizeof(int) * nb_workers);
    for (int i = 0; i < nb_workers; i++)
    {
        threadsSleptArr[i] = -1;
    }
    sleep(0.02);                     // warmup duration
    while (shutdown != 1)
    {
        likwid_read();                  // supported in hclib-light
        double JPI_curr = likwid_JPI(); // supported in hclib-light
        configure_DOP(JPI_prev, JPI_curr, threadsSleptArr);
        JPI_prev = JPI_curr;
        sleep(0.01);
    }
}

static void initializeKey() {
    pthread_key_create(&selfKey, NULL);
}

void set_current_worker(int wid) {
    pthread_setspecific(selfKey, &workers[wid].id);
}

int hclib_current_worker() {
    return *((int *) pthread_getspecific(selfKey));
}

int hclib_num_workers() {
    return nb_workers;
}

//FWD declaration for pthread_create
void * worker_routine(void * args);

void setup() {
    // Build queues
    not_done = 1;
    pthread_once(&selfKeyInitialized, initializeKey);
    workers = (hclib_worker_state*) malloc(sizeof(hclib_worker_state) * nb_workers);
    for(int i=0; i<nb_workers; i++) {
      workers[i].deque = malloc(sizeof(deque_t));
      void * val = NULL;
      dequeInit(workers[i].deque, val);
      workers[i].current_finish = NULL;
      workers[i].id = i;
    }
    sleepCounterArr = (int *) malloc(sizeof(int) * nb_workers);
    for (int i = 0; i < nb_workers; i++)
    {
        sleepCounterArr[i] = 0;
    }
    condArr = (pthread_cond_t *) malloc(sizeof(pthread_cond_t) * nb_workers);
    for (int i = 0; i < nb_workers; i++)
    {
        pthread_cond_init(&condArr[i], NULL);
    }
    mutexArr = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t) * nb_workers);
    for (int i = 0; i < nb_workers; i++)
    {
        pthread_mutex_init(&mutexArr[i], NULL);
    }
    // Start workers
    for(int i=1;i<nb_workers;i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_create(&workers[i].tid, &attr, &worker_routine, &workers[i].id);
    }
    set_current_worker(0);
    // allocate root finish
    start_finish();
}

void check_in_finish(finish_t * finish) {
    if(finish) hc_atomic_inc(&(finish->counter));
}

void check_out_finish(finish_t * finish) {
    if(finish) hc_atomic_dec(&(finish->counter));
}

void hclib_init(int argc, char **argv) {
    printf("---------HCLIB_RUNTIME_INFO-----------\n");
    printf(">>> HCLIB_WORKERS\t= %s\n", getenv("HCLIB_WORKERS"));
    printf("----------------------------------------\n");
    nb_workers = (getenv("HCLIB_WORKERS") != NULL) ? atoi(getenv("HCLIB_WORKERS")) : 1;
    setup();
    benchmark_start_time_stats = mysecond();
    likwid_init();
}

void execute_task(task_t * task) {
    finish_t* current_finish = task->current_finish;
    int wid = hclib_current_worker();
    hclib_worker_state* ws = &workers[wid];
    ws->current_finish = current_finish;
    task->_fp((void *)task->args);
    check_out_finish(current_finish);
    free(task);
}

void spawn(task_t * task) {
    // get current worker
    int wid = hclib_current_worker();
    hclib_worker_state* ws = &workers[wid];
    check_in_finish(ws->current_finish);
    task->current_finish = ws->current_finish;
    // push on worker deq
    dequePush(ws->deque, task);
    ws->total_push++;
}

void hclib_async(generic_frame_ptr fct_ptr, void * arg) {
    task_t * task = malloc(sizeof(*task));
    *task = (task_t){
        ._fp = fct_ptr,
        .args = arg,
    };
    spawn(task);
}

void slave_worker_finishHelper_routine(finish_t* finish) {
   int wid = hclib_current_worker();
   while(finish->counter > 0) {
        if (sleepCounterArr[wid] > 0)
        {
            pthread_cond_wait(&condArr[wid], &mutexArr[wid]);
        }
       task_t* task = dequePop(workers[wid].deque);
       if (!task) {
           // try to steal
           int i = 1;
           while(finish->counter > 0 && i < nb_workers) {
               task = dequeSteal(workers[(wid+i)%(nb_workers)].deque);
	       if(task) {
		   workers[wid].total_steals++;	   
	           break;
	       }
	       i++;
	   }
        }
        if(task) {
            execute_task(task);
        }
    }
}

void start_finish() {
    int wid = hclib_current_worker();
    hclib_worker_state* ws = &workers[wid];
    finish_t * finish = (finish_t*) malloc(sizeof(finish_t));
    finish->parent = ws->current_finish;
    check_in_finish(finish->parent);
    ws->current_finish = finish;
    finish->counter = 0;
}

void end_finish(){ 
    int wid = hclib_current_worker();
    hclib_worker_state* ws = &workers[wid];
    finish_t* current_finish = ws->current_finish;
    if (current_finish->counter > 0) {
        slave_worker_finishHelper_routine(current_finish);
    }
    assert(current_finish->counter == 0);
    check_out_finish(current_finish->parent); // NULL check in check_out_finish
    ws->current_finish = current_finish->parent;
    free(current_finish);
}

void hclib_finalize() {
    end_finish();
    likwid_finalize();
    not_done = 0;
    int i;
    int tpush=workers[0].total_push, tsteals=workers[0].total_steals;
    for(i=1;i< nb_workers; i++) {
        pthread_join(workers[i].tid, NULL);
    tpush+=workers[i].total_push;
    tsteals+=workers[i].total_steals;
    }
    double duration = (mysecond() - benchmark_start_time_stats) * 1000;
    printf("============================ Tabulate Statistics ============================\n");
    printf("time.kernel\ttotalAsync\ttotalSteals\tEnergy(Joules)\tnetJPI\n");
    printf("%.3f\t%d\t%d\t%.4f\t%.10f\n",user_specified_timer,tpush,tsteals,kernel_energy,likwid_jpi);
    printf("=============================================================================\n");
    printf("===== Total Time in %.f msec =====\n", duration);
    printf("===== Test PASSED in 0.0 msec =====\n");
}

void hclib_kernel(generic_frame_ptr fct_ptr, void * arg) {
    likwid_start();
    likwid_read();
    // start daemon profiler thread
    pthread_t profiler;
    pthread_create(&profiler, NULL, (void *)daemon_profiler, NULL);
    double startEnergy = likwid_energy();
    double start = mysecond();
    fct_ptr(arg);
    user_specified_timer = (mysecond() - start)*1000;
    likwid_read();
    kernel_energy = likwid_energy() - startEnergy;
    likwid_jpi = likwid_JPI();
    likwid_stop();
    shutdown = 1;
    pthread_join(profiler, NULL);
    for (int i = 0; i < nb_workers; i++)
    {
        if (sleepCounterArr[i] != 0)
        {
            pthread_cond_signal(&condArr[i]);
            // pthread_mutex_unlock(&mutexArr[i]);
            // printf("Signaling thread %d\n", i);
        }
        sleepCounterArr[i] = 0;
    }
    // printf("Kernel Energy: %.4f\n", kernel_energy);
}

void hclib_finish(generic_frame_ptr fct_ptr, void * arg) {
    start_finish();
    fct_ptr(arg);
    end_finish();
}

void* worker_routine(void * args) {
    int wid = *((int *) args);
   set_current_worker(wid);
   while(not_done) {
        // pthread_mutex_lock(&mutexArr[wid]);
        if (sleepCounterArr[wid] > 0)
        {
            pthread_cond_wait(&condArr[wid], &mutexArr[wid]);
        }
        // pthread_mutex_unlock(&mutexArr[wid]);
       task_t* task = dequePop(workers[wid].deque);
       if (!task) {
           // try to steal
           int i = 1;
           while (i < nb_workers) {
               task = dequeSteal(workers[(wid+i)%(nb_workers)].deque);
	       if(task) {
                   workers[wid].total_steals++;
                   break;
               }
	       i++;
	   }
        }
        if(task) {
            execute_task(task);
        }
    }
    return NULL;
}
