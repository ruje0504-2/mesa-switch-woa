/* SPDX-License-Identifier: MIT
 * Manifest-free decoder for tgd3d's versioned numeric and timing events.
 */
#include "tgd3d_trace.h"
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstring>

static const GUID provider = TGD3D_TRACE_GUID_INIT;
static unsigned matched, invalid;

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

int wmain(int argc, WCHAR **argv)
{
   if (argc != 2) { std::fprintf(stderr, "usage: tgd3d-trace-export.exe trace.etl\n"); return 2; }
   EVENT_TRACE_LOGFILEW log = {};
   log.LogFileName = argv[1]; log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
   log.EventRecordCallback = record;
   TRACEHANDLE trace = OpenTraceW(&log);
   if (trace == INVALID_PROCESSTRACE_HANDLE) return 1;
   ULONG status = ProcessTrace(&trace, 1, nullptr, nullptr); CloseTrace(trace);
   std::fprintf(stderr, "status=%lu matched=%u invalid=%u events_lost=%lu buffers_lost=%lu\n",
      status, matched, invalid, log.LogfileHeader.EventsLost, log.LogfileHeader.BuffersLost);
   return status || !matched || invalid || log.LogfileHeader.EventsLost || log.LogfileHeader.BuffersLost ? 1 : 0;
}
