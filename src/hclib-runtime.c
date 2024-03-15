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

pthread_key_t selfKey;
pthread_once_t selfKeyInitialized = PTHREAD_ONCE_INIT;

hclib_worker_state *workers;
int *worker_id;
int nb_workers;
int not_done;
static double user_specified_timer = 0;
static double benchmark_start_time_stats = 0;
int tracing_enabled = 0;
int replay_enabled = 0;
workerState *workerStateArr; //array which will store the state of each worker

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
    // initialize worker state array
    workerStateArr = (workerState *)malloc(sizeof(workerState) * nb_workers);

    for (int i = 0; i < nb_workers; i++)
    {
        workers[i].deque = malloc(sizeof(deque_t));
        void *val = NULL;
        dequeInit(workers[i].deque, val);
        workers[i].current_finish = NULL;
        workers[i].id = i;
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
    nb_workers = (getenv("HCLIB_WORKERS") != NULL) ? atoi(getenv("HCLIB_WORKERS")) : 2;
    setup();
    benchmark_start_time_stats = mysecond();
}

void execute_task(task_t *task)
{
    // printf("Executing task execexec %d\n", task->id);
    finish_t *current_finish = task->current_finish;
    int wid = hclib_current_worker();
    hclib_worker_state *ws = &workers[wid];
    ws->current_finish = current_finish;
    task->_fp((void *)task->args);
    check_out_finish(current_finish);
    // printf("Task %d executed by worker execexec%d\n", task->id, wid);
    // free(task);
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
    ws->total_push++;
}

void hclib_async(generic_frame_ptr fct_ptr, void *arg)
{
    task_t *task = malloc(sizeof(*task));
    *task = (task_t){
        ._fp = fct_ptr,
        .args = arg,
        // adding a unique id to the task whenever async(task) is created
        .id = workerStateArr[hclib_current_worker()].asynCounter,
    };

    // printf("new task created with id as %d by worker %d with existing asynCounter as %d\n", task->id, hclib_current_worker(), workerStateArr[hclib_current_worker()].asynCounter);
    workerStateArr[hclib_current_worker()].asynCounter+=1;
    if (replay_enabled)
    {
        // send to theif if was earlier stolen
        // check if the task was stolen
        stolenTaskList *stlCurr = workerStateArr[hclib_current_worker()].stl;
        // printf("New async task %d is being created\n", task->id);
        while (stlCurr != NULL)
        {
            // printf("Checking against %d\n", stlCurr->task->taskID);
            if (stlCurr->task->taskID == task->id)
            {
                // printf("Task %d was stolen and now will be executed by worker %d\n", task->id, stlCurr->task->workExecutor);
                // send to theif
                int wid = hclib_current_worker();
                hclib_worker_state* ws = &workers[wid];
                check_in_finish(ws->current_finish);
                task->current_finish = ws->current_finish;
                workerStateArr[stlCurr->task->workExecutor].stolenTasks[stlCurr->task->stealCounter] = *task;
                return;
            }
            stlCurr = stlCurr->next;
        }
    }
    spawn(task);
}

