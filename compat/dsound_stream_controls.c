/* Xbox DirectSound stream controls for the silent backend. The XDK exports
 * these setters directly; the stream's seven-entry XMediaObject vtable handles
 * lifetime/status/packets only. Nothing is audible, so accept every setting.
 * dsound_nxdk.c implements them for the audible backends. */
#include "xbox.h"

int __stdcall IDirectSoundStream_SetMixBins(void *stream, unsigned int mix_bin_mask)
{
  (void)stream;
  (void)mix_bin_mask;
  return 0;
}

int __stdcall IDirectSoundStream_SetMixBinVolumes_12(void *stream,
                                                    unsigned int mix_bin_mask,
                                                    const int *volumes)
{
  (void)stream;
  (void)mix_bin_mask;
  (void)volumes;
  return 0;
}

int __stdcall IDirectSoundStream_SetAllParameters(void *stream, void *params,
                                                 int deferred)
{
  (void)stream;
  (void)params;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetMode(void *stream, int mode, int deferred)
{
  (void)stream;
  (void)mode;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetPosition(void *stream, float x, float y,
                                            float z, int deferred)
{
  (void)stream;
  (void)x;
  (void)y;
  (void)z;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetVelocity(void *stream, float x, float y,
                                            float z, int deferred)
{
  (void)stream;
  (void)x;
  (void)y;
  (void)z;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetConeOrientation(void *stream, float x, float y,
                                                   float z, int deferred)
{
  (void)stream;
  (void)x;
  (void)y;
  (void)z;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetMinDistance(void *stream, float distance,
                                               int deferred)
{
  (void)stream;
  (void)distance;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetMaxDistance(void *stream, float distance,
                                               int deferred)
{
  (void)stream;
  (void)distance;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetConeAngles(void *stream, int inner_angle,
                                              int outer_angle, int deferred)
{
  (void)stream;
  (void)inner_angle;
  (void)outer_angle;
  (void)deferred;
  return 0;
}

int __stdcall IDirectSoundStream_SetConeOutsideVolume(void *stream, int volume,
                                                     int deferred)
{
  (void)stream;
  (void)volume;
  (void)deferred;
  return 0;
}
