#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

intptr_t standalone_XFindFirstNickname(int, uint16_t *, int);
int standalone_XSetNicknameW(const uint16_t *, int);

static unsigned char disk[50 * 68];
static unsigned long disk_size;
static int exists, allocations, fail_read, fail_write, fail_alloc;

void *nick_open(const char *path, unsigned long access, unsigned long share,
                void *security, unsigned long disposition, unsigned long flags,
                void *template_file)
{
  (void)access; (void)share; (void)security; (void)flags; (void)template_file;
  assert(strcmp(path, "E:\\UDATA\\NICKNAME.XBN") == 0);
  if (disposition == 3 && !exists) return (void *)-1;
  if (disposition == 2) { exists = 1; disk_size = 0; }
  return (void *)1;
}
int nick_read(void *file, void *to, unsigned long size, unsigned long *read, void *ov)
{
  (void)file; (void)ov;
  *read = size < disk_size ? size : disk_size;
  memcpy(to, disk, *read);
  return !fail_read;
}
int nick_write(void *file, const void *from, unsigned long size, unsigned long *written, void *ov)
{
  (void)file; (void)ov;
  assert(size == sizeof(disk));
  *written = fail_write ? 0 : size;
  memcpy(disk, from, *written);
  disk_size = *written;
  return !fail_write;
}
int nick_close(void *file) { assert(file == (void *)1); return 1; }
void *nick_alloc(unsigned int flags, unsigned int size)
{
  void *p;
  (void)flags;
  assert(size == 0xd54);
  if (fail_alloc) return 0;
  p = malloc(size);
  if (p) allocations++;
  return p;
}
void *nick_free(void *p) { assert(p); free(p); allocations--; return 0; }

static void close_find(intptr_t handle)
{
  /* This is the 'NICK' branch of the production XFindClose dispatcher. */
  assert(handle != -1 && handle != 0);
  assert(*(uint32_t *)handle == 0x4e49434b);
  nick_free((void *)handle);
}

int main(void)
{
  uint16_t name[32], first[] = {'S','p','a','r','t','a','n',0};
  uint16_t second[] = {'C','h','i','e','f',0};
  intptr_t h;
  assert(standalone_XFindFirstNickname(0, name, 32) == -1);
  assert(standalone_XSetNicknameW(first, 1));
  assert(disk_size == 0xd48);
  h = standalone_XFindFirstNickname(0, name, 32);
  assert(memcmp(name, first, sizeof(first)) == 0);
  close_find(h);
  assert(standalone_XSetNicknameW(second, 1));
  h = standalone_XFindFirstNickname(1, name, 32);
  assert(memcmp(name, second, sizeof(second)) == 0);
  close_find(h);
  assert(standalone_XFindFirstNickname(0, name, 3) == -1);
  memset(disk + 4, 'x', 64); /* unterminated first record is skipped */
  h = standalone_XFindFirstNickname(0, name, 32);
  assert(memcmp(name, first, sizeof(first)) == 0);
  close_find(h);
  fail_alloc = 1;
  assert(standalone_XFindFirstNickname(0, name, 32) == -1);
  fail_alloc = 0;
  fail_read = 1;
  assert(standalone_XFindFirstNickname(0, name, 32) == -1);
  fail_read = 0;
  disk_size--;
  assert(standalone_XFindFirstNickname(0, name, 32) == -1);
  fail_write = 1;
  assert(!standalone_XSetNicknameW(second, 1));
  assert(!standalone_XSetNicknameW(0, 1));
  assert(!allocations);
  return 0;
}
