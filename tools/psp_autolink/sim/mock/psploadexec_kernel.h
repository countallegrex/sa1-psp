#pragma once
struct SceKernelLoadExecVSHParam {
    unsigned size, args;
    void *argp;
    const char *key;
    unsigned vshmain_args_size;
    void *vshmain_args;
    char *configfile;
    unsigned unk4, unk5;
};
