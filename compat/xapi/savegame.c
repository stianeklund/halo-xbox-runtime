/*
 * Standalone compat: XAPILIB saved-game and directory-enumeration routines.
 *
 * Transcribed from cachebeta.xbe (kb object XAPILIB:physmem.obj):
 *   XCreateSaveGame      0x1d2f22 (kb FUN_001d2f22)
 *   XDeleteSaveGame      0x1d3185 (kb FUN_001d3185)
 *   XFindFirstSaveGame   0x1d3254
 *   XFindNextSaveGame    0x1d335b
 *   XFindClose           0x1d33a2
 *   FindFirstFileA       0x1d3576 (kb FUN_001d3576; files.c calls it by address)
 *   FindNextFileA        0x1d3683 (kb FUN_001d3683)
 *   XGetSectionHandleA   0x1d8a88 (kb FUN_001d8a88; XapiMapLetterToDirectory)
 * and their helpers:
 *   save name -> directory name   0x1d2ae7
 *   SaveMeta.xbx BOM check        0x1d2b79
 *   SaveMeta.xbx "key=value" read 0x1d2bbd
 *   fill XGAME_FIND_DATA          0x1d2d48
 *   SaveMeta.xbx name check       0x1d2e48
 *   touch directory write time    0x1d2ec6
 *   FILE_DIRECTORY_INFORMATION -> WIN32_FIND_DATAA 0x1d350a
 *
 * A save game is the directory <root><12 hex digits of a 48-bit hash of the
 * name> holding SaveMeta.xbx, a UTF-16 "Name=<name>\r\n" file. The root Halo
 * passes is "u:\" (the wide string at 0x2ba7e0), the title's user-data drive
 * that init.c maps like XapiInitProcess.
 *
 * The file APIs the originals call through XAPI (CreateFileA, ReadFile,
 * WriteFile, SetFilePointer, CreateDirectoryA, RemoveDirectoryA,
 * CloseHandle, Get/SetLastError) are nxdk's winapi equivalents here.
 */
#include "xbox.h"

typedef void *compat_handle;

compat_handle __cdecl sg_CreateFileA(const char *path, unsigned long access,
                                     unsigned long share, void *security,
                                     unsigned long disposition, unsigned long flags,
                                     compat_handle template_file) __asm__("_CreateFileA");
int __cdecl sg_ReadFile(compat_handle file, void *buffer, unsigned long size,
                        unsigned long *read, void *overlapped) __asm__("_ReadFile");
int __cdecl sg_WriteFile(compat_handle file, const void *buffer, unsigned long size,
                         unsigned long *written, void *overlapped) __asm__("_WriteFile");
unsigned long __cdecl sg_SetFilePointer(compat_handle file, long distance, long *high,
                                        unsigned long method) __asm__("_SetFilePointer");
int __cdecl sg_CreateDirectoryA(const char *path, void *security) __asm__("_CreateDirectoryA");
int __cdecl sg_RemoveDirectoryA(const char *path) __asm__("_RemoveDirectoryA");
int __cdecl sg_CloseHandle(compat_handle handle) __asm__("_CloseHandle");
unsigned long __cdecl sg_GetLastError(void) __asm__("_GetLastError");
void __cdecl sg_SetLastError(unsigned long error) __asm__("_SetLastError");
void *__cdecl sg_memmove(void *dst, const void *src, size_t size) __asm__("_memmove");
void *__stdcall sg_LocalAlloc(unsigned int flags, unsigned int size) __asm__("_FUN_001d0bb9@8");
void *__stdcall sg_LocalFree(void *memory) __asm__("_LocalFree@4");
void __stdcall sg_XapiSetLastNTError(NTSTATUS status) __asm__("_XapiSetLastNTError@4");

int __stdcall standalone_XCreateSaveGame(const char *root, const wchar_t *name,
                                         unsigned long disposition, unsigned long flags,
                                         char *path_out, unsigned int path_size)
    __asm__("_FUN_001d2f22@24");
int __stdcall standalone_XDeleteSaveGame(const char *root, const wchar_t *name)
    __asm__("_FUN_001d3185@8");
