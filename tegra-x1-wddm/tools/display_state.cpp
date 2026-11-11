/* SPDX-License-Identifier: MIT */
/* Capture/restore the user's single-display mode from an interactive session.
 * Adapter LUIDs are diagnostic data, not persistent display identities.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

static constexpr DWORD required_fields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;

struct snapshot {
   DWORD magic, version;
   SYSTEMTIME utc;
   WCHAR source_name[CCHDEVICENAME];
   WCHAR monitor_path[128];
   WCHAR adapter_device_id[128];
   DEVMODEW current, registry;
   DISPLAYCONFIG_PATH_INFO path;
   DISPLAYCONFIG_SOURCE_MODE source;
   DISPLAYCONFIG_TARGET_MODE target;
   DWORD system_dpi;
};

static LONG read_state(snapshot &s)
{
   s = {};
   s.magic = 0x53444754; /* TGDS */
   s.version = 1;
   GetSystemTime(&s.utc);
   UINT32 path_count = 0, mode_count = 0;
   std::vector<DISPLAYCONFIG_PATH_INFO> paths;
   std::vector<DISPLAYCONFIG_MODE_INFO> modes;
   LONG result = ERROR_INSUFFICIENT_BUFFER;
   for (unsigned attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; attempt++) {
      result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
      if (result != ERROR_SUCCESS)
         return result;
      paths.resize(path_count);
      modes.resize(mode_count);
      result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                  &mode_count, modes.data(), nullptr);
   }
   if (result != ERROR_SUCCESS)
      return result;
   if (path_count != 1)
      return ERROR_NOT_SUPPORTED;
   s.path = paths[0];
   const DWORD source_index = s.path.sourceInfo.modeInfoIdx;
   const DWORD target_index = s.path.targetInfo.modeInfoIdx;
   if (source_index >= mode_count || target_index >= mode_count ||
       modes[source_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
       modes[target_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
      return ERROR_INVALID_DATA;
   s.source = modes[source_index].sourceMode;
   s.target = modes[target_index].targetMode;

   DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
   source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                    s.path.sourceInfo.adapterId, s.path.sourceInfo.id};
   result = DisplayConfigGetDeviceInfo(&source.header);
   if (result != ERROR_SUCCESS)
      return result;
   std::memcpy(s.source_name, source.viewGdiDeviceName, sizeof(s.source_name));
   DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
   target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                    s.path.targetInfo.adapterId, s.path.targetInfo.id};
   result = DisplayConfigGetDeviceInfo(&target.header);
   if (result != ERROR_SUCCESS)
      return result;
   std::memcpy(s.monitor_path, target.monitorDevicePath, sizeof(s.monitor_path));

   DISPLAY_DEVICEW adapter = {};
   adapter.cb = sizeof(adapter);
   for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &adapter, 0); index++) {
      if (!std::wcscmp(adapter.DeviceName, s.source_name)) {
         std::memcpy(s.adapter_device_id, adapter.DeviceID, sizeof(s.adapter_device_id));
         break;
      }
   }
   if (!s.adapter_device_id[0])
      return ERROR_NOT_FOUND;
   s.current.dmSize = sizeof(s.current);
   s.registry.dmSize = sizeof(s.registry);
   if (!EnumDisplaySettingsExW(s.source_name, ENUM_CURRENT_SETTINGS, &s.current, 0) ||
       !EnumDisplaySettingsExW(s.source_name, ENUM_REGISTRY_SETTINGS, &s.registry, 0))
      return ERROR_INVALID_DATA;
   if ((s.current.dmFields & required_fields) != required_fields || s.current.dmDriverExtra)
      return ERROR_NOT_SUPPORTED;
   s.system_dpi = GetDpiForSystem();
   return ERROR_SUCCESS;
}

static void json_string(const WCHAR *value)
{
   char utf8[1024];
   int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
   std::putchar('"');
   for (int i = 0; i < length - 1; i++) {
      unsigned char c = utf8[i];
      if (c == '"' || c == '\\')
         std::putchar('\\');
      if (c < 32)
         std::printf("\\u%04x", c);
      else
         std::putchar(c);
   }
   std::putchar('"');
}

static void print_mode(const DEVMODEW &m)
{
   std::printf("{\"width\":%lu,\"height\":%lu,\"orientation\":%lu,\"bpp\":%lu,"
               "\"hz\":%lu,\"x\":%ld,\"y\":%ld,\"flags\":%lu,\"fields\":%lu}",
               m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayOrientation, m.dmBitsPerPel,
               m.dmDisplayFrequency, m.dmPosition.x, m.dmPosition.y, m.dmDisplayFlags, m.dmFields);
}

