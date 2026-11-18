/*
 * ECNG 3006 - Lab 2, Question 4
 * FreeRTOS Debug/RunTime Trace macros
 *
 * FreeRTOS has empty "trace macros" placed at key points inside the kernel
 * (a task switching in or out, a task blocking, a task becoming ready...).
 * Defining them here makes the kernel call our own functions (in trace.c)
 * at those moments, so we can record when each task is blocked.
 *
 * This file must be included at the END of FreeRTOSConfig.h, because the
 * kernel only sees trace macros that are defined in its config file.
 */

#ifndef TRACE_HOOKS_H
#define TRACE_HOOKS_H

#ifndef __ASSEMBLER__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slot 0 = any other task (idle, timer), slots 1..4 = our tasks */
#define TRACE_SLOTS  5

/* What we record for each task (times are in ticks, 1 tick = 1 ms) */
typedef struct {
    uint32_t runs;            /* times the task was switched in            */
    uint32_t mutexBlocks;     /* times it blocked waiting for the mutex    */
    uint32_t mutexWaitMax;    /* longest time spent blocked on the mutex   */
    uint32_t delayBlocks;     /* times it blocked in vTaskDelay()          */
    uint32_t cpuWaitMax;      /* longest wait for the CPU after unblocking */
} TraceStats_t;

/* Called by the application (main.c) */
void trace_register_task(uint8_t slot, void *taskHandle);
void trace_register_mutex(void *mutexHandle);
void trace_get_stats(TraceStats_t *out);        /* copies all TRACE_SLOTS */

/* Called by the kernel, through the macros below */
void trace_switched_in(void *tcb, uint32_t tick);
void trace_switched_out(void *tcb, uint32_t tick);
void trace_moved_to_ready(void *tcb, uint32_t tick);
void trace_blocking_on_queue(void *queue);
void trace_task_delay(void);

#ifdef __cplusplus
}
#endif

/* ---------------- The trace macros ----------------
 * pxCurrentTCB (the running task) and xTickCount (the tick counter) are
 * variables inside the kernel's tasks.c, where these macros are expanded.
 * #undef first in case FreeRTOSConfig.h already defined any of them.
 */

/* A task is about to start running */
#undef  traceTASK_SWITCHED_IN
#define traceTASK_SWITCHED_IN() \
        trace_switched_in((void *)pxCurrentTCB, (uint32_t)xTickCount)

/* The running task is about to stop running */
#undef  traceTASK_SWITCHED_OUT
#define traceTASK_SWITCHED_OUT() \
        trace_switched_out((void *)pxCurrentTCB, (uint32_t)xTickCount)

/* A task has been put back in the Ready list (e.g. its delay ended,
 * or the mutex it was waiting for was given) */
#undef  traceMOVED_TASK_TO_READY_STATE
#define traceMOVED_TASK_TO_READY_STATE(pxTCB) \
        trace_moved_to_ready((void *)(pxTCB), (uint32_t)xTickCount)

/* The running task is about to BLOCK waiting on a queue/semaphore/mutex */
#undef  traceBLOCKING_ON_QUEUE_RECEIVE
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) \
        trace_blocking_on_queue((void *)(pxQueue))

/* The running task is about to BLOCK in vTaskDelay() */
#undef  traceTASK_DELAY
#define traceTASK_DELAY() \
        trace_task_delay()

#endif /* __ASSEMBLER__ */

#endif /* TRACE_HOOKS_H */