int __stdcall standalone_XFindFirstSaveGame(const char *root, unsigned char *find_data)
    __asm__("_XFindFirstSaveGame@8");
int __stdcall standalone_XFindNextSaveGame(void *handle, unsigned char *find_data)
    __asm__("_XFindNextSaveGame@8");
int __stdcall standalone_XFindClose(void *handle) __asm__("_XFindClose@4");
compat_handle __stdcall standalone_FindFirstFileA(const char *path, unsigned char *find_data)
    __asm__("_FUN_001d3576@8");
int __stdcall standalone_FindNextFileA(compat_handle handle, unsigned char *find_data)
    __asm__("_FUN_001d3683@8");
int __stdcall standalone_XGetSectionHandleA(const char *section_name)
    __asm__("_FUN_001d8a88@4");

#define INVALID_HANDLE ((compat_handle)-1)
#define MAX_PATH_LENGTH 0x104
/* XDeleteSaveGame's [EBP-0x144] path buffer. */
#define SAVE_DELETE_PATH_LENGTH 0x144

#define GENERIC_READ_ACCESS 0x80000000ul
#define GENERIC_READ_WRITE 0xc0000000ul
#define GENERIC_READ_SYNCHRONIZE 0x80100000ul
#define GENERIC_READ_WRITE_SYNCHRONIZE 0xc0100000ul
#define SHARE_READ 1
#define CREATE_NEW_DISPOSITION 1
#define OPEN_EXISTING_DISPOSITION 3
#define OPEN_ALWAYS_DISPOSITION 4
#define ATTRIBUTE_SYSTEM 0x4
#define ATTRIBUTE_DIRECTORY 0x10
#define ATTRIBUTE_NORMAL 0x80
#define FLAG_BACKUP_SEMANTICS 0x02000000ul
#define MOVE_BEGIN 0
#define MOVE_CURRENT 1

#define NT_FILE_OPEN_IF 3
#define NT_META_CREATE_OPTIONS 0x22 /* SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY */
#define NT_LIST_DIRECTORY_ACCESS 0x100001 /* SYNCHRONIZE | FILE_LIST_DIRECTORY */
#define NT_SHARE_READ_WRITE 3
#define NT_DIRECTORY_OPEN_OPTIONS 0x4021
#define NT_FILE_BASIC_INFORMATION 4
#define NT_FILE_DIRECTORY_INFORMATION 1

#define ERROR_NOT_ENOUGH_MEMORY_CODE 8
#define ERROR_NO_MORE_FILES_CODE 0x12
#define ERROR_INVALID_PARAMETER_CODE 0x57
#define ERROR_CANNOT_MAKE_CODE 0x52
#define ERROR_ALREADY_EXISTS_CODE 0xb7
#define ERROR_NOT_FOUND_CODE 0x490
#define ERROR_NO_MATCH_CODE 0x491

#define SAVE_META_BOM 0xfeff
#define SAVE_META_NAME_CHARS 0x80
#define SAVE_META_TEXT_CHARS 0x98
#define SAVE_META_READ_CHARS 0x8c
#define SAVE_DIRECTORY_DIGITS 12
#define SAVE_NAME_HASH_MODULUS 0xffffffffffc5ull
#define SAVE_NAME_HASH_SHIFT 16
#define XCREATESAVEGAME_NOCOPY 0x1

/* WIN32_FIND_DATAA + XGAME_FIND_DATA tail (0x344 bytes, the size
 * saved_game_files.c allocates). */
#define FIND_ATTRIBUTES 0x00
#define FIND_FILE_NAME 0x2c
#define FIND_ALTERNATE_NAME 0x130
#define FIND_SAVE_DIRECTORY 0x140
#define FIND_SAVE_NAME 0x244

#define FIND_HANDLE_MAGIC_SAVE 0x53425645 /* 'SBVE' */
#define FIND_HANDLE_MAGIC_NICKNAME 0x4e49434b /* 'NICK' */
#define FIND_HANDLE_MAGIC_OTHER 0xbe4bea00
#define FIND_HANDLE_SIZE 0x110
#define DIRECTORY_QUERY_SIZE 0x148

