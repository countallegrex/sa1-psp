# Backends

One directory per target. Both implement the same small interface used by
`hle/`: draw submission, texture upload, audio output, input, file I/O,
timing, logging.

- `desktop/` SDL2 + OpenGL fixed-function pipeline. Debug platform only. Must
  not use any feature the PSP GE lacks (no shaders, no >8 lights, no
  non-power-of-two textures, no 32-bit depth).
- `psp/` sceGu, sceAudio, sceCtrl, sceIo, pspsdk. The product.
