/*
 * Standalone compat: XAPILIB process start-up (XapiInitProcess).
 *
 * The original start-up mounts the title's utility drive (Z:) when the XBE
 * init flags ask for it (cachebeta.xbe: XINIT_MOUNT_UTILITYDRIVE). nxdk's
 * CRT start-up does not, so standalone_entry does it before handing over to
 * nxdk's WinMainCRTStartup. D: comes from libnxdk_automount_d.
 *
 * Before that (0x1d82d9..0x1d82e5, skipped when the init flags carry
 * XINIT_NO_SETUP_HARD_DISK) it maps the title's T: and U: drives to
 * \Device\Harddisk0\partition1\TDATA\<title id> and UDATA\<title id>
 * through FUN_001d819f (lifted in src/halo/cseries/xbox_crt.c). U: holds the
 * saved games (profiles); without it every XCreateSaveGame fails. The title
 * id and name come from the running image's certificate, so the standalone
 * image's saves live under its own title id, not cachebeta.xbe's 4d530004.
 * The original's partition1 check (XapiValidateDiskPartition, 0x1d7d21) and
 * D: link are not repeated here.
 *
 * Behavior transcribed from cachebeta.xbe: XapiSelectCachePartition
 * (0x1d3aa8), XMountUtilityDrive (0x1d3c95), XapiValidateDiskPartition
 * (0x1d7d21). Formatting uses nxdk's nxFormatVolume instead of XAPI's
 * FATX formatter.
 */
#include "xbox.h"

#define XINIT_MOUNT_UTILITYDRIVE 0x00000001
#define XINIT_FORMAT_UTILITYDRIVE 0x00000002
#define XINIT_NO_SETUP_HARD_DISK 0x00000008
/* cachebeta.xbe's init flags. cxbe writes its own into the standalone
 * image, so the original title's value is carried here. */
#define HALO_XBE_INIT_FLAGS XINIT_MOUNT_UTILITYDRIVE
#define XBE_CERTIFICATE_ADDRESS 0x10118
#define XBE_CERTIFICATE_TITLE_NAME 0xc

#define CACHE_DB_OFFSET 0x800
#define CACHE_DB_SIZE 0x200
#define CACHE_DB_MAGIC 0x97315286
#define CACHE_DB_VERSION 2
#define CACHE_DB_SIGNATURE 0xaa550000
#define CACHE_DB_SIGNATURE_INDEX 126
#define CACHE_DB_MAX_ENTRIES 0x29
#define FIRST_CACHE_PARTITION 3
#define UTILITY_BYTES_PER_CLUSTER 0x4000

#define GENERIC_READ_WRITE_SYNCHRONIZE 0xc0100000
#define FILE_SHARE_READ_WRITE 3
#define FILE_OPEN_VOLUME_ACCESS 0x100001 /* SYNCHRONIZE | FILE_READ_DATA */
#define FILE_OPEN_VOLUME_OPTIONS 0x800021
#define FileFsSizeInformation 3

typedef struct {
  unsigned long title_id;
  unsigned long partition; /* relative to FIRST_CACHE_PARTITION */
  unsigned long in_use;
} cache_db_entry;

typedef union {
  unsigned long dwords[CACHE_DB_SIZE / 4];
  struct {
    unsigned long magic;
    unsigned long version;
    cache_db_entry entries[CACHE_DB_MAX_ENTRIES];
  } db;
} cache_db_sector;

extern ULONG *standalone_imp_HalDiskCachePartitionCount __asm__("__imp__HalDiskCachePartitionCount");
unsigned char __cdecl standalone_nxFormatVolume(const char *volume_path,
                                                unsigned int bytes_per_cluster)
    __asm__("_nxFormatVolume");
void __cdecl WinMainCRTStartup(void);
void *__cdecl compat_memmove(void *dst, const void *src, size_t size) __asm__("_memmove");

static ANSI_STRING cache_db_device_name;
static ANSI_STRING utility_drive_link;

static void init_ansi(ANSI_STRING *s, char *text)
{
  RtlInitAnsiString(s, text);
}