typedef struct {
  unsigned long magic;
  compat_handle find;
  unsigned long root_length;
  char root[MAX_PATH_LENGTH];
} save_find_handle_t;

static const char save_meta_file[] = "\\SaveMeta.xbx"; /* 0x2c1c40 */
static const char save_delete_prefix[] = "\\??\\"; /* 0x2c1d28 */
static const wchar_t save_meta_name_key[] = L"Name";   /* 0x2c1c90 */
static const wchar_t save_meta_crlf[] = L"\r\n";       /* 0x2c1cb8 */
static const wchar_t save_meta_nocopy[] = L"NoCopy=1\r\n"; /* 0x2c1cc0 */
static const wchar_t save_meta_format[] = L"%lc%ls%lc%ls%ls";          /* 0x2c1ce0 */
static const wchar_t save_meta_format_nocopy[] = L"%lc%ls%lc%ls%ls%ls"; /* 0x2c1d00 */

static unsigned int sg_strlen(const char *s)
{
  const char *p = s;

  while (*p) p++;
  return (unsigned int)(p - s);
}

static void sg_strcpy(char *dst, const char *src)
{
  while ((*dst++ = *src++) != 0) {
  }
}

static unsigned int sg_wcslen(const wchar_t *s)
{
  const wchar_t *p = s;

  while (*p) p++;
  return (unsigned int)(p - s);
}

static int sg_wcscmp(const wchar_t *a, const wchar_t *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)*a - (int)*b;
}

static wchar_t *sg_wcsstr(const wchar_t *s, const wchar_t *find)
{
  unsigned int n = sg_wcslen(find);
  unsigned int i;

  for (; *s; s++) {
    for (i = 0; i < n && s[i] == find[i]; i++) {
    }
    if (i == n) {
      return (wchar_t *)s;
    }
  }
  return n == 0 ? (wchar_t *)s : 0;
}

/* LIBCMT wcsncpy: copies up to n characters and pads with NULs. */
static void sg_wcsncpy(wchar_t *dst, const wchar_t *src, unsigned int n)
{
  while (n && (*dst = *src) != 0) {
    dst++;
    src++;
    n--;
  }
  while (n--) {
    *dst++ = 0;
  }
}

/* 0x1d789a lstrcpynA: at most size - 1 characters, always terminated. */
static void sg_lstrcpyn(char *dst, const char *src, unsigned int size)
{
  if (size == 0) {
    return;
  }
  while (size) {
    if (*src == 0) {
      break;
    }
    *dst++ = *src++;
    size--;
  }
  if (size == 0) {
    dst--;
  }
  *dst = 0;
}

/* 0x1d2ae7: h = (h * 0x10000 + c) mod 0xffffffffffc5 over the name, then
 * twelve upper-case hex digits, most significant first. The original's
 * third argument (remaining buffer size) is unused. */
static void save_name_to_directory(const wchar_t *name, char *directory)
{
  unsigned long long hash;
  int i;
  int digit;

  hash = 0;
  for (; *name; name++) {
    hash = (hash * (1ull << SAVE_NAME_HASH_SHIFT) + *name) % SAVE_NAME_HASH_MODULUS;
  }
  for (i = SAVE_DIRECTORY_DIGITS - 1; i >= 0; i--) {
    digit = (int)(hash >> ((SAVE_DIRECTORY_DIGITS - 1 - i) * 4)) & 0xf;
    directory[i] = (char)(digit > 9 ? digit + 'A' - 10 : digit + '0');
  }
  directory[SAVE_DIRECTORY_DIGITS] = 0;
}

/* 0x1d2b79. As in the original, a successful two-byte read passes without
 * looking at the value; the BOM is only checked when the read came up
 * short (the original then tests whatever the stack word held). */
static int save_meta_has_bom(compat_handle file)
{
  unsigned long read;
  unsigned short word;

  word = 0;
  sg_SetFilePointer(file, 0, 0, MOVE_BEGIN);
  if (sg_ReadFile(file, &word, sizeof(word), &read, 0) && read == sizeof(word)) {
    return 1;
  }
  return word == SAVE_META_BOM;
}

