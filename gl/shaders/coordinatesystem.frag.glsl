#version 330

in vec3 vfColor;
out vec4 fo_FragColor;

void main(void) {
  fo_FragColor = vec4(vfColor, 1.0);
}
