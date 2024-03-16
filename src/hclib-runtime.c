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
    nb_workers = (getenv("HCLIB_WORKERS") != NULL) ? atoi(getenv("HCLIB_WORKERS")) : 16;
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
    if (!replay_enabled){
        ws->total_push++;
    }
}

void hclib_async(generic_frame_ptr fct_ptr, void *arg)
{
    int wid = hclib_current_worker();
    task_t *task = malloc(sizeof(*task));
    *task = (task_t){
        ._fp = fct_ptr,
        .args = arg,
        // adding a unique id to the task whenever async(task) is created
        .id = workerStateArr[wid].asynCounter,
    };

    // printf("new task created with id as %d by worker %d with existing asynCounter as %d\n", task->id, hclib_current_worker(), workerStateArr[hclib_current_worker()].asynCounter);
    workerStateArr[wid].asynCounter+=1;
    if (replay_enabled)
    {
        // send to theif if was earlier stolen
        // check if the task was stolen
        // stolenTaskList *stlCurr = workerStateArr[wid].stl;
        stolenTaskList *stlcurr = workerStateArr[wid].stlHead;
        // printf("New async task %d is being created\n", task->id);
        // while (stlcurr != NULL)
        // {
            // printf("Checking against %d by %d\n", stlCurr->task->taskID, wid);
            if (stlcurr && stlcurr->task->taskID == task->id)
            {
                if (stlcurr->task->taskID!=task->id){
                    printf("ERROR %d %d\n", stlcurr->task->taskID, task->id);
                }
                // printf("Task %d was stolen and now will be executed by worker %d\n", task->id, stlCurr->task->workExecutor);
                // send to theif
                hclib_worker_state* ws = &workers[wid];
                check_in_finish(ws->current_finish);
                task->current_finish = ws->current_finish;
                workerStateArr[wid].stlHead = workerStateArr[wid].stlHead->next;
                // if (stlcurr->task->workExecutor == wid)
                // {
                //     spawn(task);
                // }
                workerStateArr[stlcurr->task->workExecutor].stolenTasks[stlcurr->task->stealCounter] = *task;
                workerStateArr[stlcurr->task->workExecutor].stolenTasksAvailableArr[stlcurr->task->stealCounter] = 1;
                return;
            }
            // break;
            // stlCurr = stlCurr->next;
        // }
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
                    if (tracing_enabled){
                        stolenTask *st = (stolenTask *)malloc(sizeof(stolenTask));
                        st->workCreator = (wid + i) % (nb_workers);
                        st->workExecutor = wid;
                        st->stealCounter = workerStateArr[wid].stealCounter;
                        st->taskID = task->id;
                        workerStateArr[wid].stealCounter+=1; //updating state of current worker after stealing
                        
                        stolenTaskList *stl = (stolenTaskList *)malloc(sizeof(stolenTaskList));
                        stl->task = st;
                        stl->next = NULL;
                        if (workerStateArr[wid].stlHead == NULL)
                        {
                            workerStateArr[wid].stl = stl;
                            workerStateArr[wid].stlHead = stl;
                        }
                        else
                        {
                            workerStateArr[wid].stlHead->next = stl;
                            workerStateArr[wid].stlHead = stl;
                        }
                        // stl->next = workerStateArr[wid].stl;
                        // workerStateArr[wid].stl = stl;
                        // workerStateArr[wid].stlHead = stl;
                    }
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
                // if (workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id != -1)
                if (hc_cas(&workerStateArr[wid].stolenTasksAvailableArr[workerStateArr[wid].stealCounter], 1, 0) == 1)
                {
                    // sleep(0.01);
                    // printf("Task1 found with current steal counter %d worker %d\n", workerStateArr[wid].stealCounter, wid);
                    task = &workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter];    
                    workerStateArr[wid].stealCounter+=1;
                    break;
                }
                // sleep(0.00001);
                // printf("Task1 not found with current steal counter %d woker %d id %d\n", workerStateArr[wid].stealCounter, wid, workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id);
            }
        }

        if (task && task->id != -1)
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
        if (!task && !replay_enabled)
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
                    if (tracing_enabled){
                        stolenTask *st = (stolenTask *)malloc(sizeof(stolenTask));
                        st->workCreator = (wid + i) % (nb_workers);
                        st->workExecutor = wid;
                        st->stealCounter = workerStateArr[wid].stealCounter;
                        st->taskID = task->id;
                        workerStateArr[wid].stealCounter++; //updating state of current worker after stealing

                        stolenTaskList *stl = (stolenTaskList *)malloc(sizeof(stolenTaskList));
                        stl->task = st;
                        stl->next = NULL;

                        if (workerStateArr[wid].stlHead == NULL)
                        {
                            workerStateArr[wid].stl = stl;
                            workerStateArr[wid].stlHead = stl;
                        }
                        else
                        {
                            workerStateArr[wid].stlHead->next = stl;
                            workerStateArr[wid].stlHead = stl;
                        }
                        // stl->next = workerStateArr[wid].stl;
                        // workerStateArr[wid].stl = stl;
                    }
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
                // if (workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id != -1)
                if (hc_cas(&workerStateArr[wid].stolenTasksAvailableArr[workerStateArr[wid].stealCounter], 1, 0) == 1)
                {
                    // printf("Task2 found with current steal counter %d worker %d\n", workerStateArr[wid].stealCounter, wid);
                    task = &workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter];    
                    workerStateArr[wid].stealCounter+=1;
                    break;
                }
                // sleep(0.00001);
                // printf("Task2 not found with current steal counter %d woker %d id %d\n", workerStateArr[wid].stealCounter, wid, workerStateArr[wid].stolenTasks[workerStateArr[wid].stealCounter].id);
            }
        }
        if (task != NULL && task->id != -1){
            execute_task(task);
        }
    }
    return NULL;
}