/* 0x1d2bbd: finds "<key>=<value>\r\n" reading 0x8c-character chunks after
 * the BOM, copies at most value_chars - 1 characters of the value, and seeks
 * the file back to just past that line. Lines too long for the buffer are
 * skipped. The chunk buffer is NUL-terminated only at its end, so the
 * searches may see bytes left over from an earlier chunk, as in the
 * original (whose buffer starts out uninitialized, not zeroed). */
static int save_meta_read_value(compat_handle file, const wchar_t *key, wchar_t *value,
                                int value_chars)
{
  wchar_t chunk[SAVE_META_READ_CHARS + 1];
  wchar_t *write;
  wchar_t *found;
  wchar_t *line_end;
  unsigned long space;
  unsigned long read;
  unsigned long valid;
  unsigned long offset;
  unsigned long remaining;
  unsigned int key_length;
  unsigned int length;
  unsigned int copy;
  int skipping;
  int reuse;
  unsigned int i;

  for (i = 0; i <= SAVE_META_READ_CHARS; i++) {
    chunk[i] = 0;
  }
  key_length = sg_wcslen(key);
  write = chunk;
  space = SAVE_META_READ_CHARS;
  skipping = 0;
  reuse = 0;
  read = 0;
  for (;;) {
    if (!reuse) {
      if (!sg_ReadFile(file, write, space * sizeof(wchar_t), &read, 0) || read == 0) {
        return 0;
      }
    }
    reuse = 0;
    valid = read + (unsigned long)(write - chunk) * sizeof(wchar_t);

    if (skipping) {
      found = sg_wcsstr(chunk, save_meta_crlf);
      write = chunk;
      space = SAVE_META_READ_CHARS;
      if (found == 0) {
        continue;
      }
      offset = (unsigned long)(found - chunk) * sizeof(wchar_t) + 4;
      if (offset < valid) {
        remaining = valid - offset;
        sg_memmove(chunk, (unsigned char *)chunk + offset, remaining);
        write = (wchar_t *)((unsigned char *)chunk + remaining);
        space = SAVE_META_READ_CHARS - remaining / sizeof(wchar_t);
      }
      skipping = 0;
      continue;
    }

    found = sg_wcsstr(chunk, key);
    if (found == 0 || found[key_length] != L'=') {
      skipping = 1;
      reuse = 1;
      continue;
    }
    found += key_length + 1;
    line_end = sg_wcsstr(found, save_meta_crlf);
    if (line_end == 0) {
      skipping = 1;
      write = chunk;
      space = SAVE_META_READ_CHARS;
      continue;
    }
    length = (unsigned int)(line_end - found);
    copy = (int)(value_chars - 1) < (int)length ? (unsigned int)(value_chars - 1) : length;
    sg_wcsncpy(value, found, copy);
    value[copy] = 0;
    offset = (unsigned long)(line_end - chunk) * sizeof(wchar_t) + 4;
    if (offset < valid) {
      sg_SetFilePointer(file, (long)(offset - valid), 0, MOVE_CURRENT);
    }
    return 1;
  }
}

/* 0x1d2e48: 0 when <directory>\SaveMeta.xbx names this save. OPEN_ALWAYS is
 * the original's disposition: a directory without the file gets an empty
 * one, which then fails the check. */
static unsigned long save_meta_check_name(const char *meta_path, const wchar_t *name)
{
  wchar_t stored[SAVE_META_NAME_CHARS];
  compat_handle file;
  unsigned long result;

  result = 0;
  file = sg_CreateFileA(meta_path, GENERIC_READ_SYNCHRONIZE, 0, 0, OPEN_ALWAYS_DISPOSITION,
                        ATTRIBUTE_SYSTEM, 0);
  if (file == INVALID_HANDLE) {
    return sg_GetLastError();
  }
  if (!save_meta_has_bom(file) ||
      !save_meta_read_value(file, save_meta_name_key, stored, SAVE_META_NAME_CHARS) ||
      sg_wcscmp(stored, name) != 0) {
    result = ERROR_NO_MATCH_CODE;
  }
  sg_CloseHandle(file);
  return result;
}

