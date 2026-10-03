#version 450
// PoC-B v0.1 — 注入用三角形 (片元着色器)
// 直通插值色, 不透明 (alpha=1): 注入矩形必须完全覆盖该屏幕区域, 便于像素探针判读
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(vColor, 1.0);
}
