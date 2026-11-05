#pragma once
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
int xSemaphoreTakeRecursive(SemaphoreHandle_t handle,unsigned timeout);
int xSemaphoreGiveRecursive(SemaphoreHandle_t handle);