void slave_worker_finishHelper_routine(finish_t *finish)
{
    int wid = hclib_current_worker();
    while (finish->counter > 0 )
    {
        task_t *task = dequePop(workers[wid].deque);
        if (!task && !replay_enabled)
        {
            // try to steal
            int i = 1;
            while (finish->counter > 0 && i < nb_workers)
            {
                task = dequeSteal(workers[(wid + i) % (nb_workers)].deque);
                if (task)
                {
                    // printf("Worker %d stole task %d from worker %d\n", wid, task->id, (wid + i) % (nb_workers));
                    workers[wid].total_steals++;
                    // now we have a stealed task, we need to add this to the list of stolen tasks
                    stolenTask *st = (stolenTask *)malloc(sizeof(stolenTask));
                    st->workCreator = (wid + i) % (nb_workers);
                    st->workExecutor = wid;
                    st->stealCounter = workerStateArr[wid].stealCounter;
                    st->taskID = task->id;
                    workerStateArr[wid].stealCounter+=1; //updating state of current worker after stealing
                    
                    stolenTaskList *stl = (stolenTaskList *)malloc(sizeof(stolenTaskList));
                    stl->task = st;
                    stl->next = workerStateArr[wid].stl;
                    workerStateArr[wid].stl = stl;
                    break;
                }
                i++;
            }
        }
        else if (!task && replay_enabled)
        {
            // tasks will be send by some other worker to our list otherwise we will keep on waiting
            // while (!task)
            // while (workerStateArr[wid].stealCounter < workerStateArr[wid].tempCounter && workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id != -1)
            while (finish->counter > 0)
            {
                sleep(0.001);
                // printf("Task1 not found with current steal counter %d woker %d id %d\n", workerStateArr[wid].stealCounter, wid, workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id);
                if (workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id != -1)
                {
                    // sleep(0.01);
                    // printf("Task1 found with current steal counter %d worker %d\n", workerStateArr[wid].stealCounter, wid);
                    task = &workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter];    
                    workerStateArr[wid].stealCounter+=1;
                    break;
                }
            }
            // finish_t *current_finish = task->current_finish;
            // hclib_worker_state* ws = &workers[wid];
            // ws->current_finish = current_finish;
            // printf("before executing task1\n");
            // task->_fp((void *)task->args);
            // printf("after executing task1\n");
            // check_out_finish(current_finish);
            // printf("Task1 executed by worker %d\n", wid);
            // check_out_finish(task->current_finish);
            // free(task);
            // continue;
        }

        if (task != NULL && task->id != -1)
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

void *worker_routine(void *args)
{
    int wid = *((int *)args);
    set_current_worker(wid);
    while (not_done)
    {
        task_t *task = dequePop(workers[wid].deque);
        if (!task && tracing_enabled && !replay_enabled)
        {
            // try to steal
            int i = 1;
            while (i < nb_workers)
            {
                task = dequeSteal(workers[(wid + i) % (nb_workers)].deque);
                if (task)
                {
                    // printf("Worker %d stole task %d from worker %d\n", wid, task->id, (wid + i) % (nb_workers));
                    workers[wid].total_steals++;
                    // now we have a stealed task, we need to add this to the list of stolen tasks
                    stolenTask *st = (stolenTask *)malloc(sizeof(stolenTask));
                    st->workCreator = (wid + i) % (nb_workers);
                    st->workExecutor = wid;
                    st->stealCounter = workerStateArr[wid].stealCounter;
                    st->taskID = task->id;
                    workerStateArr[wid].stealCounter++; //updating state of current worker after stealing

                    stolenTaskList *stl = (stolenTaskList *)malloc(sizeof(stolenTaskList));
                    stl->task = st;
                    stl->next = workerStateArr[wid].stl;
                    workerStateArr[wid].stl = stl;

                    break;
                }
                i++;
            }
        }
        else if (!task && replay_enabled)
        {
            // tasks will be send by some other worker to our list otherwise we will keep on waiting
            // printf("Task not found with current steal counter %d woker %d\n", workerStateArr[wid].stealCounter, wid);
            // printf("id of the task at current steal counter is %d\n", workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter]->id);
            // printf("entry\n");
            while (not_done)
            {
                sleep(0.001);
                // printf("Task2 not found with current steal counter %d woker %d id %d\n", workerStateArr[wid].stealCounter, wid, workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id);
                if (workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id != -1)
                {
                    // printf("Task2 found with current steal counter %d worker %d\n", workerStateArr[wid].stealCounter, wid);
                    task = &workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter];    
                    workerStateArr[wid].stealCounter+=1;
                    break;
                }
            }
            // finish_t *current_finish = task->current_finish;
            // hclib_worker_state* ws = &workers[wid];
            // ws->current_finish = current_finish;
            // printf("before executing task2\n");
            // task->_fp((void *)task->args);
            // printf("after executing task2\n");
            // check_out_finish(current_finish);
            // printf("Task2 executed by worker %d\n", wid);
            // continue;
        }
        if (task != NULL && task->id != -1){
            execute_task(task);
        }
    }
    return NULL;
}


