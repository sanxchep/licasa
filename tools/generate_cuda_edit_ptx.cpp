#include "imaging/compute/edit_kernels.h"

#include <QFile>
#include <QTextStream>

#include <nvrtc.h>

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: generate_cuda_edit_ptx OUTPUT_HEADER\n");
        return 2;
    }

    nvrtcProgram program = nullptr;
    if (nvrtcCreateProgram(&program, Licasa::editKernelSource, "licasa_edits.cu", 0, nullptr,
                           nullptr) != NVRTC_SUCCESS) {
        return 3;
    }
    const char* options[]{"--std=c++11", "--fmad=false", "--gpu-architecture=compute_52"};
    const nvrtcResult compilation = nvrtcCompileProgram(program, 3, options);
    if (compilation != NVRTC_SUCCESS) {
        size_t length = 0;
        nvrtcGetProgramLogSize(program, &length);
        std::vector<char> log(length);
        if (length) {
            nvrtcGetProgramLog(program, log.data());
            std::fputs(log.data(), stderr);
        }
        nvrtcDestroyProgram(&program);
        return 4;
    }

    size_t length = 0;
    if (nvrtcGetPTXSize(program, &length) != NVRTC_SUCCESS) {
        nvrtcDestroyProgram(&program);
        return 5;
    }
    std::vector<char> ptx(length);
    if (nvrtcGetPTX(program, ptx.data()) != NVRTC_SUCCESS) {
        nvrtcDestroyProgram(&program);
        return 6;
    }
    nvrtcDestroyProgram(&program);

    QFile output(QString::fromLocal8Bit(argv[1]));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return 7;
    }
    QTextStream stream(&output);
    stream << "#pragma once\n\nnamespace Licasa {\n"
              "inline constexpr char embeddedCudaEditPtx[] = R\"LICASAPTX(\n";
    stream << QByteArray(ptx.data(), qsizetype(length - 1));
    stream << "\n)LICASAPTX\";\n} // namespace Licasa\n";
    return stream.status() == QTextStream::Ok ? 0 : 8;
}
