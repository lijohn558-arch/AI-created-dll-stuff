#version 450
// PoC-B v0.1 — 注入用三角形 (顶点着色器)
// 无顶点缓冲: 用 gl_VertexIndex 从常量数组取坐标 (Vulkan 标准最小三角形写法)
// NDC 方向: Vulkan 的 y 轴向下 (viewport 变换 y=-1 落在视口顶) → 顶点朝上须取 y<0
layout(location = 0) out vec3 vColor;

const vec2 P[3] = vec2[3](vec2(-0.66, 0.60), vec2(0.66, 0.60), vec2(0.0, -0.72));
const vec3 C[3] = vec3[3](vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 0.55, 1.0));

void main()
{
	gl_Position = vec4(P[gl_VertexIndex], 0.0, 1.0);
	vColor = C[gl_VertexIndex];
}
