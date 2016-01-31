#include <cstdint>

#include <fstream>
#include <sstream>
#include <vector>

#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION 120
#include <CL/cl2.hpp>

#include "common.hpp"


static_assert(sizeof(std::uint32_t) == sizeof(cl_uint), "OpenCL uint has to be 32bit");
static_assert(sizeof(char32_t) == sizeof(cl_char4), "OpenCL char4 has not the same width as our Unicode characters");


cl::Program buildProgramFromFile(const std::string& fname, const cl::Context& context, const std::vector<cl::Device>& devices) {
    std::ifstream file(fname.c_str());
    if (file.fail()) {
        throw user_error("Cannot open file " + fname);
    }

    std::string sourceCode;
    file.seekg(0, std::ios::end);
    sourceCode.reserve(static_cast<std::size_t>(file.tellg()));
    file.seekg(0, std::ios::beg);
    sourceCode.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    cl::Program program(context, sourceCode);
    try {
        program.build(devices);
    } catch (const cl::Error& /*e*/) {
        std::stringstream ss;
        ss << "OpenCl build errors:" << std::endl;
        for (const auto& dev : devices) {
            std::string buildLog = program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(dev);
            if (!buildLog.empty()) {
                ss << buildLog << std::endl;
            }
        }
        throw internal_exception(ss.str());
    }
    return program;
}

std::vector<std::uint32_t> runEngine(const serial::graph& graph, const std::u32string& fcontent) {
    std::vector<cl::Platform> pool_platforms;
    cl::Platform::get(&pool_platforms);
    if (pool_platforms.empty()) {
        throw user_error("no OpenCL platforms found!");
    }
    cl::Platform platform = pool_platforms[0]; // XXX: make this selectable!

    std::vector<cl::Device> pool_devices;
    platform.getDevices(CL_DEVICE_TYPE_ALL, &pool_devices);
    if (pool_devices.empty()) {
        throw user_error("no OpenCL devices found!");
    }
    std::vector<cl::Device> devices{pool_devices[0]}; // XXX: make this a user choice!
    for (const auto& dev : devices) {
        if (!dev.getInfo<CL_DEVICE_ENDIAN_LITTLE>()) {
            throw user_error("not all selected devices are little endian!");
        }
    }

    cl::Context context(devices);

    cl::Program programAutomaton = buildProgramFromFile("automaton.cl", context, devices);
    cl::Kernel kernelAutomaton(programAutomaton, "automaton");

    cl::Buffer dAutomatonData(
        context,
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        graph.size() * sizeof(std::uint8_t),
        const_cast<void*>(static_cast<const void*>(graph.data.data())) // that's ok, trust me ;)
    );

    cl::Buffer dText(
        context,
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        fcontent.size() * sizeof(char32_t),
        const_cast<void*>(static_cast<const void*>(fcontent.data())) // that's ok, trust me ;)
    );

    cl::Buffer dOutput(
        context,
        CL_MEM_READ_WRITE,
        fcontent.size() * sizeof(cl_uint),
        nullptr
    );

    kernelAutomaton.setArg(0, static_cast<cl_uint>(graph.n));
    kernelAutomaton.setArg(1, static_cast<cl_uint>(graph.m));
    kernelAutomaton.setArg(2, static_cast<cl_uint>(graph.o));
    kernelAutomaton.setArg(3, static_cast<cl_uint>(fcontent.size()));
    kernelAutomaton.setArg(4, dAutomatonData);
    kernelAutomaton.setArg(5, dText);
    kernelAutomaton.setArg(6, dOutput);

    cl::CommandQueue queue(context, devices[0]);

    // XXX: should we use local groups?
    queue.enqueueNDRangeKernel(kernelAutomaton, cl::NullRange, cl::NDRange(fcontent.size()), cl::NullRange);

    std::vector<uint32_t> output(fcontent.size(), 0);
    queue.enqueueReadBuffer(dOutput, false, 0, fcontent.size() * sizeof(cl_uint), output.data());

    queue.finish();

    return output;
}