/* 0x1d2ec6: CreateFileA(dir, GENERIC_READ | GENERIC_WRITE, 0, NULL,
 * OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS) and a FILE_BASIC_INFORMATION
 * with only LastWriteTime set. nxdk's CreateFileA would pass the flag bits
 * on as file attributes, so the directory is opened directly, the way the
 * XAPI CreateFileA (0x1d1d85) turns that call into NtCreateFile. */
static void save_directory_touch(const char *path)
{
  ANSI_STRING name;
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK io;
  FILE_BASIC_INFORMATION basic;
  HANDLE handle;
  unsigned int i;

  RtlInitAnsiString(&name, path);
  attributes.RootDirectory = ObDosDevicesDirectory();
  attributes.ObjectName = &name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  if (NtCreateFile(&handle, GENERIC_READ_WRITE | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                   &attributes, &io, 0, 0, 0, FILE_OPEN,
                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT) < 0) {
    return;
  }
  for (i = 0; i < sizeof(basic); i++) {
    ((unsigned char *)&basic)[i] = 0;
  }
  KeQuerySystemTime(&basic.LastWriteTime);
  NtSetInformationFile(handle, &io, &basic, sizeof(basic), NT_FILE_BASIC_INFORMATION);
  NtClose(handle);
}

/* 0x1d2f22 XCreateSaveGame. Returns a Win32 error code; path_out receives
 * "<root><dir>\". */
int __stdcall standalone_XCreateSaveGame(const char *root, const wchar_t *name,
                                         unsigned long disposition, unsigned long flags,
                                         char *path_out, unsigned int path_size)
{
  char path[MAX_PATH_LENGTH];
  wchar_t meta_text[SAVE_META_TEXT_CHARS];
  ANSI_STRING meta_name;
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK io;
  LARGE_INTEGER allocation;
  HANDLE meta;
  NTSTATUS status;
  unsigned long error;
  unsigned long check;
  unsigned long written;
  unsigned int directory_length;
  unsigned int bytes;

  sg_strcpy(path, root);
  save_name_to_directory(name, path + sg_strlen(path));
  directory_length = sg_strlen(path);

  if (disposition == OPEN_EXISTING_DISPOSITION) {
    sg_strcpy(path + directory_length, save_meta_file);
    check = save_meta_check_name(path, name);
    path[directory_length] = 0;
    if (check != 0) {
      return (int)check;
    }
    goto copy_path;
  }

  if (!sg_CreateDirectoryA(path, 0)) {
    error = sg_GetLastError();
    check = ERROR_NO_MATCH_CODE;
    if (error == ERROR_ALREADY_EXISTS_CODE) {
      sg_strcpy(path + directory_length, save_meta_file);
      check = save_meta_check_name(path, name);
      path[directory_length] = 0;
      if (check != 0) {
        return ERROR_CANNOT_MAKE_CODE;
      }
      save_directory_touch(path);
    }
    if (disposition == CREATE_NEW_DISPOSITION || check != 0) {
      return (int)error;
    }
    goto copy_path;
  }

  /* New directory: write SaveMeta.xbx. */
  sg_strcpy(path + directory_length, save_meta_file);
  RtlInitAnsiString(&meta_name, path);
  attributes.RootDirectory = ObDosDevicesDirectory();
  attributes.ObjectName = &meta_name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  allocation.QuadPart = 1;
  status = NtCreateFile(&meta, GENERIC_READ_WRITE_SYNCHRONIZE, &attributes, &io, &allocation,
                        ATTRIBUTE_SYSTEM, 0, NT_FILE_OPEN_IF, NT_META_CREATE_OPTIONS);
  path[directory_length] = 0;
  if (status < 0) {
    error = RtlNtStatusToDosError(status);
    goto remove_directory;
  }
  if (flags & XCREATESAVEGAME_NOCOPY) {
    FUN_001dd6f5((unsigned short *)meta_text, SAVE_META_TEXT_CHARS,
                 (const unsigned short *)save_meta_format_nocopy, SAVE_META_BOM,
                 save_meta_name_key, L'=', name, save_meta_crlf, save_meta_nocopy);
  } else {
    FUN_001dd6f5((unsigned short *)meta_text, SAVE_META_TEXT_CHARS,
                 (const unsigned short *)save_meta_format, SAVE_META_BOM, save_meta_name_key,
                 L'=', name, save_meta_crlf, save_meta_nocopy);
  }
  bytes = sg_wcslen(meta_text) * sizeof(wchar_t);
  while (bytes != 0) {
    if (!sg_WriteFile(meta, meta_text, bytes, &written, 0)) {
      error = sg_GetLastError();
      sg_CloseHandle(meta);
      goto remove_directory;
    }
    bytes -= written;
  }
  sg_CloseHandle(meta);

copy_path:
  if (path_out != 0) {
    sg_lstrcpyn(path_out, path, path_size);
    if (directory_length < path_size - 1) {
      path_out[directory_length] = '\\';
      path_out[directory_length + 1] = 0;
    }
  }
  return 0;

remove_directory:
  sg_RemoveDirectoryA(path);
  return (int)error;
}