void merge(stolenTaskList **start, stolenTaskList *left, stolenTaskList *right) {
    stolenTaskList *merged = NULL;
    stolenTaskList **temp = &merged;

    while (left != NULL && right != NULL) {
        if (left->task->taskID <= right->task->taskID) {
            *temp = left;
            left = left->next;
        } else {
            *temp = right;
            right = right->next;
        }
        temp = &((*temp)->next);
    }

    *temp = (left != NULL) ? left : right;
    *start = merged;
}

void mergeSort(stolenTaskList **start) {
    stolenTaskList *head = *start;
    stolenTaskList *left;
    stolenTaskList *right;

    if (head == NULL || head->next == NULL) {
        return;
    }

    // Split the list into halves
    left = head;
    right = head->next;
    while (right != NULL && right->next != NULL) {
        left = left->next;
        right = right->next->next;
    }
    right = left->next;
    left->next = NULL;
    left = head;

    // Recursively sort each half
    mergeSort(&left);
    mergeSort(&right);

    // Merge the sorted halves
    merge(start, left, right);
}

void hclib_start_tracing()
{
    // printf("Tracing enabled\n");
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
        // workerStateArr[i].stealCounter = 0;
        if (!replay_enabled)
        {
            workerStateArr[i].stlHead = NULL;
            workerStateArr[i].stl = NULL;
        }
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
            workerStateArr[i].stlHead = workerStateArr[i].stl;
            for (int j = 0; j < workerStateArr[i].tempCounter+1; j++)
            {
                workerStateArr[i].stolenTasks[j].id = -1;
                // workerStateArr[i].stolenTasksAvailableArr[j] = 0;
                // workerStateArr[i].stolenTaskCounter[j] = 0;
            }
        }
    }
    if (!replay_enabled)
    {
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
                                // workerStateArr[j].stlHead = stlCurr;
                            }
                            tmp->next = workerStateArr[wid].stl;
                            workerStateArr[wid].stl = tmp;
                            // workerStateArr[wid].stlHead = tmp;
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

        // Now sort the stolen list of each worker based on the task id in increasing order
        // for (int i = 0; i < nb_workers; i++)
        // {
        //     stolenTaskList *start = workerStateArr[i].stl;
        //     stolenTaskList *trav;
        //     stolenTaskList *travNext;
        //     int swapped;

        //     if (start == NULL)
        //         continue;
        //     do
        //     {
        //         swapped = 0;
        //         trav = start;

        //         while (trav->next != NULL)
        //         {
        //             travNext = trav->next;

        //             if (trav->task->taskID > travNext->task->taskID)
        //             {
        //                 // Swap tasks
        //                 stolenTask *temp = trav->task;
        //                 trav->task = travNext->task;
        //                 travNext->task = temp;

        //                 swapped = 1;
        //             }
        //             trav = trav->next;
        //         }
        //     } while (swapped);
        // }

        for (int i = 0; i < nb_workers; i++)
        {
            mergeSort(&workerStateArr[i].stl);
        }

        // Now creating arrays for each worker to store the stolen tasks
        for (int i = 0; i < nb_workers; i++)
        {
            workerStateArr[i].tempCounter = workerStateArr[i].stealCounter;
            workerStateArr[i].stolenTasks = (task_t *)malloc(sizeof(task_t) * (workerStateArr[i].stealCounter+1));
            workerStateArr[i].stolenTasksAvailableArr = (int *)malloc(sizeof(int) * (workerStateArr[i].stealCounter+1));
            for (int j = 0; j < workerStateArr[i].stealCounter+1; j++)
            {
                // workerStateArr[i].stolenTasks[j]= malloc(sizeof(task_t));
                task_t *task = malloc(sizeof(*task));
                *task = (task_t){
                    .id = -1,
                };
                workerStateArr[i].stolenTasks[j] = *task;
                workerStateArr[i].stolenTasksAvailableArr[j] = 0;
            }
            workerStateArr[i].stealCounter = 0;
        } 
        for (int i = 0; i < nb_workers; i++)
        {
            workerStateArr[i].stlHead = workerStateArr[i].stl;
        }
        replay_enabled = 1;
    }
}