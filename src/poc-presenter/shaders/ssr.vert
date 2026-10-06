#version 450
// SSR v1 全屏三角 (顶点着色器)
// 无顶点缓冲: gl_VertexIndex 取 (0,0)(2,0)(0,2) 三点, 覆盖整个视口 (屏幕外像素被裁掉)
// Vulkan 正视口下 NDC y=-1 落在视口顶 => vUV.y=0 = 图像第 0 行 = D3D11 的 v=0 (顶部)
// 与 2c 入向/2d 出向镜像的行序一致, 采样时不再需要翻转。
layout(location = 0) out vec2 vUV;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	vUV = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