/* 0x1d3185 XDeleteSaveGame. Returns a Win32 error code. The save's
 * SaveMeta.xbx must name it (0x1d2e48); then "\??\<root><dir>\" goes to
 * the lifted directory-tree delete FUN_001d7cb4, whose NTSTATUS is mapped
 * with RtlNtStatusToDosError. */
int __stdcall standalone_XDeleteSaveGame(const char *root, const wchar_t *name)
{
  char path[SAVE_DELETE_PATH_LENGTH];
  char *root_path;
  unsigned int end;
  unsigned long check;

  sg_strcpy(path, save_delete_prefix);
  root_path = path + sg_strlen(path);
  sg_strcpy(root_path, root);
  save_name_to_directory(name, path + sg_strlen(path));
  end = sg_strlen(path);
  sg_strcpy(path + end, save_meta_file);
  check = save_meta_check_name(root_path, name);
  path[end] = 0;
  if (check != 0) {
    return (int)check;
  }
  path[end] = '\\';
  path[end + 1] = 0;
  return (int)RtlNtStatusToDosError(FUN_001d7cb4(path));
}

/* 0x1d2d48: completes an XGAME_FIND_DATA from the directory entry found;
 * non-zero when it is a save game directory with a readable name. */
static int save_find_data_fill(unsigned char *find_data, const char *root,
                               unsigned int root_length)
{
  char *directory;
  compat_handle meta;
  unsigned int total;
  int result;

  result = 0;
  if (!(*(unsigned long *)(find_data + FIND_ATTRIBUTES) & ATTRIBUTE_DIRECTORY)) {
    return 0;
  }
  directory = (char *)find_data + FIND_SAVE_DIRECTORY;
  total = sg_strlen((char *)find_data + FIND_FILE_NAME) + root_length;
  sg_strcpy(directory, root);
  sg_strcpy(directory + root_length, (char *)find_data + FIND_FILE_NAME);
  if (total < MAX_PATH_LENGTH - 1) {
    directory[total] = '\\';
    directory[total + 1] = 0;
  }
  if ((int)total < (int)(MAX_PATH_LENGTH - sizeof(save_meta_file))) {
    sg_strcpy(directory + total, save_meta_file);
    meta = sg_CreateFileA(directory, GENERIC_READ_ACCESS, SHARE_READ, 0,
                          OPEN_EXISTING_DISPOSITION, ATTRIBUTE_NORMAL, 0);
    if (meta != INVALID_HANDLE) {
      result = save_meta_has_bom(meta);
      if (result) {
        result = save_meta_read_value(meta, save_meta_name_key,
                                      (wchar_t *)(find_data + FIND_SAVE_NAME),
                                      SAVE_META_NAME_CHARS);
      }
      sg_CloseHandle(meta);
    }
    directory[total + 1] = 0;
  }
  return result;
}

