/* Port of the XDK DirectSound I3DL2 reverb parameter builder
 * (cachebeta.xbe FUN_0020ab80, called from CMcpxAPU_Commit3dSettings).
 * See xdk_i3dl2.c. */
#ifndef XDK_I3DL2_H
#define XDK_I3DL2_H

#include <stdint.h>

/* listener: DSI3DL2LISTENER as 12 dwords (LONG fields as int32, FLOAT fields
 *           as their IEEE bit patterns).
 * state:    0x220-byte buffer. Bytes 0..0x117 must hold the reverb effect's
 *           DSP state (GetEffectData offset 0). The function rewrites the
 *           dwords 0xac+8k (k = 0..13) as scratch and fills 0x118..0x21f. */
void xdk_i3dl2_reverb_parameters(const int32_t listener[12], uint8_t state[0x220]);

#endif
