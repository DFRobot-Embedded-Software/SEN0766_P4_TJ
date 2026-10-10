#pragma once
#include <cstddef>

struct FusionDense {
    int inputs, outputs;
    const float *weight, *bias;
};
struct FusionBN {
    const float *weight, *bias, *mean, *variance;
    float epsilon;
};
struct FusionDS {
    int kernel;
    const float *depthwise;
    FusionBN depthwise_bn;
    FusionDense pointwise;
    FusionBN pointwise_bn;
};
struct FusionMS {
    FusionDS branches[3];
    FusionDense fusion;
    FusionBN fusion_bn, output_bn;
};
struct FusionLayerNorm {
    int channels;
    const float *weight, *bias;
    float epsilon;
};
struct FusionSensorInput {
    float radar[50], env[50];
    double epoch_sec;
    int zone, room;
};
struct FusionSensorWorkspace {
    float features[370], x[320], y[320], depthwise[320], concatenated[1280];
};

extern "C" size_t fusion_workspace_bytes();
extern "C" bool fusion_sensor_encode(const FusionSensorInput *input, float *features32,
                                     FusionSensorWorkspace *workspace);
extern "C" void fusion_classify(const float *audio96, const float *sensor32, float *logits6);