static unsigned long title_id(void)
{
  return *(unsigned long *)(*(unsigned char **)XBE_CERTIFICATE_ADDRESS + 8);
}

/* 0x1d3aa8. Picks the cache partition for this title from the cache
 * database in sector 4 of partition 0 and records the choice (most recently
 * used first). *is_new is set when the title had no partition yet. */
static NTSTATUS select_cache_partition(int format_clean, unsigned long *partition,
                                       int *is_new)
{
  static char device_path[] = "\\Device\\Harddisk0\\partition0";
  cache_db_sector sector;
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK io;
  LARGE_INTEGER offset;
  HANDLE handle;
  NTSTATUS status;
  unsigned long id;
  unsigned long count;
  unsigned long found;
  unsigned long chosen;
  unsigned long slot;
  unsigned long i;
  unsigned long j;

  id = title_id();
  init_ansi(&cache_db_device_name, device_path);
  attributes.RootDirectory = 0;
  attributes.ObjectName = &cache_db_device_name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  status = NtOpenFile(&handle, GENERIC_READ_WRITE_SYNCHRONIZE, &attributes, &io,
                      FILE_SHARE_READ_WRITE, FILE_SYNCHRONOUS_IO_ALERT);
  if (status < 0) {
    return status;
  }
  offset.QuadPart = CACHE_DB_OFFSET;
  status = NtReadFile(handle, 0, 0, 0, &io, &sector, CACHE_DB_SIZE, &offset);
  if (status >= 0) {
    found = *standalone_imp_HalDiskCachePartitionCount - 1;
    if (sector.db.magic != CACHE_DB_MAGIC ||
        sector.dwords[CACHE_DB_SIGNATURE_INDEX] != CACHE_DB_SIGNATURE ||
        sector.db.version != CACHE_DB_VERSION) {
      for (i = 0; i < CACHE_DB_SIZE / 4; i++) {
        sector.dwords[i] = 0;
      }
      sector.db.magic = CACHE_DB_MAGIC;
      sector.db.version = CACHE_DB_VERSION;
      sector.dwords[CACHE_DB_SIGNATURE_INDEX] = CACHE_DB_SIGNATURE;
    }
    chosen = 0;
    *is_new = 1;
    count = *standalone_imp_HalDiskCachePartitionCount;
    if (count > CACHE_DB_MAX_ENTRIES) {
      count = CACHE_DB_MAX_ENTRIES;
    }
    for (i = 0; i < count; i++) {
      if (sector.db.entries[i].title_id == id && sector.db.entries[i].in_use) {
        chosen = sector.db.entries[i].partition + FIRST_CACHE_PARTITION;
        *is_new = 0;
        found = i;
        break;
      }
    }
    if (chosen == 0) {
      /* Highest partition no in-use entry claims; else evict the LRU one. */
      for (i = 0; i < count; i++) {
        for (j = 0; j < count; j++) {
          if (sector.db.entries[j].in_use && sector.db.entries[j].partition == i) {
            break;
          }
        }
        if (j == count) {
          chosen = i + FIRST_CACHE_PARTITION;
        }
      }
      if (chosen == 0) {
        chosen = sector.db.entries[count - 1].partition + FIRST_CACHE_PARTITION;
      }
    }
    if (chosen >= count + FIRST_CACHE_PARTITION) {
      chosen = count + FIRST_CACHE_PARTITION - 1;
    }
    slot = format_clean ? count - 1 : 0;
    *partition = chosen;
    if (!format_clean && found != 0) {
      compat_memmove(&sector.db.entries[1], &sector.db.entries[0],
                     found * sizeof(cache_db_entry));
    }
    sector.db.entries[slot].title_id = id;
    sector.db.entries[slot].partition = chosen - FIRST_CACHE_PARTITION;
    sector.db.entries[slot].in_use = !format_clean;
    NtWriteFile(handle, 0, 0, 0, &io, &sector, CACHE_DB_SIZE, &offset);
  }
  NtClose(handle);
  return status;
}

