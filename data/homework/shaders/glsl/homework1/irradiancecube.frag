#version 450

// Generates an irradiance cube from an environment map using convolution

layout (location = 0) in vec3 inWorldPos;

layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform samplerCube samplerEnv;

layout(push_constant) uniform PushConsts {
	layout (offset = 64) float deltaPhi;
	layout (offset = 68) float deltaTheta;
} consts;

#define PI 3.1415926535897932384626433832795

void main() {
    vec3 N = normalize(inWorldPos);

    // tangent space calculation from origin point
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    const float TWO_PI = PI * 2.0;
	const float HALF_PI = PI * 0.5;

    vec3 irradiance = vec3(0.0);

    // irradiance的卷积采样区域是一个半球，所以积分范围是[0, 2PI] [0, PI / 2]
    float sampleDelta = 0.025;
    uint samlerCount = 0u;

    // 对wi进行卷积，wi的分布范围是整个半球
    for (float phi = 0.0; phi < TWO_PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < HALF_PI; theta += sampleDelta) {
            // spherical to cartesian (in tangent space)
            vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));

            // tangent space to world
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N; 

            irradiance += texture(samplerEnv, sampleVec).rgb * cos(theta) * sin(theta);
            samlerCount++;
        }
    }

    irradiance = PI * irradiance * (1.0 / float(samlerCount));

    outColor = vec4(irradiance, 1.0);
}