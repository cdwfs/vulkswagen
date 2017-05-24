// How to draw:
// topology: triangle list
// draw 36 indices (no index/vertex buffer necessary)
// disable depth write
// enable depth test 
// disable front & back face culling
// scale cube vertices by zfar/sqrt(3) or roughly 0.575*zfar

#version 450
#pragma shader_stage(vertex)

const uint indices[6*6] = {
  0,1,2, 3,2,1, // -Z
  4,0,6, 2,6,0, // -X
  1,5,3, 7,3,5, // +X
  5,4,7, 6,7,4, // +Z
  4,5,0, 1,0,5, // -Y
  2,3,6, 7,6,3, // +Y
};

layout (set = 0, binding = 0) uniform SceneUniforms {
  vec4 time_and_res;
  vec4 eye;
  mat4 viewproj;
  mat4 view;
  mat4 proj;
  vec4 zrange; // .x=near, .y=far, .zw=unused
} scene_consts;

layout (location = 0) out vec3 texcoord;

void main() {
  uint index = indices[gl_VertexIndex];
  texcoord = vec3(
    ((index & 1) != 0) ? +1.0 : -1.0,
    ((index & 2) != 0) ? +1.0 : -1.0,
    ((index & 4) != 0) ? +1.0 : -1.0);

  mat3 view_rot = mat3(scene_consts.view);
  const float inv_sqrt3 = 0.575;  // slightly low to ensure corners are always closer than zfar
  gl_Position = scene_consts.proj * vec4(view_rot * (0.575 * scene_consts.zrange.y * texcoord), 1.0);

  // cubemaps use left-handed coordinate systems; I use right-handed. This is the easiest fix.
  texcoord.z *= -1;
}
