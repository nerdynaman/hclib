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

/****************************************************/
/* DEQUE API                                        */
/****************************************************/

typedef struct {
    volatile int head;
    volatile int tail;
    volatile void ** data;
} deque_t;

typedef struct hclib_worker_state {
    pthread_t tid; // the pthread associated
    struct finish_t* current_finish;
    deque_t * deque;
    int id; // The id, identify a worker
    long total_push;
    long total_steals;
    // The following fields are used for implementing the private deques and mailbox
    volatile int thief_id; // The thief will put his id here if he wants a task from this worker
    deque_t * mailbox; // The mailbox for the current worker to receive any stolen tasks
    volatile int mailbox_size; // The head of the mailbox
} hclib_worker_state;


#define INIT_DEQUE_CAPACITY 16000

void dequeInit(deque_t * deq, void * initValue);
void * dequeSteal(deque_t * deq);
void dequePush(deque_t* deq, void* entry);
void * dequePop(deque_t * deq);
void dequeDestroy(deque_t* deq);


int dequeStealMailbox(hclib_worker_state *ws, int thief_id);