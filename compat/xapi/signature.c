/*
 * Standalone compat: XAPILIB content signatures (XCalculateSignature*).
 *
 * Transcribed from cachebeta.xbe:
 *   XCalculateSignatureBegin  0x1d426b
 *   XCalculateSignatureUpdate 0x1d42a9
 *   XCalculateSignatureEnd    0x1d42c3
 *   HMAC-SHA1 begin / end     0x1d4157 / 0x1d41be (ipad 0x36, opad 0x5c)
 *   non-roamable re-sign      0x1d4230
 *
 * The handle is a 0x7c-byte LocalAlloc block: flags, the key pointer (the
 * kernel's XboxSignatureKey export, loaded from the import slot 0x253238)
 * and the XcSHA context. The signature is HMAC-SHA1(XboxSignatureKey, data);
 * XCALCSIG_FLAG_NON_ROAMABLE re-signs that digest with XboxHDKey (0x253234).
 *
 * The kernel derives XboxSignatureKey from the running XBE certificate's
 * signature key when it loads the image, so signatures only match those of
 * cachebeta.xbe when the standalone image carries the same certificate keys
 * (tools/xbe_make_writable.py copies them in).
 */
#include "xbox.h"

#define XCALCSIG_FLAG_NON_ROAMABLE 0x00000001
#define SIGNATURE_KEY_LENGTH 0x10
#define SHA_DIGEST_LENGTH 0x14
#define HMAC_BLOCK_LENGTH 0x40
#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5c
#define SIGNATURE_HANDLE_SIZE 0x7c
#define ERROR_NOT_ENOUGH_MEMORY 8
#define INVALID_SIGNATURE_HANDLE (-1)

typedef struct {
  unsigned long flags;
  unsigned char *key;
  unsigned char sha[SIGNATURE_HANDLE_SIZE - 8]; /* XcSHA context */
} signature_handle_t;

extern unsigned char *standalone_imp_XboxSignatureKey __asm__("__imp__XboxSignatureKey");
extern unsigned char *standalone_imp_XboxHDKey __asm__("__imp__XboxHDKey");

void __cdecl sig_SetLastError(unsigned long error) __asm__("_SetLastError");
unsigned long __cdecl sig_GetLastError(void) __asm__("_GetLastError");
void *__stdcall sig_LocalAlloc(unsigned int flags, unsigned int size) __asm__("_FUN_001d0bb9@8");
void *__stdcall sig_LocalFree(void *memory) __asm__("_LocalFree@4");

int __stdcall standalone_XCalculateSignatureBegin(unsigned long flags)
    __asm__("_XCalculateSignatureBegin@4");
int __stdcall standalone_XCalculateSignatureUpdate(signature_handle_t *handle,
                                                   const void *buffer, unsigned long size)
    __asm__("_XCalculateSignatureUpdate@12");
int __stdcall standalone_XCalculateSignatureEnd(signature_handle_t *handle, void *signature)
    __asm__("_XCalculateSignatureEnd@8");

/* 0x1d4157 */
static void hmac_begin(const unsigned char *key, unsigned long key_length, unsigned char *sha)
{
  unsigned char pad[HMAC_BLOCK_LENGTH];
  unsigned long i;

  if (key_length > HMAC_BLOCK_LENGTH) {
    key_length = HMAC_BLOCK_LENGTH;
  }
  for (i = 0; i < HMAC_BLOCK_LENGTH; i++) {
    pad[i] = i < key_length ? key[i] : 0;
  }
  for (i = 0; i < HMAC_BLOCK_LENGTH; i++) {
    pad[i] ^= HMAC_IPAD;
  }
  XcSHAInit(sha);
  XcSHAUpdate(sha, pad, HMAC_BLOCK_LENGTH);
}

/* 0x1d41be: the inner digest lands right after the opad block, and the
 * outer hash runs over both (0x54 bytes). */
static void hmac_end(unsigned char *sha, const unsigned char *key, unsigned long key_length,
                     unsigned char *digest)
{
  unsigned char pad[HMAC_BLOCK_LENGTH + SHA_DIGEST_LENGTH];
  unsigned long i;

  if (key_length > HMAC_BLOCK_LENGTH) {
    key_length = HMAC_BLOCK_LENGTH;
  }
  for (i = 0; i < HMAC_BLOCK_LENGTH; i++) {
    pad[i] = i < key_length ? key[i] : 0;
  }
  for (i = 0; i < HMAC_BLOCK_LENGTH; i++) {
    pad[i] ^= HMAC_OPAD;
  }
  XcSHAFinal(sha, pad + HMAC_BLOCK_LENGTH);
  XcSHAInit(sha);
  XcSHAUpdate(sha, pad, sizeof(pad));
  XcSHAFinal(sha, digest);
}

/* 0x1d4230 */
static int sign_non_roamable(signature_handle_t *handle, unsigned char *digest,
                             unsigned char *signature)
{
  hmac_begin(standalone_imp_XboxHDKey, SIGNATURE_KEY_LENGTH, handle->sha);
  XcSHAUpdate(handle->sha, digest, SHA_DIGEST_LENGTH);
  hmac_end(handle->sha, standalone_imp_XboxHDKey, SIGNATURE_KEY_LENGTH, signature);
  return 1;
}

/* 0x1d426b */
int __stdcall standalone_XCalculateSignatureBegin(unsigned long flags)
{
  signature_handle_t *handle;

  handle = (signature_handle_t *)sig_LocalAlloc(0, SIGNATURE_HANDLE_SIZE);
  if (handle == 0) {
    sig_SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return INVALID_SIGNATURE_HANDLE;
  }
  handle->flags = flags;
  handle->key = standalone_imp_XboxSignatureKey;
  hmac_begin(handle->key, SIGNATURE_KEY_LENGTH, handle->sha);
  return (int)handle;
}

/* 0x1d42a9 */
int __stdcall standalone_XCalculateSignatureUpdate(signature_handle_t *handle,
                                                   const void *buffer, unsigned long size)
{
  XcSHAUpdate(handle->sha, (unsigned char *)buffer, size);
  return 0;
}

/* 0x1d42c3. The handle is freed whether or not a signature is wanted. */
int __stdcall standalone_XCalculateSignatureEnd(signature_handle_t *handle, void *signature)
{
  unsigned char digest[SHA_DIGEST_LENGTH];
  unsigned long non_roamable;
  int ok;

  ok = 1;
  non_roamable = handle->flags & XCALCSIG_FLAG_NON_ROAMABLE;
  if (signature != 0) {
    hmac_end(handle->sha, handle->key, SIGNATURE_KEY_LENGTH,
             non_roamable ? digest : (unsigned char *)signature);
    if (non_roamable) {
      ok = sign_non_roamable(handle, digest, (unsigned char *)signature);
    }
  }
  sig_LocalFree(handle);
  return ok ? 0 : (int)sig_GetLastError();
}
