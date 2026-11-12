/* SPDX-License-Identifier: MIT */
#ifndef WSI_WIN32_PACING_H
#define WSI_WIN32_PACING_H

#include <windows.h>
#include <stdint.h>
#include <string.h>

/* CPU presentation has no swapchain wait object. Use a private one-shot
 * timer, without changing the system timer resolution or polling the CPU.
 * A late frame resets the deadline instead of accumulating catch-up frames. */
struct wsi_win32_pacer {
   HANDLE timer;
   int64_t frequency;
   int64_t period;
   int64_t next;
};

static inline BOOL
wsi_win32_pacer_init(struct wsi_win32_pacer *pacer, uint32_t fps)
{
   memset(pacer, 0, sizeof(*pacer));
   if (!fps)
      return TRUE;
   LARGE_INTEGER frequency;
   if (!QueryPerformanceFrequency(&frequency))
      return FALSE;
   pacer->frequency = frequency.QuadPart;
   pacer->period = (frequency.QuadPart + fps - 1) / fps;
   pacer->timer = CreateWaitableTimerExW(NULL, NULL,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
   return pacer->timer != NULL;
}

static inline BOOL
wsi_win32_pacer_wait(struct wsi_win32_pacer *pacer)
{
   if (!pacer->timer)
      return TRUE;
   LARGE_INTEGER now;
   if (!QueryPerformanceCounter(&now))
      return FALSE;
   while (now.QuadPart < pacer->next) {
      LARGE_INTEGER due;
      due.QuadPart = -((pacer->next - now.QuadPart) * INT64_C(10000000) +
                      pacer->frequency - 1) / pacer->frequency;
      if (!SetWaitableTimer(pacer->timer, &due, 0, NULL, NULL, FALSE) ||
          WaitForSingleObject(pacer->timer, INFINITE) != WAIT_OBJECT_0 ||
          !QueryPerformanceCounter(&now))
         return FALSE;
   }
   pacer->next = now.QuadPart + pacer->period;
   return TRUE;
}

static inline void
wsi_win32_pacer_finish(struct wsi_win32_pacer *pacer)
{
   if (pacer->timer)
      CloseHandle(pacer->timer);
   pacer->timer = NULL;
}

#endif
