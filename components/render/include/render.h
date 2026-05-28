#pragma once
#include <cstdint>

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
 * Free framebuffers and delete mutex. Safe to call without prior render_init.
 */
void render_deinit(void);
