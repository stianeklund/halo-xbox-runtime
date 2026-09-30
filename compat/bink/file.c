/*
 * Standalone compat: RAD Bink file IO backend (BinkFileOpen, cachebeta.xbe
 * 0x23f3a0).
 *
 * Reads the movie through a 4KB-page read-ahead ring that the RADCB IO
 * thread fills; frame reads drain the ring first and read the rest
 * directly. The state lives in BINKIO's 128-byte backend area.
 */
#include "internal.h"

#define INVALID_HANDLE ((void *)-1)
#define GENERIC_READ 0x80000000ul
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_FLAGS 0x08000080ul /* NORMAL | SEQUENTIAL_SCAN */
#define FILE_PAGE_SIZE 4096
#define FILE_OPEN_HANDLE 0x800000 /* BINKFILEHANDLE: name is an open handle */

/* BinkFileOpen's state in BINKIO's 128-byte backend area (io->pad_4c). */
struct bink_file_state {
  void *handle;
  unsigned long read_position;    /* file offset of the next ring fill */
  unsigned long logical_position; /* file offset the caller reads next */
  unsigned char *read_buffer;
  volatile unsigned long empty_bytes;
  unsigned long reserved_14;
  unsigned char *buffer;
  unsigned char *buffer_end;
  unsigned char *write_buffer;
  unsigned long borrowed_handle;
  unsigned long start_offset;
  unsigned long file_size;
};
typedef char bink_file_state_fits[sizeof(struct bink_file_state) <= 0x80 ? 1 : -1];

#define FILE_STATE(io) ((struct bink_file_state *)(io)->pad_4c)

static void locked_add(volatile unsigned long *value, unsigned long amount)
{
  __atomic_fetch_add(value, amount, __ATOMIC_SEQ_CST);
}

static void locked_sub(volatile unsigned long *value, unsigned long amount)
{
  __atomic_fetch_sub(value, amount, __ATOMIC_SEQ_CST);
}

static unsigned long bink_file_read(struct bink_file_state *f, void *destination, unsigned long size)
{
  unsigned long read = 0;

  if (!rad_ReadFile(f->handle, destination, size, &read, 0)) {
    return 0;
  }
  return read;
}

static void bink_file_seek(struct bink_file_state *f, unsigned long offset)
{
  rad_SetFilePointer(f->handle, (long)(f->start_offset + offset), 0, FILE_BEGIN);
}

static unsigned long __stdcall bink_file_read_header(BINKIO *io, long offset, void *destination,
                                                unsigned long size)
{
  struct bink_file_state *f = FILE_STATE(io);
  unsigned long read, remaining;

  if (io->suspend_callback) {
    io->suspend_callback(io);
  }
  if (offset != -1 && f->read_position != (unsigned long)offset) {
    bink_file_seek(f, (unsigned long)offset);
    f->read_position = (unsigned long)offset;
  }
  read = bink_file_read(f, destination, size);
  f->read_position += read;
  f->logical_position = f->read_position;
  remaining = f->file_size - f->logical_position;
  io->CurBufSize = remaining < io->BufSize ? remaining : io->BufSize;
  if (io->resume_callback) {
    io->resume_callback(io);
  }
  return read;
}

static unsigned long __stdcall bink_file_read_frame(BINKIO *io, unsigned long frame, long offset,
                                               void *destination, unsigned long size)
{
  struct bink_file_state *f = FILE_STATE(io);
  unsigned char *out = destination;
  unsigned long suspended = 0, total = 0, started, read, remaining;

  (void)frame;
  if (io->ReadError) {
    return 0;
  }
  started = FUN_002328c0();
  if (offset != -1 && f->logical_position != (unsigned long)offset) {
    suspended = 1;
    if (io->suspend_callback) {
      io->suspend_callback(io);
    }
    if ((unsigned long)offset > f->logical_position &&
        (unsigned long)offset <= f->read_position) {
      unsigned long skip = (unsigned long)offset - f->logical_position;
      f->logical_position = (unsigned long)offset;
      f->empty_bytes += skip;
      io->CurBufUsed -= skip;
      f->read_buffer += skip;
      if (f->read_buffer >= f->buffer_end) {
        f->read_buffer -= io->BufSize;
      }
    } else {
      bink_file_seek(f, (unsigned long)offset);
      f->read_position = (unsigned long)offset;
      f->logical_position = (unsigned long)offset;
      f->empty_bytes = io->BufSize;
      io->CurBufUsed = 0;
      f->read_buffer = f->buffer;
      f->write_buffer = f->buffer;
    }
  }
  /* Take what the read-ahead ring holds; suspend once and retry so a read
   * the IO thread just finished is used instead of hitting the disc. */
  for (;;) {
    unsigned long buffered = io->CurBufUsed;
    if (buffered) {
      unsigned long tail;
      if (buffered > size) {
        buffered = size;
      }
      size -= buffered;
      f->logical_position += buffered;
      total += buffered;
      tail = (unsigned long)(f->buffer_end - f->read_buffer);
      if (tail <= buffered) {
        memcpy(out, f->read_buffer, tail);
        out += tail;
        buffered -= tail;
        f->read_buffer = f->buffer;
        locked_sub(&io->CurBufUsed, tail);
        locked_add(&f->empty_bytes, tail);
      }
      if (buffered) {
        memcpy(out, f->read_buffer, buffered);
        out += buffered;
        f->read_buffer += buffered;
        locked_sub(&io->CurBufUsed, buffered);
        locked_add(&f->empty_bytes, buffered);
      }
    }
    if (!size || suspended) {
      break;
    }
    suspended = 1;
    if (io->suspend_callback) {
      io->suspend_callback(io);
    }
  }
  if (size) {
    unsigned long read_started = FUN_002328c0(), finished;
    /* The ring is empty here, so the file pointer is at logical_position. */
    read = bink_file_read(f, out, size);
    if (read < size) {
      io->ReadError = 1;
    }
    f->read_position += read;
    f->logical_position += read;
    io->BytesRead += read;
    total += read;
    finished = FUN_002328c0();
    io->TotalTime += finished - read_started;
    io->ForegroundTime += finished - started;
  } else {
    io->ForegroundTime += FUN_002328c0() - started;
  }
  remaining = f->file_size - f->logical_position;
  io->CurBufSize = remaining < io->BufSize ? remaining : io->BufSize;
  if (io->CurBufUsed + FILE_PAGE_SIZE > io->CurBufSize) {
    io->CurBufSize = io->CurBufUsed;
  }
  if (suspended && io->resume_callback) {
    io->resume_callback(io);
  }
  return total;
}

