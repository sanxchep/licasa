#include "imaging/compute/edit_compute.h"
#include "imaging/compute/edit_kernels.h"

#include <QLibrary>
#include <QScopeGuard>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Licasa {
namespace {

constexpr size_t maximumReusableRasterBytes = 256 * 1024 * 1024;

class OpenClEditDevice final : public EditComputeDevice {
  public:
    ~OpenClEditDevice() override
    {
        releaseReusableBuffers();
        if (parameterBuffer) {
            clReleaseMemObject(parameterBuffer);
        }
        if (colorKernel) {
            clReleaseKernel(colorKernel);
        }
        if (sharpenKernel) {
            clReleaseKernel(sharpenKernel);
        }
        if (program) {
            clReleaseProgram(program);
        }
        if (queue) {
            clReleaseCommandQueue(queue);
        }
        if (context) {
            clReleaseContext(context);
        }
    }

    bool initialize(QString& error)
    {
        error = QStringLiteral("OpenCL library or required entry point unavailable");
        if (!library.load()) {
            return false;
        }
#define LOAD(name)                                                                                 \
    if (!(name = reinterpret_cast<decltype(name)>(library.resolve(#name))))                        \
        return false;
        LOAD(clGetPlatformIDs)
        LOAD(clGetDeviceIDs)
        LOAD(clGetDeviceInfo)
        LOAD(clCreateContext)
        LOAD(clCreateCommandQueue)
        LOAD(clCreateProgramWithSource)
        LOAD(clBuildProgram)
        LOAD(clGetProgramBuildInfo)
        LOAD(clCreateKernel)
        LOAD(clCreateBuffer)
        LOAD(clSetKernelArg)
        LOAD(clEnqueueWriteBuffer)
        LOAD(clEnqueueNDRangeKernel)
        LOAD(clEnqueueReadBuffer)
        LOAD(clFinish)
        LOAD(clReleaseMemObject)
        LOAD(clReleaseKernel)
        LOAD(clReleaseProgram)
        LOAD(clReleaseCommandQueue)
        LOAD(clReleaseContext)
#undef LOAD
        error = QStringLiteral("No OpenCL GPU with double precision is available");
        cl_uint count = 0;
        if (clGetPlatformIDs(0, nullptr, &count) != CL_SUCCESS || count == 0) {
            return false;
        }
        std::vector<cl_platform_id> platforms(count);
        if (clGetPlatformIDs(count, platforms.data(), nullptr) != CL_SUCCESS) {
            return false;
        }
        cl_device_id device = nullptr;
        for (const auto platform : platforms) {
            cl_uint deviceCount = 0;
            if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &deviceCount) !=
                CL_SUCCESS) {
                continue;
            }
            std::vector<cl_device_id> devices(deviceCount);
            if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, deviceCount, devices.data(),
                               nullptr) != CL_SUCCESS) {
                continue;
            }
            for (const auto candidate : devices) {
                cl_device_fp_config precision = 0;
                if (clGetDeviceInfo(candidate, CL_DEVICE_DOUBLE_FP_CONFIG, sizeof(precision),
                                    &precision, nullptr) == CL_SUCCESS &&
                    precision != 0) {
                    device = candidate;
                    break;
                }
            }
            if (device) {
                break;
            }
        }
        if (!device) {
            return false;
        }
        char deviceName[256]{};
        clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(deviceName), deviceName, nullptr);
        description = QStringLiteral("OpenCL: %1").arg(QString::fromLatin1(deviceName));
        error = QStringLiteral("OpenCL context/program initialization failed");
        context = clCreateContext(nullptr, 1, &device, nullptr, nullptr, nullptr);
        if (!context) {
            return false;
        }
        queue = clCreateCommandQueue(context, device, 0, nullptr);
        if (!queue) {
            return false;
        }
        const char* source = editKernelSource;
        program = clCreateProgramWithSource(context, 1, &source, nullptr, nullptr);
        if (!program) {
            return false;
        }
        if (clBuildProgram(program, 1, &device, "-cl-std=CL1.2", nullptr, nullptr) != CL_SUCCESS) {
            char log[4096]{};
            clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, nullptr);
            error += QStringLiteral(": %1").arg(QString::fromLatin1(log));
            return false;
        }
        colorKernel = clCreateKernel(program, "adjustColor", nullptr);
        sharpenKernel = clCreateKernel(program, "sharpenImage", nullptr);
        parameterBuffer =
            clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(EditUniforms), nullptr, nullptr);
        return colorKernel && sharpenKernel && parameterBuffer;
    }

    bool run(const QImage& source, QImage& destination, quint64 sourceIdentity,
             const EditUniforms& values, bool color, bool sharpen,
             const std::atomic_bool* cancelled) override
    {
        const size_t bytes = size_t(source.sizeInBytes());
        cl_mem transientInput = nullptr;
        cl_mem transientOutput = nullptr;
        cl_mem input = nullptr;
        cl_mem output = nullptr;
        const bool reusable = bytes <= maximumReusableRasterBytes;
        const bool singleStage = color != sharpen;
        const bool allowResidentBase =
            reusable && singleStage && sourceIdentity != 0 &&
            !qEnvironmentVariableIsSet("LICASA_DISABLE_GPU_BASE_RESIDENCY");
        if (reusable) {
            if (!reserveReusableBuffers(bytes)) {
                return false;
            }
            input = reusableInput;
            output = reusableOutput;
            const bool residentHit = allowResidentBase &&
                                     residentSourceIdentity == sourceIdentity &&
                                     residentSourceBytes == bytes;
            if ((!residentHit &&
                 clEnqueueWriteBuffer(queue, input, CL_FALSE, 0, bytes, source.constBits(), 0,
                                      nullptr, nullptr) != CL_SUCCESS) ||
                clEnqueueWriteBuffer(queue, parameterBuffer, CL_FALSE, 0, sizeof(values),
                                     values.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
                return false;
            }
            if (allowResidentBase && !residentHit) {
                residentSourceIdentity = sourceIdentity;
                residentSourceBytes = bytes;
            }
            if (color && sharpen) {
                invalidateResidentBase();
            }
        } else {
            transientInput = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                            bytes, const_cast<uchar*>(source.constBits()), nullptr);
            transientOutput = clCreateBuffer(context, CL_MEM_READ_WRITE, bytes, nullptr, nullptr);
            input = transientInput;
            output = transientOutput;
            if (!input || !output ||
                clEnqueueWriteBuffer(queue, parameterBuffer, CL_FALSE, 0, sizeof(values),
                                     values.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
                if (transientInput) {
                    clReleaseMemObject(transientInput);
                }
                if (transientOutput) {
                    clReleaseMemObject(transientOutput);
                }
                return false;
            }
        }

        const auto release = qScopeGuard([&] {
            // Do not release request-scoped buffers while queued kernels still
            // reference them. Retained buffers stay valid for the next edit.
            clFinish(queue);
            if (transientInput) {
                clReleaseMemObject(transientInput);
            }
            if (transientOutput) {
                clReleaseMemObject(transientOutput);
            }
        });

        const int width = source.width();
        const int height = source.height();
        const int offset = 0;
        const int end = width * height;
        const size_t workSize = size_t(end);
        for (const auto kernel :
             {color ? colorKernel : nullptr, sharpen ? sharpenKernel : nullptr}) {
            if (!kernel) {
                continue;
            }
            if (cancelled && cancelled->load(std::memory_order_relaxed)) {
                return false;
            }
            if (clSetKernelArg(kernel, 0, sizeof(input), &input) != CL_SUCCESS ||
                clSetKernelArg(kernel, 1, sizeof(output), &output) != CL_SUCCESS ||
                clSetKernelArg(kernel, 2, sizeof(parameterBuffer), &parameterBuffer) !=
                    CL_SUCCESS ||
                clSetKernelArg(kernel, 3, sizeof(width), &width) != CL_SUCCESS ||
                clSetKernelArg(kernel, 4, sizeof(height), &height) != CL_SUCCESS ||
                clSetKernelArg(kernel, 5, sizeof(offset), &offset) != CL_SUCCESS ||
                clSetKernelArg(kernel, 6, sizeof(end), &end) != CL_SUCCESS ||
                clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &workSize, nullptr, 0, nullptr,
                                       nullptr) != CL_SUCCESS) {
                return false;
            }
            std::swap(input, output);
        }

        // Queue ordering preserves color->sharpen. Fence once instead of after
        // each million-pixel batch, then honor cancellation before readback.
        if (clFinish(queue) != CL_SUCCESS ||
            (cancelled && cancelled->load(std::memory_order_relaxed))) {
            return false;
        }
        return clEnqueueReadBuffer(queue, input, CL_TRUE, 0, bytes, destination.bits(), 0, nullptr,
                                   nullptr) == CL_SUCCESS;
    }

    void releaseImageBuffers() override
    {
        if (queue) {
            clFinish(queue);
        }
        releaseReusableBuffers();
    }

    size_t retainedImageBytes() const override { return 2 * reusableCapacity; }

  private:
    bool reserveReusableBuffers(size_t bytes)
    {
        if (reusableCapacity >= bytes && reusableInput && reusableOutput) {
            return true;
        }

        cl_mem input = clCreateBuffer(context, CL_MEM_READ_WRITE, bytes, nullptr, nullptr);
        cl_mem output = clCreateBuffer(context, CL_MEM_READ_WRITE, bytes, nullptr, nullptr);
        if (!input || !output) {
            if (input) {
                clReleaseMemObject(input);
            }
            if (output) {
                clReleaseMemObject(output);
            }
            return false;
        }

        releaseReusableBuffers();
        reusableInput = input;
        reusableOutput = output;
        reusableCapacity = bytes;
        return true;
    }

    void invalidateResidentBase()
    {
        residentSourceIdentity = 0;
        residentSourceBytes = 0;
    }

    void releaseReusableBuffers()
    {
        invalidateResidentBase();
        if (reusableInput) {
            clReleaseMemObject(reusableInput);
            reusableInput = nullptr;
        }
        if (reusableOutput) {
            clReleaseMemObject(reusableOutput);
            reusableOutput = nullptr;
        }
        reusableCapacity = 0;
    }

    QLibrary library{QStringLiteral("OpenCL"), 1};
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_program program = nullptr;
    cl_kernel colorKernel = nullptr;
    cl_kernel sharpenKernel = nullptr;
    cl_mem reusableInput = nullptr;
    cl_mem reusableOutput = nullptr;
    cl_mem parameterBuffer = nullptr;
    size_t reusableCapacity = 0;
    quint64 residentSourceIdentity = 0;
    size_t residentSourceBytes = 0;
#define API(name) decltype(&::name) name = nullptr;
    API(clGetPlatformIDs)
    API(clGetDeviceIDs)
    API(clGetDeviceInfo)
    API(clCreateContext)
    API(clCreateCommandQueue)
    API(clCreateProgramWithSource)
    API(clBuildProgram)
    API(clGetProgramBuildInfo)
    API(clCreateKernel)
    API(clCreateBuffer)
    API(clSetKernelArg)
    API(clEnqueueWriteBuffer)
    API(clEnqueueNDRangeKernel)
    API(clEnqueueReadBuffer)
    API(clFinish)
    API(clReleaseMemObject)
    API(clReleaseKernel)
    API(clReleaseProgram)
    API(clReleaseCommandQueue)
    API(clReleaseContext)
#undef API
};

} // namespace

std::unique_ptr<EditComputeDevice> createOpenClEditDevice(QString& error)
{
    auto device = std::make_unique<OpenClEditDevice>();
    if (!device->initialize(error)) {
        return {};
    }
    return device;
}

} // namespace Licasa
