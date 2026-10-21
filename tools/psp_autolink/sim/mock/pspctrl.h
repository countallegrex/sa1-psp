#pragma once
typedef struct { unsigned TimeStamp; unsigned Buttons; unsigned char Lx, Ly, Rsrv[6]; } SceCtrlData;
#define PSP_CTRL_LTRIGGER 0x100
#define PSP_CTRL_RTRIGGER 0x200
int sceCtrlPeekBufferPositive(SceCtrlData *d, int n);
