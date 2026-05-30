#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate two PSRAM framebuffers and create the swap mutex.
 * @return true on success.
 */
bool render_init(void);

/**
 * Return a pointer to the "back" framebuffer for the decoder to write into.
 * @return 320*240 RGB565 buffer in PSRAM; 4-byte aligned.
 * @note   Not thread-safe; must be called from the single decode task.
 */
uint16_t* render_back_buffer(void);

/**
 * Mark the back buffer as ready and start the DMA blit.
 * Waits for the previous DMA transfer to complete first (double-buffer swap).
 * Thread-safe (protected by internal mutex).
 */
void render_present(void);

/**
 * Copy a 4× downsampled (80x60) thumbnail from the just-presented frame.
 * Should be called after render_present completes. Best-effort: if no frame
 * has been presented yet the call is a silent no-op. Thread-safe.
 */
void render_capture_thumb(void);

/**
 * Draw the FREEZE overlay (blinking corner badge) on top of the current
 * front buffer. Intended for the link_ui_task while link state is FREEZE.
 * Thread-safe (acquires the same mutex as render_present).
 */
void render_show_freeze(void);

/**
 * Draw the DISCONNECTED status screen: dark fill, "AGUARDANDO LINK", offline
 * counter, and the 80x60 thumbnail of the last good frame if available.
 * Thread-safe.
 * @param since_ms  Milliseconds since the last valid rx (offline duration).
 */
void render_show_disconnected(uint32_t since_ms);

/**
 * Free framebuffers and delete mutex. Safe to call without prior render_init.
 */
void render_deinit(void);

#ifdef __cplusplus
}
#endif
