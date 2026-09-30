#ifndef HALO_XNET_SECURITY_H
#define HALO_XNET_SECURITY_H
#include "xnet.h"
#include <stddef.h>
#define XSEC_KEYS 128
#define XSEC_PEERS 64
#define XSEC_MAX_SEGMENT 1400
#define XSEC_MAX_PACKET 1440
#define XSEC_QUEUE 4
/* All calls are serialized by the caller (the lwIP core lock on Xbox).
 * Crypto callbacks return zero on success. No packet or callback owns a pointer
 * into the context after returning. Wire format: cachebeta 01.10.12.2276. */
struct xsec_ops {
  int (*random)(void *, void *, unsigned);
  void (*hmac)(void *, const uint8_t *, const void *, unsigned, const void *,
               unsigned, uint8_t *);
  int (*cbc)(void *, int, const uint8_t *, unsigned, const uint8_t *, uint8_t *,
             unsigned);
  int (*modexp)(void *, const uint8_t *, const uint8_t *, uint8_t *);
  int (*send)(void *, const uint8_t *, const uint8_t *, unsigned, int);
  void (*deliver)(void *, uint32_t, uint8_t, const uint8_t *, unsigned);
};
struct xsec_key {
  int used;
  uint8_t id[8], auth[16], exponent[96], public_value[96];
};
struct xsec_queued {
  unsigned len;
  uint8_t protocol, data[XSEC_MAX_SEGMENT];
};
struct xsec_peer {
  int used, state, key, initiator;
  struct halo_xnaddr addr;
  uint32_t local_spi, remote_spi, tx_sequence, rx_base, rx_bits, retry_at,
      deadline;
  uint64_t remote_serial;
  uint8_t nonce_local[8], nonce_remote[8], tx_auth[16], rx_auth[16], tx_des[8],
      rx_des[8], iv[8];
  unsigned queued, retries;
  struct xsec_queued queue[XSEC_QUEUE];
};
struct xsec {
  struct xsec_ops ops;
  void *user;
  uint8_t mac[6], title_auth[16], title_des[24];
  uint16_t generation;
  uint64_t serial;
  uint32_t now;
  struct xsec_key keys[XSEC_KEYS];
  struct xsec_peer peers[XSEC_PEERS];
};
extern const uint8_t xsec_modulus[96];
int xsec_init(struct xsec *, const struct xsec_ops *, void *, const uint8_t *,
              const uint8_t *, const uint8_t *);
void xsec_clear(struct xsec *);
int xsec_register(struct xsec *, const uint8_t *, const uint8_t *);
int xsec_unregister(struct xsec *, const uint8_t *);
int xsec_map(struct xsec *, const struct halo_xnaddr *, const uint8_t *,
             uint32_t *);
int xsec_send_segment(struct xsec *, uint32_t, uint8_t, const uint8_t *,
                      unsigned);
int xsec_receive(struct xsec *, const uint8_t *, const uint8_t *, unsigned,
                 int);
void xsec_tick(struct xsec *, uint32_t);
/* Exposed for independent reference fixtures, also used by the session path. */
int xsec_encode(struct xsec *, const uint8_t *, const uint8_t *, unsigned,
                uint32_t, uint32_t, const uint8_t *, uint8_t, const uint8_t *,
                unsigned, uint8_t *, unsigned *);
int xsec_decode(struct xsec *, const uint8_t *, const uint8_t *, unsigned,
                const uint8_t *, unsigned, uint8_t *, unsigned *, uint8_t *);
#endif
