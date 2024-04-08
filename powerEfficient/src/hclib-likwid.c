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

/**
 * USAGE:
 * (a) Download and install likwid from a release version:
 * 	wget -c https://github.com/RRZE-HPC/likwid/archive/refs/tags/v5.3.0.tar.gz
 *
 * (b) Install likwid exactly as shown below (don't change anything)
 * 	cd likwid-5.3.0
 * 	make
 * 	sudo make install
 *
 * (c) Find the name of event and the associated MSR register by looking into likwid wiki:
 *     (i) Find the CPU name
 *     		$ lscpu | grep "Model name"
 *		  Model name: Intel(R) Xeon(R) CPU E5-2650 v3 @ 2.30GHz
 *     (ii) Search for the processor name on google, e.g., in my case "Intel(R) Xeon(R) CPU E5-2650"
 *          https://www.intel.com/content/www/us/en/products/sku/81705/intel-xeon-processor-e52650-v3-25m-cache-2-30-ghz/specifications.html
 *          Note the "Code name" is showing "Haswell"
 *     (iii) Click on the link "haswell" in https://github.com/RRZE-HPC/likwid/wiki/
 *     	    https://github.com/RRZE-HPC/likwid/wiki/Haswell
 *     (iv) Find the event name and the corresponding counter name for INSTR_RETIRED_ANY and PWR_PKG_ENERGY
 *     (v) Add them into the string "estr" and string array "enames" EXACTLY as shown below.
 *
 * (d) The likwid_init and likwid_finalize APIs defined in this file must be used once
 *     at the entry and exit to the main() method, respectively.
 *
 * (e) The likwid_start() and likwid_stop() APIs must be used once to surround the computation kernel only.
 *     The monitoring starts from start() API and is stopped inside the stop() API.
 *
 * (f) You can call likwid_read() once and then read instructions and energy.
 *     Net instructions and energy is the difference of the values since it was called the last time.
 *     JPI is not calculated as the difference. It is the absolute value.
 *
 * (g) Run your likwid program as follows (You don't need a sudo access):
 *	LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH ./a.out
 *
 */

/*
 * Code derived from: https://github.com/RRZE-HPC/likwid/blob/master/examples/C-likwidAPI.c
 */

#ifdef PERFCOUNTER
//////////////////////////////////////////////////////////////////////////////
char estr[] = "PWR_PKG_ENERGY:PWR0,INSTR_RETIRED_ANY:FIXC0";
int index_energy = 0;
int index_instr = 1;
char* enames[2] = {"PWR_PKG_ENERGY:PWR0", "INSTR_RETIRED_ANY:FIXC0"};
//////////////////////////////////////////////////////////////////////////////

int GID;
int *cpus;
CpuTopology_t cputopo;
int nCPUs;

void likwid_finalize()
{
    free(cpus);
    perfmon_finalize();
    affinity_finalize();
    numa_finalize();
    topology_finalize();
}

void likwid_init()
{
    topology_init();
    numa_init();
    affinity_init();
    timer_init();
    cputopo = get_cpuTopology();
    nCPUs = cputopo->numHWThreads;
    cpus = (int *)malloc(nCPUs * sizeof(int));
    if (!cpus)
    {
        affinity_finalize();
        numa_finalize();
        topology_finalize();
        return;
    }
    int c = 0;
    int i = 0;
    for (i = 0; i < nCPUs; i++)
    {
        cpus[c] = cputopo->threadPool[i].apicId;
        c++;
    }
    perfmon_init(nCPUs, cpus);
    GID = perfmon_addEventSet(estr);
    if (GID < 0)
    {
        printf("Failed to add performance group ENERGY\n");
        GID = 1;
        likwid_finalize();
    }
}

void likwid_start()
{
    perfmon_setupCounters(GID);
    perfmon_startCounters();
}

double energy = 0, instructions = 0;

void likwid_read() {
    perfmon_readCounters();
    for (int k = 0; k < nCPUs; k++) {
        energy += perfmon_getLastResult(GID, index_energy, k);
        instructions += perfmon_getLastResult(GID, index_instr, k);
    }
}

double likwid_energy() { return energy; }

double likwid_instruction() { return instructions; }

double likwid_JPI()
{
    return (energy/instructions);
}

void likwid_stop()
{
    perfmon_stopCounters();
}

#else
void likwid_finalize() { }
void likwid_stop() { }
double likwid_JPI() { return 0; }
void likwid_read() { }
double likwid_energy() { return 0; }
double likwid_instruction() { return 0; }
void likwid_start() { }
void likwid_init() { }
#endif
