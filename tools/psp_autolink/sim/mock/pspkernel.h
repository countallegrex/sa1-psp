#pragma once
#include <stddef.h>
typedef int SceUID;
typedef unsigned int SceSize;
typedef long long SceOff;
typedef int (*SceKernelThreadEntry)(SceSize, void *);
#define PSP_MODULE_INFO(a, b, c, d) static const int module_info_dummy = 0
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int p, int s, unsigned a, void *o);
int sceKernelStartThread(SceUID t, SceSize a, void *p);
int sceKernelDeleteThread(SceUID t);
int sceKernelTerminateDeleteThread(SceUID t);
int sceKernelExitDeleteThread(int s);
int sceKernelDelayThread(unsigned us);
#include "pspiofilemgr.h"
