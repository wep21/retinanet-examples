/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "engine.h"

#include <iostream>
#include <fstream>

#include <NvOnnxConfig.h>
#include <NvOnnxParser.h>

#include "plugins/DecodePlugin.h"
#include "plugins/NMSPlugin.h"
#include "plugins/DecodeRotatePlugin.h"
#include "plugins/NMSRotatePlugin.h"
#include "calibrator.h"

#include <stdio.h>
#include <string>

using namespace nvinfer1;
using namespace nvonnxparser;

namespace odtk {

class Logger : public ILogger {
public:
    Logger(bool verbose)
        : _verbose(verbose) {
    }

    void log(Severity severity, const char *msg) noexcept override {
        if (_verbose || ((severity != Severity::kINFO) && (severity != Severity::kVERBOSE)))
            cout << msg << endl;
    }

private:
   bool _verbose{false};
};

void Engine::_load(const string &path) {
    ifstream file(path, ios::in | ios::binary);
    file.seekg (0, file.end);
    size_t size = file.tellg();
    file.seekg (0, file.beg);

    auto buffer = std::unique_ptr<char[]>(new char[size]);
    file.read(buffer.get(), size);
    file.close();

    _engine = std::unique_ptr<ICudaEngine>(_runtime->deserializeCudaEngine(buffer.get(), size));
}

void Engine::_prepare() {
    _context = std::unique_ptr<IExecutionContext>(_engine->createExecutionContext());
    _context->setOptimizationProfileAsync(0, _stream);
    cudaStreamCreate(&_stream);
}

Engine::Engine(const string &engine_path, bool verbose) {
    Logger logger(verbose);
    _runtime = std::unique_ptr<IRuntime>(createInferRuntime(logger));
    _load(engine_path);
    _prepare();
}

Engine::~Engine() {
    if (_stream) cudaStreamDestroy(_stream);
}

Engine::Engine(const char *onnx_model, size_t onnx_size, const vector<int>& dynamic_batch_opts,
    string precision, float score_thresh, int top_n, const vector<vector<float>>& anchors, 
    bool rotated, float nms_thresh, int detections_per_im, const vector<string>& calibration_images,
    string model_name, string calibration_table, bool verbose, size_t workspace_size) {

    Logger logger(verbose);
    _runtime = std::unique_ptr<IRuntime>(createInferRuntime(logger));

    bool fp16 = precision.compare("FP16") == 0;
    bool int8 = precision.compare("INT8") == 0;

    // Create builder
    auto builder = std::unique_ptr<IBuilder>(createInferBuilder(logger));
    auto builderConfig = std::unique_ptr<IBuilderConfig>(builder->createBuilderConfig());
    // Allow use of FP16 layers when running in INT8
    if(fp16 || int8) builderConfig->setFlag(BuilderFlag::kFP16);
    builderConfig->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, workspace_size);
    
