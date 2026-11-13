/* SPDX-License-Identifier: MIT
 * Manifest-free decoder for tgd3d's versioned numeric and timing events.
 */
#include "tgd3d_trace.h"
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

static const GUID provider = TGD3D_TRACE_GUID_INIT;
static unsigned matched, invalid;
static const WCHAR *live_session;
static ULONGLONG last_state;

static void json_string(const WCHAR *value)
{
   char utf8[2048] = {};
   WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
   std::putchar('"');
   for (const unsigned char *p = (const unsigned char *)utf8; *p; p++) {
      if (*p == '"' || *p == '\\') std::putchar('\\');
      if (*p < 32) std::printf("\\u%04x", *p);
      else std::putchar(*p);
   }
   std::putchar('"');
}

template<typename T> static bool payload(EVENT_RECORD *event, T *out)
{
   if (event->UserDataLength != sizeof(T)) return false;
   std::memcpy(out, event->UserData, sizeof(T));
   return !out->image[31];
}

static void header(const EVENT_HEADER &h, const WCHAR *image)
{
   ++matched;
   std::printf("{\"id\":%u,\"pid\":%lu,\"tid\":%lu,\"time\":%llu,\"image\":",
      h.EventDescriptor.Id, h.ProcessId, h.ThreadId, (unsigned long long)h.TimeStamp.QuadPart);
   json_string(image);
}

static void WINAPI record(EVENT_RECORD *event)
{
   const auto &h = event->EventHeader;
   if (std::memcmp(&h.ProviderId, &provider, sizeof(provider))) return;
   if (h.EventDescriptor.Version != 1) { ++invalid; return; }
   if (h.EventDescriptor.Id == TGD3D_TRACE_ERROR) {
      tgd3d_trace_error p;
      if (!payload(event, &p) || p.file[63]) { ++invalid; return; }
      header(h, p.image);
      std::printf(",\"status\":\"%08x\",\"line\":%u,\"file\":", (unsigned)p.result, p.line);
      json_string(p.file);
   } else if (h.EventDescriptor.Id == TGD3D_TRACE_MESSAGE) {
      tgd3d_trace_message p;
      if (!payload(event, &p) || p.message[255]) { ++invalid; return; }
      header(h, p.image); std::printf(",\"message\":"); json_string(p.message);
   } else if (h.EventDescriptor.Id == TGD3D_TRACE_TIMING) {
      tgd3d_trace_timing p;
      if (!payload(event, &p)) { ++invalid; return; }
      header(h, p.image);
      std::printf(",\"status\":\"%08x\",\"kind\":%u,\"sequence\":%llu,\"completed\":%llu,\"qpc_frequency\":%llu,\"qpc\":[",
         (unsigned)p.result, p.kind, (unsigned long long)p.sequence,
         (unsigned long long)p.completed, (unsigned long long)p.qpc_frequency);
      for (unsigned i = 0; i < 4; i++) std::printf("%s%llu", i ? "," : "", (unsigned long long)p.qpc[i]);
      std::printf("],\"value\":[");
      for (unsigned i = 0; i < 6; i++) std::printf("%s%llu", i ? "," : "", (unsigned long long)p.value[i]);
      std::putchar(']');
   } else {
      tgd3d_trace_data p;
      if (!payload(event, &p)) { ++invalid; return; }
      header(h, p.image);
      std::printf(",\"status\":\"%08x\",\"arg0\":%llu,\"arg1\":%llu",
         (unsigned)p.result, (unsigned long long)p.arg0, (unsigned long long)p.arg1);
   }
   std::puts("}");
}

/* Live-buffer/query handling reused from the existing D3D trace exporter.
 * See tegra-x1-wddm/tools/tgd3d-trial/tool-provenance.json for the exact source snapshot. */
static ULONG live_state(const char *event, ULONG process_status = ERROR_SUCCESS)
{
   struct {
      EVENT_TRACE_PROPERTIES properties;
      WCHAR session[1024], filename[1024];
   } buffer = {};
   auto props = &buffer.properties;
   props->Wnode.BufferSize = sizeof(buffer);
   props->LoggerNameOffset = sizeof(*props);
   props->LogFileNameOffset = sizeof(*props) + sizeof(buffer.session);
   ULONG status = ControlTraceW(0, live_session, props, EVENT_TRACE_CONTROL_QUERY);
   if (!status && !(props->LogFileMode & EVENT_TRACE_REAL_TIME_MODE))
      status = ERROR_INVALID_PARAMETER;
   FILETIME now;
   GetSystemTimeAsFileTime(&now);
   const uint64_t time = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
   std::printf("{\"event\":\"%s\",\"time\":%llu,\"pid\":%lu,\"session\":",
      event, (unsigned long long)time, GetCurrentProcessId());
   json_string(live_session);
   std::printf(",\"query_status\":%lu,\"process_status\":%lu,\"matched\":%u,\"invalid\":%u,"
      "\"events_lost\":%lu,\"log_buffers_lost\":%lu,\"realtime_buffers_lost\":%lu}\n",
      status, process_status, matched, invalid, props->EventsLost,
      props->LogBuffersLost, props->RealTimeBuffersLost);
   last_state = GetTickCount64();
   std::fflush(stdout);
   return status;
}

static ULONG WINAPI live_buffer(EVENT_TRACE_LOGFILEW *)
{
   /* Flush complete records at buffer boundaries, not per printf/event. */
   if (GetTickCount64() - last_state >= 1000) {
      if (live_state("TraceState") != ERROR_SUCCESS)
         return FALSE;
   }
   return std::fflush(stdout) == 0;
}

int wmain(int argc, WCHAR **argv)
{
   const bool follow = argc == 3 && !std::wcscmp(argv[1], L"--follow");
   if (argc != 2 && !follow) {
      std::fprintf(stderr, "usage: tgd3d-trace-export.exe trace.etl | --follow session\n");
      return 2;
   }
   EVENT_TRACE_LOGFILEW log = {};
   log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
   if (follow) {
      live_session = log.LoggerName = argv[2];
      log.ProcessTraceMode |= PROCESS_TRACE_MODE_REAL_TIME;
      log.BufferCallback = live_buffer;
   } else {
      log.LogFileName = argv[1];
   }
   log.EventRecordCallback = record;
   TRACEHANDLE handle = OpenTraceW(&log);
   if (handle == INVALID_PROCESSTRACE_HANDLE) {
      std::fprintf(stderr, "OpenTrace failed: %lu\n", GetLastError()); return 1;
   }
   if (follow && live_state("TraceStart") != ERROR_SUCCESS) {
      CloseTrace(handle);
      return 1;
   }
   const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
   CloseTrace(handle);
   if (follow)
      live_state("TraceEnd", status);
   std::fprintf(stderr, "ProcessTrace=%lu matched=%u invalid=%u events_lost=%lu buffers_lost=%lu end_time=%llu buffers_written=%lu\n",
                status, matched, invalid, log.LogfileHeader.EventsLost, log.LogfileHeader.BuffersLost,
                (unsigned long long)log.LogfileHeader.EndTime.QuadPart, log.LogfileHeader.BuffersWritten);
   return status || invalid || !matched || std::ferror(stdout) ? 1 : 0;
}
