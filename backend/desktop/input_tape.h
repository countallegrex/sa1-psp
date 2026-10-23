/* input_tape.h - input tapes, strict determinism and state checks
 * (input_tape.c, tests/replay/README.md).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

/* Consumes argv[*i] (and its value) if it is one of the tape options. */
int input_tape_arg(int argc, char **argv, int *i);
/* After 1ST_READ.BIN is loaded, before hle_init: hashes the title, loads the
 * tape and the checks, opens the recording, sets strict mode.  0 = ok. */
int input_tape_start(const char *title_path);
/* The replayed tape's length in frames (0 without --replay). */
uint32_t input_tape_frames(void);
/* Each frame, once hle_pad_state holds the live or scripted pad: replays or
 * records it and returns the buttons to use.  scripted = --press is active. */
uint32_t input_tape_pad(uint32_t frame, uint32_t buttons, int scripted);
/* Each frame, where --dump writes: evaluates the checks due. */
void input_tape_frame_end(uint32_t frame);
/* At exit (why = NULL) or on a trap ("trap"): closes the recording, prints
 * the verdict and returns the exit code to use. */
int input_tape_finish(int code, const char *why);
/* A save state was loaded at frame: rows due before it are marked done. */
void input_tape_loaded(uint32_t frame);
/* 1 when --record is active (a tape cannot start from a loaded state). */
int input_tape_recording(void);
/* The sound timing the run must give the guest: 1 real, 0 none (a tape
 * recorded before M3), -1 when neither a tape nor --sound-timing says. */
int input_tape_sound_timing(void);
/* The host FPU rounding the run must use: 1 toward zero (the console's),
 * 0 to nearest (a tape recorded before 2026-10-19), -1 not given. */
int input_tape_fpu_zero(void);
int input_tape_fmac_fused(void);     /* 1: fmac rounds once (sh4_rt.h FMAC_F) */
int input_tape_round_gate(void);     /* 1: the stick's round gate (hle_pad_round_gate) */
/* The replayed tape's aspect line: 1 wide, 0 4:3, -1 none.  The aspect is
 * game logic (the clip rectangle the game culls by), so a replay without
 * --aspect takes it. */
int input_tape_aspect_wide(void);
