/*
 * Copyright (c) 2018 cTuning foundation.
 * See CK COPYRIGHT.txt for copyright details.
 *
 * SPDX-License-Identifier: BSD-3-Clause.
 * See CK LICENSE.txt for licensing details.
 */
#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <memory>
#include <array>
#include <algorithm>
#include <sys/stat.h>
#include "armnn/ArmNN.hpp"
#include "armnn/Exceptions.hpp"
#include "armnn/Tensor.hpp"
#include "armnn/INetwork.hpp"
#include "armnnTfParser/ITfParser.hpp"

#include "benchmark.h"

using namespace std;
using namespace CK;

template <typename TData, typename TInConverter, typename TOutConverter>
class ArmNNBenchmark : public Benchmark<TData, TInConverter, TOutConverter> {
public:
    ArmNNBenchmark(const BenchmarkSettings* settings, TData *in_ptr, TData *out_ptr, int input_index)
            : Benchmark<TData, TInConverter, TOutConverter>(settings, in_ptr, out_ptr) {
    }
};

armnn::InputTensors MakeInputTensors(const std::pair<armnn::LayerBindingId,
        armnn::TensorInfo>& input, const void* inputTensorData)
{
    return { {input.first, armnn::ConstTensor(input.second, inputTensorData) } };
}

armnn::OutputTensors MakeOutputTensors(const std::pair<armnn::LayerBindingId,
        armnn::TensorInfo>& output, void* outputTensorData)
{
    return { {output.first, armnn::Tensor(output.second, outputTensorData) } };
}

int main(int argc, char* argv[]) {
    bool use_neon                   = getenv_b("USE_NEON");
    bool use_opencl                 = getenv_b("USE_OPENCL");
    string input_layer_name         = getenv_s("CK_ENV_TENSORFLOW_MODEL_INPUT_LAYER_NAME");
    string output_layer_name        = getenv_s("CK_ENV_TENSORFLOW_MODEL_OUTPUT_LAYER_NAME");
    unsigned input_shape_height     = getenv_i("CK_ENV_TENSORFLOW_MODEL_IMAGE_HEIGHT");
    unsigned input_shape_width      = getenv_i("CK_ENV_TENSORFLOW_MODEL_IMAGE_WIDTH");
    unsigned input_shape_channels   = 3;

    try {
        init_benchmark();

        BenchmarkSettings settings;

        // TODO: learn how to process batches
        // currently interpreter->tensor(input_index)->dims[0] = 1
        if (settings.batch_size != 1)
            throw string("Only BATCH_SIZE=1 is currently supported");

        BenchmarkSession session(&settings);

        unique_ptr<IBenchmark> benchmark;
        armnnTfParser::ITfParserPtr parser = armnnTfParser::ITfParser::Create();
        armnn::NetworkId networkIdentifier;
        armnn::IRuntime::CreationOptions options;
        armnn::IRuntimePtr runtime = armnn::IRuntime::Create(options);
        armnn::OutputTensors outputTensor;
        armnn::InputTensors inputTensor;

        // Optimize the network for a specific runtime compute device, e.g. CpuAcc, GpuAcc
        //std::vector<armnn::BackendId> optOptions = {armnn::Compute::CpuAcc, armnn::Compute::GpuAcc};
        std::vector<armnn::BackendId> optOptions = {armnn::Compute::CpuRef};
        if( use_neon && use_opencl) {
            optOptions = {armnn::Compute::CpuAcc, armnn::Compute::GpuAcc};
        } else if( use_neon ) {
            optOptions = {armnn::Compute::CpuAcc};
        } else if( use_opencl ) {
            optOptions = {armnn::Compute::GpuAcc};
        }

        cout << "\nLoading graph..." << endl;
        measure_setup([&]{
            armnn::TensorShape input_tensor_shape({ 1, input_shape_height, input_shape_width, input_shape_channels }); // NHWC
            armnn::INetworkPtr network = parser->CreateNetworkFromBinaryFile(
                settings.graph_file.c_str(),
                { { input_layer_name, input_tensor_shape} },
                { output_layer_name }
            );
            if (!network)
                throw "Failed to load graph from file";

            armnnTfParser::BindingPointInfo inputBindingInfo = parser->GetNetworkInputBindingInfo(input_layer_name);
            armnnTfParser::BindingPointInfo outputBindingInfo = parser->GetNetworkOutputBindingInfo(output_layer_name);

            armnn::TensorShape inShape = inputBindingInfo.second.GetShape();
            armnn::TensorShape outShape = outputBindingInfo.second.GetShape();
            std::size_t inSize = inShape[0] * inShape[1] * inShape[2] * inShape[3];
            std::size_t outSize = outShape[0] * outShape[1];

            armnn::IOptimizedNetworkPtr optNet = armnn::Optimize(*network, optOptions, runtime->GetDeviceSpec());

            runtime->LoadNetwork(networkIdentifier, std::move(optNet));
            float *output = new float[outSize];
            float *input = new float[inSize];

            outputTensor = MakeOutputTensors(outputBindingInfo, output);
            inputTensor = MakeInputTensors(inputBindingInfo, input);

            benchmark.reset(new ArmNNBenchmark<float, InNormalize, OutCopy>(&settings, input, output, 0));

            int out_num = outShape[0];
            int out_classes = outShape[1];
            cout << format("Output tensor dimensions: %d*%d", out_num, out_classes) << endl;
            if (out_classes != settings.num_classes && out_classes != settings.num_classes+1)
                throw format("Unsupported number of classes in graph's output tensor. Supported numbers are %d and %d",
                             settings.num_classes, settings.num_classes+1);
            benchmark->has_background_class = out_classes == settings.num_classes+1;
        });

        cout << "\nProcessing batches..." << endl;
        measure_prediction([&]{
            while (session.get_next_batch()) {
                session.measure_begin();
                benchmark->load_images(session.batch_files());
                session.measure_end_load_images();

                session.measure_begin();
                if (runtime->EnqueueWorkload(networkIdentifier, inputTensor, outputTensor) != armnn::Status::Success)
                    throw "Failed to invoke the classifier";
                session.measure_end_prediction();

                benchmark->save_results(session.batch_files());
            }
        });

        finish_benchmark(session);
    }
    catch (const string& error_message) {
        cerr << "ERROR: " << error_message << endl;
        return -1;
    }
    return 0;
}