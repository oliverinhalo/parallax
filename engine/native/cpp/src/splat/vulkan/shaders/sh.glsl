// Real spherical harmonics, bands 0 to 3, matching evaluateSh() on the host.
#ifndef PARALLAX_SPLAT_SH_GLSL
#define PARALLAX_SPLAT_SH_GLSL

#define SH_C1 0.4886025119029199
const float kShC2[5] = float[5](1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
                                -1.0925484305920792, 0.5462742152960396);
const float kShC3[7] = float[7](-0.5900435899266435, 2.890611442640554, -0.4570457994644658,
                                0.3731763325901154, -0.4570457994644658, 1.445305721320277,
                                -0.5900435899266435);

/// Fills `basis` and returns how many entries were written.
int shBasis(int degree, vec3 d, out float basis[15]) {
  for (int i = 0; i < 15; ++i) basis[i] = 0.0;
  if (degree < 1) return 0;
  float x = d.x, y = d.y, z = d.z;
  basis[0] = -SH_C1 * y;
  basis[1] = SH_C1 * z;
  basis[2] = -SH_C1 * x;
  if (degree < 2) return 3;
  float xx = x * x, yy = y * y, zz = z * z;
  float xy = x * y, yz = y * z, xz = x * z;
  basis[3] = kShC2[0] * xy;
  basis[4] = kShC2[1] * yz;
  basis[5] = kShC2[2] * (2.0 * zz - xx - yy);
  basis[6] = kShC2[3] * xz;
  basis[7] = kShC2[4] * (xx - yy);
  if (degree < 3) return 8;
  basis[8] = kShC3[0] * y * (3.0 * xx - yy);
  basis[9] = kShC3[1] * xy * z;
  basis[10] = kShC3[2] * y * (4.0 * zz - xx - yy);
  basis[11] = kShC3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy);
  basis[12] = kShC3[4] * x * (4.0 * zz - xx - yy);
  basis[13] = kShC3[5] * z * (xx - yy);
  basis[14] = kShC3[6] * x * (xx - 3.0 * yy);
  return 15;
}

void shBasisJacobian(int count, vec3 d, out vec3 jacobian[15]) {
  for (int i = 0; i < 15; ++i) jacobian[i] = vec3(0.0);
  if (count < 1) return;
  float x = d.x, y = d.y, z = d.z;
  jacobian[0] = vec3(0.0, -SH_C1, 0.0);
  jacobian[1] = vec3(0.0, 0.0, SH_C1);
  jacobian[2] = vec3(-SH_C1, 0.0, 0.0);
  if (count < 8) return;
  float xx = x * x, yy = y * y, zz = z * z;
  jacobian[3] = kShC2[0] * vec3(y, x, 0.0);
  jacobian[4] = kShC2[1] * vec3(0.0, z, y);
  jacobian[5] = kShC2[2] * vec3(-2.0 * x, -2.0 * y, 4.0 * z);
  jacobian[6] = kShC2[3] * vec3(z, 0.0, x);
  jacobian[7] = kShC2[4] * vec3(2.0 * x, -2.0 * y, 0.0);
  if (count < 15) return;
  jacobian[8] = kShC3[0] * vec3(6.0 * x * y, 3.0 * xx - 3.0 * yy, 0.0);
  jacobian[9] = kShC3[1] * vec3(y * z, x * z, x * y);
  jacobian[10] = kShC3[2] * vec3(-2.0 * x * y, 4.0 * zz - xx - 3.0 * yy, 8.0 * y * z);
  jacobian[11] = kShC3[3] * vec3(-6.0 * x * z, -6.0 * y * z, 6.0 * zz - 3.0 * xx - 3.0 * yy);
  jacobian[12] = kShC3[4] * vec3(4.0 * zz - 3.0 * xx - yy, -2.0 * x * y, 8.0 * x * z);
  jacobian[13] = kShC3[5] * vec3(2.0 * x * z, -2.0 * y * z, xx - yy);
  jacobian[14] = kShC3[6] * vec3(3.0 * xx - 3.0 * yy, -6.0 * x * y, 0.0);
}

#endif  // PARALLAX_SPLAT_SH_GLSL
