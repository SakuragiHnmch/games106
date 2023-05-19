#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inColor;
layout (location = 4) in vec4 inJointIndices;
layout (location = 5) in vec4 inJointWeights;

// global descriptor
layout (set = 0, binding = 0) uniform UBOScene
{
	mat4 projection;
	mat4 view;
	mat4 model;
	vec4 lightPos;
	vec4 viewPos;
} uboScene;

// per node descriptor
layout (set = 2, binding = 0) uniform UBOBone
{
	mat4 nodeMatrix;
	mat4 jointMatrices[64];
	float jointCount;
} uboBone;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out vec3 outLightVec;

void main() 
{
	outNormal = inNormal;
	outColor = inColor;
	outUV = inUV;

	if (uboBone.jointCount != 0) {
		// skeleton Animation

		// Calculate skinned matrix from weights and joint indices of currrent vertex
		// The matrix of node hierarchy is embedded in jointMatrix
		mat4 skinMat = 
			inJointWeights.x * uboBone.jointMatrices[int(inJointIndices.x)] +
			inJointWeights.y * uboBone.jointMatrices[int(inJointIndices.y)] +
			inJointWeights.z * uboBone.jointMatrices[int(inJointIndices.z)] +
			inJointWeights.w * uboBone.jointMatrices[int(inJointIndices.w)];

		gl_Position = uboScene.projection * uboScene.view * skinMat * vec4(inPos.xyz, 1.0);
	} else {
		gl_Position = uboScene.projection * uboScene.view * uboBone.nodeMatrix * vec4(inPos.xyz, 1.0);
	}
	
	vec4 pos = uboScene.view * vec4(inPos, 1.0);
	outNormal = mat3(uboScene.view) * inNormal;
	outLightVec = uboScene.lightPos.xyz - pos.xyz;
	outViewVec = uboScene.viewPos.xyz - pos.xyz;	
}