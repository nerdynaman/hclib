HClib-Light
=============================================

HClib-Light (this version) is a lightweight implementation of the original HCLIB that
only supports the async-finish APIs. It is a minimalistic implementation derived specially
for the the CSE513 course at IIIT-Delhi. 

Implementation Details
---------------------------------------------

There have been multiple runtime optimizations made in the HClib-Light implementation to make it efficient.

### Linked List based private queues

In this approach we implemented private queues for each worker thread using linked lists. This way the worker thread can directly pick up the tasks from its own queue and execute them. This reduces the contention on the global queue and need for locking and unlocking the global queue. 

### Private Queues and Mailboxes for Work Stealing

In this approach for improving work stealing we implemented a private queue for each worker thread which stores the tasks that are created by the worker thread itself. This way the worker thread can directly pick up the tasks from its own queue and execute them. This reduces the contention on the global queue and need for locking and unlocking the global queue. Further if some worker thread is idle and has no tasks in its own queue then it can steal tasks from other worker threads. The mechanism for stealing tasks from other worker threads is such that there is a mailbox corresponding to each worker thread, whenever some worker A wants to steal tasks from worker B, it sends a message to the mailbox of worker B. The worker B then sends the tasks to worker A. This way the worker A can steal tasks from worker B without any contention on the mailbox of worker B.

### Profiling based approach for work stealing

In this approach we implemented a profiling based approach for work stealing. This approach is suitable for programs such as iterative averaging where similar work is done in each iteration. In this approach we profile the tasks and the worker threads. We track which task(async) is spawned by which worker thread further which thread steals the task and executes it. We maintain a profiling table which stores the profiling information. In subsequent iterations we use this profiling information to execute information which reduces any further overhead for stealing tasks.

### Power efficient 

In this approach we implement a solution to optimise power consumption of porgrams with minimum performance cost degradation. We implement this by constantly monitoring the power consumption of application by making kernel level threads sleep and awake while executing tasks and ensuring we reach the minimum power consumption level.


Installation
---------------------------------------------

HClib follows your standard bootstrap, configure, and make installation
procedure. An install.sh script is provided for your convenience that will
build and install HClib. Simply run the script to install:

    ./install.sh
By default, HClib will be installed to `$PWD/hclib-install`.

You will need to set the `HCLIB_ROOT` environment variable to point to your
HClib installation directory. You can automatically set this variable after
installation by sourcing the `hclib_setup_env.sh` script.

Running HClib Programs
---------------------------------------------
cd hclib/test
make
HCLIB_WORKERS=4 ./fib

The above invocation would create four HClib workers (pthread) that would
use work-stealing algorithm for load-balancing of async tasks

Dependencies
---------------------------------------------

* automake
* gcc >= 4.8.4, or clang >= 3.5
  (must support -std=c11 and -std=c++11)
