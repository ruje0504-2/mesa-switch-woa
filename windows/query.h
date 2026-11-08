/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_QUERY_H
#define TEGRA_GPU_QUERY_H
#include "context.h"

/* Memory/topology queries for the existing context engine adapter handle.
 * Start/stop and query calls must be serialized by the adapter lifecycle.
 * This does not expose a complete adapter: DRIVERCAPS and UMD-private queries
 * remain unsupported until submission, TDR and the UMD contract are wired.
 */
DXGKDDI_QUERYADAPTERINFO TgGpuDdiQueryAdapterInfo;
void TgGpuRegisterQueryDdi(DRIVER_INITIALIZATION_DATA *);
#endif
