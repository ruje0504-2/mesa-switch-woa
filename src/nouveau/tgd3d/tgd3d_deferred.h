/* SPDX-License-Identifier: MIT */
#ifndef TGD3D_DEFERRED_H
#define TGD3D_DEFERRED_H

/* Session-local, manual-reset event held by the diagnostic logon helper.
 * A reboot/logoff destroys it: readiness cannot leak from an earlier boot. */
#define TGD3D_DESKTOP_READY_EVENT L"Local\\SwitchWOA.Tgd3d.DesktopReady.v1"

#if defined(TGD3D_CPU_SYNC_CONTROL) && defined(TGD3D_DEFERRED_INIT)
#error The permanent CPU control and deferred GPU bridge are different diagnostics
#endif

#endif
