#pragma once
#include "../network.h"

void netloaderTask(void* arg);
void netloaderUpdate(void);
void netloaderDrawBot(void);
void netloaderExit(void);
/* Listen for 3dslink in the background from now on, and start what comes. */
void netloaderStartBackground(void);
