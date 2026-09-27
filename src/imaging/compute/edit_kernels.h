#pragma once

namespace Licasa {

// Shared arithmetic for OpenCL C and CUDA C++. Keep the operation order in
// image_edit_pipeline.cpp; both compilers must disable contraction/fast math.
inline constexpr char editKernelSource[] = R"GPU(
#ifdef __OPENCL_VERSION__
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#pragma OPENCL FP_CONTRACT OFF
#define KERNEL __kernel
#define GLOBAL __global
#define DEVICE
typedef uint Pixel;
#define PIXEL_INDEX ((int)get_global_id(0) + offset)
#else
#define KERNEL extern "C" __global__
#define GLOBAL
#define DEVICE __device__
typedef unsigned int Pixel;
#define PIXEL_INDEX ((int)(blockIdx.x * blockDim.x + threadIdx.x) + offset)
#endif

DEVICE double bounded(double value, double low, double high) {
    return fmin(high, fmax(low, value));
}
DEVICE Pixel channel(double value) {
    return (Pixel)bounded(floor(value + 0.5), 0.0, 255.0);
}
DEVICE double component(Pixel pixel, int shift) {
    return (double)((pixel >> shift) & 255u);
}

KERNEL void adjustColor(GLOBAL const Pixel* source, GLOBAL Pixel* destination,
                        GLOBAL const double* p, int width, int height, int offset, int end) {
    const int index = PIXEL_INDEX;
    if (index >= end) return;
    const Pixel pixel = source[index];
    if ((pixel >> 24) == 0) { destination[index] = pixel; return; }
    const double exposure = p[0], contrast = p[1], highlights = p[2], shadows = p[3];
    const double saturationAmount = p[4], vibrance = p[5], warmth = p[6], tint = p[7], vignette = p[8];
    double red = component(pixel, 16) * exposure;
    double green = component(pixel, 8) * exposure;
    double blue = component(pixel, 0) * exposure;
    double shift = 0.0;
    if (highlights != 0.0 || shadows != 0.0) {
        const double luminance = bounded((0.299*red + 0.587*green + 0.114*blue)/255.0, 0.0, 1.0);
        const double shadow = 1.0 - luminance;
        shift = shadows*72.0*(shadow*shadow) + highlights*72.0*(luminance*luminance);
    }
    red = (red - 127.5)*contrast + 127.5 + shift;
    green = (green - 127.5)*contrast + 127.5 + shift;
    blue = (blue - 127.5)*contrast + 127.5 + shift;
    red += warmth + tint*0.45;
    green -= tint;
    blue += -warmth + tint*0.45;
    const double gray = 0.299*red + 0.587*green + 0.114*blue;
    double saturation = fmax(0.0, 1.0 + saturationAmount);
    if (vibrance != 0.0) {
        const double chroma = bounded((fmax(red, fmax(green, blue)) - fmin(red, fmin(green, blue)))/255.0, 0.0, 1.0);
        saturation = fmax(0.0, 1.0 + saturationAmount + vibrance*(1.0-chroma)*0.75);
    }
    red = gray + (red-gray)*saturation;
    green = gray + (green-gray)*saturation;
    blue = gray + (blue-gray)*saturation;
    if (vignette > 0.0) {
        const double cx = fmax(1.0, (width-1)*0.5), cy = fmax(1.0, (height-1)*0.5);
        const double nx = (index%width-cx)/cx, ny = (index/width-cy)/cy;
        const double radius = bounded(sqrt(nx*nx+ny*ny), 0.0, 1.5);
        const double edge = bounded((radius-0.28)/0.92, 0.0, 1.0);
        const double factor = 1.0-vignette*0.72*(edge*edge*(3.0-2.0*edge));
        red *= factor; green *= factor; blue *= factor;
    }
    destination[index] = (pixel & 0xff000000u) | (channel(red)<<16) | (channel(green)<<8) | channel(blue);
}

KERNEL void sharpenImage(GLOBAL const Pixel* source, GLOBAL Pixel* destination,
                         GLOBAL const double* p, int width, int height, int offset, int end) {
    const int index = PIXEL_INDEX;
    if (index >= end) return;
    const Pixel center = source[index];
    const int x = index%width, y = index/width;
    if (x == 0 || y == 0 || x == width-1 || y == height-1) {
        destination[index] = center; return;
    }
    Pixel pixel = center & 0xff000000u;
    const double strength = p[9];
    for (int shift = 0; shift <= 16; shift += 8) {
        const double value = component(center, shift);
        const double detail = value*4.0 - component(source[index-1],shift)
            - component(source[index+1],shift) - component(source[index-width],shift)
            - component(source[index+width],shift);
        pixel |= channel(value + detail*strength) << shift;
    }
    destination[index] = pixel;
}
)GPU";

} // namespace Licasa
