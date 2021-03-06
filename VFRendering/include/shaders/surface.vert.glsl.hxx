#ifndef SURFACE_VERT_GLSL_HXX
#define SURFACE_VERT_GLSL_HXX

static const std::string SURFACE_VERT_GLSL = R"LITERAL(
#version 330

uniform mat4 uProjectionMatrix;
uniform mat4 uModelviewMatrix;
in vec3 ivPosition;
in vec3 ivDirection;
out vec3 vfPosition;
out vec3 vfDirection;


void main(void) {
    vfPosition = ivPosition;
  vfDirection = normalize(ivDirection);
  gl_Position = uProjectionMatrix * (uModelviewMatrix * vec4(ivPosition, 1.0));
}
)LITERAL";

#endif