struct desktop_context {
   DWORD session = MAXDWORD, console_session = MAXDWORD, input_error = 0;
   WCHAR desktop[128] = {}, input[128] = {};
};

static desktop_context read_desktop()
{
   desktop_context context;
   DWORD bytes = 0;
   ProcessIdToSessionId(GetCurrentProcessId(), &context.session);
   context.console_session = WTSGetActiveConsoleSessionId();
   GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                             context.desktop, sizeof(context.desktop), &bytes);
   HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
   context.input_error = input ? ERROR_SUCCESS : GetLastError();
   if (input) {
      if (!GetUserObjectInformationW(input, UOI_NAME, context.input, sizeof(context.input), &bytes))
         context.input_error = GetLastError();
      CloseDesktop(input);
   }
   return context;
}

static bool visible_desktop()
{
   const auto context = read_desktop();
   return context.session == context.console_session && !context.input_error &&
          context.desktop[0] && !std::wcscmp(context.desktop, context.input);
}

static void print_state(const snapshot &s, const WCHAR *operation, bool changed, LONG status)
{
   const auto context = read_desktop();
   std::printf("{\"operation\":"); json_string(operation);
   std::printf(",\"session\":%lu,\"console_session\":%lu,\"desktop\":",
               context.session, context.console_session);
   json_string(context.desktop);
   std::printf(",\"input_desktop\":"); json_string(context.input);
   std::printf(",\"input_desktop_error\":%lu", context.input_error);
   std::printf(",\"changed\":%s,\"status\":%ld,\"source_name\":", changed ? "true" : "false", status);
   json_string(s.source_name);
   std::printf(",\"monitor_path\":"); json_string(s.monitor_path);
   std::printf(",\"adapter_device_id\":"); json_string(s.adapter_device_id);
   std::printf(",\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\"system_dpi\":%lu,\"current\":",
               s.utc.wYear, s.utc.wMonth, s.utc.wDay, s.utc.wHour, s.utc.wMinute, s.utc.wSecond, s.system_dpi);
   print_mode(s.current);
   std::printf(",\"registry\":"); print_mode(s.registry);
   std::printf(",\"source_mode\":{\"width\":%u,\"height\":%u},"
               "\"target_signal\":{\"width\":%u,\"height\":%u},\"path_rotation\":%u}\n",
               s.source.width, s.source.height, s.target.targetVideoSignalInfo.activeSize.cx,
               s.target.targetVideoSignalInfo.activeSize.cy, s.path.targetInfo.rotation);
}

static bool same_mode(const DEVMODEW &a, const DEVMODEW &b)
{
   return a.dmPelsWidth == b.dmPelsWidth && a.dmPelsHeight == b.dmPelsHeight &&
      a.dmDisplayOrientation == b.dmDisplayOrientation && a.dmBitsPerPel == b.dmBitsPerPel &&
      a.dmDisplayFrequency == b.dmDisplayFrequency && a.dmDisplayFlags == b.dmDisplayFlags &&
      a.dmPosition.x == b.dmPosition.x && a.dmPosition.y == b.dmPosition.y;
}

