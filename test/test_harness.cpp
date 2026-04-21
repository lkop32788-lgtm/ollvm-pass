/// test_harness.cpp — User-mode functional verification harness
///
/// Compiles and links against test_driver.cpp (with the stub header) to
/// verify that the driver logic works correctly after obfuscation.
///
/// Build (no WDK, no Windows required):
///
///   g++ -std=c++17 -I../include test_harness.cpp test_driver.cpp -o harness
///   ./harness
///
/// Expected output:
///   [PASS] Echo basic
///   [PASS] Echo magic
///   [PASS] Bad IOCTL code
///   [PASS] Null input
///   [PASS] Buffer too small
///   All tests passed.

#include "test_stub.h"
#include <cstdio>
#include <cstring>
#include <cassert>

// ── Bring in the driver logic ──────────────────────────────────────────────
// Forward declarations matching test_driver.cpp symbols.
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);

// ── Helper to build a fake IRP and dispatch it ────────────────────────────
static NTSTATUS DispatchIoctl(PDRIVER_OBJECT Driver,
                              ULONG Code,
                              void *Buf, ULONG InLen, ULONG OutLen,
                              ULONG *BytesWritten) {
  _IRP Irp{};
  Irp.AssociatedIrp.SystemBuffer = Buf;
  Irp.CurrentStackLocation.MajorFunction = IRP_MJ_DEVICE_CONTROL;
  Irp.CurrentStackLocation.Parameters.DeviceIoControl.IoControlCode      = Code;
  Irp.CurrentStackLocation.Parameters.DeviceIoControl.InputBufferLength  = InLen;
  Irp.CurrentStackLocation.Parameters.DeviceIoControl.OutputBufferLength = OutLen;

  auto Dispatch = reinterpret_cast<NTSTATUS(*)(PDEVICE_OBJECT, PIRP)>(
      Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL]);
  NTSTATUS St = Dispatch(Driver->DeviceObject, &Irp);
  *BytesWritten = (ULONG)Irp.IoStatus.Information;
  return St;
}

// ── IOCTL code (must match test_driver.cpp) ────────────────────────────────
#define FILE_DEVICE_UNKNOWN  0x00000022u
#define FILE_ANY_ACCESS      0u
#define METHOD_BUFFERED      0u
#define CTL_CODE(dev,func,meth,acc) \
    (((dev)<<16)|((acc)<<14)|((func)<<2)|(meth))
#define IOCTL_ECHO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ── Test helpers ──────────────────────────────────────────────────────────
static int gPassed = 0, gFailed = 0;

#define EXPECT_EQ(a, b, label)                                          \
  do {                                                                  \
    if ((a) == (b)) { printf("[PASS] %s\n", label); ++gPassed; }       \
    else { printf("[FAIL] %s  (got %d, expected %d)\n",                \
                  label, (int)(a), (int)(b)); ++gFailed; }             \
  } while (0)

int main() {
  // ── Initialise the driver ──────────────────────────────────────────────
  _DRIVER_OBJECT Driver{};
  UNICODE_STRING RegPath{};
  NTSTATUS st = DriverEntry(&Driver, &RegPath);
  if (!NT_SUCCESS(st)) {
    printf("[FAIL] DriverEntry returned 0x%08x\n", (unsigned)st);
    return 1;
  }

  // ── Test 1: Basic echo ─────────────────────────────────────────────────
  {
    char Buf[32] = "Hello, Kernel!";
    ULONG Written = 0;
    st = DispatchIoctl(&Driver, IOCTL_ECHO, Buf, 14, 32, &Written);
    EXPECT_EQ(NT_SUCCESS(st) ? 1 : 0, 1, "Echo basic (status)");
    EXPECT_EQ(Written, 14u, "Echo basic (bytes written)");
    EXPECT_EQ(memcmp(Buf, "Hello, Kernel!", 14), 0, "Echo basic (content)");
  }

  // ── Test 2: Magic constant echo ───────────────────────────────────────
  {
    uint32_t Magic = 0xDEADBEEF;
    ULONG Written = 0;
    st = DispatchIoctl(&Driver, IOCTL_ECHO, &Magic, sizeof(Magic),
                       sizeof(Magic), &Written);
    EXPECT_EQ(NT_SUCCESS(st) ? 1 : 0, 1, "Echo magic (status)");
    EXPECT_EQ(Written, (ULONG)sizeof(Magic), "Echo magic (bytes written)");
    // First byte should be 'M' after the magic path.
    EXPECT_EQ(((char *)&Magic)[0], 'M', "Echo magic (first byte = 'M')");
  }

  // ── Test 3: Bad IOCTL code ────────────────────────────────────────────
  {
    char Buf[4] = "XYZ";
    ULONG Written = 0;
    st = DispatchIoctl(&Driver, 0xDEAD, Buf, 3, 4, &Written);
    EXPECT_EQ(st, (NTSTATUS)STATUS_INVALID_DEVICE_REQUEST, "Bad IOCTL code");
  }

  // ── Test 4: Null input buffer ─────────────────────────────────────────
  {
    ULONG Written = 0;
    st = DispatchIoctl(&Driver, IOCTL_ECHO, nullptr, 0, 4, &Written);
    EXPECT_EQ(st, (NTSTATUS)STATUS_INVALID_PARAMETER, "Null input");
  }

  // ── Test 5: Output buffer too small ──────────────────────────────────
  {
    char Buf[32] = "LongishData1234";
    ULONG Written = 0;
    st = DispatchIoctl(&Driver, IOCTL_ECHO, Buf, 15, 4, &Written);
    EXPECT_EQ(st, (NTSTATUS)STATUS_BUFFER_TOO_SMALL, "Buffer too small");
  }

  // ── Summary ──────────────────────────────────────────────────────────
  printf("\n%d passed, %d failed.\n", gPassed, gFailed);
  if (gFailed == 0) {
    printf("All tests passed.\n");
    return 0;
  }
  return 1;
}
