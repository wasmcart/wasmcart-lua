/* wc_native.h — the native-host seam for the wasmcart-lua engine.
 *
 * Under wasm, a host lives outside the cart's linear memory and reaches the
 * shared regions through uint32_t offsets in wc_info_t. Under a NATIVE build
 * (-DWC_NATIVE_HOST) the host is linked into the same address space, so it
 * gets real pointers instead — those uint32_t fields would truncate on a
 * 64-bit target and are not used.
 *
 * Nothing here changes the wasm ABI. This header is compiled only when
 * WC_NATIVE_HOST is defined; wasm builds never see it.
 *
 * The host must also PROVIDE the wc_* functions that wasmcart.h declares as
 * `env` imports (wc_log, wc_asset_size, wc_load_asset, wc_pad_*, ...). Under
 * wasm those are imports; natively they are ordinary extern symbols the host
 * links in. wasmcart.h's non-wasm fallbacks are no-op `static inline`s, so a
 * native host defines WC_NATIVE_HOST to suppress them and supply its own.
 */
#ifndef WC_NATIVE_H
#define WC_NATIVE_H

#include <stdint.h>
#include "wasmcart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t        *framebuffer;        /* MAX_WIDTH*MAX_HEIGHT XRGB */
    float           *audio_ring;         /* audio_cap*2 interleaved f32 */
    uint32_t        *audio_write_cursor;
    uint32_t         audio_cap;
    wc_pad_t        *pads;               /* [4] */
    wc_pointer_t    *pointers;           /* [10]: 0 = mouse, 1-9 = touch */
    wc_wheel_t      *wheel;              /* scroll delta, 1/120 notch (v3.1) */
    uint8_t         *keys;               /* [32] bitmask */
    wc_time_t       *time;
    wc_host_info_t  *host_info;
    wc_info_t       *info;               /* width/height/flags stay valid */
    uint8_t         *save;
    uint32_t         save_size;
} wc_native_regions_t;

/* Valid immediately (the regions are static storage); the width/height in
 * ->info are only final AFTER wc_init(), since conf.lua may resize. */
const wc_native_regions_t *wc_native_regions(void);

#ifdef __cplusplus
}
#endif

#endif /* WC_NATIVE_H */