static LONG configure(const snapshot &current, const snapshot &saved, UINT32 flags)
{
   /* Rebind the saved modes to the current adapter, whose LUID can change
    * across the driver rollback/reboot. Use CCD timing data rather than the
    * legacy DEVMODE refresh value reported by BasicDisplay. */
   DISPLAYCONFIG_PATH_INFO path = current.path;
   DISPLAYCONFIG_MODE_INFO modes[2] = {};
   modes[0].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
   modes[0].id = path.sourceInfo.id;
   modes[0].adapterId = path.sourceInfo.adapterId;
   modes[0].sourceMode = saved.source;
   modes[1].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
   modes[1].id = path.targetInfo.id;
   modes[1].adapterId = path.targetInfo.adapterId;
   modes[1].targetMode = saved.target;
   path.sourceInfo.modeInfoIdx = 0;
   path.targetInfo.modeInfoIdx = 1;
   path.targetInfo.rotation = saved.path.targetInfo.rotation;
   path.targetInfo.scaling = saved.path.targetInfo.scaling;
   path.targetInfo.refreshRate = saved.path.targetInfo.refreshRate;
   path.targetInfo.scanLineOrdering = saved.path.targetInfo.scanLineOrdering;
   return SetDisplayConfig(1, &path, 2, modes, flags | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
}

int wmain(int argc, WCHAR **argv)
{
   if (argc < 2 || argc > 3 || (std::wcscmp(argv[1], L"show") && std::wcscmp(argv[1], L"visible") &&
       std::wcscmp(argv[1], L"save") && std::wcscmp(argv[1], L"verify") &&
       std::wcscmp(argv[1], L"test") && std::wcscmp(argv[1], L"restore")) ||
       ((!std::wcscmp(argv[1], L"show") || !std::wcscmp(argv[1], L"visible")) ? argc != 2 : argc != 3)) {
      std::fprintf(stderr, "usage: display-state.exe {show|visible} | {save|verify|test|restore} snapshot.bin\n");
      return 2;
   }
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   snapshot current;
   LONG result = read_state(current);
   if (result) {
      std::fprintf(stderr, "Read display state failed: %ld (use an interactive single-display session)\n", result);
      return 1;
   }
   if (!std::wcscmp(argv[1], L"show")) {
      print_state(current, argv[1], false, 0);
      return 0;
   }
   if (!std::wcscmp(argv[1], L"visible")) {
      bool visible = visible_desktop();
      print_state(current, argv[1], false, visible ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
      return visible ? 0 : 1;
   }
   if (!std::wcscmp(argv[1], L"save")) {
      HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      DWORD bytes = 0;
      bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, &current, sizeof(current), &bytes, nullptr) &&
                bytes == sizeof(current) && FlushFileBuffers(file);
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
      if (!ok) { std::fprintf(stderr, "Snapshot write failed: %lu\n", GetLastError()); return 1; }
      print_state(current, argv[1], false, 0);
      return 0;
   }
   snapshot saved = {};
   FILE *file = _wfopen(argv[2], L"rb");
   bool ok = file && std::fread(&saved, 1, sizeof(saved), file) == sizeof(saved) && std::fgetc(file) == EOF;
   if (file) std::fclose(file);
   if (!ok || saved.magic != current.magic || saved.version != current.version ||
       saved.current.dmSize != sizeof(DEVMODEW) || saved.current.dmDriverExtra ||
       (saved.current.dmFields & required_fields) != required_fields ||
       saved.current.dmDisplayOrientation > DMDO_270 || !saved.current.dmPelsWidth || !saved.current.dmPelsHeight ||
       saved.monitor_path[127] || saved.adapter_device_id[127] ||
       (saved.monitor_path[0] ? std::wcscmp(saved.monitor_path, current.monitor_path) :
                               std::wcscmp(saved.adapter_device_id, current.adapter_device_id))) {
      std::fprintf(stderr, "Snapshot is invalid or belongs to a different display\n");
      return 1;
   }
   bool matches = same_mode(saved.current, current.current) &&
                   saved.path.targetInfo.rotation == current.path.targetInfo.rotation;
   if (!std::wcscmp(argv[1], L"verify")) {
      print_state(current, argv[1], false, matches ? 0 : ERROR_INVALID_DATA);
      return matches ? 0 : 1;
   }
   if (!std::wcscmp(argv[1], L"restore") && matches) {
      print_state(current, argv[1], false, 0);
      return 0;
   }
   if (!visible_desktop()) {
      print_state(current, argv[1], false, ERROR_ACCESS_DENIED);
      return 1;
   }
   result = configure(current, saved, SDC_VALIDATE);
   if (result != ERROR_SUCCESS) {
      print_state(current, argv[1], false, result);
      return 1;
   }
   bool changed = false;
   if (!std::wcscmp(argv[1], L"restore") && !matches) {
      result = configure(current, saved, SDC_APPLY | SDC_SAVE_TO_DATABASE);
      if (result != ERROR_SUCCESS) { print_state(current, argv[1], false, result); return 1; }
      changed = true;
   }
   const snapshot &expected = !std::wcscmp(argv[1], L"test") ? current : saved;
   snapshot after;
   result = read_state(after);
   if (!result && (!same_mode(expected.current, after.current) ||
       expected.path.targetInfo.rotation != after.path.targetInfo.rotation))
      result = ERROR_INVALID_DATA;
   print_state(after, argv[1], changed, result);
   return result ? 1 : 0;
}