static unsigned long __stdcall bink_file_get_buffer_size(BINKIO *io, unsigned long size)
{
  (void)io;
  return (size + FILE_PAGE_SIZE - 1) / FILE_PAGE_SIZE * FILE_PAGE_SIZE;
}

static void __stdcall bink_file_set_info(BINKIO *io, void *buffer, unsigned long size,
                                    unsigned long file_size, unsigned long simulate)
{
  struct bink_file_state *f = FILE_STATE(io);

  (void)simulate; /* rate simulation is a RAD debugging aid */
  if (io->suspend_callback) {
    io->suspend_callback(io);
  }
  size = size / FILE_PAGE_SIZE * FILE_PAGE_SIZE;
  f->buffer = buffer;
  f->read_buffer = buffer;
  f->write_buffer = buffer;
  f->buffer_end = (unsigned char *)buffer + size;
  io->BufSize = size;
  f->empty_bytes = size;
  io->CurBufUsed = 0;
  f->file_size = file_size;
  if (io->resume_callback) {
    io->resume_callback(io);
  }
}

static void __stdcall bink_file_close(BINKIO *io)
{
  struct bink_file_state *f = FILE_STATE(io);

  if (io->suspend_callback) {
    io->suspend_callback(io);
  }
  if (!f->borrowed_handle) {
    rad_CloseHandle(f->handle);
  }
  if (io->resume_callback) {
    io->resume_callback(io);
  }
}

/* Reads one page into the read-ahead ring; returns bytes read, or -1 when
 * the foreground holds the IO lock. */
static unsigned long __stdcall bink_file_idle(BINKIO *io)
{
  struct bink_file_state *f = FILE_STATE(io);
  unsigned long working = io->Working, read = 0;

  if (io->ReadError) {
    return 0;
  }
  if (io->try_suspend_callback && io->try_suspend_callback(io)) {
    unsigned long remaining = f->file_size - f->read_position;
    if (f->buffer && f->empty_bytes >= FILE_PAGE_SIZE && remaining >= FILE_PAGE_SIZE) {
      unsigned long started = FUN_002328c0(), elapsed;
      io->DoingARead = 1;
      read = bink_file_read(f, f->write_buffer, FILE_PAGE_SIZE);
      io->DoingARead = 0;
      if (read < FILE_PAGE_SIZE) {
        io->ReadError = 1;
      }
      io->BytesRead += read;
      f->read_position += read;
      f->write_buffer += read;
      if (f->write_buffer >= f->buffer_end) {
        f->write_buffer = f->buffer;
      }
      locked_sub(&f->empty_bytes, read);
      locked_add(&io->CurBufUsed, read);
      if (io->CurBufUsed > io->BufHighUsed) {
        io->BufHighUsed = io->CurBufUsed;
      }
      elapsed = FUN_002328c0() - started;
      io->TotalTime += elapsed;
      if (!working && !io->Working) {
        io->IdleTime += elapsed;
      } else {
        io->ThreadTime += elapsed;
      }
    } else {
      io->CurBufSize = io->CurBufUsed;
    }
    if (io->resume_callback) {
      io->resume_callback(io);
    }
  } else {
    if (io->idle_on_callback) {
      io->idle_on_callback(io);
    }
    read = 0xfffffffful;
  }
  return read;
}

/* 0x23f3a0 */
long __stdcall BinkFileOpen(BINKIO *io, const char *name, unsigned long flags)
{
  struct bink_file_state *f = FILE_STATE(io);

  csmemset(io, 0, sizeof(*io));
  if (flags & FILE_OPEN_HANDLE) {
    f->handle = (void *)name;
    f->borrowed_handle = 1;
    f->start_offset = rad_SetFilePointer(f->handle, 0, 0, FILE_CURRENT);
  } else {
    f->handle = rad_CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                                FILE_FLAGS, 0);
    if (f->handle == INVALID_HANDLE) {
      f->handle = rad_CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                  OPEN_EXISTING, FILE_FLAGS, 0);
      if (f->handle == INVALID_HANDLE) {
        return 0;
      }
    }
  }
  io->ReadHeader = bink_file_read_header;
  io->ReadFrame = bink_file_read_frame;
  io->GetBufferSize = bink_file_get_buffer_size;
  io->SetInfo = bink_file_set_info;
  io->Idle = bink_file_idle;
  io->Close = bink_file_close;
  return 1;
}
