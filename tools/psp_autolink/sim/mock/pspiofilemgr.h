#pragma once
typedef int SceUID;
typedef long long SceOff;
typedef struct { int st_mode; unsigned st_attr; SceOff st_size; char pad[64]; } SceIoStat;
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_APPEND 0x100
#define PSP_O_CREAT 0x200
#define PSP_O_TRUNC 0x400
int sceIoGetstat(const char *p, SceIoStat *s);
SceUID sceIoOpen(const char *p, int f, int m);
int sceIoRead(SceUID fd, void *b, unsigned n);
int sceIoWrite(SceUID fd, const void *b, unsigned n);
int sceIoClose(SceUID fd);