/* 0x1d7d21: the volume mounts and answers a size query. */
static NTSTATUS validate_disk_partition(ANSI_STRING *path)
{
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK io;
  unsigned char size_info[0x18];
  HANDLE handle;
  NTSTATUS status;

  attributes.RootDirectory = 0;
  attributes.ObjectName = path;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  status = NtOpenFile(&handle, FILE_OPEN_VOLUME_ACCESS, &attributes, &io,
                      FILE_SHARE_READ_WRITE, FILE_OPEN_VOLUME_OPTIONS);
  if (status >= 0) {
    status = NtQueryVolumeInformationFile(handle, &io, size_info, sizeof(size_info),
                                          FileFsSizeInformation);
    NtClose(handle);
  }
  return status;
}

/* nxFormatVolume opens the raw partition, so it takes the path without the
 * trailing slash. */
static int format_volume(char *volume_path, char *slash)
{
  int ok;

  *slash = 0;
  ok = standalone_nxFormatVolume(volume_path, UTILITY_BYTES_PER_CLUSTER);
  *slash = '\\';
  return ok;
}

/* 0x1d3c95. */
static int mount_utility_drive(int format_clean)
{
  static char volume_path[64];
  static char link_name[] = "\\??\\Z:";
  ANSI_STRING volume_with_slash;
  ANSI_STRING volume;
  unsigned long partition;
  int is_new;
  int format;
  NTSTATUS status;
  char *p;
  char *slash;

  is_new = 0;
  status = select_cache_partition(format_clean, &partition, &is_new);
  if (status < 0) {
    return 0;
  }
  format = format_clean || is_new;

  /* "\Device\Harddisk0\Partition%d\" */
  p = volume_path;
  {
    static const char prefix[] = "\\Device\\Harddisk0\\Partition";
    const char *q;

    for (q = prefix; *q; q++) {
      *p++ = *q;
    }
  }
  if (partition >= 10) {
    *p++ = (char)('0' + partition / 10);
  }
  *p++ = (char)('0' + partition % 10);
  *p++ = '\\';
  *p = 0;
  init_ansi(&volume_with_slash, volume_path);
  init_ansi(&volume, volume_path);
  volume.Length--;

  slash = p - 1;
  if (format) {
    if (!format_volume(volume_path, slash)) {
      return 0;
    }
  }
  status = validate_disk_partition(&volume_with_slash);
  if (status < 0) {
    if (!format && format_volume(volume_path, slash)) {
      status = validate_disk_partition(&volume_with_slash);
    }
    if (status < 0) {
      return 0;
    }
  }
  init_ansi(&utility_drive_link, link_name);
  status = IoCreateSymbolicLink(&utility_drive_link, &volume);
  return status >= 0;
}

/* Runs on nxdk's main thread as a C initializer (.CRT$XI*), ahead of Halo's
 * main. It cannot run in standalone_entry: nxdk's WinMainCRTStartup sizes
 * TLS and starts main on a new thread, so the entry thread has no TLS and
 * nxdk's thread_local SetLastError (reached from XGetSectionHandleA's
 * not-found path during the U: mapping) faults there. */
static int __cdecl xapi_init_process(void)
{
  unsigned long init_flags;

  init_flags = HALO_XBE_INIT_FLAGS;
  /* The original fails start-up when the mapping's NTSTATUS is negative
   * (0x1d82ea), but the lift is void and does not return it. */
  if (!(init_flags & XINIT_NO_SETUP_HARD_DISK)) {
    FUN_001d819f(title_id(), (short *)(*(unsigned char **)XBE_CERTIFICATE_ADDRESS +
                                       XBE_CERTIFICATE_TITLE_NAME));
  }
  /* The original start-up boots to the dashboard on failure. Keep running
   * instead so the failure surfaces in the game's own asserts. */
  if ((init_flags & XINIT_MOUNT_UTILITYDRIVE) &&
      !mount_utility_drive((init_flags & XINIT_FORMAT_UTILITYDRIVE) != 0)) {
    DbgPrint("[halo-standalone] utility drive mount failed\n");
  }
  return 0;
}

__attribute__((section(".CRT$XIU"), used)) static int(__cdecl *const xapi_init_process_entry)(
  void) = xapi_init_process;

void __cdecl standalone_entry(void)
{
  WinMainCRTStartup();
}
