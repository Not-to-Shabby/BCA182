#include <stdbool.h>
#include "system_state.h"
#include "logic.h"
#include "rtos_objects.h"
#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#define STATE_CHECK_MS  250U

void StateTask(void *argument)
{
    (void)argument;
    SystemState state = SYSTEM_ACTIVE;
    TickType_t lastMotionTick = xTaskGetTickCount();

    Log("[System] Initial Mode: ACTIVE\r\n");

    for (;;)
    {
        /* Wait for motion event or periodic timeout check */
        EventBits_t bits = 0;
        if (systemEvents != NULL)
        {
            bits = xEventGroupWaitBits(systemEvents, EVENT_MOTION,
                                       pdTRUE, pdFALSE,
                                       pdMS_TO_TICKS(STATE_CHECK_MS));
        }

        bool motion = (bits & EVENT_MOTION) != 0;
        TickType_t now = xTaskGetTickCount();

        if (motion)
        {
            lastMotionTick = now;
        }

        uint32_t msSinceMotion = (uint32_t)(now - lastMotionTick) * portTICK_PERIOD_MS;

        SystemState newState = evaluateSystemState(state, motion, msSinceMotion,
                                                   INACTIVITY_TIMEOUT_MS);

        if (newState != state)
        {
            state = newState;

            if (systemEvents != NULL)
            {
                if (state == SYSTEM_ACTIVE)
                {
                    (void)xEventGroupSetBits(systemEvents, EVENT_ACTIVE);
                }
                else
                {
                    (void)xEventGroupClearBits(systemEvents, EVENT_ACTIVE);
                }
            }

            Log_Begin();
            Log("[System] State transitioned to: ");
            Log(systemStateName(state));
            Log("\r\n");
            Log_End();
        }
    }
}
