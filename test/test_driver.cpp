///
/// test_driver.cpp — Minimal Windows kernel driver used for testing
///                   all six obfuscation passes end-to-end.
///
/// This file is compiled first WITHOUT the obfuscation plugin so we can
/// verify that the IR is well-formed; then WITH the plugin to verify that
/// the resulting binary is still functionally correct (via a user-mode
/// emulation harness in test_harness.cpp).
///
/// Build command (Windows, clang-cl):
///
///   clang-cl -O2 -target x86_64-pc-windows-msvc                       \
///            -fpass-plugin=../build/OllvmPass.dll                      \
///            -mllvm -passes="ollvm-all"                                \
///            /kernel /GS- /GL- /W3 test_driver.cpp -o test_driver.sys
///
/// The driver implements a trivial device that echoes back any data
/// written to it via IOCTL so functional correctness is easy to verify.

// ── Kernel headers ─────────────────────────────────────────────────────────
// When building with the WDK these are available.  For unit testing on
// Linux/macOS without WDK, we use a thin stub header (test_stub.h).
#ifdef _WIN32
#include <ntddk.h>
#else
#include "test_stub.h"
#endif

// ── Driver constants ───────────────────────────────────────────────────────
#define OLLVM_DEVICE_NAME L"\\Device\\OllvmTestDevice"
#define OLLVM_SYMLINK_NAME L"\\DosDevices\\OllvmTestDevice"
#define IOCTL_ECHO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ── Forward declarations ───────────────────────────────────────────────────
static NTSTATUS OllvmDispatchCreate(PDEVICE_OBJECT, PIRP);
static NTSTATUS OllvmDispatchClose(PDEVICE_OBJECT, PIRP);
static NTSTATUS OllvmDispatchDeviceControl(PDEVICE_OBJECT, PIRP);
static void     OllvmUnload(PDRIVER_OBJECT);

// ── Global state ──────────────────────────────────────────────────────────
static PDEVICE_OBJECT g_DeviceObject = nullptr;

// ── String that will be encrypted by the StringEncryption pass ────────────
static const char kBannerMsg[] = "OllvmTestDriver loaded successfully.\n";

// ── A function with non-trivial control flow (exercises CFF pass) ─────────
static NTSTATUS ProcessRequest(ULONG Code, PVOID InputBuf, ULONG InputLen,
                               PVOID OutputBuf, ULONG OutputLen,
                               PULONG BytesWritten) {
  *BytesWritten = 0;

  // Constant that will be obfuscated by the ConstantEncryption pass.
  const ULONG Magic = 0xDEADBEEF;

  if (Code != IOCTL_ECHO)
    return STATUS_INVALID_DEVICE_REQUEST;

  if (InputBuf == nullptr || InputLen == 0)
    return STATUS_INVALID_PARAMETER;

  if (OutputBuf == nullptr || OutputLen < InputLen)
    return STATUS_BUFFER_TOO_SMALL;

  // Copy echo data — arithmetic here will be substituted by InstructionSub.
  ULONG toCopy = InputLen;
  if (toCopy > OutputLen)
    toCopy = OutputLen;

  RtlCopyMemory(OutputBuf, InputBuf, toCopy);
  *BytesWritten = toCopy;

  // Use the magic constant (exercises ConstantEncryption).
  if (*(PULONG)OutputBuf == Magic) {
    // Special MAGIC echo — uppercase first byte.
    ((PCHAR)OutputBuf)[0] = 'M';
  }

  return STATUS_SUCCESS;
}

// ── A helper whose pointer will be placed in the indirect dispatch table ──
static void LogMessage(const char *Msg) {
  UNREFERENCED_PARAMETER(Msg);
  // DbgPrint("%s", Msg);   // commented out to avoid kernel log dependency
}

// ── IRP dispatch routines ─────────────────────────────────────────────────
static NTSTATUS OllvmDispatchCreate(PDEVICE_OBJECT /*DeviceObject*/, PIRP Irp) {
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

static NTSTATUS OllvmDispatchClose(PDEVICE_OBJECT /*DeviceObject*/, PIRP Irp) {
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

static NTSTATUS OllvmDispatchDeviceControl(PDEVICE_OBJECT /*DeviceObject*/, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  ULONG Code     = Stack->Parameters.DeviceIoControl.IoControlCode;
  ULONG InputLen = Stack->Parameters.DeviceIoControl.InputBufferLength;
  ULONG OutputLen= Stack->Parameters.DeviceIoControl.OutputBufferLength;
  PVOID Buf      = Irp->AssociatedIrp.SystemBuffer;
  ULONG Written  = 0;

  // Call via a function pointer so IndirectBranching has something to work on.
  LogMessage(kBannerMsg);

  NTSTATUS Status = ProcessRequest(Code, Buf, InputLen, Buf, OutputLen, &Written);

  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Written;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

// ── Unload ────────────────────────────────────────────────────────────────
static void OllvmUnload(PDRIVER_OBJECT DriverObject) {
  UNICODE_STRING symlink;
  RtlInitUnicodeString(&symlink, OLLVM_SYMLINK_NAME);
  IoDeleteSymbolicLink(&symlink);
  IoDeleteDevice(DriverObject->DeviceObject);
}

// ── DriverEntry ──────────────────────────────────────────────────────────
extern "C"
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING /*RegistryPath*/) {
  UNICODE_STRING devName, symName;
  RtlInitUnicodeString(&devName, OLLVM_DEVICE_NAME);
  RtlInitUnicodeString(&symName, OLLVM_SYMLINK_NAME);

  NTSTATUS Status = IoCreateDevice(
      DriverObject,
      0,
      &devName,
      FILE_DEVICE_UNKNOWN,
      FILE_DEVICE_SECURE_OPEN,
      FALSE,
      &g_DeviceObject);

  if (!NT_SUCCESS(Status))
    return Status;

  Status = IoCreateSymbolicLink(&symName, &devName);
  if (!NT_SUCCESS(Status)) {
    IoDeleteDevice(g_DeviceObject);
    return Status;
  }

  DriverObject->DriverUnload = OllvmUnload;
  DriverObject->MajorFunction[IRP_MJ_CREATE]         = OllvmDispatchCreate;
  DriverObject->MajorFunction[IRP_MJ_CLOSE]          = OllvmDispatchClose;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = OllvmDispatchDeviceControl;

  return STATUS_SUCCESS;
}
