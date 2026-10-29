// [nfs2se 0034] Shared C++/Slang profile; artistic density removal only.
#ifndef NFS2SE_CLOUD_OPENING_PROFILE_H
#define NFS2SE_CLOUD_OPENING_PROFILE_H
float cloudOpeningSmooth(float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t * t * (3.0f - 2.0f * t);
}
float cloudOpeningDensity(float sinElevation, float cosAzimuth, float centerSin,
                          float widthSin, float innerCos, float outerCos,
                          float minimumDensity) {
  if (minimumDensity >= 1.0f) return 1.0f;
  float distance = sinElevation - centerSin;
  distance = distance < 0.0f ? -distance : distance;
  if (cosAzimuth <= outerCos || distance >= widthSin) return 1.0f;
  const float span = innerCos - outerCos;
  const float azimuth = cloudOpeningSmooth((cosAzimuth - outerCos) / (span > 0.00001f ? span : 0.00001f));
  const float elevation = cloudOpeningSmooth(1.0f - distance / (widthSin > 0.00001f ? widthSin : 0.00001f));
  return 1.0f - (1.0f - minimumDensity) * azimuth * elevation;
}
#endif
