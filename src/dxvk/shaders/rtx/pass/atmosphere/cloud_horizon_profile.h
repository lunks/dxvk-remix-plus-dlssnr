// [nfs2se 0033] Shared C++/Slang scalar profile; no camera basis, azimuth or time.
#ifndef NFS2SE_CLOUD_HORIZON_PROFILE_H
#define NFS2SE_CLOUD_HORIZON_PROFILE_H
float cloudHorizonDensity(float sinElevation, float fadeStartSin, float fadeEndSin,
                          float zenithDensity) {
  if (zenithDensity >= 1.0f) return 1.0f; // exact legacy default
  const float span = fadeEndSin - fadeStartSin;
  float t = (sinElevation - fadeStartSin) / (span > 0.00001f ? span : 0.00001f);
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float smooth = t * t * (3.0f - 2.0f * t);
  return zenithDensity + (1.0f - zenithDensity) * (1.0f - smooth);
}
#endif
