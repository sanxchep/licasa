#include "cuda_edit_ptx.h"
#include "imaging/compute/edit_compute.h"

#include <QLibrary>
#include <QScopeGuard>

#include <algorithm>
#include <cstddef>
#include <cuda.h>

namespace Licasa {
namespace {

constexpr size_t maximumReusableRasterBytes = 256 * 1024 * 1024;

class CudaEditDevice final : public EditComputeDevice {
  public:
    ~CudaEditDevice() override
    {
        if (context) {
            if (cuCtxPushCurrent(context) == CUDA_SUCCESS) {
                releaseReusableBuffers();
                if (parameterBuffer) {
                    cuMemFree(parameterBuffer);
                }
                if (module) {
                    cuModuleUnload(module);
                }
                CUcontext previous = nullptr;
                cuCtxPopCurrent(&previous);
            }
            cuCtxDestroy(context);
        }
    }

    bool initialize(QString& error)
    {
        error = QStringLiteral("CUDA driver unavailable");
        if (!driver.load()) {
            return false;
        }
#define LOAD(library, name, symbol)                                                                \
    if (!(name = reinterpret_cast<decltype(name)>(library.resolve(symbol))))                       \
        return false;
        LOAD(driver, cuInit, "cuInit")
        LOAD(driver, cuDeviceGet, "cuDeviceGet")
        LOAD(driver, cuDeviceGetName, "cuDeviceGetName")
        LOAD(driver, createContext, "cuCtxCreate_v2")
        LOAD(driver, cuCtxDestroy, "cuCtxDestroy_v2")
        LOAD(driver, cuCtxPushCurrent, "cuCtxPushCurrent_v2")
        LOAD(driver, cuCtxPopCurrent, "cuCtxPopCurrent_v2")
        LOAD(driver, cuCtxSynchronize, "cuCtxSynchronize")
        LOAD(driver, cuModuleLoadData, "cuModuleLoadData")
        LOAD(driver, cuModuleGetFunction, "cuModuleGetFunction")
        LOAD(driver, cuModuleUnload, "cuModuleUnload")
        LOAD(driver, cuMemAlloc, "cuMemAlloc_v2")
        LOAD(driver, cuMemFree, "cuMemFree_v2")
        LOAD(driver, cuMemcpyHtoD, "cuMemcpyHtoD_v2")
        LOAD(driver, cuMemcpyDtoH, "cuMemcpyDtoH_v2")
        LOAD(driver, cuLaunchKernel, "cuLaunchKernel")
#undef LOAD
        error = QStringLiteral("CUDA device/context initialization failed");
        CUdevice device = 0;
        if (cuInit(0) != CUDA_SUCCESS || cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
            return false;
        }
        if (createContext(&context, CU_CTX_SCHED_BLOCKING_SYNC, device) != CUDA_SUCCESS) {
            return false;
        }
        const auto popContext = qScopeGuard([&] {
            CUcontext previous = nullptr;
            cuCtxPopCurrent(&previous);
        });
        char deviceName[256]{};
        cuDeviceGetName(deviceName, sizeof(deviceName), device);
        description = QStringLiteral("CUDA: %1").arg(QString::fromLatin1(deviceName));
        if (cuModuleLoadData(&module, embeddedCudaEditPtx) != CUDA_SUCCESS ||
            cuModuleGetFunction(&colorKernel, module, "adjustColor") != CUDA_SUCCESS ||
            cuModuleGetFunction(&sharpenKernel, module, "sharpenImage") != CUDA_SUCCESS ||
            cuMemAlloc(&parameterBuffer, sizeof(EditUniforms)) != CUDA_SUCCESS) {
            return false;
        }
        return true;
    }

