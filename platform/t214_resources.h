/* SPDX-License-Identifier: MIT */
/* Shared by the HDH-001 ACPI table and the Windows render backend. */
#ifndef TEGRA_GPU_T214_RESOURCES_H
#define TEGRA_GPU_T214_RESOURCES_H

#define T214_GPU_ACPI_ID          "SWOA0F01"
#define T214_GPU_REG_BASE         0x57000000
#define T214_GPU_REG_SIZE         0x01000000
#define T214_GPU_BAR1_BASE        0x58000000
#define T214_GPU_BAR1_SIZE        0x01000000
#define T214_GPU_AUX_BASE         0x538F0000
#define T214_GPU_AUX_SIZE         0x00001000
/* Tegra GIC SPI 157/158, plus the 32 private interrupts for ACPI GSIs. */
#define T214_GPU_STALL_GSI        189
#define T214_GPU_NONSTALL_GSI     190

#endif
