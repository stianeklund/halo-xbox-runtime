/*
 * Standalone compat: XAPILIB asynchronous file IO (fileio.obj).
 *
 * nxdk's winapi has no ReadFileEx / WriteFileEx. Transcribed from
 * cachebeta.xbe: both issue the Nt call with the kernel's
 * NtUserIoApcDispatcher as the APC routine and the caller's completion
 * routine as its context, using the OVERLAPPED block as the IO status block.
 * An NT error status becomes the last error (XapiSetLastNTError, 0x1d2296).
 *
 * CopyFileA (0x1d21f2, kb FUN_001d21f2) is CopyFileExA(existing, new, NULL,
 * NULL, NULL, fail_if_exists ? COPY_FILE_FAIL_IF_EXISTS : 0) in the XDK;
 * nxdk's winapi has the same routine, so it forwards there.
 */
#include "xbox.h"

void __cdecl nxdk_SetLastError(unsigned long error) __asm__("_SetLastError");
int __cdecl nxdk_CopyFileA(const char *existing_file_name, const char *new_file_name,
                           int fail_if_exists) __asm__("_CopyFileA");

/* The kernel export itself, as the original's push [0x25318c] passes it. The
 * vendored header declares NtUserIoApcDispatcher without dllimport, so the
 * bare name is the import library's jmp thunk. Observed on xemu: the kernel delivers
 * the completion as a user APC (ending the alertable wait with
 * WAIT_IO_COMPLETION) only for the export's own address. Given the thunk, it runs
 * as a kernel APC at any point and the wait times out, which fires
 * FUN_001ba930's asserts in the cache copy thread. */
extern PIO_APC_ROUTINE kernel_NtUserIoApcDispatcher __asm__("__imp__NtUserIoApcDispatcher@12");

/* decl.h carries these kb entries; bind the real signatures to the
 * decorated names the legacy trampolines resolve to. */
int __stdcall standalone_ReadFileEx(HANDLE handle, void *buffer, unsigned int size,
                                    void *overlapped, void *completion_routine)
    __asm__("_ReadFileEx@20");
int __stdcall standalone_WriteFileEx(HANDLE handle, const void *buffer, unsigned int size,
                                     void *overlapped, void *completion_routine)
    __asm__("_WriteFileEx@20");

#define OVERLAPPED_OFFSET_LOW 2  /* dword index of Offset in OVERLAPPED */
#define OVERLAPPED_OFFSET_HIGH 3 /* dword index of OffsetHigh */
#define NT_ERROR_MASK 0xc0000000

static LARGE_INTEGER overlapped_offset(void *overlapped)
{
  LARGE_INTEGER offset;

  offset.LowPart = ((unsigned long *)overlapped)[OVERLAPPED_OFFSET_LOW];
  offset.HighPart = (long)((unsigned long *)overlapped)[OVERLAPPED_OFFSET_HIGH];
  return offset;
}

static int finish(NTSTATUS status)
{
  if (((unsigned long)status & NT_ERROR_MASK) == NT_ERROR_MASK) {
    nxdk_SetLastError(RtlNtStatusToDosError(status));
    return 0;
  }
  return 1;
}

/* 0x1d19e7 */
int __stdcall standalone_ReadFileEx(HANDLE handle, void *buffer, unsigned int size,
                                    void *overlapped, void *completion_routine)
{
  LARGE_INTEGER offset;

  offset = overlapped_offset(overlapped);
  return finish(NtReadFile(handle, 0, kernel_NtUserIoApcDispatcher,
                           completion_routine, (PIO_STATUS_BLOCK)overlapped, buffer, size,
                           &offset));
}

/* Not XDK behavior: bring-up diagnostic for the cache copy writing zeros past
 * ~96 MB of a10. Large writes are logged at issue and at completion with the
 * number of zero sectors and a hash sampled from each sector, so the log
 * shows whether the buffer was already zero when issued, was changed while
 * the write was in flight, or completed short. */
#define WRITE_TRACE_MIN_SIZE 0x100000u
#define WRITE_TRACE_SLOTS 4
#define SECTOR_BYTES 512u

typedef void(__stdcall *write_completion_routine)(unsigned long error, unsigned long bytes,
                                                  void *overlapped);

typedef struct write_trace {
  void *overlapped;
  write_completion_routine routine;
  const unsigned long *buffer;
  unsigned int size;
  unsigned long offset;
  unsigned long hash;
} write_trace;

static write_trace write_traces[WRITE_TRACE_SLOTS];

static unsigned long sample_buffer(const unsigned long *buffer, unsigned int size,
                                   unsigned int *zero_sectors)
{
  unsigned long hash;
  unsigned int sector;
  unsigned int words;
  const unsigned long *w;

  hash = 0;
  *zero_sectors = 0;
  words = SECTOR_BYTES / 4;
  for (sector = 0; sector < size / SECTOR_BYTES; sector++) {
    w = buffer + sector * words;
    if (w[0] == 0 && w[1] == 0 && w[words / 2] == 0 && w[words - 1] == 0) {
      (*zero_sectors)++;
    }
    hash = (hash << 5 | hash >> 27) ^ w[0] ^ w[words / 2];
  }
  return hash;
}

