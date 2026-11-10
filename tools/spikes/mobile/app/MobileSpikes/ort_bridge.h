// M1 spike S1 (throwaway). The text tower through ONNX Runtime's C API, linked statically: the same
// API src/infer/ort.cpp uses, without its dlopen.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mv_ort mv_ort;

const char* mv_ort_version(void);

// `optimization`: 0 disabled, 1 basic, 2 extended, 99 all (the desktop's). NULL on failure, with
// the reason in `error`.
mv_ort* mv_ort_open(const char* model_path, int threads, int optimization, char* error, size_t error_cap);
void mv_ort_close(mv_ort* ort);

// The model's input names, comma-separated, and the output the encode returns.
const char* mv_ort_inputs(const mv_ort* ort);
const char* mv_ort_output(const mv_ort* ort);

// One text of `context` ids, `used` of them real. Writes up to `cap` floats; returns how many, or
// -1 with the reason in `error`.
long mv_ort_encode(mv_ort* ort, const int64_t* ids, size_t context, size_t used, float* out, size_t cap,
                   char* error, size_t error_cap);

#ifdef __cplusplus
}
#endif
