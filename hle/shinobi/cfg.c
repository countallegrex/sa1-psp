/* cfg.c - system settings and the VMU/backup library: the answers a console
 * with the US settings, no VMU and an erased flash user partition gives.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

/* syCfg: 0x8C5FF2CC Init(work) -> 0; 0x8C5FF2DE Exit -> 0;
 * 0x8C5FF31E GetSoundMode(&mode) -> *mode = 0 (stereo);
 * 0x8C5FF39C GetLanguage(&lang) -> *lang = 1 (English). */
void sdk_8c5ff2cc(sh4_ctx *c) { HLE_ENTER(0x8c5ff2ccu, "syCfgInit"); RET(ARG0 ? 0 : -1); }
void sdk_8c5ff2de(sh4_ctx *c) { HLE_ENTER(0x8c5ff2deu, "syCfgExit"); RET(0); }
void sdk_8c5ff31e(sh4_ctx *c) { HLE_ENTER(0x8c5ff31eu, "syCfgGetSoundMode"); hle_wr32(ARG0, 0); RET(0); }
void sdk_8c5ff39c(sh4_ctx *c) { HLE_ENTER(0x8c5ff39cu, "syCfgGetLanguage"); hle_wr32(ARG0, 1); RET(0); }

/* Flash user partition (game settings block): 0x8C604044 open -> -8 (no
 * KATANA_FLASH header, so the game uses its defaults), 0x8C6041D4 read
 * request -> -11 (not open), 0x8C604250 status -> 1 (done; 2 would be
 * polled forever), 0x8C6042E4 close. */
void sdk_8c604044(sh4_ctx *c) { HLE_ENTER(0x8c604044u, "syFlashOpen"); RET(-8); }
void sdk_8c6041d4(sh4_ctx *c) { HLE_ENTER(0x8c6041d4u, "syFlashReadBlock"); RET(-11); }
void sdk_8c604250(sh4_ctx *c) { HLE_ENTER(0x8c604250u, "syFlashServer"); RET(1); }
void sdk_8c6042e4(sh4_ctx *c) { HLE_ENTER(0x8c6042e4u, "syFlashClose"); }

/* bu (VMU file system), no VMU inserted.
 * 0x8C602B68 buInit(capacity, drives, work, done_cb): succeeds and calls the
 * completion callback at once, as the SDK does.
 * 0x8C6033E4 drive busy? -> 0 (the game spins while it is -1).
 * 0x8C602DF2 drive mounted? -> 0.
 * 0x8C602F5C buGetDiskInfo -> 0xFF01 (generic error).
 * 0x8C602FB8 buFindFirstFile -> -2 (no file; -1 would mean busy).
 * 0x8C603560 buSaveFile -> -1 (busy: no medium to write).
 * 0x8C603384 bu operation -> 0xFF01.
 * 0x8C602CB8 buExecServer, 0x8C602BF0 detach server: nothing to do.
 * 0x8C602DDA / 0x8C602DE0: completion and progress callbacks, kept. */
static uint32_t bu_complete_cb, bu_progress_cb;

void sdk_8c602b68(sh4_ctx *c) {
    HLE_ENTER(0x8c602b68u, "buInit");
    uint32_t cb = ARG3;
    RET(0);
    if (cb) {
        sa1_log("buInit: calling the game's init callback 0x%08x", cb);
        hle_call_guest(c, cb, 0, 0, 0, 0);
        RET(0);
    }
}
void sdk_8c6033e4(sh4_ctx *c) { HLE_ENTER(0x8c6033e4u, "buIsBusy"); RET(0); }
void sdk_8c602df2(sh4_ctx *c) { HLE_ENTER(0x8c602df2u, "buIsMounted"); RET(0); }
void sdk_8c602f5c(sh4_ctx *c) { HLE_ENTER(0x8c602f5cu, "buGetDiskInfo"); RET(0xFF01); }
void sdk_8c602fb8(sh4_ctx *c) { HLE_ENTER(0x8c602fb8u, "buFindFirstFile"); RET(-2); }
void sdk_8c603560(sh4_ctx *c) { HLE_ENTER(0x8c603560u, "buSaveFile"); RET(-1); }
void sdk_8c603384(sh4_ctx *c) { HLE_ENTER(0x8c603384u, "buOperation"); RET(0xFF01); }
void sdk_8c602cb8(sh4_ctx *c) { HLE_ENTER(0x8c602cb8u, "buExecServer"); }
void sdk_8c602bf0(sh4_ctx *c) { HLE_ENTER(0x8c602bf0u, "buDetachServer"); }
void sdk_8c602dda(sh4_ctx *c) { HLE_ENTER(0x8c602ddau, "buSetCompleteCallback"); bu_complete_cb = ARG0; }
void sdk_8c602de0(sh4_ctx *c) { HLE_ENTER(0x8c602de0u, "buSetProgressCallback"); bu_progress_cb = ARG0; }

/* pdLcd / VMU screen: none present. */
void sdk_8c602ab4(sh4_ctx *c) { HLE_ENTER(0x8c602ab4u, "pdLcdIsStandard"); RET(0); }
void sdk_8c602998(sh4_ctx *c) { HLE_ENTER(0x8c602998u, "pdLcdDirection"); RET(0); }
void sdk_8c602a6a(sh4_ctx *c) { HLE_ENTER(0x8c602a6au, "pdVmsLcdWrite"); RET(-2); }

/* syHw / syCache: cache maintenance and hardware init are nothing here.
 * 0x8C6024D0 syHwInit, 0x8C6024FA syHwInit2, 0x8C5FC58A write-back range,
 * 0x8C5FC81C invalidate all, 0x8C5FC0C2 instruction-cache invalidate (the
 * game calls it after loading code overlays), 0x8C603B84 sysinfo/flash ID
 * patch. */
void sdk_8c6024d0(sh4_ctx *c) { HLE_ENTER(0x8c6024d0u, "syHwInit"); }
void sdk_8c6024fa(sh4_ctx *c) { HLE_ENTER(0x8c6024fau, "syHwInit2"); }
void sdk_8c5fc58a(sh4_ctx *c) { HLE_ENTER(0x8c5fc58au, "syCacheWriteBack"); }
void sdk_8c5fc81c(sh4_ctx *c) { HLE_ENTER(0x8c5fc81cu, "syCacheInvalidateAll"); }
void sdk_8c5fc0c2(sh4_ctx *c) { HLE_ENTER(0x8c5fc0c2u, "syCacheICI"); }
void sdk_8c603b84(sh4_ctx *c) { HLE_ENTER(0x8c603b84u, "syBtSysInfoInit"); RET(0); }

/* 0x8C604500: the SDK's byte memcpy(dst, src, n). */
void sdk_8c604500(sh4_ctx *c) {
    HLE_ENTER(0x8c604500u, "memcpy (SDK)");
    uint32_t d = ARG0, s = ARG1, n = ARG2;
    for (uint32_t i = 0; i < n; i++) hle_wr8(d + i, hle_rd8(s + i));
}
