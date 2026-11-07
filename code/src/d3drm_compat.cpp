// The one function Mcm2.exe imported from d3drm.dll, which Windows 11 no
// longer ships. Behaviour matches d3drm (and Wine's reimplementation):
// the axis is normalized in place, and the result is normalized.
#include <windows.h>
#include <cmath>

struct Vec3 { float x, y, z; };
struct Quat { float s; Vec3 v; };

static Vec3* Normalize(Vec3* u) {
  float len = std::sqrt(u->x * u->x + u->y * u->y + u->z * u->z);
  if (len != 0.0f) {
    u->x /= len; u->y /= len; u->z /= len;
  } else {
    u->x = 1.0f; u->y = 0.0f; u->z = 0.0f;
  }
  return u;
}

static Quat Mul(const Quat& a, const Quat& b) {
  Quat r;
  r.s = a.s * b.s - (a.v.x * b.v.x + a.v.y * b.v.y + a.v.z * b.v.z);
  r.v.x = a.s * b.v.x + b.s * a.v.x + (a.v.y * b.v.z - a.v.z * b.v.y);
  r.v.y = a.s * b.v.y + b.s * a.v.y + (a.v.z * b.v.x - a.v.x * b.v.z);
  r.v.z = a.s * b.v.z + b.s * a.v.z + (a.v.x * b.v.y - a.v.y * b.v.x);
  return r;
}

extern "C" Vec3* WINAPI D3DRMVectorRotate(Vec3* r, Vec3* v, Vec3* axis,
                                                                float theta) {
  Vec3 n = *Normalize(axis);
  float c = std::cos(theta * 0.5f), s = std::sin(theta * 0.5f);
  Quat q1{c, {n.x * s, n.y * s, n.z * s}};
  Quat q2{c, {-n.x * s, -n.y * s, -n.z * s}};
  Quat p{0.0f, *v};
  Quat out = Mul(Mul(q1, p), q2);
  *r = *Normalize(&out.v);
  return r;
}