static void __stdcall traced_write_complete(unsigned long error, unsigned long bytes,
                                            void *overlapped)
{
  write_trace *trace;
  write_completion_routine routine;
  unsigned long hash;
  unsigned int zero_sectors;
  int i;

  trace = NULL;
  for (i = 0; i < WRITE_TRACE_SLOTS; i++) {
    if (write_traces[i].overlapped == overlapped) {
      trace = &write_traces[i];
      break;
    }
  }
  if (trace == NULL) {
    DbgPrint("[wfx] done: unknown overlapped=%p err=%lu bytes=%lx\n", overlapped, error, bytes);
    return;
  }
  hash = sample_buffer(trace->buffer, trace->size, &zero_sectors);
  DbgPrint("[wfx] done off=%08lx size=%x bytes=%lx err=%lu zero=%u hash=%08lx%s%s\n",
           trace->offset, trace->size, bytes, error, zero_sectors, hash,
           hash != trace->hash ? " CHANGED" : "", bytes != trace->size ? " SHORT" : "");
  routine = trace->routine;
  trace->overlapped = NULL;
  routine(error, bytes, overlapped);
}

static void *trace_write_issue(const void *buffer, unsigned int size, void *overlapped,
                               void *completion_routine, LARGE_INTEGER offset)
{
  write_trace *trace;
  unsigned int zero_sectors;
  int i;

  if (size < WRITE_TRACE_MIN_SIZE) {
    return completion_routine;
  }
  trace = NULL;
  for (i = 0; i < WRITE_TRACE_SLOTS; i++) {
    if (write_traces[i].overlapped == NULL) {
      trace = &write_traces[i];
      break;
    }
  }
  if (trace == NULL) {
    DbgPrint("[wfx] no free trace slot off=%08lx\n", offset.LowPart);
    return completion_routine;
  }
  trace->overlapped = overlapped;
  trace->routine = (write_completion_routine)completion_routine;
  trace->buffer = (const unsigned long *)buffer;
  trace->size = size;
  trace->offset = offset.LowPart;
  trace->hash = sample_buffer(trace->buffer, size, &zero_sectors);
  DbgPrint("[wfx] issue off=%08lx size=%x buf=%p zero=%u hash=%08lx first=%08lx\n",
           trace->offset, size, buffer, zero_sectors, trace->hash, trace->buffer[0]);
  return (void *)traced_write_complete;
}

/* 0x1d1a38 */
int __stdcall standalone_WriteFileEx(HANDLE handle, const void *buffer, unsigned int size,
                                     void *overlapped, void *completion_routine)
{
  LARGE_INTEGER offset;

  offset = overlapped_offset(overlapped);
  completion_routine = trace_write_issue(buffer, size, overlapped, completion_routine, offset);
  return finish(NtWriteFile(handle, 0, kernel_NtUserIoApcDispatcher,
                            completion_routine, (PIO_STATUS_BLOCK)overlapped, (void *)buffer,
                            size, &offset));
}

/* 0x1d0ee1 GetFileAttributesExA (kb FUN_001d0ee1; files.c calls it by
 * address). The information level is not examined: the original always
 * fills a WIN32_FILE_ATTRIBUTE_DATA from NtQueryFullAttributesFile. */
int __stdcall standalone_GetFileAttributesExA(const char *path, int level, void *data)
    __asm__("_FUN_001d0ee1@12");

void __stdcall standalone_fileio_XapiSetLastNTError(NTSTATUS status)
    __asm__("_XapiSetLastNTError@4");

int __stdcall standalone_GetFileAttributesExA(const char *path, int level, void *data)
{
  ANSI_STRING name;
  OBJECT_ATTRIBUTES attributes;
  FILE_NETWORK_OPEN_INFORMATION info;
  unsigned long *out;
  NTSTATUS status;

  (void)level;
  RtlInitAnsiString(&name, path);
  attributes.RootDirectory = ObDosDevicesDirectory();
  attributes.ObjectName = &name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  status = NtQueryFullAttributesFile(&attributes, &info);
  if (status < 0) {
    standalone_fileio_XapiSetLastNTError(status);
    return 0;
  }
  out = (unsigned long *)data;
  out[0] = info.FileAttributes;
  out[1] = info.CreationTime.LowPart;
  out[2] = (unsigned long)info.CreationTime.HighPart;
  out[3] = info.LastAccessTime.LowPart;
  out[4] = (unsigned long)info.LastAccessTime.HighPart;
  out[5] = info.LastWriteTime.LowPart;
  out[6] = (unsigned long)info.LastWriteTime.HighPart;
  out[7] = (unsigned long)info.EndOfFile.HighPart;
  out[8] = info.EndOfFile.LowPart;
  return 1;
}

int __stdcall standalone_CopyFileA(const char *existing_file_name, const char *new_file_name,
                                   int fail_if_exists) __asm__("_FUN_001d21f2@12");

int __stdcall standalone_CopyFileA(const char *existing_file_name, const char *new_file_name,
                                   int fail_if_exists)
{
  return nxdk_CopyFileA(existing_file_name, new_file_name, fail_if_exists != 0);
}
