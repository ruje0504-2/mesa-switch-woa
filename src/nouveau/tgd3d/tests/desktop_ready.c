/* SPDX-License-Identifier: MIT
 * Diagnostic logon helper. It only publishes session readiness; all driver
 * work and D3D runtime callbacks remain on the driver's DDI caller thread.
 */
#include <windows.h>
#include <sddl.h>
#include "tgd3d_deferred.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
   (void)instance; (void)previous; (void)command; (void)show;
   PSECURITY_DESCRIPTOR descriptor = NULL;
   /* Interactive users may publish readiness. DWM's well-known group only
    * waits; numeric SID also works when the account name cannot be resolved. */
   if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;0x100000;;;S-1-5-90-0)",
          SDDL_REVISION_1, &descriptor, NULL))
      return (int)GetLastError();
   SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
   HANDLE ready = CreateEventW(&attributes, TRUE, FALSE, TGD3D_DESKTOP_READY_EVENT);
   DWORD error = GetLastError();
   LocalFree(descriptor);
   if (!ready)
      return (int)error;
   if (error == ERROR_ALREADY_EXISTS) {
      CloseHandle(ready);
      return 0; /* The first helper owns the event for this session. */
   }
   const ULONGLONG deadline = GetTickCount64() + 60000;
   while (GetTickCount64() < deadline) {
      HWND desktop = GetShellWindow();
      HWND taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
      if (desktop && taskbar && IsWindowVisible(desktop) && IsWindowVisible(taskbar)) {
         if (!SetEvent(ready)) {
            error = GetLastError();
            CloseHandle(ready);
            return (int)error;
         }
         /* Kernel objects disappear when their last handle closes. Retain
          * this one until logoff, including when no UMD has opened it yet. */
         Sleep(INFINITE);
      }
      Sleep(250);
   }
   CloseHandle(ready);
   return ERROR_TIMEOUT;
}