void hclib_start_tracing()
{
    printf("Tracing enabled\n");
    // for (int i = 0; i < nb_workers; i++)
    // {
    //     printf("Worker %d's stolen task list\n", i);
    //     stolenTaskList *stlCurr = workerStateArr[i].stl;
    //     while (stlCurr != NULL)
    //     {
    //         printf("Task %d was stolen by worker %d from worker %d\n", stlCurr->task->taskID, stlCurr->task->workExecutor, stlCurr->task->workCreator);
    //         stlCurr = stlCurr->next;
    //     }
    // }
    for (int i = 0; i < nb_workers; i++)
    {
        workerStateArr[i].asynCounter = i * (UINT16_MAX / nb_workers);
        workerStateArr[i].stealCounter = 0;
    }
    tracing_enabled = 1;
}
void hclib_stop_tracing()
{
    if (replay_enabled)
    {
        // clear out stolenTasks list
        for (int i = 0; i < nb_workers; i++)
        {
            // workerStateArr[i].tempCounter = workerStateArr[i].stealCounter;
            workerStateArr[i].stealCounter = 0;
            for (int j = 0; j < workerStateArr[i].tempCounter+1; j++)
            {
                workerStateArr[i].stolenTasks[j] = (task_t){.id = -1};
            }
        }
    }
    if (!replay_enabled)
    {
        // First print entire details of stolen task list of each worker
        // for (int i = 0; i < nb_workers; i++)
        // {
        //     printf("Worker %d's stolen task list\n", i);
        //     stolenTaskList *stlCurr = workerStateArr[i].stl;
        //     while (stlCurr != NULL)
        //     {
        //         printf("Task %d was stolen by worker %d from worker %d\n", stlCurr->task->taskID, stlCurr->task->workExecutor, stlCurr->task->workCreator);
        //         stlCurr = stlCurr->next;
        //     }
        // }
        // aggregate all the stolen tasks for each worker
        // look for the tasks with creator as wid and put them in the stolen list of wid worker while deleting from j
        for (int i = 0; i < nb_workers; i++)
        {
            int wid = i;
            for (int j=0 ; j < nb_workers; j++)
            {
                if (i != j) // do not look into your own list
                {
                    stolenTaskList *stlCurr = workerStateArr[j].stl;
                    stolenTaskList *stlPrev = NULL;
                    stolenTaskList *tmp;
                    while (stlCurr != NULL)
                    {
                        if (stlCurr->task->workCreator == wid)
                        {
                            tmp = stlCurr;
                            // add this task to the worker's list
                            stlCurr = stlCurr->next;
                            if (stlPrev != NULL)
                            {
                                stlPrev->next = stlCurr;
                            }
                            else
                            {
                                workerStateArr[j].stl = stlCurr;
                            }
                            tmp->next = workerStateArr[wid].stl;
                            workerStateArr[wid].stl = tmp;
                            tmp = stlCurr;
                        }
                        else
                        {
                            stlPrev = stlCurr;
                            stlCurr = stlCurr->next;
                        }
                    }
                    
            }
        }
    }

        // // Now sort the stolen list of each worker based on the task id
        // for (int i = 0; i < nb_workers; i++)
        // {
        //     stolenTaskList *stlCurr = workerStateArr[i].stl;
        //     stolenTaskList *stlPrev = NULL;
        //     while (stlCurr != NULL)
        //     {
        //         stolenTaskList *stlNext = stlCurr->next;
        //         while (stlNext != NULL)
        //         {
        //             if (stlCurr->task->taskID > stlNext->task->taskID)
        //             {
        //                 stlPrev->next = stlNext;
        //                 stlCurr->next = stlNext->next;
        //                 stlNext->next = stlCurr;
        //             }
        //             stlNext = stlNext->next;
        //         }
        //         stlCurr = stlCurr->next;
        //     }
        // }

        // Now creating arrays for each worker to store the stolen tasks
        for (int i = 0; i < nb_workers; i++)
        {
            workerStateArr[i].tempCounter = workerStateArr[i].stealCounter;
            workerStateArr[i].stolenTasks = (task_t *)malloc(sizeof(task_t) * (workerStateArr[i].stealCounter+1));
            for (int j = 0; j < workerStateArr[i].stealCounter+1; j++)
            {
                // workerStateArr[i].stolenTasks[j]= malloc(sizeof(task_t));
                workerStateArr[i].stolenTasks[j] = (task_t) { .id = -1 };
            }
            workerStateArr[i].stealCounter = 0;
        } 
        replay_enabled = 1;

    }
}