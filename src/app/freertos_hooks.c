#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* If any FreeRTOS allocation fails we want to know about it loudly. */
void vApplicationMallocFailedHook(void) { panic("FreeRTOS malloc failed"); }

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    panic("Stack overflow in task: %s", name ? name : "(unnamed)");
}

/* FreeRTOSConfig has configUSE_IDLE_HOOK / configUSE_TICK_HOOK set.
 * Provide trivial implementations so the link succeeds. */
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