/* 0x1d3254 */
int __stdcall standalone_XFindFirstSaveGame(const char *root, unsigned char *find_data)
{
  char pattern[MAX_PATH_LENGTH];
  save_find_handle_t *handle;
  compat_handle find;
  unsigned int root_length;
  int more;

  sg_strcpy(pattern, root);
  root_length = sg_strlen(pattern);
  sg_strcpy(pattern + root_length, "*");
  find = standalone_FindFirstFileA(pattern, find_data);
  if (find != INVALID_HANDLE && !save_find_data_fill(find_data, root, root_length)) {
    do {
      more = standalone_FindNextFileA(find, find_data);
    } while (more && !save_find_data_fill(find_data, root, root_length));
    if (!more) {
      sg_CloseHandle(find);
      sg_SetLastError(ERROR_NO_MORE_FILES_CODE);
      find = INVALID_HANDLE;
    }
  }
  if (find == INVALID_HANDLE) {
    return -1;
  }
  handle = (save_find_handle_t *)sg_LocalAlloc(0, FIND_HANDLE_SIZE);
  if (handle == 0) {
    sg_CloseHandle(find);
    sg_SetLastError(ERROR_NOT_ENOUGH_MEMORY_CODE);
    return -1;
  }
  handle->magic = FIND_HANDLE_MAGIC_SAVE;
  handle->find = find;
  handle->root_length = root_length;
  sg_lstrcpyn(handle->root, root, MAX_PATH_LENGTH);
  return (int)handle;
}

/* 0x1d335b */
int __stdcall standalone_XFindNextSaveGame(void *handle, unsigned char *find_data)
{
  save_find_handle_t *save = (save_find_handle_t *)handle;
  int more;

  more = standalone_FindNextFileA(save->find, find_data);
  while (more && !save_find_data_fill(find_data, save->root, save->root_length)) {
    more = standalone_FindNextFileA(save->find, find_data);
  }
  return more;
}

/* 0x1d33a2: also closes the other XAPI find handles by their magic. */
int __stdcall standalone_XFindClose(void *handle)
{
  unsigned long *block = (unsigned long *)handle;
  compat_handle find;

  switch (block[0]) {
  case FIND_HANDLE_MAGIC_NICKNAME:
    sg_LocalFree(handle);
    return 1;
  case FIND_HANDLE_MAGIC_SAVE:
    find = (compat_handle)block[1];
    break;
  case FIND_HANDLE_MAGIC_OTHER:
    find = (compat_handle)block[3];
    break;
  default:
    return 0;
  }
  sg_LocalFree(handle);
  return sg_CloseHandle(find);
}

/* 0x1d350a */
static void find_data_from_directory_information(const FILE_DIRECTORY_INFORMATION *info,
                                                 unsigned char *find_data)
{
  unsigned long *out = (unsigned long *)find_data;
  unsigned long i;

  out[0] = info->FileAttributes;
  out[1] = info->CreationTime.LowPart;
  out[2] = (unsigned long)info->CreationTime.HighPart;
  out[3] = info->LastAccessTime.LowPart;
  out[4] = (unsigned long)info->LastAccessTime.HighPart;
  out[5] = info->LastWriteTime.LowPart;
  out[6] = (unsigned long)info->LastWriteTime.HighPart;
  out[7] = (unsigned long)info->EndOfFile.HighPart;
  out[8] = info->EndOfFile.LowPart;
  for (i = 0; i < info->FileNameLength; i++) {
    find_data[FIND_FILE_NAME + i] = (unsigned char)info->FileName[i];
  }
  find_data[FIND_FILE_NAME + info->FileNameLength] = 0;
  find_data[FIND_ALTERNATE_NAME] = 0;
}

/* 0x1d3576 FindFirstFileA: the directory part (through the last '\') is
 * opened, the rest is the NtQueryDirectoryFile mask, "*.*" meaning none. */
