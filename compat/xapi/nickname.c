/* Cachebeta XAPI nicknames: 0x1d26f3 (set), 0x1d29eb (find first).
 * NICKNAME.XBN holds 50 {title_id, UTF-16 name[32]} records. Find handles
 * start with 'NICK', as required by XFindClose at 0x1d33a2. A failed find
 * returns -1, never the null handle produced by a generic missing stub.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef HALO_NICKNAME_HOST_TEST
#define __stdcall
#define __cdecl
#define NICK_SYMBOL(s)
#else
#define NICK_SYMBOL(s) __asm__(s)
#endif

typedef void *nick_handle;
typedef struct {
  uint32_t title;
  uint16_t name[32];
} nick_record;
typedef struct {
  uint32_t magic;
  uint32_t index;
  uint32_t title_only;
  nick_record records[50];
} nick_find;

nick_handle __cdecl nick_open(const char *, unsigned long, unsigned long,
                              void *, unsigned long, unsigned long, nick_handle)
    NICK_SYMBOL("_CreateFileA");
int __cdecl nick_read(nick_handle, void *, unsigned long, unsigned long *, void *)
    NICK_SYMBOL("_ReadFile");
int __cdecl nick_write(nick_handle, const void *, unsigned long, unsigned long *, void *)
    NICK_SYMBOL("_WriteFile");
int __cdecl nick_close(nick_handle) NICK_SYMBOL("_CloseHandle");
void *__stdcall nick_alloc(unsigned int, unsigned int) NICK_SYMBOL("_FUN_001d0bb9@8");
void *__stdcall nick_free(void *) NICK_SYMBOL("_LocalFree@4");

intptr_t __stdcall standalone_XFindFirstNickname(int, uint16_t *, int)
    NICK_SYMBOL("_FUN_001d29eb@12");
int __stdcall standalone_XSetNicknameW(const uint16_t *, int)
    NICK_SYMBOL("_XSetNicknameW@8");

static const char nickname_path[] = "E:\\UDATA\\NICKNAME.XBN";

static uint32_t nickname_title(void)
{
#ifdef HALO_NICKNAME_HOST_TEST
  return 0x4d530004;
#else
  /* The running XBE header points at its certificate, whose TitleID is +8. */
  return *(uint32_t *)(*(uintptr_t *)0x10118 + 8);
#endif
}

static unsigned int nickname_length(const uint16_t *name)
{
  unsigned int i;
  for (i = 0; i < 32 && name[i]; i++) {}
  return i;
}

static uint16_t nickname_fold(uint16_t c)
{
  /* XAPI's comparison is ASCII case-insensitive for console nicknames. */
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int nickname_equal(const uint16_t *a, const uint16_t *b)
{
  unsigned int i;
  for (i = 0; i < 32; i++) {
    if (nickname_fold(a[i]) != nickname_fold(b[i])) return 0;
    if (!a[i]) return 1;
  }
  return 1;
}

static void nickname_copy(uint16_t *to, const uint16_t *from)
{
  unsigned int i;
  for (i = 0; i < 31 && from[i]; i++) to[i] = from[i];
  for (; i < 32; i++) to[i] = 0;
}

intptr_t __stdcall standalone_XFindFirstNickname(int title_only,
                                                uint16_t *name, int chars)
{
  nick_handle file;
  nick_find *find;
  unsigned long bytes;
  unsigned int i, j;
  int duplicate;

  if (!name || chars <= 0) return -1;
  file = nick_open(nickname_path, 0x80100000ul, 4, 0, 3, 0x80, 0);
  if (file == (nick_handle)-1) return -1;
  find = (nick_find *)nick_alloc(0, sizeof(*find));
  if (!find) {
    nick_close(file);
    return -1;
  }
  bytes = 0;
  if (!nick_read(file, find->records, sizeof(find->records), &bytes, 0) ||
      bytes != sizeof(find->records)) {
    nick_close(file);
    nick_free(find);
    return -1;
  }
  nick_close(file);
  find->magic = 0x4e49434b;
  find->title_only = title_only;
  for (i = 0; i < 50; i++) {
    if (!find->records[i].name[0] ||
        nickname_length(find->records[i].name) >= (unsigned int)chars ||
        nickname_length(find->records[i].name) == 32) continue;
    if (title_only && find->records[i].title != nickname_title()) continue;
    duplicate = 0;
    if (!title_only) {
      for (j = 0; j < i; j++) {
        if (nickname_equal(find->records[i].name, find->records[j].name)) {
          duplicate = 1;
          break;
        }
      }
    }
    if (duplicate) continue;
    /* Copy only the selected string: callers may supply fewer than 32 chars. */
    for (j = 0; ; j++) {
      name[j] = find->records[i].name[j];
      if (!name[j]) break;
    }
    find->index = i + 1;
    return (intptr_t)find;
  }
  nick_free(find);
  return -1;
}

int __stdcall standalone_XSetNicknameW(const uint16_t *name, int replace)
{
  nick_record records[50];
  uint16_t saved[32];
  nick_handle file;
  unsigned long bytes;
  unsigned int i, j;
  uint32_t title;
  int found;

  if (!name || !name[0]) return 0;
  title = nickname_title();
  for (i = 0; i < sizeof(records); i++) ((unsigned char *)records)[i] = 0;
  file = nick_open(nickname_path, 0x80100000ul, 1, 0, 3, 0x80, 0);
  if (file != (nick_handle)-1) {
    bytes = 0;
    if (!nick_read(file, records, sizeof(records), &bytes, 0) || bytes != sizeof(records)) {
      for (i = 0; i < sizeof(records); i++) ((unsigned char *)records)[i] = 0;
    }
    nick_close(file);
  }
  nickname_copy(saved, name);
  found = 0;
  for (i = 0; i < 50; i++) {
    if (!records[i].name[0]) break;
    if (records[i].title == title && nickname_equal(records[i].name, name)) {
      found = 1;
      if (!replace) nickname_copy(saved, records[i].name);
      break;
    }
  }
  if (found && i == 0 && nickname_equal(saved, records[0].name) && !replace) return 1;
  if (i >= 50) i = 49;
  for (j = i; j > 0; j--) records[j] = records[j - 1];
  records[0].title = title;
  nickname_copy(records[0].name, saved);
  file = nick_open(nickname_path, 0xc0100000ul, 1, 0, 2, 0x80, 0);
  if (file == (nick_handle)-1) return 0;
  bytes = 0;
  found = nick_write(file, records, sizeof(records), &bytes, 0) && bytes == sizeof(records);
  nick_close(file);
  return found;
}
