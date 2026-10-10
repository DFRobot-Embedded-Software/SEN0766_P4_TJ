#include "fusion_native.h"
#include "fusion_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
using namespace fusion_weights;

void dense(const float *input, float *output, const FusionDense &layer)
{
    for (int o = 0; o < layer.outputs; ++o) {
        float sum = layer.bias ? layer.bias[o] : 0.0f;
        for (int c = 0; c < layer.inputs; ++c) sum += input[c] * layer.weight[o * layer.inputs + c];
        output[o] = sum;
    }
}

float batch_norm(float value, int channel, const FusionBN &bn)
{
    return (value - bn.mean[channel]) / std::sqrt(bn.variance[channel] + bn.epsilon)
           * bn.weight[channel] + bn.bias[channel];
}

float gelu(float value)
{
    // Match torch.nn.GELU(approximate="none"), retaining the erf definition.
    return 0.5f * value * (1.0f + std::erf(value * 0.7071067811865475f));
}

void layer_norm(const float *input, float *output, const FusionLayerNorm &norm)
{
    float mean = 0.0f, variance = 0.0f;
    for (int c = 0; c < norm.channels; ++c) mean += input[c];
    mean /= norm.channels;
    for (int c = 0; c < norm.channels; ++c) {
        const float delta = input[c] - mean;
        variance += delta * delta;
    }
    const float inverse_std = 1.0f / std::sqrt(variance / norm.channels + norm.epsilon);
    for (int c = 0; c < norm.channels; ++c)
        output[c] = (input[c] - mean) * inverse_std * norm.weight[c] + norm.bias[c];
}

void temporal_block(const float *input, float *output, const FusionMS &block,
                    FusionSensorWorkspace *w)
{
    for (int t = 0; t < 10; ++t)
        std::memcpy(w->concatenated + t * 128, input + t * 32, 32 * sizeof(float));
    for (int b = 0; b < 3; ++b) {
        const auto &branch = block.branches[b];
        for (int t = 0; t < 10; ++t) {
            for (int c = 0; c < 32; ++c) {
                float sum = 0.0f;
                for (int k = 0; k < branch.kernel; ++k) {
                    const int source = t + k - branch.kernel / 2;
                    if (source >= 0 && source < 10)
                        sum += input[source * 32 + c] * branch.depthwise[c * branch.kernel + k];
                }
                w->depthwise[t * 32 + c] = gelu(batch_norm(sum, c, branch.depthwise_bn));
            }
        }
        for (int t = 0; t < 10; ++t) {
            float *destination = w->concatenated + t * 128 + (b + 1) * 32;
            dense(w->depthwise + t * 32, destination, branch.pointwise);
            for (int c = 0; c < 32; ++c)
                destination[c] = gelu(batch_norm(destination[c], c, branch.pointwise_bn));
        }
    }
    for (int t = 0; t < 10; ++t) {
        dense(w->concatenated + t * 128, output + t * 32, block.fusion);
        for (int c = 0; c < 32; ++c) {
            const float residual = input[t * 32 + c] + batch_norm(output[t * 32 + c], c, block.fusion_bn);
            output[t * 32 + c] = std::max(0.0f, batch_norm(residual, c, block.output_bn));
        }
    }
}
}  // namespace

extern "C" size_t fusion_workspace_bytes() { return sizeof(FusionSensorWorkspace); }

extern "C" bool fusion_sensor_encode(const FusionSensorInput *input, float *output,
                                     FusionSensorWorkspace *w)
{
    if (!input || !output || !w || !std::isfinite(input->epoch_sec) ||
        input->zone < 0 || input->zone >= 7 || input->room < 0 || input->room >= 7) return false;
    for (int t = 0; t < 10; ++t) {
        float *features = w->features + t * 37;
        for (int c = 0; c < 5; ++c) {
            float value = input->env[t * 5 + c];
            if (!std::isfinite(value) || (c == 3 && value < 0)) return false;
            if (c == 3) value = std::log1p(value);
            features[c] = (value - sensor_env_mean[c]) / (sensor_env_std[c] + 1e-8f);
        }
        const float code_value = input->radar[t * 5 + 4];
        if (!std::isfinite(code_value) || code_value < 0 || code_value > 3 ||
            code_value != std::floor(code_value)) return false;
        const int code = static_cast<int>(code_value);
        float counts[4], region[20], encoded[16];
        for (int c = 0; c < 4; ++c) {
            const float value = input->radar[t * 5 + c];
            if (!std::isfinite(value) || value < 0) return false;
            counts[c] = (value - sensor_count_mean[c]) / (sensor_count_std[c] + 1e-8f);
        }
        std::memcpy(region, sensor_zoom_embedding + input->zone * 8, 8 * sizeof(float));
        dense(counts, region + 8, count_projection);
        std::memcpy(region + 16, sensor_event_embedding + code * 4, 4 * sizeof(float));
        dense(region, encoded, region_projection);
        std::memcpy(features + 5, encoded, 16 * sizeof(float));
        // Round t_day to float32 before normalization, matching the Python input.
        const float t_day = static_cast<float>((input->epoch_sec + t - sensor_t_ref) / 86400.0);
        const float time = (t_day - sensor_time_mean[0]) / (sensor_time_std[0] + 1e-8f);
        dense(&time, features + 21, time_linear);
        dense(&time, features + 22, time_periodic);
        for (int c = 22; c < 29; ++c) features[c] = std::sin(features[c]);
        std::memcpy(features + 29, sensor_room_embedding + input->room * 8, 8 * sizeof(float));
        dense(features, w->x + t * 32, input_projection);
        for (int c = 0; c < 32; ++c) w->x[t * 32 + c] = gelu(batch_norm(w->x[t * 32 + c], c, input_bn));
    }
    temporal_block(w->x, w->y, blocks[0], w);
    temporal_block(w->y, w->x, blocks[1], w);
    for (int c = 0; c < 32; ++c) {
        float sum = 0.0f;
        for (int t = 0; t < 10; ++t) sum += w->x[t * 32 + c];
        output[c] = sum / 10.0f;
        if (!std::isfinite(output[c])) return false;
    }
    return true;
}

extern "C" void fusion_classify(const float *audio, const float *sensor, float *logits)
{
    float projected[48], joined[80], hidden[32];
    dense(audio, projected, audio_projection);
    layer_norm(projected, joined, audio_norm);
    layer_norm(sensor, joined + 48, sensor_norm);
    dense(joined, hidden, mlp_hidden);
    for (float &value : hidden) value = std::max(0.0f, value);
    dense(hidden, logits, mlp_output);
}
