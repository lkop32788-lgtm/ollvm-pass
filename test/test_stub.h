/// test_stub.h — Minimal WDK-like stubs for building / testing on non-Windows
///               platforms (Linux CI, macOS) without the full WDK installed.
///
/// This header is only included when _WIN32 is NOT defined.  It provides
/// enough definitions for the test_driver.cpp file to compile and allows
/// user-mode unit tests to exercise the driver logic through a thin
/// emulation shim.

#pragma once

#include <cstdint>
#include <cstring>
#include <cwchar>

// ── Primitive types ────────────────────────────────────────────────────────
using NTSTATUS  = int32_t;
using ULONG     = uint32_t;
using ULONG_PTR = uintptr_t;
using USHORT    = uint16_t;
using UCHAR     = uint8_t;
using BOOLEAN   = uint8_t;
using PVOID     = void *;
using PCHAR     = char *;
using PWSTR     = wchar_t *;
using PULONG    = ULONG *;
using BOOL      = int;

#define FALSE 0
#define TRUE  1

// ── Status codes ──────────────────────────────────────────────────────────
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010L)
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001L)

// ── IOCTL macro ───────────────────────────────────────────────────────────
#define FILE_DEVICE_UNKNOWN  0x00000022u
#define FILE_DEVICE_SECURE_OPEN 0x100u
#define FILE_ANY_ACCESS      0u
#define METHOD_BUFFERED      0u
#define CTL_CODE(dev,func,meth,acc) \
    (((dev)<<16)|((acc)<<14)|((func)<<2)|(meth))

// ── IRP and stack structures (minimal) ────────────────────────────────────
struct IO_STACK_LOCATION_DevCtl {
  ULONG OutputBufferLength;
  ULONG InputBufferLength;
  ULONG IoControlCode;
  PVOID Type3InputBuffer;
};

struct IO_STACK_LOCATION_Params {
  IO_STACK_LOCATION_DevCtl DeviceIoControl;
};

struct _IO_STACK_LOCATION {
  UCHAR MajorFunction;
  IO_STACK_LOCATION_Params Parameters;
};
using IO_STACK_LOCATION  = _IO_STACK_LOCATION;
using PIO_STACK_LOCATION = _IO_STACK_LOCATION *;

struct _IRP {
  struct {
    NTSTATUS Status;
    ULONG_PTR Information;
  } IoStatus;
  union {
    PVOID SystemBuffer;
  } AssociatedIrp;
  IO_STACK_LOCATION CurrentStackLocation;
};
using IRP  = _IRP;
using PIRP = _IRP *;

static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp) {
  return &Irp->CurrentStackLocation;
}

#define IO_NO_INCREMENT 0

static inline void IoCompleteRequest(PIRP /*Irp*/, int /*Boost*/) {}

// ── UNICODE_STRING ────────────────────────────────────────────────────────
struct _UNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR  Buffer;
};
using UNICODE_STRING  = _UNICODE_STRING;
using PUNICODE_STRING = _UNICODE_STRING *;

static inline void RtlInitUnicodeString(PUNICODE_STRING DestinationString,
                                        const wchar_t *SourceString) {
  if (SourceString) {
    size_t len = wcslen(SourceString);
    DestinationString->Length        = (USHORT)(len * sizeof(wchar_t));
    DestinationString->MaximumLength = (USHORT)((len + 1) * sizeof(wchar_t));
    DestinationString->Buffer        = const_cast<PWSTR>(SourceString);
  } else {
    DestinationString->Length = DestinationString->MaximumLength = 0;
    DestinationString->Buffer = nullptr;
  }
}

// ── Device / Driver objects (minimal) ─────────────────────────────────────
struct _DEVICE_OBJECT;
using PDEVICE_OBJECT = _DEVICE_OBJECT *;

// Dispatch function type
using DRIVER_DISPATCH_FN = NTSTATUS(PDEVICE_OBJECT, struct _IRP *);

struct _DRIVER_OBJECT {
  PDEVICE_OBJECT DeviceObject;
  DRIVER_DISPATCH_FN *MajorFunction[28];
  void (*DriverUnload)(struct _DRIVER_OBJECT *);
};
using DRIVER_OBJECT  = _DRIVER_OBJECT;
using PDRIVER_OBJECT = _DRIVER_OBJECT *;

struct _DEVICE_OBJECT {
  PDRIVER_OBJECT DriverObject;
};

// These are type-macros in the WDK; on the stub they expand to the return type.
#define DRIVER_UNLOAD    void __cdecl
#define DRIVER_DISPATCH  NTSTATUS __cdecl

#define IRP_MJ_CREATE          0x00
#define IRP_MJ_CLOSE           0x02
#define IRP_MJ_DEVICE_CONTROL  0x0E

static inline NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject,
                                      ULONG /*ExtSize*/,
                                      PUNICODE_STRING /*Name*/,
                                      ULONG /*DevType*/,
                                      ULONG /*Flags*/,
                                      BOOLEAN /*Exclusive*/,
                                      PDEVICE_OBJECT *DeviceObject) {
  static _DEVICE_OBJECT FakeDevice;
  FakeDevice.DriverObject = DriverObject;
  *DeviceObject = &FakeDevice;
  return STATUS_SUCCESS;
}

static inline NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING /*Link*/,
                                            PUNICODE_STRING /*Target*/) {
  return STATUS_SUCCESS;
}

static inline void IoDeleteSymbolicLink(PUNICODE_STRING /*Link*/) {}
static inline void IoDeleteDevice(PDEVICE_OBJECT /*DevObj*/) {}

static inline void RtlCopyMemory(PVOID dst, const PVOID src, ULONG len) {
  memcpy(dst, src, len);
}

#define UNREFERENCED_PARAMETER(p) ((void)(p))
