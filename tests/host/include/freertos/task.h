#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;

#ifdef __cplusplus
extern "C" {
#endif
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task,
                                   const char *name,
                                   configSTACK_DEPTH_TYPE stack_depth,
                                   void *argument,
                                   UBaseType_t priority,
                                   TaskHandle_t *task_handle,
                                   BaseType_t core_id);
BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       configSTACK_DEPTH_TYPE stack_depth,
                       void *argument,
                       UBaseType_t priority,
                       TaskHandle_t *task_handle);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
#ifdef __cplusplus
}
#endif
