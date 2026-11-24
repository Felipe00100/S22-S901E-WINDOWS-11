/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_IR3_REF_H
#define ADRENO_IR3_REF_H

#include "adreno_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Precompiled A730 compute diagnostic kernel used to validate the
 * CP_LOAD_STATE6 / CP_EXEC_CS path independently of the offline compiler.
 *
 * Source (Mesa computerator):
 *   @localsize 1, 1, 1
 *   @buf 4 (c0.x)
 *   @const(c1.x) 0
 *   @invocationid(r0.x)
 *   mov.u32u32 r1.x, c0.x
 *   mov.u32u32 r2.x, c1.x
 *   stg.u32 g[r1.x], r2.x, 1
 *   end
 *
 * c0.xy = 64-bit destination GPU VA, c1.x = 32-bit value. The shader is
 * 128 bytes / one instruction group, constlen 4 vec4s.
 */
#define ADRENO_IR3_TEST_SHADER_BYTES 128u
#define ADRENO_IR3_TEST_SHADER_UNITS 1u
#define ADRENO_IR3_TEST_CONST_VEC4 2u
#define ADRENO_IR3_TEST_CONSTLEN 4u
#define ADRENO_IR3_TEST_FULLREG 3u
#define ADRENO_IR3_TEST_HALFREG 0u
#define ADRENO_IR3_TEST_BRANCHSTACK 0u
#define ADRENO_IR3_TEST_THREADSIZE 1u /* THREAD128 */
#define ADRENO_IR3_TEST_SHARED_SIZE 1u
#define ADRENO_IR3_TEST_CONST_RAM_MODE 0u /* CONSTLEN_128 */

extern const uint8_t adreno_ir3_test_shader[ADRENO_IR3_TEST_SHADER_BYTES];

/*
 * IR3 produced by the offline DXIL/SPIR-V compiler for:
 *   cbuffer RootConstants : register(b0) { uint value; };
 *   RWByteAddressBuffer outbuf : register(u0);
 *   outbuf.Store(0, value);
 * via dxil-spirv --ssbo-uav --no-bda --root-constant 0 0 4 4 and
 * adreno-ir3c's set 0 / binding 0 descriptor lowering.
 * The UMD supplies the descriptor-table UAV VA in c0.xy and the root
 * constants in c1.xyzw, matching D3D12 root parameter order.
 */
#define ADRENO_IR3_SSBO_TEST_SHADER_BYTES 128u
extern const uint8_t
    adreno_ir3_ssbo_test_shader[ADRENO_IR3_SSBO_TEST_SHADER_BYTES];

#define ADRENO_IR3_REF_MAX_SHADER_BYTES 4096u
#define ADRENO_IR3_REF_MAX_CONST_VEC4 256u
#define ADRENO_IR3_REF_MAX_CONST_WORDS (ADRENO_IR3_REF_MAX_CONST_VEC4 * 4u)

/*
 * State captured from CP_LOAD_STATE6_FRAG by the reference executors. It is
 * intentionally small and fixed-size: this is a validation path, not a
 * general command processor.
 */
typedef struct adreno_ir3_ref_state {
    uint8_t shader[ADRENO_IR3_REF_MAX_SHADER_BYTES];
    uint32_t shader_bytes;
    uint32_t constants[ADRENO_IR3_REF_MAX_CONST_WORDS];
    uint32_t const_vec4s;
} adreno_ir3_ref_state;

/* Read GPU memory for an indirect shader preload. */
typedef enum adreno_status (*adreno_ir3_ref_read)(void *ctx, uint64_t iova,
                                                   void *dst, uint32_t size);

/* Write one 32-bit word into the reference executor's memory. */
typedef enum adreno_status (*adreno_ir3_ref_write32)(void *ctx, uint64_t iova,
                                                      uint32_t value);

void adreno_ir3_ref_reset(adreno_ir3_ref_state *state);
enum adreno_status adreno_ir3_ref_load_shader(adreno_ir3_ref_state *state,
                                              uint64_t iova, uint32_t units,
                                              adreno_ir3_ref_read read,
                                              void *ctx);
enum adreno_status adreno_ir3_ref_load_const(adreno_ir3_ref_state *state,
                                             uint32_t dst_vec4,
                                             uint32_t units,
                                             const uint32_t *data);

/*
 * Execute the small IR3 compute subset used by the tests: cat1 MOV from the
 * constant file, cat6 STG.u32, and cat0 NOP/END. Unsupported instructions
 * return ADRENO_ERR_UNSUPPORTED; this is deliberately not a general IR3
 * emulator.
 */
enum adreno_status adreno_ir3_ref_exec_test(
    const uint8_t *shader, uint32_t shader_bytes, const uint32_t *constants,
    uint32_t const_vec4s, const uint32_t grid[3],
    adreno_ir3_ref_write32 write32, void *ctx);

enum adreno_status adreno_ir3_ref_exec_state(
    const adreno_ir3_ref_state *state, const uint32_t grid[3],
    adreno_ir3_ref_write32 write32, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_IR3_REF_H */
