#ifndef HALIDE_HALIDERUNTIMECUDA_H
#define HALIDE_HALIDERUNTIMECUDA_H

#include "HalideRuntime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file
 *  Routines specific to the Halide Cuda runtime.
 */

extern const struct halide_device_interface_t *halide_cuda_device_interface();

/** These are forward declared here to allow clients to override the
 *  Halide Cuda runtime. Do not call them. */
// @{
extern int halide_cuda_initialize_kernels(void *user_context, void **state_ptr,
                                          const char *src, int size);
extern int halide_cuda_run(void *user_context,
                           void *state_ptr,
                           const char *entry_name,
                           int blocksX, int blocksY, int blocksZ,
                           int threadsX, int threadsY, int threadsZ,
                           int shared_mem_bytes,
                           size_t arg_sizes[],
                           void *args[],
                           int8_t arg_is_buffer[],
                           int num_attributes,
                           float* vertex_buffer,
                           int num_coords_dim0,
                           int num_coords_dim1);
// @}

/** Set the underlying cuda device poiner for a buffer. The device
 * pointer should be allocated using cuMemAlloc or similar and must
 * have an extent large enough to cover that specified by the buffer_t
 * extent fields. The dev field of the buffer_t must be NULL when this
 * routine is called. This call can fail due to running out of memory
 * or being passed an invalid device pointer. The device and host
 * dirty bits are left unmodified. */
extern int halide_cuda_wrap_device_ptr(void *user_context, struct buffer_t *buf, uintptr_t device_ptr);

/** Disconnect this buffer_t from the device pointer it was previously
 * wrapped around. Should only be called for a buffer_t that
 * halide_cuda_wrap_device_ptr was previously called on. Frees any
 * storage associated with the binding of the buffer_t and the device
 * pointer, but does not free the device pointer. The previously
 * wrapped device pointer is returned. . The dev field of the buffer_t
 * will be NULL on return.
 */
extern uintptr_t halide_cuda_detach_device_ptr(void *user_context, struct buffer_t *buf);

/** Return the underlying device pointer for a buffer_t. This buffer
 *  must be valid on a Cuda device, or not have any associated device
 *  memory. If there is no device memory (dev field is NULL), this
 *  returns 0.
 */
extern uintptr_t halide_cuda_get_device_ptr(void *user_context, struct buffer_t *buf);

#ifdef __cplusplus
} // End extern "C"
#endif

#endif // HALIDE_HALIDERUNTIMECUDA_H
