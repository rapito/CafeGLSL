/*
 * Copyright © 2016 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Lowers the additional tex_src_plane src, generated by nir_lower_tex
 * for planar YUV textures, into separate samplers, matching the logic
 * that mesa/st uses to insert additional sampler view/state (since both
 * sides need to agree).
 *
 * This should run after nir_lower_samplers.
 */

#include "util/u_string.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "st_nir.h"

typedef struct {
   struct nir_shader *shader;

   unsigned lower_2plane;
   unsigned lower_3plane;

   /* Maps a primary sampler (used for Y) to the U or UV sampler.  In
    * case of 3-plane YUV format, the V plane is next sampler after U.
    */
   unsigned char sampler_map[PIPE_MAX_SAMPLERS][2];
} lower_tex_src_state;

static nir_variable *
find_sampler(lower_tex_src_state *state, unsigned samp)
{
   /* NOTE: arrays of samplerExternalOES do not appear to be allowed: */
   nir_foreach_uniform_variable(var, state->shader)
      if (var->data.binding == samp)
         return var;
   return NULL;
}

static void
add_sampler(lower_tex_src_state *state, unsigned orig_binding,
            unsigned new_binding, const char *ext)
{
   const struct glsl_type *samplerExternalOES =
      glsl_sampler_type(GLSL_SAMPLER_DIM_EXTERNAL, false, false, GLSL_TYPE_FLOAT);
   nir_variable *new_sampler, *orig_sampler =
         find_sampler(state, orig_binding);
   char *name;

   assert(false);
   UNUSED int r = 0;//asprintf(&name, "%s:%s", orig_sampler->name, ext);
   new_sampler = nir_variable_create(state->shader, nir_var_uniform,
                             samplerExternalOES, name);
   free(name);

   new_sampler->data.binding = new_binding;
}

static void
assign_extra_samplers(lower_tex_src_state *state, unsigned free_slots)
{
   unsigned mask = state->lower_2plane | state->lower_3plane;

   while (mask) {
      unsigned extra, y_samp = u_bit_scan(&mask);

      if (state->lower_3plane & (1 << y_samp)) {
         /* two additional planes (U and V): */
         extra = u_bit_scan(&free_slots);
         state->sampler_map[y_samp][0] = extra;

         add_sampler(state, y_samp, extra, "u");

         extra = u_bit_scan(&free_slots);
         state->sampler_map[y_samp][1] = extra;

         add_sampler(state, y_samp, extra, "v");
      } else {
         /* single additional UV plane: */
         extra = u_bit_scan(&free_slots);
         state->sampler_map[y_samp][0] = extra;

         add_sampler(state, y_samp, extra, "uv");
      }
   }
}

static void
lower_tex_src_plane_block(nir_builder *b, lower_tex_src_state *state, nir_block *block)
{
   nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_tex)
         continue;

      nir_tex_instr *tex = nir_instr_as_tex(instr);
      int plane_index = nir_tex_instr_src_index(tex, nir_tex_src_plane);

      if (plane_index < 0)
         continue;

      nir_const_value *plane = nir_src_as_const_value(tex->src[plane_index].src);
      assume(plane);

      if (plane[0].i32 > 0) {
         unsigned y_samp = tex->texture_index;
         int tex_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
         if (tex_index >= 0) {
            nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_index].src);
            y_samp = nir_deref_instr_get_variable(deref)->data.binding;
         }

         assume(((state->lower_3plane & (1 << y_samp)) && plane[0].i32 < 3) ||
               (plane[0].i32 < 2));

         unsigned u_v_samp = state->sampler_map[y_samp][plane[0].i32 - 1];
         BITSET_SET(state->shader->info.textures_used, u_v_samp);
         BITSET_SET(state->shader->info.samplers_used, u_v_samp);

         /* For drivers using PIPE_CAP_NIR_SAMPLERS_AS_DEREF, we need
          * to reference the correct sampler nir variable.
          */
         int samp_index = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
         if (tex_index >= 0 && samp_index >= 0) {
            b->cursor = nir_before_instr(&tex->instr);

            nir_variable* samp = find_sampler(state, u_v_samp);
            assert(samp);

            nir_deref_instr *tex_deref_instr = nir_build_deref_var(b, samp);
            nir_ssa_def *tex_deref = &tex_deref_instr->dest.ssa;

            nir_instr_rewrite_src(&tex->instr,
                                  &tex->src[tex_index].src,
                                  nir_src_for_ssa(tex_deref));
            nir_instr_rewrite_src(&tex->instr,
                                  &tex->src[samp_index].src,
                                  nir_src_for_ssa(tex_deref));
         } else {
            /* For others we need to update texture_index */
            assume(tex->texture_index == tex->sampler_index);
            tex->texture_index = tex->sampler_index = u_v_samp;
         }
      }

      nir_tex_instr_remove_src(tex, plane_index);
   }
}

static void
lower_tex_src_plane_impl(lower_tex_src_state *state, nir_function_impl *impl)
{
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      lower_tex_src_plane_block(&b, state, block);
   }

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);
}

void
st_nir_lower_tex_src_plane(struct nir_shader *shader, unsigned free_slots,
                           unsigned lower_2plane, unsigned lower_3plane)
{
   lower_tex_src_state state = {0};

   state.shader = shader;
   state.lower_2plane = lower_2plane;
   state.lower_3plane = lower_3plane;

   assign_extra_samplers(&state, free_slots);

   nir_foreach_function(function, shader) {
      if (function->impl)
         lower_tex_src_plane_impl(&state, function->impl);
   }
}
