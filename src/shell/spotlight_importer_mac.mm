// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 15: MediaViewerSpotlight.mdimporter, the Mac twin of the Explorer
// property handler (plan/10 PR 15, plan/12 2026-09-25). A CFPlugIn Spotlight
// importer for the D5 clip containers macOS does not index (Matroska, WebM,
// AVI, MPEG-TS; packaging/macos/Spotlight-Info.plist.in).
//
// Out of process by construction: mdworker loads it, never Finder or the
// app. It reads the file through the shared read model (meta::read: headers
// only, a bounded probe) and reports what shell/spotlight_fields.h decides.
// No state between files, no cache, nothing logged about a file (rule 6).
#import <CoreFoundation/CFPlugInCOM.h>
#import <CoreServices/CoreServices.h>
#include <os/log.h>

#include <string>

#include "meta/meta.h"
#include "shell/spotlight_fields.h"

namespace {

// One line per file: the outcome as a status name and a field count. Never a
// path, a name or a value (rule 6).
os_log_t importer_log() {
  static os_log_t log = os_log_create("io.github.longtimeno-c.mediaviewer.spotlight", "import");
  return log;
}

// 5C0E7B2A-94D1-4F63-A8E5-2B7C1D9F0E46: the factory (Spotlight-Info.plist.in).
CFUUIDRef factory_id() {
  return CFUUIDGetConstantUUIDWithBytes(kCFAllocatorDefault, 0x5C, 0x0E, 0x7B, 0x2A, 0x94, 0xD1, 0x4F, 0x63, 0xA8,
                                        0xE5, 0x2B, 0x7C, 0x1D, 0x9F, 0x0E, 0x46);
}

CFTypeRef make_value(const mv::shell::spotlight_field& f) {
  using kind = mv::shell::spotlight_field::kind;
  switch (f.type) {
    case kind::number:
      return CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &f.number);
    case kind::text:
      return CFStringCreateWithCString(kCFAllocatorDefault, f.text.c_str(), kCFStringEncodingUTF8);
    case kind::texts: {
      CFMutableArrayRef a = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
      for (const std::string& t : f.texts) {
        if (CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, t.c_str(), kCFStringEncodingUTF8)) {
          CFArrayAppendValue(a, s);
          CFRelease(s);
        }
      }
      return a;
    }
    case kind::date:
      return CFDateCreate(kCFAllocatorDefault,
                          static_cast<CFAbsoluteTime>(f.unix_seconds) - kCFAbsoluteTimeIntervalSince1970);
  }
  return nullptr;
}

Boolean get_metadata(void*, CFMutableDictionaryRef attributes, CFStringRef, CFStringRef path) {
  if (!attributes || !path) return false;
  char utf8[PATH_MAX * 2];
  if (!CFStringGetFileSystemRepresentation(path, utf8, sizeof(utf8))) return false;
  try {
    auto m = mv::meta::read(utf8);
    if (!m) {
      os_log(importer_log(), "read failed: %{public}s", mv::status_name(m.error()));
      return false;
    }
    const auto fields = mv::shell::spotlight_fields(m.value());
    os_log_debug(importer_log(), "clip=%d fields=%zu", m->is_clip ? 1 : 0, fields.size());
    for (const auto& f : fields) {
      CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, f.key.c_str(), kCFStringEncodingUTF8);
      CFTypeRef value = make_value(f);
      if (key && value) CFDictionarySetValue(attributes, key, value);
      if (key) CFRelease(key);
      if (value) CFRelease(value);
    }
    return !fields.empty();
  } catch (...) {
    return false;  // never an exception into mdworker
  }
}

// ---- The CFPlugIn COM shape Spotlight loads (Apple's importer template) -----
struct importer;
HRESULT query_interface(void* self, REFIID iid, LPVOID* out);
ULONG add_ref(void* self);
ULONG release(void* self);

MDImporterInterfaceStruct g_vtable = {nullptr, query_interface, add_ref, release, get_metadata};

struct importer {
  MDImporterInterfaceStruct* vtable;
  CFUUIDRef factory;
  UInt32 refs;
};

importer* make_importer(CFUUIDRef factory) {
  auto* p = static_cast<importer*>(malloc(sizeof(importer)));
  if (!p) return nullptr;
  p->vtable = &g_vtable;
  p->factory = static_cast<CFUUIDRef>(CFRetain(factory));
  CFPlugInAddInstanceForFactory(factory);
  p->refs = 1;
  return p;
}

HRESULT query_interface(void* self, REFIID iid, LPVOID* out) {
  CFUUIDRef wanted = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, iid);
  const bool ok = CFEqual(wanted, kMDImporterInterfaceID) || CFEqual(wanted, IUnknownUUID);
  CFRelease(wanted);
  if (!ok) {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  add_ref(self);
  *out = self;
  return S_OK;
}

ULONG add_ref(void* self) { return ++static_cast<importer*>(self)->refs; }

ULONG release(void* self) {
  auto* p = static_cast<importer*>(self);
  if (--p->refs > 0) return p->refs;
  CFPlugInRemoveInstanceForFactory(p->factory);
  CFRelease(p->factory);
  free(p);
  return 0;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void* MetadataImporterPluginFactory(CFAllocatorRef,
                                                                                      CFUUIDRef type) {
  if (!CFEqual(type, kMDImporterTypeID)) return nullptr;
  return make_importer(factory_id());
}