    bool run(const QImage& source, QImage& destination, quint64 sourceIdentity,
             const EditUniforms& values, bool color, bool sharpen,
             const std::atomic_bool* cancelled) override
    {
        if (cuCtxPushCurrent(context) != CUDA_SUCCESS) {
            return false;
        }

        CUdeviceptr transientInput = 0;
        CUdeviceptr transientOutput = 0;
        const auto release = qScopeGuard([&] {
            // A failed/cancelled launch must finish before transient memory can
            // be freed or retained staging can be reused by the next request.
            cuCtxSynchronize();
            if (transientInput) {
                cuMemFree(transientInput);
            }
            if (transientOutput) {
                cuMemFree(transientOutput);
            }
            CUcontext previous = nullptr;
            cuCtxPopCurrent(&previous);
        });

        const size_t bytes = size_t(source.sizeInBytes());
        const bool reusable = bytes <= maximumReusableRasterBytes;
        const bool singleStage = color != sharpen;
        const bool allowResidentBase =
            reusable && singleStage && sourceIdentity != 0 &&
            !qEnvironmentVariableIsSet("LICASA_DISABLE_GPU_BASE_RESIDENCY");
        CUdeviceptr input = 0;
        CUdeviceptr output = 0;
        if (reusable) {
            if (!reserveReusableBuffers(bytes)) {
                return false;
            }
            input = reusableInput;
            output = reusableOutput;
        } else {
            if (cuMemAlloc(&transientInput, bytes) != CUDA_SUCCESS ||
                cuMemAlloc(&transientOutput, bytes) != CUDA_SUCCESS) {
                return false;
            }
            input = transientInput;
            output = transientOutput;
        }

        const bool residentHit = allowResidentBase && residentSourceIdentity == sourceIdentity &&
                                 residentSourceBytes == bytes;
        if ((!residentHit && cuMemcpyHtoD(input, source.constBits(), bytes) != CUDA_SUCCESS) ||
            cuMemcpyHtoD(parameterBuffer, values.data(), sizeof(values)) != CUDA_SUCCESS) {
            return false;
        }
        if (allowResidentBase && !residentHit) {
            residentSourceIdentity = sourceIdentity;
            residentSourceBytes = bytes;
        }
        // Two passes swap back into reusableInput on the second kernel, so the
        // retained source is no longer immutable after this request.
        if (reusable && color && sharpen) {
            invalidateResidentBase();
        }

        int width = source.width();
        int height = source.height();
        int offset = 0;
        int end = width * height;
        const unsigned int blocks = (unsigned(end) + 255u) / 256u;
        for (const auto kernel :
             {color ? colorKernel : nullptr, sharpen ? sharpenKernel : nullptr}) {
            if (!kernel) {
                continue;
            }
            if (cancelled && cancelled->load(std::memory_order_relaxed)) {
                return false;
            }
            void* arguments[]{&input, &output, &parameterBuffer, &width, &height, &offset, &end};
            if (cuLaunchKernel(kernel, blocks, 1, 1, 256, 1, 1, 0, nullptr, arguments, nullptr) !=
                CUDA_SUCCESS) {
                return false;
            }
            std::swap(input, output);
        }

        // The default stream preserves color->sharpen ordering. One fence per
        // edit avoids serializing every million-pixel slice while still making
        // cancellation observable before readback/publish.
        if (cuCtxSynchronize() != CUDA_SUCCESS ||
            (cancelled && cancelled->load(std::memory_order_relaxed))) {
            return false;
        }
        return cuMemcpyDtoH(destination.bits(), input, bytes) == CUDA_SUCCESS;
    }

    void releaseImageBuffers() override
    {
        if (!context || cuCtxPushCurrent(context) != CUDA_SUCCESS) {
            return;
        }
        cuCtxSynchronize();
        releaseReusableBuffers();
        CUcontext previous = nullptr;
        cuCtxPopCurrent(&previous);
    }

    size_t retainedImageBytes() const override { return 2 * reusableCapacity; }

  private:
    bool reserveReusableBuffers(size_t bytes)
    {
        if (reusableCapacity >= bytes && reusableInput && reusableOutput) {
            return true;
        }

        CUdeviceptr input = 0;
        CUdeviceptr output = 0;
        if (cuMemAlloc(&input, bytes) != CUDA_SUCCESS ||
            cuMemAlloc(&output, bytes) != CUDA_SUCCESS) {
            if (input) {
                cuMemFree(input);
            }
            if (output) {
                cuMemFree(output);
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
            cuMemFree(reusableInput);
            reusableInput = 0;
        }
        if (reusableOutput) {
            cuMemFree(reusableOutput);
            reusableOutput = 0;
        }
        reusableCapacity = 0;
    }

    QLibrary driver{QStringLiteral("cuda"), 1};
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUfunction colorKernel = nullptr;
    CUfunction sharpenKernel = nullptr;
    CUdeviceptr reusableInput = 0;
    CUdeviceptr reusableOutput = 0;
    CUdeviceptr parameterBuffer = 0;
    size_t reusableCapacity = 0;
    quint64 residentSourceIdentity = 0;
    size_t residentSourceBytes = 0;
#define API(name) decltype(&::name) name = nullptr;
    API(cuInit)
    API(cuDeviceGet)
    API(cuDeviceGetName)
    // CUDA 13 maps the unsuffixed name to a newer signature. Resolve and type
    // the stable v2 entry point together, including when newer headers are used.
    decltype(&::cuCtxCreate_v2) createContext = nullptr;
    API(cuCtxDestroy)
    API(cuCtxPushCurrent)
    API(cuCtxPopCurrent)
    API(cuCtxSynchronize)
    API(cuModuleLoadData)
    API(cuModuleGetFunction)
    API(cuModuleUnload)
    API(cuMemAlloc)
    API(cuMemFree)
    API(cuMemcpyHtoD)
    API(cuMemcpyDtoH)
    API(cuLaunchKernel)
#undef API
};

} // namespace

std::unique_ptr<EditComputeDevice> createCudaEditDevice(QString& error)
{
    auto device = std::make_unique<CudaEditDevice>();
    if (!device->initialize(error)) {
        return {};
    }
    return device;
}

} // namespace Licasa