compat_handle __stdcall standalone_FindFirstFileA(const char *path, unsigned char *find_data)
{
  unsigned long long entry[DIRECTORY_QUERY_SIZE / 8];
  ANSI_STRING directory;
  ANSI_STRING mask;
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK io;
  HANDLE handle;
  NTSTATUS status;
  unsigned short length;

  RtlInitAnsiString(&directory, path);
  attributes.RootDirectory = ObDosDevicesDirectory();
  attributes.ObjectName = &directory;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  length = directory.Length;
  while (length != 0 && directory.Buffer[length - 1] != '\\') {
    length--;
  }
  mask.Length = mask.MaximumLength = (unsigned short)(directory.Length - length);
  mask.Buffer = directory.Buffer + length;
  directory.Length = directory.MaximumLength = length;
  if (length == 0 || mask.Length == 0) {
    sg_SetLastError(ERROR_INVALID_PARAMETER_CODE);
    return INVALID_HANDLE;
  }
  if (mask.Length == 3 && mask.Buffer[0] == '*' && mask.Buffer[1] == '.' &&
      mask.Buffer[2] == '*') {
    mask.Length = 0;
  }
  status = NtOpenFile(&handle, NT_LIST_DIRECTORY_ACCESS, &attributes, &io, NT_SHARE_READ_WRITE,
                      NT_DIRECTORY_OPEN_OPTIONS);
  if (status < 0) {
    sg_XapiSetLastNTError(status);
    return INVALID_HANDLE;
  }
  status = NtQueryDirectoryFile(handle, 0, 0, 0, &io, entry, DIRECTORY_QUERY_SIZE,
                                NT_FILE_DIRECTORY_INFORMATION, &mask, FALSE);
  if (status < 0) {
    NtClose(handle);
    sg_XapiSetLastNTError(status);
    return INVALID_HANDLE;
  }
  find_data_from_directory_information((FILE_DIRECTORY_INFORMATION *)entry, find_data);
  return handle;
}

/* 0x1d3683 FindNextFileA */
int __stdcall standalone_FindNextFileA(compat_handle handle, unsigned char *find_data)
{
  unsigned long long entry[DIRECTORY_QUERY_SIZE / 8];
  IO_STATUS_BLOCK io;
  NTSTATUS status;

  status = NtQueryDirectoryFile(handle, 0, 0, 0, &io, entry, DIRECTORY_QUERY_SIZE,
                                NT_FILE_DIRECTORY_INFORMATION, 0, FALSE);
  if (status < 0) {
    sg_XapiSetLastNTError(status);
    return 0;
  }
  find_data_from_directory_information((FILE_DIRECTORY_INFORMATION *)entry, find_data);
  return 1;
}

/* 0x1d8a88 XGetSectionHandleA: the running image's section header with this
 * name (RtlEqualString, case-insensitive), else ERROR_NOT_FOUND and -1. The
 * standalone image has none of cachebeta.xbe's $$XTIMAGE / $$XSIMAGE, so
 * XapiMapLetterToDirectory skips the TitleImage / SaveImage files. */
#define XBE_SECTION_COUNT_ADDRESS 0x1011c
#define XBE_SECTION_HEADERS_ADDRESS 0x10120
#define XBE_SECTION_HEADER_SIZE 0x38
#define XBE_SECTION_NAME_OFFSET 0x14

int __stdcall standalone_XGetSectionHandleA(const char *section_name)
{
  ANSI_STRING wanted;
  ANSI_STRING name;
  unsigned char *header;
  unsigned char *end;

  RtlInitAnsiString(&wanted, section_name);
  header = *(unsigned char **)XBE_SECTION_HEADERS_ADDRESS;
  end = header + *(unsigned long *)XBE_SECTION_COUNT_ADDRESS * XBE_SECTION_HEADER_SIZE;
  for (; header < end; header += XBE_SECTION_HEADER_SIZE) {
    RtlInitAnsiString(&name, *(char **)(header + XBE_SECTION_NAME_OFFSET));
    if (RtlEqualString(&wanted, &name, TRUE)) {
      return (int)header;
    }
  }
  sg_SetLastError(ERROR_NOT_FOUND_CODE);
  return -1;
}
