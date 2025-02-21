#version 450

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inTangent;

layout (location = 0) out vec4 outFragColor;

// per material descriptor
layout (set = 1, binding = 0) uniform sampler2D albedoMap;
layout (set = 1, binding = 1) uniform sampler2D normalMap;
layout (set = 1, binding = 2) uniform sampler2D aoMap;
layout (set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;

layout (set = 0, binding = 0) uniform UBOScene
{
	mat4 projection;
	mat4 view;
	mat4 model;
	vec4 lightPos[4];
	vec4 viewPos;
} uboScene;

layout (set = 0, binding = 1) uniform samplerCube irradianceMap;
layout (set = 0, binding = 2) uniform samplerCube prefilterMap;
layout (set = 0, binding = 3) uniform sampler2D brdfLUT;

#define PI 3.1415926535897932384626433832795
#define ALBEDO pow(texture(albedoMap, inUV).rgb, vec3(2.2))

// Normal Distribution function
float D_GGX(float dotNH, float roughness) {
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float denom = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
	return (alpha2) / (PI * denom * denom);
}

// Geoemtric Shadowing function
float G_SchlicksmithGGX(float dotNL, float dotNV, float roughness) {
	float r = (roughness + 1.0);
	float k = (r * r) / 8.0;
	float GL = dotNL / (dotNL * (1.0 - k) + k);
	float GV = dotNV / (dotNV * (1.0 - k) + k);
	return GL * GV;
}

// Fresnel function
vec3 F_Schlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}
vec3 F_SchlickR(float cosTheta, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 calculateNormal() {
	vec3 tangentNormal = texture(normalMap, inUV).xyz * 2.0 - 1.0;

	vec3 N = normalize(inNormal);
	vec3 T = normalize(inTangent.xyz);
	vec3 B = normalize(cross(N, T));
	mat3 TBN = mat3(T, B, N);
	return normalize(TBN * tangentNormal);
}

vec3 directLightsContribution(vec3 L, vec3 V, vec3 N, vec3 F0, float metallic, float roughness) {
	// Precalculate vectors and dot products	
	vec3 H = normalize (V + L);
	float dotNH = clamp(dot(N, H), 0.0, 1.0);
	float dotNV = clamp(dot(N, V), 0.0, 1.0);
	float dotNL = clamp(dot(N, L), 0.0, 1.0);

	// Light color fixed vec3(1.0)

	float D = D_GGX(dotNH, roughness); 
	float G = G_SchlicksmithGGX(dotNL, dotNV, roughness);
	vec3 F = F_Schlick(dotNV, F0);		

	vec3 specluar = D * F * G / (4.0 * dotNL * dotNV + 0.001);

	vec3 kS = F;
	vec3 kD = vec3(1.0) - kS;
	kD *= 1.0 - metallic;

	return (kD * ALBEDO / PI + specluar) * dotNL;
}

vec3 prefilteredReflection(vec3 R, float roughness)
{
	const float MAX_REFLECTION_LOD = 9.0;
	float lod = roughness * MAX_REFLECTION_LOD;
	float lodf = floor(lod);
	float lodc = ceil(lod);
	vec3 a = textureLod(prefilterMap, R, lodf).rgb;
	vec3 b = textureLod(prefilterMap, R, lodc).rgb;
	return mix(a, b, lod - lodf);
}

vec3 Tonemap_ACES(vec3 c) {
    //ACES RRT/ODT curve fit courtesy of Stephen Hill
	vec3 a = c * (c + 0.0245786) - 0.000090537;
	vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
	return a / b;
}

void main() 
{
	vec3 N = calculateNormal();
	vec3 V = normalize(uboScene.viewPos.xyz - inWorldPos);
	vec3 R = reflect(-V, N);

	vec3 albedo = ALBEDO;
	float metallic = texture(metallicRoughnessMap, inUV).r;
	float roughness = texture(metallicRoughnessMap, inUV).g;
	float ao = texture(aoMap, inUV).r;

	vec3 F0 = vec3(0.04);
	F0 = mix(F0, albedo, metallic);

	// calculate direct light contribution
	vec3 Lo = vec3(0.0);
	for (int i = 0; i < uboScene.lightPos.length(); i++) {
		vec3 L = normalize(uboScene.lightPos[i].xyz - inWorldPos);
		Lo += directLightsContribution(L, V, N, F0, metallic, roughness);
	}

	// ambient light
	// replace hardcode ambient light with environments lighting
	vec3 F = F_SchlickR(max(dot(N, V), 0.0), F0, roughness);

	vec3 kS = F;
	vec3 kD = 1.0 -kS;
	kD *= 1.0 - metallic;

	vec3 irradiance = texture(irradianceMap, N).rgb;
	vec3 diffuse = irradiance * albedo;

	// sample both prefilterMap and brdfLUT
	// combine them together as per Split-Sum approximation
	// to get the IBL specluar part
	vec3 prefilteredColor = prefilteredReflection(R, roughness);
	vec2 brdf = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
	vec3 specluar = prefilteredColor * (F * brdf.x + brdf.y);

	// final IBL ambient
	vec3 ambient = (kD * diffuse + specluar) * ao;

	vec3 color = ambient + Lo;

	// tonemapping
	color = Tonemap_ACES(color);

	// gamma correction
	color = pow(color, vec3(1.0 / 2.2));

	outFragColor = vec4(color, 1.0);
}