    // Parse ONNX FCN
    cout << "Building " << precision << " core model..." << endl;
    const auto flags = 1U << static_cast<int>(NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    auto network = std::unique_ptr<INetworkDefinition>(builder->createNetworkV2(flags));
    auto parser = std::unique_ptr<IParser>(createParser(*network, logger));
    parser->parse(onnx_model, onnx_size);
    
    auto input = network->getInput(0);
    auto inputDims = input->getDimensions();
    auto profile = builder->createOptimizationProfile();
    auto inputName = input->getName();
    auto profileDimsmin = Dims4{dynamic_batch_opts[0], inputDims.d[1], inputDims.d[2], inputDims.d[3]};
    auto profileDimsopt = Dims4{dynamic_batch_opts[1], inputDims.d[1], inputDims.d[2], inputDims.d[3]};
    auto profileDimsmax = Dims4{dynamic_batch_opts[2], inputDims.d[1], inputDims.d[2], inputDims.d[3]};

    profile->setDimensions(inputName, nvinfer1::OptProfileSelector::kMIN, profileDimsmin);
    profile->setDimensions(inputName, nvinfer1::OptProfileSelector::kOPT, profileDimsopt);
    profile->setDimensions(inputName, nvinfer1::OptProfileSelector::kMAX, profileDimsmax);
    
    if(profile->isValid())
        builderConfig->addOptimizationProfile(profile);

    std::unique_ptr<Int8EntropyCalibrator> calib;
    if (int8) {
        builderConfig->setFlag(BuilderFlag::kINT8);
        // Calibration is performed using kOPT values of the profile.
        // Calibration batch size must match this profile.
        builderConfig->setCalibrationProfile(profile);
        ImageStream stream(dynamic_batch_opts[1], inputDims, calibration_images);
        calib = std::unique_ptr<Int8EntropyCalibrator>(new Int8EntropyCalibrator(stream, model_name, calibration_table));
        builderConfig->setInt8Calibrator(calib.get());
    }

    // Add decode plugins
    cout << "Building accelerated plugins..." << endl;
    vector<DecodePlugin> decodePlugins;
    vector<DecodeRotatePlugin> decodeRotatePlugins;
    vector<ITensor *> scores, boxes, classes;
    auto nbOutputs = network->getNbOutputs();
    
    for (int i = 0; i < nbOutputs / 2; i++) {
        auto classOutput = network->getOutput(i);
        auto boxOutput = network->getOutput(nbOutputs / 2 + i);
        auto outputDims = classOutput->getDimensions();
        int scale = inputDims.d[2] / outputDims.d[2];
        auto decodePlugin = DecodePlugin(score_thresh, top_n, anchors[i], scale);
        auto decodeRotatePlugin = DecodeRotatePlugin(score_thresh, top_n, anchors[i], scale);
        decodePlugins.push_back(decodePlugin); 
        decodeRotatePlugins.push_back(decodeRotatePlugin);
        vector<ITensor *> inputs = {classOutput, boxOutput};
        auto layer = (!rotated) ? network->addPluginV2(inputs.data(), inputs.size(), decodePlugin) \
                    : network->addPluginV2(inputs.data(), inputs.size(), decodeRotatePlugin);
        scores.push_back(layer->getOutput(0));
        boxes.push_back(layer->getOutput(1));
        classes.push_back(layer->getOutput(2));
    }

    // Cleanup outputs
    for (int i = 0; i < nbOutputs; i++) {
        auto output = network->getOutput(0);
        network->unmarkOutput(*output);
    }

    // Concat tensors from each feature map
    vector<ITensor *> concat;
    for (auto tensors : {scores, boxes, classes}) {
        auto layer = network->addConcatenation(tensors.data(), tensors.size());
        concat.push_back(layer->getOutput(0));
    }
    
    // Add NMS plugin
    auto nmsPlugin = NMSPlugin(nms_thresh, detections_per_im);
    auto nmsRotatePlugin = NMSRotatePlugin(nms_thresh, detections_per_im);
    auto layer = (!rotated) ? network->addPluginV2(concat.data(), concat.size(), nmsPlugin) \
                : network->addPluginV2(concat.data(), concat.size(), nmsRotatePlugin);
    vector<string> names = {"scores", "boxes", "classes"};
    for (int i = 0; i < layer->getNbOutputs(); i++) {
        auto output = layer->getOutput(i);
        network->markOutput(*output);
        output->setName(names[i].c_str());
    }
    
    // Build engine
    cout << "Applying optimizations and building TRT CUDA engine..." << endl;
    _plan = std::unique_ptr<IHostMemory>(builder->buildSerializedNetwork(*network, *builderConfig));
}

void Engine::save(const string &path) {
    cout << "Writing to " << path << "..." << endl;
    ofstream file(path, ios::out | ios::binary);
    file.write(reinterpret_cast<const char*>(_plan->data()), _plan->size());
}

void Engine::infer(vector<void *> &buffers, int batch){
    for (int32_t i = 0, e = _engine->getNbIOTensors(); i < e; i++)
    {
        auto const name = _engine->getIOTensorName(i);
        _context->setTensorAddress(name, buffers.at(i));
    }
    _context->enqueueV3(_stream);
    cudaStreamSynchronize(_stream);
}

vector<int> Engine::getInputSize() {
    const auto dims = _engine->getTensorShape(
	_engine->getIOTensorName(0)
    );
    return {dims.d[2], dims.d[3]};
}

int Engine::getMaxBatchSize() {
    return 1;
}

int Engine::getMaxDetections() {
    const auto dims = _engine->getTensorShape(
        _engine->getIOTensorName(1)
    );    
    return dims.d[1];
}

int Engine::getStride() {
    return 1;
}

}
