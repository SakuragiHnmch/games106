#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inColor;
layout (location = 4) in vec4 inJointIndices;
layout (location = 5) in vec4 inJointWeights;
layout (location = 6) in vec4 inTangent;

// global
layout (set = 0, binding = 0) uniform UBOScene
{
	mat4 projection;
	mat4 view;
	mat4 model;
	vec4 lightPos[4];
	vec4 viewPos;
} uboScene;

// per node descriptor
layout (set = 2, binding = 0) uniform UBOBone
{
	mat4 nodeMatrix;
	mat4 jointMatrices[64];
	float jointCount;
} uboBone;

layout (location = 0) out vec3 outWorldPos;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec4 outTangent;

void main() 
{
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

		outWorldPos = vec3(uboScene.model * skinMat * vec4(inPos.xyz, 1.0));
		outNormal = mat3(uboScene.model * skinMat) * inNormal;
		outTangent = vec4(mat3(uboScene.model * skinMat) * inTangent.xyz, inTangent.w);
		gl_Position = uboScene.projection * uboScene.view * vec4(outWorldPos, 1.0);
	} else {
		outWorldPos = vec3(uboScene.model * uboBone.nodeMatrix * vec4(inPos.xyz, 1.0));
		outNormal = mat3(uboScene.model * uboBone.nodeMatrix) * inNormal;
		outTangent = vec4(mat3(uboScene.model * uboBone.nodeMatrix) * inTangent.xyz, inTangent.w);
		gl_Position = uboScene.projection * uboScene.view * vec4(outWorldPos, 1.0);
	}

	// flip Y
	gl_Position.y = -gl_Position.y;
}