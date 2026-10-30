// [nfs2se 0035] Shared C++/Slang, deterministic non-tiled weather variation.
#ifndef NFS2SE_CLOUD_VARIETY_PROFILE_H
#define NFS2SE_CLOUD_VARIETY_PROFILE_H
#ifdef __cplusplus
#include <cmath>
using uint = unsigned int;
#endif
#include "cloud_opening_profile.h"
uint cloudVarietyHash(uint x, uint z) {
  uint h = x * 0x8da6b343u ^ z * 0xd8163841u ^ 0xcb1ab31fu;
  h ^= h >> 16; h *= 0x7feb352du;
  h ^= h >> 15; h *= 0x846ca68bu;
  return h ^ (h >> 16);
}
float cloudVarietyCorner(int x, int z) {
  return float(cloudVarietyHash(uint(x), uint(z)) & 0x00ffffffu) / 16777215.0f;
}
float cloudVarietyValue(float x, float z) {
  int ix = int(floor(x)), iz = int(floor(z));
  float u = cloudOpeningSmooth(x - float(ix));
  float v = cloudOpeningSmooth(z - float(iz));
  float a = cloudVarietyCorner(ix, iz), b = cloudVarietyCorner(ix+1, iz);
  float c = cloudVarietyCorner(ix, iz+1), d = cloudVarietyCorner(ix+1, iz+1);
  return (a + (b-a)*u)*(1.0f-v) + (c + (d-c)*u)*v;
}
float cloudVarietyNoise(float xKm, float zKm, float frequency) {
  float x=xKm*frequency, z=zKm*frequency;
  // Rotated second octave: no shared grid axis or baked24km texture period.
  float a=cloudVarietyValue(x+13.27f,z-7.19f);
  float b=cloudVarietyValue((0.8f*x+0.6f*z)*2.17f-31.41f,
                            (-0.6f*x+0.8f*z)*2.17f+17.73f);
  return 2.0f*(0.65f*a+0.35f*b)-1.0f;
}
float cloudVarietyOffset(float sinElevation, float cosAzimuth, float centerSin,
                         float widthSin, float innerCos, float outerCos,
                         float strengthKm, float weather) {
  if (strengthKm <= 0.0f) return 0.0f;
  float weight = 1.0f-cloudOpeningDensity(sinElevation,cosAzimuth,centerSin,
                                         widthSin,innerCos,outerCos,0.0f);
  return strengthKm * weight * weather;
}
float cloudVarietyStepBound(float sdfMinKm, float baseOffsetKm,
                            float maxOutwardKm, float strengthKm) {
  // A nearby sample may grow by the full positive weather excursion. Using
  // the CURRENT local weather here could skip a cloud in the next region.
  return sdfMinKm - baseOffsetKm - maxOutwardKm - strengthKm;
}
#endif
