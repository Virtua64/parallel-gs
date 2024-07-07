// SPDX-FileCopyrightText: 2024 Arntzen Software AS
// SPDX-FileContributor: Hans-Kristian Arntzen
// SPDX-FileContributor: Runar Heyer
// SPDX-License-Identifier: LGPL-3.0+

#include "page_tracker.hpp"
#include "gs_interface.hpp"
#include "gs_util.hpp"
#include "shaders/swizzle_utils.h"
#include "muglm/muglm_impl.hpp"
#include "gs_registers_debug.hpp"

namespace ParallelGS
{
GSInterface::GSInterface()
	: tracker(*this)
{
	setup_handlers();
	registers.prmodecont.desc.AC = PRMODECONTBits::AC_DEFAULT;
}

bool GSInterface::init(Vulkan::Device *device, const GSOptions &options)
{
	vram_size = options.vram_size;
	uint32_t num_pages = vram_size / PageSize;
	tracker.set_num_pages(num_pages);
	uint32_t num_pages_u32 = (num_pages + 31) / 32;
	sync_host_vram_pages.resize(num_pages_u32);
	sync_vram_host_pages.resize(num_pages_u32);
	page_buffer.reserve(num_pages_u32);

	set_super_sampling_rate(options.super_sampling);

	if (!renderer.init(device, options))
		return false;

	render_pass.positions.reserve(MaxPrimitivesPerFlush * 3);
	render_pass.attributes.reserve(MaxPrimitivesPerFlush * 3);
	render_pass.prim.reserve(MaxPrimitivesPerFlush);
	return true;
}

void GSInterface::set_super_sampling_rate(SuperSampling super_sampling)
{
	switch (super_sampling)
	{
	case SuperSampling::X1:
		sampling_rate_x_log2 = 0;
		sampling_rate_y_log2 = 0;
		break;

	case SuperSampling::X2:
		sampling_rate_x_log2 = 0;
		sampling_rate_y_log2 = 1;
		break;

	case SuperSampling::X4:
		sampling_rate_x_log2 = 1;
		sampling_rate_y_log2 = 1;
		break;

	case SuperSampling::X8:
		sampling_rate_x_log2 = 1;
		sampling_rate_y_log2 = 2;
		break;

	case SuperSampling::X16:
		sampling_rate_x_log2 = 2;
		sampling_rate_y_log2 = 2;
		break;
	}

	renderer.invalidate_super_sampling_state();
}

void GSInterface::flush_render_pass(FlushReason reason)
{
	ParallelGS::RenderPass rp = {};

	if (render_pass.primitive_count)
	{
		rp.positions = render_pass.positions.data();
		rp.attributes = render_pass.attributes.data();
		rp.prims = render_pass.prim.data();
		rp.num_primitives = render_pass.primitive_count;

		rp.fb.frame = render_pass.frame;
		rp.fb.z = render_pass.zbuf;

		rp.states = render_pass.state_vectors.data();
		rp.num_states = render_pass.state_vectors.size();

		rp.textures = render_pass.tex_infos.data();
		rp.num_textures = render_pass.tex_infos.size();

		// Somewhat arbitrary. Try to balance binning load.
		uint32_t tile_width = ((render_pass.bb.z - render_pass.bb.x) >> FB_SWIZZLE_WIDTH_LOG2) + 1;
		uint32_t tile_height = ((render_pass.bb.w - render_pass.bb.y) >> FB_SWIZZLE_HEIGHT_LOG2) + 1;
		uint32_t binning_cost = tile_width * tile_height * rp.num_primitives;
		if (binning_cost < 10 * 1000)
			rp.coarse_tile_size_log2 = 3;
		else if (binning_cost < 10 * 1000 * 1000)
			rp.coarse_tile_size_log2 = 4;
		else if (binning_cost < 100 * 1000 * 1000)
			rp.coarse_tile_size_log2 = 5;
		else
			rp.coarse_tile_size_log2 = 6;

		if (sampling_rate_y_log2 != 0 && rp.coarse_tile_size_log2 > 3)
			rp.coarse_tile_size_log2 -= 1;

		assert(render_pass.bb.z < rp.fb.frame.desc.FBW * BUFFER_WIDTH_SCALE);

		rp.base_x = render_pass.bb.x;
		rp.base_y = render_pass.bb.y;
		rp.coarse_tiles_width = ((render_pass.bb.z - render_pass.bb.x) >> rp.coarse_tile_size_log2) + 1;
		rp.coarse_tiles_height = ((render_pass.bb.w - render_pass.bb.y) >> rp.coarse_tile_size_log2) + 1;

		rp.feedback_texture = render_pass.has_color_feedback;
		rp.feedback_texture_psm = render_pass.feedback_psm;
		rp.feedback_texture_cpsm = render_pass.feedback_cpsm;

		// Affects shader variants.
		rp.z_sensitive = render_pass.z_sensitive;
		rp.has_aa1 = render_pass.has_aa1;
		rp.has_scanmsk = render_pass.has_scanmsk;

		// Debug stuff
		rp.feedback_color = debug_mode.feedback_render_target;
		rp.feedback_depth = debug_mode.feedback_render_target && rp.z_sensitive;

		// This should be possible to vary based on dynamic usage.
		// If there are only trivial UI passes, we should make it single-sampled.
		rp.sampling_rate_x_log2 = sampling_rate_x_log2;
		rp.sampling_rate_y_log2 = sampling_rate_y_log2;

		switch (debug_mode.draw_mode)
		{
		case DebugMode::DrawDebugMode::Strided:
			// Try to balance debuggability so there's not a million events to step through
			// while being able to identify a faulty primitive.
			rp.debug_capture_stride = 16;
			break;

		case DebugMode::DrawDebugMode::Full:
			rp.debug_capture_stride = 1;
			break;

		default:
			break;
		}

		rp.label_key = render_pass.label_key++;
		rp.flush_reason = reason;
		//////

		renderer.flush_rendering(rp);

		TRACE_HEADER("FLUSH RENDER", rp);
	}

	render_pass.held_images.clear();
	render_pass.texture_map.clear();
	render_pass.tex_infos.clear();
	render_pass.state_vector_map.clear();
	render_pass.state_vectors.clear();
	render_pass.primitive_count = 0;
	render_pass.pending_palette_updates = 0;
	render_pass.bb = ivec4(INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN);
	render_pass.color_write_mask = 0;
	render_pass.z_sensitive = false;
	render_pass.z_write = false;
	render_pass.has_color_feedback = false;
	render_pass.has_aa1 = false;
	render_pass.has_scanmsk = false;
	state_tracker.dirty_flags = STATE_DIRTY_ALL_BITS;
}

void GSInterface::flush(PageTrackerFlushFlags flags, FlushReason reason)
{
	if ((flags & PAGE_TRACKER_FLUSH_HOST_VRAM_SYNC_BIT) != 0)
	{
		page_buffer.clear();
		for (size_t i = 0, n = sync_host_vram_pages.size(); i < n; i++)
		{
			Util::for_each_bit(sync_host_vram_pages[i], [i, this](uint32_t bit) {
				page_buffer.push_back(i * 32 + bit);
			});
			sync_host_vram_pages[i] = 0;
		}

		if (!page_buffer.empty())
			renderer.flush_host_vram_copy(page_buffer.data(), page_buffer.size());

		TRACE_HEADER("FLUSH HOST VRAM", Reg64<DummyBits>{0});
	}

	if ((flags & PAGE_TRACKER_FLUSH_COPY_BIT) != 0)
	{
		if ((flags & (PAGE_TRACKER_FLUSH_CACHE_BIT | PAGE_TRACKER_FLUSH_FB_BIT | PAGE_TRACKER_FLUSH_WRITE_BACK_BIT)) != 0)
		{
			TRACE_HEADER("FLUSH COPY", Reg64<DummyBits>{0});
			renderer.flush_transfer();
		}
		else
		{
			// If we're not flushing anything beyond copies, it means we're just resolving a WAW hazard internally.
			TRACE_HEADER("BARRIER COPY", Reg64<DummyBits>{0});
			renderer.transfer_overlap_barrier();
		}
	}

	if ((flags & PAGE_TRACKER_FLUSH_CACHE_BIT) != 0)
	{
		TRACE_HEADER("FLUSH CACHE UPLOAD", Reg64<DummyBits>{0});
		renderer.flush_cache_upload();
		// VRAM may have changed, so need to reset memoization state.
		render_pass.num_memoized_palettes = 0;
	}

	if ((flags & PAGE_TRACKER_FLUSH_FB_BIT) != 0)
		flush_render_pass(reason);

	if ((flags & PAGE_TRACKER_FLUSH_WRITE_BACK_BIT) != 0)
	{
		TRACE_HEADER("FLUSH WRITE BACK", Reg64<DummyBits>{0});
		page_buffer.clear();
		for (size_t i = 0, n = sync_vram_host_pages.size(); i < n; i++)
		{
			Util::for_each_bit(sync_vram_host_pages[i], [i, this](uint32_t bit) {
				page_buffer.push_back(i * 32 + bit);
			});
			sync_vram_host_pages[i] = 0;
		}

		if (!page_buffer.empty())
			renderer.flush_readback(page_buffer.data(), page_buffer.size());
	}
}

void GSInterface::sync_host_vram_page(uint32_t page_index)
{
	sync_host_vram_pages[page_index / 32] |= 1u << (page_index & 31);
}

void GSInterface::sync_vram_host_page(uint32_t page_index)
{
	sync_vram_host_pages[page_index / 32] |= 1u << (page_index & 31);
}

void GSInterface::handle_clut_upload(uint32_t ctx_index)
{
	auto &ctx = registers.ctx[ctx_index];
	auto &desc = ctx.tex0.desc;
	bool load_clut = false;

	auto CLD = uint32_t(desc.CLD);

	switch (CLD)
	{
	case TEX0Bits::CLD_LOAD:
		load_clut = true;
		break;
	case TEX0Bits::CLD_LOAD_WRITE_CBP0:
	case TEX0Bits::CLD_LOAD_WRITE_CBP1:
		load_clut = true;
		registers.cached_cbp[CLD & 1] = uint32_t(desc.CBP);
		break;
	case TEX0Bits::CLD_COMPARE_LOAD_CBP0:
	case TEX0Bits::CLD_COMPARE_LOAD_CBP1:
		load_clut = registers.cached_cbp[CLD & 1] != uint32_t(desc.CBP);
		registers.cached_cbp[CLD & 1] = uint32_t(desc.CBP);
		break;
	default:
		break;
	}

	if (!load_clut)
		return;

	// If there's a partial transfer in-flight, flush it.
	// The write should technically happen as soon as we write HWREG.
	// It's possible CLUT upload will depend on this.
	// TODO: Could hazard check this, but ... w/e. Hazards between copy and cache isn't that bad.
	if (transfer_state.host_to_local_active &&
	    transfer_state.host_to_local_payload.size() > transfer_state.last_flushed_qwords)
	{
#ifdef PARALLEL_GS_DEBUG
		LOGW("Flushing partial transfer due to palette read.\n");
#endif
		flush_pending_transfer(true);
	}

	PageRectCLUT page = {};

	uint32_t palette_width, palette_height;
	auto psm = uint32_t(desc.PSM);
	auto cpsm = uint32_t(desc.CPSM);
	bool is_8bit_palette = false;

	if (psm == PSMT8 || psm == PSMT8H)
	{
		if (desc.CSM != TEX0Bits::CSM_LAYOUT_RECT)
		{
			palette_width = 256;
			palette_height = 1;
		}
		else
		{
			palette_width = 16;
			palette_height = 16;
		}

		page.csa_mask = 0xffff;
		is_8bit_palette = true;
	}
	else
	{
		if (desc.CSM != TEX0Bits::CSM_LAYOUT_RECT)
		{
			palette_width = 16;
			palette_height = 1;

		}
		else
		{
			palette_width = 8;
			palette_height = 4;
		}

		page.csa_mask = 1u << uint32_t(desc.CSA);
	}

	// For 32-bit color, read upper CLUT bank as well.
	if (cpsm == PSMCT32)
		page.csa_mask |= page.csa_mask << 16;

	uint32_t x_offset = desc.CSM == TEX0Bits::CSM_LAYOUT_LINE ? registers.texclut.desc.COU * TEX0Bits::COU_SCALE : 0;
	uint32_t y_offset = desc.CSM == TEX0Bits::CSM_LAYOUT_LINE ? registers.texclut.desc.COV : 0;

	auto clut_page = compute_page_rect(uint32_t(desc.CBP), x_offset, y_offset,
	                                   palette_width, palette_height,
	                                   registers.texclut.desc.CBW,
	                                   cpsm);

	page.base_page = clut_page.base_page;
	page.page_width = clut_page.page_width;
	page.page_height = clut_page.page_height;
	page.page_stride = clut_page.page_stride;
	page.block_mask = clut_page.block_mask;
	page.write_mask = clut_page.write_mask;

	tracker.mark_texture_read(page);
	tracker.register_cached_clut_clobber(page);

	// Queue up palette upload.
	PaletteUploadDescriptor palette_desc = {};
	palette_desc.texclut = registers.texclut;
	palette_desc.tex0.desc = desc;

	// Normalize fields we don't care about.
	palette_desc.tex0.desc.TBP0 = 0;
	palette_desc.tex0.desc.TFX = 0;
	palette_desc.tex0.desc.TW = 0;
	palette_desc.tex0.desc.TH = 0;
	palette_desc.tex0.desc.TCC = 0;
	palette_desc.tex0.desc.TBW = 0;
	palette_desc.tex0.desc.CLD = 0;

	// CSA seems to be ignored on upload for 256 color mode.
	if (is_8bit_palette)
		palette_desc.tex0.desc.CSA = 0;

	// Try to find a memoized palette. In case game constantly uploads CLUT redundantly.
	// This is very common, and this optimization is extremely important.
	for (uint32_t i = render_pass.num_memoized_palettes; i; i--)
	{
		auto &memoized = render_pass.memoized_palettes[i - 1];
		// If a later update wrote something that this update did not write, we have diverging history.
		// Normally, games don't seem to use CSA offsets much, so this should be okay?
		if ((memoized.csa_mask & ~page.csa_mask) != 0)
			break;

		if (memoized.csa_mask == page.csa_mask &&
		    memoized.upload.texclut.bits == palette_desc.texclut.bits &&
		    memoized.upload.tex0.bits == palette_desc.tex0.bits)
		{
			if (memoized.clut_instance != render_pass.clut_instance)
				mark_texture_state_dirty();
			render_pass.clut_instance = memoized.clut_instance;

			// Move to end.
			if (i < render_pass.num_memoized_palettes)
			{
				memmove(render_pass.memoized_palettes + i - 1,
				        render_pass.memoized_palettes + i,
				        (render_pass.num_memoized_palettes - i) * sizeof(render_pass.memoized_palettes[0]));

				auto &last_memoized = render_pass.memoized_palettes[render_pass.num_memoized_palettes - 1];
				last_memoized.csa_mask = page.csa_mask;
				last_memoized.upload = palette_desc;
				last_memoized.clut_instance = render_pass.clut_instance;
			}

			return;
		}
	}

	render_pass.clut_instance = renderer.update_palette_cache(palette_desc);
	render_pass.latest_clut_instance = render_pass.clut_instance;
	render_pass.pending_palette_updates++;
	mark_texture_state_dirty();

	// Maintain a sliding window.
	if (render_pass.num_memoized_palettes == NumMemoizedPalettes)
	{
		memmove(render_pass.memoized_palettes, render_pass.memoized_palettes + 1,
		        sizeof(render_pass.memoized_palettes) - sizeof(render_pass.memoized_palettes[0]));
		render_pass.num_memoized_palettes--;
	}

	TRACE_INDEXED("MEMOIZE CLUT", render_pass.num_memoized_palettes, palette_desc);
	auto &memoized = render_pass.memoized_palettes[render_pass.num_memoized_palettes++];
	memoized.clut_instance = render_pass.clut_instance;
	memoized.csa_mask = page.csa_mask;
	memoized.upload = palette_desc;

	TRACE("CACHE CLUT", palette_desc);

	if (render_pass.pending_palette_updates >= CLUTInstances)
		tracker.flush_render_pass(FlushReason::Overflow);
}

void GSInterface::handle_tex0_write(uint32_t ctx_index)
{
	handle_clut_upload(ctx_index);
}

void GSInterface::handle_miptbl_gen(uint32_t ctx_index)
{
	auto &ctx = registers.ctx[ctx_index];
	auto &tex0 = ctx.tex0.desc;
	auto &tex1 = ctx.tex1.desc;

	if (!tex1.MTBA)
		return;

	// Auto-generate MIPTBL1 when TEX0 is written, and MTBA is set.

	uint32_t base = tex0.TBP0;
	uint32_t TW = tex0.TW;
	uint32_t TH = tex0.TH;
	uint32_t W = 1u << TW;
	uint32_t H = 1u << TH;
	uint32_t row_length_64 = W / 64;

	auto layout = get_data_structure(uint32_t(tex0.PSM));
	uint32_t num_blocks = (W >> layout.block_width_log2) * (H >> layout.block_height_log2);
	base += num_blocks;

	num_blocks /= 4;
	row_length_64 /= 2;
	ctx.miptbl_1_3.desc.TBP1 = base;
	ctx.miptbl_1_3.desc.TBW1 = row_length_64;
	base += num_blocks;

	num_blocks /= 4;
	row_length_64 /= 2;
	ctx.miptbl_1_3.desc.TBP2 = base;
	ctx.miptbl_1_3.desc.TBW2 = row_length_64;
	base += num_blocks;

	ctx.miptbl_1_3.desc.TBP3 = base;
	ctx.miptbl_1_3.desc.TBW3 = row_length_64;

	state_tracker.dirty_flags |= STATE_DIRTY_TEX_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT;
}

void GSInterface::shift_vertex_queue()
{
	// Ring-buffer feels overkill. Should lower to some straight forward SIMD moves.
	if (vertex_queue.count == 3)
	{
		vertex_queue.pos[0] = vertex_queue.pos[1];
		vertex_queue.attr[0] = vertex_queue.attr[1];
		vertex_queue.pos[1] = vertex_queue.pos[2];
		vertex_queue.attr[1] = vertex_queue.attr[2];
		vertex_queue.count = 2;
	}
}

void GSInterface::vertex_kick_xyz(Reg64<XYZBits> xyz)
{
	shift_vertex_queue();
	auto &pos = vertex_queue.pos[vertex_queue.count];
	auto &attr = vertex_queue.attr[vertex_queue.count];

	pos.pos.x = int(xyz.desc.X);
	pos.pos.y = int(xyz.desc.Y);
	// TODO: Z should be fixed point always.
	// For 24-bit, FP should be fine (every 24-bit uint can be converted to FP32 losslessly), but not for 32-bit.
	pos.z = float(xyz.desc.Z);

	attr.st.x = registers.st.desc.S;
	attr.st.y = registers.st.desc.T;
	attr.q = registers.rgbaq.desc.Q;
	attr.rgba = registers.rgbaq.words[0];
	attr.fog = float(registers.fog.desc.FOG);
	attr.uv = u16vec2(registers.uv.desc.U, registers.uv.desc.V);

	vertex_queue.count++;
	TRACE_INDEXED("VERT", vertex_queue.count, xyz);
}

void GSInterface::vertex_kick_xyzf(Reg64<XYZFBits> xyzf)
{
	shift_vertex_queue();

	auto &pos = vertex_queue.pos[vertex_queue.count];
	auto &attr = vertex_queue.attr[vertex_queue.count];

	pos.pos.x = int(xyzf.desc.X);
	pos.pos.y = int(xyzf.desc.Y);
	// TODO: Z should be fixed point always.
	// For 24-bit, FP should be fine (every 24-bit uint can be converted to FP32 losslessly), but not for 32-bit.
	pos.z = float(xyzf.desc.Z);

	attr.st.x = registers.st.desc.S;
	attr.st.y = registers.st.desc.T;
	attr.q = registers.rgbaq.desc.Q;
	attr.rgba = registers.rgbaq.words[0];
	attr.fog = float(xyzf.desc.F);
	attr.uv = u16vec2(registers.uv.desc.U, registers.uv.desc.V);

	vertex_queue.count++;
	TRACE_INDEXED("VERT", vertex_queue.count, xyzf);
}

bool GSInterface::get_and_clear_dirty_flag(StateDirtyFlags flags)
{
	bool ret = (state_tracker.dirty_flags & flags) != 0;
	if (ret)
		state_tracker.dirty_flags &= ~flags;
	return ret;
}

void GSInterface::mark_render_pass_has_texture_feedback(const TEX0Bits &tex0)
{
	if (render_pass.has_color_feedback)
	{
		if (uint32_t(tex0.PSM) != render_pass.feedback_psm ||
		    (is_palette_format(render_pass.feedback_psm) &&
		     render_pass.feedback_cpsm != uint32_t(tex0.CPSM)))
		{
			tracker.flush_render_pass(FlushReason::TextureHazard);
		}
	}

	if (!render_pass.has_color_feedback)
	{
		render_pass.has_color_feedback = true;
		render_pass.feedback_psm = uint32_t(tex0.PSM);
		render_pass.feedback_cpsm = is_palette_format(render_pass.feedback_psm) ? uint32_t(tex0.CPSM) : 0;
	}
}

void GSInterface::check_frame_buffer_state()
{
	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	if (!get_and_clear_dirty_flag(STATE_DIRTY_FB_BIT))
	{
		assert(render_pass.frame.words[0] == ctx.frame.words[0]);
		assert(render_pass.zbuf.desc.PSM == ctx.zbuf.desc.PSM);
		assert(render_pass.zbuf.desc.ZBP == ctx.zbuf.desc.ZBP);
		return;
	}

	bool fb_delta = render_pass.frame.words[0] != ctx.frame.words[0];
	bool z_delta = render_pass.zbuf.desc.PSM != ctx.zbuf.desc.PSM ||
	               render_pass.zbuf.desc.ZBP != ctx.zbuf.desc.ZBP;

	// If FRAME / ZBUF changes in meaningful ways, restart the render pass.
	// If no draw needs to read or write Z, we can change Z buffer without a flush.
	if (render_pass.primitive_count && (fb_delta || (render_pass.z_sensitive && z_delta)))
	{
		flush_pending_transfer(true);
		tracker.flush_render_pass(FlushReason::FBPointer);
	}

	if (fb_delta)
	{
		auto fb_layout = get_data_structure(ctx.frame.desc.PSM);
		render_pass.fb_page_width_log2 = fb_layout.page_width_log2;
		render_pass.fb_page_height_log2 = fb_layout.page_height_log2;
		render_pass.frame = ctx.frame;
	}

	if (z_delta)
	{
		auto z_layout = get_data_structure(ctx.zbuf.desc.PSM);
		render_pass.z_page_width_log2 = z_layout.page_width_log2;
		render_pass.z_page_height_log2 = z_layout.page_height_log2;
		render_pass.zbuf = ctx.zbuf;
	}

	assert(render_pass.frame.words[0] == ctx.frame.words[0]);
	assert(render_pass.zbuf.desc.PSM == ctx.zbuf.desc.PSM);
	assert(render_pass.zbuf.desc.ZBP == ctx.zbuf.desc.ZBP);
}

uint32_t GSInterface::find_or_place_unique_state_vector(const StateVector &state)
{
	uint32_t state_index;

	auto &last_state = state_tracker.last_state_vector;
	if (!render_pass.state_vectors.empty() &&
	    state.blend_mode == last_state.blend_mode &&
	    state.combiner == last_state.combiner &&
	    state.dimx.x == last_state.dimx.x &&
	    state.dimx.y == last_state.dimx.y)
	{
		return state_tracker.last_state_index;
	}

	Util::Hasher hasher;

	hasher.u32(state.blend_mode);
	hasher.u32(state.combiner);
	hasher.u32(state.dimx.x);
	hasher.u32(state.dimx.y);

	auto *cached_state_index = render_pass.state_vector_map.find(hasher.get());
	if (cached_state_index)
	{
		state_index = cached_state_index->get();
	}
	else
	{
		state_index = uint32_t(render_pass.state_vectors.size());
		TRACE_INDEXED("STATE", state_index, state);
		render_pass.state_vectors.push_back(state);
		render_pass.state_vector_map.emplace_replace(hasher.get(), state_index);
	}

	last_state = state;
	state_tracker.last_state_index = state_index;

	return state_index;
}

uint32_t GSInterface::drawing_kick_update_state_vector()
{
	if (!get_and_clear_dirty_flag(STATE_DIRTY_STATE_BIT))
		return state_tracker.last_state_index;

	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	StateVector state = {};

	// Dither enable
	if (registers.dthe.desc.DTHE)
	{
		state.blend_mode |= BLEND_MODE_DTHE_BIT;
		state.dimx.x = registers.dimx.words[0];
		state.dimx.y = registers.dimx.words[1];
	}

	if (ctx.test.desc.ATE && ctx.test.desc.ATST != 1) // ALWAYS pass is meaningless.
	{
		state.blend_mode |= BLEND_MODE_ATE_BIT;
		state.blend_mode |= ctx.test.desc.ATST << BLEND_MODE_ATE_MODE_OFFSET;
		state.blend_mode |= ctx.test.desc.AFAIL << BLEND_MODE_AFAIL_MODE_OFFSET;
	}

	state.blend_mode |= ctx.test.desc.DATE ? BLEND_MODE_DATE_BIT : 0;
	state.blend_mode |= ctx.test.desc.DATM ? BLEND_MODE_DATM_BIT : 0;

	// Enabling AA1 seems to imply alpha blending?
	if (prim.desc.ABE || prim.desc.AA1)
	{
		state.blend_mode |= ctx.alpha.desc.A << BLEND_MODE_A_MODE_OFFSET;
		state.blend_mode |= ctx.alpha.desc.B << BLEND_MODE_B_MODE_OFFSET;
		state.blend_mode |= ctx.alpha.desc.C << BLEND_MODE_C_MODE_OFFSET;
		state.blend_mode |= ctx.alpha.desc.D << BLEND_MODE_D_MODE_OFFSET;
	}

	if (prim.desc.ABE)
		state.blend_mode |= BLEND_MODE_ABE_BIT;

	state.blend_mode |= registers.pabe.desc.PABE ? BLEND_MODE_PABE_BIT : 0;
	state.blend_mode |= registers.colclamp.desc.CLAMP ? BLEND_MODE_COLCLAMP_BIT : 0;
	state.blend_mode |= ctx.fba.desc.FBA ? BLEND_MODE_FB_ALPHA_BIT : 0;

	if (prim.desc.TME)
	{
		state.combiner |= COMBINER_TME_BIT;
		state.combiner |= ctx.tex0.desc.TCC ? COMBINER_TCC_BIT : 0;
		state.combiner |= uint32_t(ctx.tex0.desc.TFX) << COMBINER_MODE_OFFSET;
	}

	state.combiner |= prim.desc.FGE ? COMBINER_FOG_BIT : 0;
	return find_or_place_unique_state_vector(state);
}

void GSInterface::update_texture_page_rects_and_read()
{
	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];
	auto psm = uint32_t(ctx.tex0.desc.PSM);
	auto &tex = state_tracker.tex;

	// Mark that we're starting a read. This will check for any hazards and flush render pass if need be.
	for (uint32_t level = 0; level < tex.rect.levels; level++)
	{
		if (render_pass.is_potential_color_feedback || render_pass.is_potential_depth_feedback)
		{
			assert(tex.rect.levels == 1);
			auto &rect = tex.page_rects[level];
			auto tex_base_page = uint32_t(ctx.tex0.desc.TBP0) / BLOCKS_PER_PAGE;

			// Clamp the hazard region so we don't falsely invalidate the texture.
			rect = {};
			rect.base_page = tex_base_page;
			rect.page_width = vram_size / PAGE_ALIGNMENT_BYTES;
			rect.page_height = 1;
			rect.page_stride = 0;
			rect.block_mask = UINT32_MAX;
			rect.write_mask = UINT32_MAX;

			if (render_pass.is_potential_color_feedback)
			{
				uint32_t fb_base_page = ctx.frame.desc.FBP;
				if (fb_base_page <= tex_base_page)
					fb_base_page += vram_size / PAGE_ALIGNMENT_BYTES;
				rect.page_width = std::min<uint32_t>(rect.page_width, fb_base_page - tex_base_page);
			}

			if (render_pass.is_potential_depth_feedback)
			{
				uint32_t z_base_page = ctx.zbuf.desc.ZBP;
				if (z_base_page <= tex_base_page)
					z_base_page += vram_size / PAGE_ALIGNMENT_BYTES;
				rect.page_width = std::min<uint32_t>(rect.page_width, z_base_page - tex_base_page);
			}
		}
		else
		{
			tex.page_rects[level] = compute_page_rect(
					tex.levels[level].base,
					tex.rect.x >> level,
					tex.rect.y >> level,
					tex.rect.width >> level,
					tex.rect.height >> level,
					tex.levels[level].stride,
					psm);
		}

		tracker.mark_texture_read(state_tracker.tex.page_rects[level]);
	}
}

void GSInterface::texture_page_rects_read()
{
	auto &tex = state_tracker.tex;
	for (uint32_t level = 0; level < tex.rect.levels; level++)
		tracker.mark_texture_read(state_tracker.tex.page_rects[level]);
}

void GSInterface::invalidate_texture_hash(Util::Hash hash, bool clut)
{
	if (!clut)
	{
		// Any CLUT texture will make palette bank part of the hash.
		auto *tex = render_pass.texture_map.find(hash);
		if (tex)
			tex->valid = false;
	}

	mark_texture_state_dirty();
}

void GSInterface::forget_in_render_pass_memoization()
{
	// Forget any palette memoization.
	render_pass.num_memoized_palettes = 0;
	mark_texture_state_dirty();
}

void GSInterface::mark_texture_state_dirty()
{
	state_tracker.last_texture_index = UINT32_MAX;
	state_tracker.dirty_flags |= STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT;
}

uint32_t GSInterface::drawing_kick_update_texture(ColorFeedbackMode feedback_mode, const ivec4 &uv_bb, const ivec4 &bb)
{
	if (!get_and_clear_dirty_flag(STATE_DIRTY_TEX_BIT))
	{
		assert(state_tracker.last_texture_index != UINT32_MAX);
		return state_tracker.last_texture_index;
	}

	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	if (feedback_mode == ColorFeedbackMode::Pixel)
	{
		mark_render_pass_has_texture_feedback(ctx.tex0.desc);
		// Special index indicating on-tile feedback.
		// We could add a different sentinel for depth feedback.
		// 1024k CLUT instances and 32 sub-banks. Fits in 15 bits. Use bit 15 MSB to mark feedback texture.
		return (1u << (TEX_TEXTURE_INDEX_BITS - 1u)) | (render_pass.clut_instance * 32 + uint32_t(ctx.tex0.desc.CSA));
	}

	TextureDescriptor desc = {};

	// Disregard texture state that does not affect upload.
	desc.tex0 = ctx.tex0;
	desc.tex1 = ctx.tex1;
	desc.clamp = ctx.clamp;

	auto psm = uint32_t(desc.tex0.desc.PSM);
	auto cpsm = uint32_t(desc.tex0.desc.CPSM);
	uint32_t csa_mask = 0;

	if (is_palette_format(psm))
	{
		desc.palette_bank = render_pass.clut_instance;
		desc.latest_palette_bank = render_pass.latest_clut_instance;

		// Only allowed CPSM formats are CT32 and CT16(S).
		if (cpsm != PSMCT32)
			desc.texa = registers.texa;

		if (psm == PSMT8 || psm == PSMT8H)
			csa_mask = 0xffff;
		else
			csa_mask = 1u;

		csa_mask <<= uint32_t(desc.tex0.desc.CSA);

		// For 32-bit color, read upper CLUT bank as well.
		if (cpsm == PSMCT32)
			csa_mask |= csa_mask << 16;
	}
	else
	{
		// Don't care about palette.
		desc.tex0.desc.CPSM = 0;
		desc.tex0.desc.CSA = 0;
		// TODO: May be possible to replace TEXA with image view swizzle if 0 or 0xff.
		// 0x80 is more common alpha value on PS2 though, so probably not worth the hassle.
		if (psm != PSMCT32 && psm != PSMZ32)
			desc.texa = registers.texa;
	}

	// Only affects shading
	desc.tex0.desc.TCC = 0;
	desc.tex0.desc.TFX = 0;

	// Only affects palette upload
	desc.tex0.desc.CBP = 0;
	desc.tex0.desc.CSM = 0;
	desc.tex0.desc.CLD = 0;

	// As a general rule we should cache a texture, but in feedback scenarios where there is overlap between
	// the UV BB and rendering BB, we temporarily suspend hazard tracking until we can prove a well-defined
	// rendering pattern where render region and sampling region is disjoint.
	bool cache_texture = true;

	if (feedback_mode == ColorFeedbackMode::Sliced)
	{
		// If game explicitly clamps the rect to a small region, it's likely doing well-defined feedbacks.
		// E.g. Tales of Abyss main menu ping-pong blurs.
		// This code is quite flawed, and I'm not sure what the correct solution is yet.
		if (PRIMType(prim.desc.PRIM) == PRIMType::Sprite)
		{
			// If game is using sprites, it's more likely than not it's doing explicit mip blurs, etc, so cache those.
			// The main problem we always want to avoid is heavy random triangle soup geometry that does feedback.
			cache_texture = true;
		}
		else if (desc.clamp.desc.WMS == CLAMPBits::REGION_CLAMP && desc.clamp.desc.WMT == CLAMPBits::REGION_CLAMP)
		{
			ivec4 clamped_uv_bb(
					int(desc.clamp.desc.MINU),
					int(desc.clamp.desc.MINV),
					int(desc.clamp.desc.MAXU),
					int(desc.clamp.desc.MAXV));

			ivec4 hazard_bb(
					std::max<int>(clamped_uv_bb.x, bb.x),
					std::max<int>(clamped_uv_bb.y, bb.y),
					std::min<int>(clamped_uv_bb.z, bb.z),
					std::min<int>(clamped_uv_bb.w, bb.w));

			cache_texture = hazard_bb.x > hazard_bb.z || hazard_bb.y > hazard_bb.w;
		}
		else
		{
			// Questionable, but it seems almost impossible to do this correctly and fast.
			// Need to emulate the PS2 texture cache exactly, which is just insane.
			// This should be fine in most cases.
			cache_texture = false;
		}
	}

	// In sliced mode with clamping, we can clamp harder based on uv_bb.
	// In this path, we're guaranteed to not hit wrapping with region clamp.
	// For repeat, give up. Should not happen (hopefully).
	if (feedback_mode == ColorFeedbackMode::Sliced && cache_texture &&
	    !desc.clamp.desc.has_horizontal_repeat() && !desc.clamp.desc.has_vertical_repeat())
	{
		// Narrow the texture size for purposes of reducing load, since we'll be discarding this texture right away.
		if (desc.clamp.desc.WMS == CLAMPBits::REGION_CLAMP)
		{
			// Further clamp the range.
			desc.clamp.desc.MINU = std::max<int>(
					int(desc.clamp.desc.MINU), std::min<int>(uv_bb.x, int(desc.clamp.desc.MAXU)));
			desc.clamp.desc.MAXU = std::min<int>(
					int(desc.clamp.desc.MAXU), std::max<int>(uv_bb.z, int(desc.clamp.desc.MINU)));
		}
		else
		{
			// Invent a clamp.
			desc.clamp.desc.WMS = CLAMPBits::REGION_CLAMP;
			desc.clamp.desc.MINU = std::max<int>(0, uv_bb.x);
			desc.clamp.desc.MAXU = uv_bb.z;
		}

		if (desc.clamp.desc.WMT == CLAMPBits::REGION_CLAMP)
		{
			// Further clamp the range.
			desc.clamp.desc.MINV = std::max<int>(
					int(desc.clamp.desc.MINV), std::min<int>(uv_bb.y, int(desc.clamp.desc.MAXV)));
			desc.clamp.desc.MAXV = std::min<int>(
					int(desc.clamp.desc.MAXV), std::max<int>(uv_bb.w, int(desc.clamp.desc.MINV)));
		}
		else
		{
			// Invent a clamp.
			desc.clamp.desc.WMT = CLAMPBits::REGION_CLAMP;
			desc.clamp.desc.MINV = std::max<int>(0, uv_bb.y);
			desc.clamp.desc.MAXV = uv_bb.w;
		}
	}
	else
	{
		// Ignore {MIN,MAX}{U,V} if region modes are not used.
		if (!desc.clamp.desc.has_horizontal_region())
		{
			// Normalize these so we don't create duplicate textures for different clamp modes.
			desc.clamp.desc.MINU = 0;
			desc.clamp.desc.MAXU = 0;
			desc.clamp.desc.WMS = CLAMPBits::CLAMP;
		}

		if (!desc.clamp.desc.has_vertical_region())
		{
			// Normalize these so we don't create duplicate textures for different clamp modes.
			desc.clamp.desc.MINV = 0;
			desc.clamp.desc.MAXV = 0;
			desc.clamp.desc.WMT = CLAMPBits::CLAMP;
		}
	}

	auto TW = uint32_t(desc.tex0.desc.TW);
	auto TH = uint32_t(desc.tex0.desc.TH);
	uint32_t width = 1u << TW;
	uint32_t height = 1u << TH;

	// No point in uploading mips if we never access it.
	if (!desc.tex1.desc.mmin_has_mipmap())
		desc.tex1.desc.MXL = 0;

	// Memoize this computation.
	state_tracker.tex.rect = desc.rect = GSRenderer::compute_effective_texture_rect(desc);
	state_tracker.tex.levels[0].base = desc.tex0.desc.TBP0;
	state_tracker.tex.levels[0].stride = desc.tex0.desc.TBW;

	if (desc.rect.levels >= 2)
	{
		desc.miptbp1_3.desc.TBP1 = state_tracker.tex.levels[1].base = ctx.miptbl_1_3.desc.TBP1;
		desc.miptbp1_3.desc.TBW1 = state_tracker.tex.levels[1].stride = ctx.miptbl_1_3.desc.TBW1;
	}

	if (desc.rect.levels >= 3)
	{
		desc.miptbp1_3.desc.TBP2 = state_tracker.tex.levels[2].base = ctx.miptbl_1_3.desc.TBP2;
		desc.miptbp1_3.desc.TBW2 = state_tracker.tex.levels[2].stride = ctx.miptbl_1_3.desc.TBW2;
	}

	if (desc.rect.levels >= 4)
	{
		desc.miptbp1_3.desc.TBP3 = state_tracker.tex.levels[3].base = ctx.miptbl_1_3.desc.TBP3;
		desc.miptbp1_3.desc.TBW3 = state_tracker.tex.levels[3].stride = ctx.miptbl_1_3.desc.TBW3;
	}

	if (desc.rect.levels >= 5)
	{
		desc.miptbp4_6.desc.TBP1 = state_tracker.tex.levels[4].base = ctx.miptbl_4_6.desc.TBP1;
		desc.miptbp4_6.desc.TBW1 = state_tracker.tex.levels[4].stride = ctx.miptbl_4_6.desc.TBW1;
	}

	if (desc.rect.levels >= 6)
	{
		desc.miptbp4_6.desc.TBP2 = state_tracker.tex.levels[5].base = ctx.miptbl_4_6.desc.TBP2;
		desc.miptbp4_6.desc.TBW2 = state_tracker.tex.levels[5].stride = ctx.miptbl_4_6.desc.TBW2;
	}

	if (desc.rect.levels >= 7)
	{
		desc.miptbp4_6.desc.TBP3 = state_tracker.tex.levels[6].base = ctx.miptbl_4_6.desc.TBP3;
		desc.miptbp4_6.desc.TBW3 = state_tracker.tex.levels[6].stride = ctx.miptbl_4_6.desc.TBW3;
	}

	// Only affects shading.
	desc.tex1.desc.LCM = 0;
	desc.tex1.desc.MMAG = 0;
	desc.tex1.desc.MMIN = 0;
	desc.tex1.desc.MTBA = 0;
	desc.tex1.desc.L = 0;
	desc.tex1.desc.K = 0;

	// May flush render pass if there is a hazard.
	if (cache_texture)
		update_texture_page_rects_and_read();

	// If we have called texflush, last_texture_index is invalid, and we need full re-check.
	if (state_tracker.last_texture_index != UINT32_MAX &&
	    !render_pass.tex_infos.empty() &&
	    state_tracker.last_texture_descriptor == desc)
	{
		return state_tracker.last_texture_index;
	}

	uint32_t texture_index;

	Util::Hasher hasher;
	hasher.u64(desc.tex0.bits);
	hasher.u64(desc.tex1.bits);
	hasher.u64(desc.texa.bits);
	hasher.u64(desc.miptbp1_3.bits);
	hasher.u64(desc.miptbp4_6.bits);
	hasher.u64(desc.clamp.bits);
	// Palette bank needs to be part of hash key.
	// If the same texture is being used with different palettes things break really fast.
	// We need to be able to hold different variants of the same texture in the memoization structure.
	// The page tracker never keeps more than one variant alive however, so the multiple variants only
	// live as long as we can maintain the render pass.
	hasher.u64(desc.palette_bank);

	auto *cached_index = render_pass.texture_map.find(hasher.get());

	if (cached_index && cached_index->valid)
	{
		texture_index = cached_index->index;
	}
	else
	{
		// If we're not caching in the page tracker, we have to at least do hazard tracking on the first read from VRAM.
		// Any subsequent read from this texture will ignore hazard tracking.
		if (!cache_texture)
			update_texture_page_rects_and_read();

		auto image = tracker.find_cached_texture(hasher.get());
		if (!image)
		{
			TRACE("CACHE IMAGE", desc);
			desc.hash = hasher.get();
			image = renderer.create_cached_texture(desc);

			// If this is not the case, we imply self-managed.
			// This is the case for explicit feedback where we don't want to care about hazards.
			if (cache_texture)
			{
				tracker.register_cached_texture(state_tracker.tex.page_rects, desc.rect.levels,
				                                csa_mask, render_pass.clut_instance,
				                                hasher.get(), image);
			}
		}

		texture_index = render_pass.tex_infos.size();

		if (cached_index)
		{
			cached_index->index = texture_index;
			cached_index->valid = true;
		}
		else
			render_pass.texture_map.emplace_replace(hasher.get(), texture_index);

		TextureInfo info = {};
		info.view = &image->get_view();
		info.info.sizes = vec4(float(width), float(height),
							   1.0f / float(info.view->get_view_width()),
							   1.0f / float(info.view->get_view_height()));

		if (uint32_t(desc.clamp.desc.WMS) == CLAMPBits::CLAMP)
		{
			info.info.region.x = 0.0f;
			info.info.region.z = float(info.view->get_view_width()) - 1.0f;
		}
		else if (uint32_t(desc.clamp.desc.WMS) == CLAMPBits::REGION_CLAMP)
		{
			info.info.region.x = float(uint32_t(desc.clamp.desc.MINU));
			info.info.region.z = float(uint32_t(desc.clamp.desc.MAXU));
		}

		if (uint32_t(desc.clamp.desc.WMT) == CLAMPBits::CLAMP)
		{
			info.info.region.y = 0.0f;
			info.info.region.w = float(info.view->get_view_height()) - 1.0f;
		}
		else if (uint32_t(desc.clamp.desc.WMT) == CLAMPBits::REGION_CLAMP)
		{
			info.info.region.y = float(uint32_t(desc.clamp.desc.MINV));
			info.info.region.w = float(uint32_t(desc.clamp.desc.MAXV));
		}

		info.info.bias.x = -float(desc.rect.x) * info.info.sizes.z;
		info.info.bias.y = -float(desc.rect.y) * info.info.sizes.w;

		render_pass.tex_infos.push_back(info);
		render_pass.held_images.push_back(std::move(image));
	}

	state_tracker.last_texture_descriptor = desc;
	state_tracker.last_texture_index = texture_index;
	return texture_index;
}

void GSInterface::drawing_kick_update_state(ColorFeedbackMode feedback_mode, const ivec4 &uv_bb, const ivec4 &bb)
{
	if (!get_and_clear_dirty_flag(STATE_DIRTY_PRIM_TEMPLATE_BIT))
		return;

	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	auto &p = state_tracker.prim_template;
	p = {};

	if (prim.desc.TME)
	{
		p.tex = drawing_kick_update_texture(feedback_mode, uv_bb, bb) << TEX_TEXTURE_INDEX_OFFSET;
		p.tex |= ctx.tex1.desc.MMAG == TEX1Bits::LINEAR ? TEX_SAMPLER_MAG_LINEAR_BIT : 0;
		p.tex |= ctx.clamp.desc.has_horizontal_clamp() ? TEX_SAMPLER_CLAMP_S_BIT : 0;
		p.tex |= ctx.clamp.desc.has_vertical_clamp() ? TEX_SAMPLER_CLAMP_T_BIT : 0;

		switch (ctx.tex1.desc.MMIN)
		{
		case TEX1Bits::LINEAR:
			p.tex |= TEX_SAMPLER_MIN_LINEAR_BIT;
			break;
		case TEX1Bits::NEAREST_MIPMAP_LINEAR:
			p.tex |= TEX_SAMPLER_MIPMAP_LINEAR_BIT;
			break;
		case TEX1Bits::LINEAR_MIPMAP_NEAREST:
			p.tex |= TEX_SAMPLER_MIN_LINEAR_BIT;
			break;
		case TEX1Bits::LINEAR_MIPMAP_LINEAR:
			p.tex |= TEX_SAMPLER_MIN_LINEAR_BIT | TEX_SAMPLER_MIPMAP_LINEAR_BIT;
			break;
		default:
			break;
		}

		p.tex2 = ctx.tex1.desc.LCM << TEX2_FIXED_LOD_OFFSET;
		p.tex2 |= ctx.tex1.desc.L << TEX2_L_OFFSET;
		p.tex2 |= ctx.tex1.desc.K << TEX2_K_OFFSET;
		if (ctx.tex1.desc.mmin_has_mipmap())
			p.tex |= ctx.tex1.desc.MXL << TEX_MAX_MIP_LEVEL_OFFSET;
	}

	// Update state after updating texture state, since reading a texture may cause a flush,
	// which resets the state vectors.
	p.state = drawing_kick_update_state_vector() << STATE_INDEX_BIT_OFFSET;

	if (ctx.test.desc.ZTE == TESTBits::ZTE_ENABLED)
	{
		if (ctx.test.desc.has_z_test())
		{
			p.state |= 1u << STATE_BIT_Z_TEST;
			p.state |= ctx.test.desc.ZTST == TESTBits::ZTST_GREATER ? (1u << STATE_BIT_Z_TEST_GREATER) : 0;
		}

		if (ctx.zbuf.desc.ZMSK == 0)
			p.state |= 1u << STATE_BIT_Z_WRITE;
	}

	bool color_write_needs_previous_pixels = false;

	// AA1 implies alpha-blending of some sort.
	if (prim.desc.ABE || prim.desc.AA1)
	{
		// If any of the blend factors use dst color, it's not opaque.
		// It's still possible to abuse blender to do extra math while remaining opaque.
		if (ctx.alpha.desc.A == BLEND_RGB_DEST ||
		    ctx.alpha.desc.B == BLEND_RGB_DEST ||
		    ctx.alpha.desc.C == BLEND_ALPHA_DEST ||
		    ctx.alpha.desc.D == BLEND_RGB_DEST)
		{
			color_write_needs_previous_pixels = true;
		}
	}

	// Any pixel test mode cannot be opaque.
	if ((ctx.test.desc.ATE && ctx.test.desc.ATST != ATST_ALWAYS) || ctx.test.desc.DATE || ctx.frame.desc.FBMSK != 0)
		color_write_needs_previous_pixels = true;

	// If we're in a feedback situation,
	// we cannot be opaque since sampling a texture essentially becomes blending.
	if (render_pass.is_color_feedback)
		color_write_needs_previous_pixels = true;

	// If OPAQUE, the frame buffer color content is fully written if Z test passes.
	// Final output does not depend on previous color data at all.
	if (!color_write_needs_previous_pixels)
		p.state |= 1u << STATE_BIT_OPAQUE;

	if (prim.desc.AA1)
	{
		p.state |= 1u << STATE_BIT_MULTISAMPLE;
		render_pass.has_aa1 = true;
	}

	if (registers.scanmsk.desc.has_mask())
	{
		p.state |= 1u << (STATE_BIT_SCANMSK_EVEN + registers.scanmsk.desc.MSK - SCANMSKBits::MSK_SKIP_EVEN);
		render_pass.has_scanmsk = true;
	}

	if (!prim.desc.FST)
		p.state |= 1u << STATE_BIT_PERSPECTIVE;
	if (prim.desc.IIP)
		p.state |= 1u << STATE_BIT_IIP;
	if (prim.desc.FIX)
		p.state |= 1u << STATE_BIT_FIX;
}

PageRect GSInterface::compute_fb_rect(const ivec4 &bb) const
{
	auto bb_page = bb >> ivec2(render_pass.fb_page_width_log2, render_pass.fb_page_height_log2).xyxy();
	// We know this BB is not degenerate already.

	PageRect page = {};

	page.base_page = render_pass.frame.desc.FBP;
	page.page_width = bb_page.z - bb_page.x + 1;
	page.page_height = bb_page.w - bb_page.y + 1;
	page.page_stride = render_pass.frame.desc.FBW;
	page.base_page += bb_page.x + bb_page.y * page.page_stride;
	page.block_mask = UINT32_MAX;
	page.write_mask = psm_word_write_mask(render_pass.frame.desc.PSM);

	return page;
}

PageRect GSInterface::compute_z_rect(const ivec4 &bb) const
{
	auto bb_page = bb >> ivec2(render_pass.z_page_width_log2, render_pass.z_page_height_log2).xyxy();
	// We know this BB is not degenerate already.

	PageRect page = {};

	page.base_page = render_pass.zbuf.desc.ZBP;
	page.page_width = bb_page.z - bb_page.x + 1;
	page.page_height = bb_page.w - bb_page.y + 1;
	page.page_stride = render_pass.frame.desc.FBW;
	page.base_page += bb_page.x + bb_page.y * page.page_stride;
	page.block_mask = UINT32_MAX;
	page.write_mask = psm_word_write_mask(render_pass.zbuf.desc.PSM);

	return page;
}

bool GSInterface::draw_is_degenerate()
{
	if (!get_and_clear_dirty_flag(STATE_DIRTY_DEGENERATE_BIT))
		return state_tracker.degenerate_draw;

	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	// Degenerate scissor.
	if (ctx.scissor.desc.SCAX0 > ctx.scissor.desc.SCAX1 ||
	    ctx.scissor.desc.SCAY0 > ctx.scissor.desc.SCAY1)
	{
		state_tracker.degenerate_draw = true;
		return true;
	}

	// We never pass the depth test.
	if (ctx.test.desc.ZTE == TESTBits::ZTE_ENABLED && ctx.test.desc.ZTST == TESTBits::ZTST_NEVER)
	{
		state_tracker.degenerate_draw = true;
		return true;
	}

	// We force alpha test to fail, and fail mode is to keep FB contents -> no side effects.
	if (ctx.test.desc.ATE && ctx.test.desc.ATST == ATST_NEVER && ctx.test.desc.AFAIL == AFAIL_KEEP)
	{
		state_tracker.degenerate_draw = true;
		return true;
	}

	// Any write is ignored. PS2 rendering does not have side effects.
	// Undefined ZTE seems to mean ignore depth completely.
	bool read_only_depth = ctx.zbuf.desc.ZMSK != 0 || ctx.test.desc.ZTE == TESTBits::ZTE_UNDEFINED;
	bool read_only_color = ctx.frame.desc.FBMSK == UINT32_MAX;
	state_tracker.degenerate_draw = read_only_color && read_only_depth;
	return state_tracker.degenerate_draw;
}

bool GSInterface::state_is_z_sensitive() const
{
	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	if (ctx.test.desc.ZTE == TESTBits::ZTE_ENABLED)
	{
		// We need to read depth.
		if (ctx.test.desc.has_z_test())
			return true;

		// We need to write depth.
		// ZTST_NEVER will trigger degenerate draw and won't hit this path.
		if (ctx.zbuf.desc.ZMSK == 0)
			return true;
	}

	return false;
}

void GSInterface::update_color_feedback_state()
{
	if (!get_and_clear_dirty_flag(STATE_DIRTY_FEEDBACK_BIT))
	{
		// If we're in feedback, we have to recheck state every draw. We expect that anyway
		// since FB will likely have to be flushed every draw ...
		if (render_pass.is_color_feedback)
			state_tracker.dirty_flags |= STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT;
		return;
	}

	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];
	render_pass.is_color_feedback = false;
	render_pass.is_potential_color_feedback = false;
	render_pass.is_potential_depth_feedback = false;

	if (!prim.desc.TME)
		return;

	if (ctx.clamp.desc.WMS == CLAMPBits::REGION_REPEAT || ctx.clamp.desc.WMT == CLAMPBits::REGION_REPEAT)
	{
		// Anything repeat region is too messy.
		return;
	}

	// Mip-mapping is too weird to deal with.
	if (ctx.tex1.desc.has_mipmap())
		return;

	auto tex_psm = uint32_t(ctx.tex0.desc.PSM);

	if (uint32_t(ctx.tex0.desc.TBP0) != ctx.frame.desc.FBP * BLOCKS_PER_PAGE)
	{
		// If TBP < FBP we may still have a potential feedback caused by game using randomly large TW/TH
		// and not using REGION_CLAMP properly. E.g. a 1024x1024 texture with 32-bit will cover the entirety of VRAM.
		// The end of a texture may straddle into the frame buffer
		// even if game never intends to actually sample from that region.
		// In this case, there's no reasonable way it will work, so try to clamp the page rect to avoid false hazards.
		// This will break if game actually intended to sample like this, but it seems extremely unlikely in practice.

		compute_has_potential_feedback(ctx.tex0.desc, ctx.frame.desc.FBP, ctx.zbuf.desc.ZBP,
		                               vram_size / PAGE_ALIGNMENT_BYTES,
		                               render_pass.is_potential_color_feedback,
		                               render_pass.is_potential_depth_feedback);

		// Cannot rely on render_pass.z_write fully since this is called before we commit Z-state.
		bool has_z_write = render_pass.z_write || (state_is_z_sensitive() && ctx.zbuf.desc.ZMSK == 0);

		uint32_t tex_write_mask = psm_word_write_mask(tex_psm);
		uint32_t fb_write_mask = psm_word_write_mask(render_pass.frame.desc.PSM);
		uint32_t z_write_mask = psm_word_write_mask(render_pass.zbuf.desc.PSM);

		// If aliasing with 8H and 24, that is fine.
		if ((tex_write_mask & fb_write_mask) == 0)
			render_pass.is_potential_color_feedback = false;
		if ((tex_write_mask & z_write_mask) == 0 || !has_z_write)
			render_pass.is_potential_depth_feedback = false;

		// Exit analysis, we know it's not true feedback.
		return;
	}

	if (uint32_t(ctx.tex0.desc.TBW) != ctx.frame.desc.FBW)
		return;

	// For feedback, we assume that the texture format has same bpp and swizzle format.
	if (swizzle_compat_key(tex_psm) != swizzle_compat_key(ctx.frame.desc.PSM))
		return;

	uint32_t width = 1u << uint32_t(ctx.tex0.desc.TW);
	uint32_t height = 1u << uint32_t(ctx.tex0.desc.TH);

	// Ensures that image covers entire frame buffer.
	if (ctx.frame.desc.FBW * BUFFER_WIDTH_SCALE > width)
		return;

	// There is no framebuffer height, but we can deduce it based on scissor Y max.
	if (ctx.scissor.desc.SCAY1 >= height)
		return;

	// If we're in feedback, we have to recheck state every draw. We expect that anyway
	// since FB will likely have to be flushed every draw anyway ...
	render_pass.is_color_feedback = true;
	state_tracker.dirty_flags |= STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT;
}

template <bool quad, unsigned num_vertices>
GSInterface::ColorFeedbackMode
GSInterface::deduce_color_feedback_mode(const VertexPosition *pos, const VertexAttribute *attr,
                                        const ContextState &ctx, const PRIMBits &prim, ivec4 &uv_bb, const ivec4 &bb)
{
	// Sprite and triangle is fine. Line is not ok.
	constexpr bool can_feedback = num_vertices == 3 || (quad && num_vertices == 2);
	if (!can_feedback)
		return ColorFeedbackMode::None;

	int width = 1 << int(ctx.tex0.desc.TW);
	int height = 1 << int(ctx.tex0.desc.TH);
	auto fwidth = float(width * 16);
	auto fheight = float(height * 16);
	bool needs_perspective = false;

	ivec2 uv0, uv1, uv2 = {};
	if (prim.FST)
	{
		uv0 = ivec2(attr[0].uv);
		uv1 = ivec2(attr[1].uv);
		if (!quad)
			uv2 = ivec2(attr[2].uv);
	}
	else
	{
		// If we have perspective, we cannot assume pixel correctness.
		// For sprite, Q is flat, and we only use Q0 anyway.
		if (!quad)
			if (attr[0].q != attr[1].q || attr[1].q != attr[2].q)
				needs_perspective = true;

		float inv_q0 = 1.0f / attr[0].q;
		float inv_q1 = 1.0f / attr[1].q;
		uv0 = ivec2(vec2(fwidth, fheight) * (attr[0].st * inv_q0));
		uv1 = ivec2(vec2(fwidth, fheight) * (attr[1].st * inv_q1));

		if (!quad)
		{
			float inv_q2 = 1.0f / attr[2].q;
			uv2 = ivec2(vec2(fwidth, fheight) * (attr[2].st * inv_q2));
		}
	}

	ivec2 uv_min = min(uv0, uv1);
	ivec2 uv_max = max(uv0, uv1);
	if (!quad)
	{
		uv_min = min(uv_min, uv2);
		uv_max = max(uv_max, uv2);
	}

	// Consider linear filtering if using that. Expand the BB appropriately.
	if (ctx.tex1.desc.MMAG != 0)
	{
		uv_min -= ivec2(1 << (SUBPIXEL_BITS - 1));
		uv_max += ivec2((1 << SUBPIXEL_BITS) - 1);
	}

	// This can safely become a REGION_CLAMP.
	uv_bb = ivec4(uv_min, uv_max) >> SUBPIXEL_BITS;

	// Check if we're sampling outside the texture's range. In this case we get clamp or repeat,
	// and we cannot assume 1:1 pixel mapping.
	// We'll allow equal, since bottom-right pixels won't get rendered usually.
	// Any line with linear filtering is probably not pixel feedback.
	// Anything with perspective won't work with Pixel mode either.
	if (needs_perspective || ctx.tex1.desc.MMAG == TEX1Bits::LINEAR)
		return ColorFeedbackMode::Sliced;

	// Based on the primitive BB, if the region clamp contains the full primitive BB, we cannot observe clamping,
	// so ignore the effect.
	if (uint32_t(ctx.clamp.desc.WMS) == CLAMPBits::REGION_CLAMP)
	{
		int minu = int(ctx.clamp.desc.MINU);
		int maxu = int(ctx.clamp.desc.MAXU);
		if (bb.x < minu || bb.z > maxu)
			return ColorFeedbackMode::Sliced;
	}

	if (uint32_t(ctx.clamp.desc.WMT) == CLAMPBits::REGION_CLAMP)
	{
		int minv = int(ctx.clamp.desc.MINV);
		int maxv = int(ctx.clamp.desc.MAXV);
		if (bb.y < minv || bb.w > maxv)
			return ColorFeedbackMode::Sliced;
	}

	ivec2 uv0_delta = uv0 - pos[0].pos;
	ivec2 uv1_delta = uv1 - pos[1].pos;
	ivec2 min_delta = min(uv0_delta, uv1_delta);
	ivec2 max_delta = max(uv0_delta, uv1_delta);

	if (!quad)
	{
		ivec2 uv2_delta = uv2 - pos[2].pos;
		min_delta = min(min_delta, uv2_delta);
		max_delta = max(max_delta, uv2_delta);
	}

	int min_delta2 = min(min_delta.x, min_delta.y);
	int max_delta2 = max(max_delta.x, max_delta.y);

	// The UV offset must be in range of [0, 2^SUBPIXEL_BITS - 1]. This guarantees snapping with NEAREST.
	// 8 is ideal. That means pixel centers during interpolation will land exactly in the center of the texel.
	// In theory we could allow LINEAR if uv delta was exactly 8 for all vertices.
	if (min_delta2 < 0 || max_delta2 >= (1 << SUBPIXEL_BITS))
		return ColorFeedbackMode::Sliced;

	// Perf go brrrrrrr.
	return ColorFeedbackMode::Pixel;
}

template <bool list_primitive, bool fan_primitive, bool quad, unsigned num_vertices>
void GSInterface::drawing_kick_append()
{
	auto &prim = registers.prim;
	auto &ctx = registers.ctx[prim.desc.CTXT];

	VertexAttribute attr[3];
	VertexPosition pos[3];

	int off_x = int(ctx.xyoffset.desc.OFX);
	int off_y = int(ctx.xyoffset.desc.OFY);

	if (num_vertices == 1)
	{
		pos[0] = vertex_queue.pos[vertex_queue.count - 1];
		attr[0] = vertex_queue.attr[vertex_queue.count - 1];

		pos[0].pos.x -= off_x + (1 << (SUBPIXEL_BITS - 1));
		pos[0].pos.y -= off_y + (1 << (SUBPIXEL_BITS - 1));

		pos[1] = pos[0];
		pos[1].pos.x += 1 << SUBPIXEL_BITS;
		pos[1].pos.y += 1 << SUBPIXEL_BITS;
	}
	else if (num_vertices == 2)
	{
		for (uint32_t i = 0; i < num_vertices; i++)
		{
			pos[i] = vertex_queue.pos[vertex_queue.count - 1 - i];
			attr[i] = vertex_queue.attr[vertex_queue.count - 1 - i];
			pos[i].pos.x -= off_x;
			pos[i].pos.y -= off_y;
		}
	}
	else if (num_vertices == 3)
	{
		for (uint32_t i = 0; i < num_vertices; i++)
		{
			pos[i] = vertex_queue.pos[2 - i];
			attr[i] = vertex_queue.attr[2 - i];
			pos[i].pos.x -= off_x;
			pos[i].pos.y -= off_y;
		}
	}

	ivec2 lo_pos = muglm::min(pos[0].pos, pos[1].pos);
	ivec2 hi_pos = muglm::max(pos[0].pos, pos[1].pos);

	// Take into account line expansion just to be safe.
	constexpr bool is_line = !quad && num_vertices == 2;

	if (!quad && !is_line)
	{
		lo_pos = muglm::min(pos[2].pos, lo_pos);
		hi_pos = muglm::max(pos[2].pos, hi_pos);
	}

	hi_pos -= 1;
	// Tighten the bounding box according to top-left raster rules.
	if (quad || !registers.prim.desc.AA1)
		lo_pos += (1 << int(SUBPIXEL_BITS - sampling_rate_y_log2)) - 1;

	lo_pos >>= int(SUBPIXEL_BITS);
	hi_pos >>= int(SUBPIXEL_BITS);

	if (is_line)
	{
		lo_pos -= ivec2(1);
		hi_pos += ivec2(1);
	}

	ivec2 sci_lo = ivec2(ctx.scissor.desc.SCAX0, ctx.scissor.desc.SCAY0);
	ivec2 sci_hi = ivec2(ctx.scissor.desc.SCAX1, ctx.scissor.desc.SCAY1);
	lo_pos = muglm::max(lo_pos, sci_lo);
	hi_pos = muglm::min(hi_pos, sci_hi);

	// TODO: separate state update for scissor update.
	hi_pos.x = std::min<int>(hi_pos.x, int(ctx.frame.desc.FBW * BUFFER_WIDTH_SCALE) - 1);
	ivec4 bb = ivec4(lo_pos, hi_pos);

	// Check for degenerate BB. Can happen if primitive is clipped away completely by scissor.
	if (bb.z < bb.x || bb.w < bb.y)
	{
		TRACE("Degenerate BB", bb);
		return;
	}

	update_color_feedback_state();
	auto feedback_mode = ColorFeedbackMode::None;
	ivec4 uv_bb = {};
	if (render_pass.is_color_feedback)
		feedback_mode = deduce_color_feedback_mode<quad, num_vertices>(pos, attr, ctx, prim.desc, uv_bb, bb);

	// If there's a partial transfer in-flight, flush it.
	// The write should technically happen as soon as we write HWREG.
	// This can trigger a texture invalidation. We need to do it here, before checking for texture dirty state.
	if (prim.desc.TME && transfer_state.host_to_local_active &&
	    transfer_state.host_to_local_payload.size() > transfer_state.last_flushed_qwords)
	{
#ifdef PARALLEL_GS_DEBUG
		LOGW("Flushing partial transfer due to texture read ...\n");
#endif
		flush_pending_transfer(true);
	}

	// Even if no state changes, we have to consider potential hazards.
	// If a hazard does occur, dirty bits will be set appropriately,
	// re-triggering state checks.
	check_frame_buffer_state();

	assert(bb.z < int(render_pass.frame.desc.FBW * BUFFER_WIDTH_SCALE));
	assert(bb.z < int(ctx.frame.desc.FBW * BUFFER_WIDTH_SCALE));

	// Have to make sure it's still safe to read the texture we're using.
	// Only do this when dirty flag is not set. Otherwise, we'll check it when resolving texture index anyway.
	if (prim.desc.TME && (state_tracker.dirty_flags & STATE_DIRTY_TEX_BIT) == 0)
		texture_page_rects_read();

	drawing_kick_update_state(feedback_mode, uv_bb, bb);
	const auto &prim_state = state_tracker.prim_template;

	PrimitiveAttribute prim_attr;
	prim_attr.tex = prim_state.tex;
	prim_attr.tex2 = prim_state.tex2;
	prim_attr.state = prim_state.state;
	prim_attr.fbmsk = ctx.frame.desc.FBMSK;
	prim_attr.fogcol = registers.fogcol.words[0];
	prim_attr.alpha = (ctx.alpha.desc.FIX << ALPHA_AFIX_OFFSET) |
	                  (ctx.test.desc.AREF << ALPHA_AREF_OFFSET);

	if (quad)
	{
		prim_attr.state |= 1u << STATE_BIT_PARALLELOGRAM;
		prim_attr.state |= 1u << STATE_BIT_SPRITE;
		prim_attr.state |= 1u << STATE_BIT_SNAP_RASTER;
		prim_attr.state &= ~(1u << STATE_BIT_MULTISAMPLE);
	}
	else if (is_line)
	{
		prim_attr.state |= 1u << STATE_BIT_PARALLELOGRAM;
		prim_attr.state |= 1u << STATE_BIT_LINE;
		// Lines always have less than full coverage, if using AA1, never write Z.
		if ((prim_attr.state & (1u << STATE_BIT_MULTISAMPLE)) != 0)
			prim_attr.state &= ~(1u << STATE_BIT_Z_WRITE);
	}

	if (num_vertices == 1)
	{
		// Don't interpolate anything.
		prim_attr.state |= 1u << STATE_BIT_FIX;
		// Don't think we can reasonably upscale a point. Games can rely on the rounding to generate an exact pixel.
		prim_attr.state |= 1u << STATE_BIT_SNAP_RASTER;
	}

	// If our damage region expands, then mark hazards.
	// This avoids spam where we have to remark pages as dirty every single draw.
	bool rp_expands = false;
	bool is_z_sensitive = state_is_z_sensitive();

	// We go from no Z pages to at least read-only Z.
	if (!render_pass.z_sensitive && is_z_sensitive)
	{
		render_pass.z_sensitive = true;
		rp_expands = true;
	}

	// We go from read-only Z to read-write Z.
	if (is_z_sensitive && ctx.zbuf.desc.ZMSK == 0 && !render_pass.z_write)
	{
		render_pass.z_write = true;
		// With Z writes existing, we might have a feedback we didn't have before.
		state_tracker.dirty_flags |= STATE_DIRTY_FEEDBACK_BIT;
		rp_expands = true;
	}

	// Color write mask increases, redamage all pages.
	uint32_t write_mask = ~ctx.frame.desc.FBMSK;
	if ((write_mask & render_pass.color_write_mask) != write_mask)
	{
		render_pass.color_write_mask |= write_mask;
		rp_expands = true;
	}

	// Expand render pass BB.
	// If we expand, damage pages.
	// Writing fine-grained FB results is too costly on CPU,
	// but it is an option if we have to in certain scenarios.
	if (bb.x < render_pass.bb.x) { rp_expands = true; render_pass.bb.x = bb.x; }
	if (bb.y < render_pass.bb.y) { rp_expands = true; render_pass.bb.y = bb.y; }
	if (bb.z > render_pass.bb.z) { rp_expands = true; render_pass.bb.z = bb.z; }
	if (bb.w > render_pass.bb.w) { rp_expands = true; render_pass.bb.w = bb.w; }

	if (rp_expands)
	{
		// Damage pages.
		// This is very conservative, and potentially can trigger hazards which should not exist,
		// but this seems unlikely without solid proof that games care.
		auto fb_rect = compute_fb_rect(render_pass.bb);
		fb_rect.write_mask &= render_pass.color_write_mask;
		tracker.mark_fb_write(fb_rect);

		if (render_pass.z_sensitive)
		{
			auto z_rect = compute_z_rect(render_pass.bb);
			if (render_pass.z_write)
				tracker.mark_fb_write(z_rect);
			else
				tracker.mark_fb_read(z_rect);
		}
	}

	prim_attr.bb = i16vec4(bb);

	TRACE("Primitive", prim_attr);
	TRACE("DRAW", render_pass.primitive_count);

	render_pass.prim[render_pass.primitive_count] = prim_attr;
	memcpy(render_pass.positions.data() + 3 * render_pass.primitive_count, pos, sizeof(pos));
	memcpy(render_pass.attributes.data() + 3 * render_pass.primitive_count, attr, sizeof(attr));
	render_pass.primitive_count++;

	// Mark state as explicitly not dirty now. If we ended up flushing render pass due to e.g. texture state,
	// some dirty bits will remain set, despite not actually being dirty.
	state_tracker.dirty_flags = 0;
}

template <bool list_primitive, bool fan_primitive, bool quad, unsigned num_vertices>
void GSInterface::drawing_kick_maintain_queue()
{
	static_assert(!fan_primitive || !list_primitive, "Cannot be both fan and list primitive.");
	static_assert(num_vertices <= 3 && num_vertices >= 1, "Num vertices out of range.");
	static_assert(!quad || num_vertices != 3, "Cannot have quad primitive with 3 vertices.");

	if (fan_primitive)
	{
		vertex_queue.pos[1] = vertex_queue.pos[2];
		vertex_queue.attr[1] = vertex_queue.attr[2];
		vertex_queue.count = 2;
	}
	else if (list_primitive)
	{
		vertex_queue.count = 0;
	}

	// Strip primitive will shift queue on next vertex kick.
}

template <bool list_primitive, bool fan_primitive, bool quad, unsigned num_vertices>
void GSInterface::drawing_kick_primitive(bool adc)
{
	if (vertex_queue.count < num_vertices)
		return;

	if (!adc)
	{
		if (!draw_is_degenerate())
			drawing_kick_append<list_primitive, fan_primitive, quad, num_vertices>();
		else
			TRACE("Degenerate Draw", DummyBits{});
	}

	// We seem to do queue maintenance regardless after a vertex kick.
	drawing_kick_maintain_queue<list_primitive, fan_primitive, quad, num_vertices>();
}

void GSInterface::drawing_kick_invalid(bool)
{
	// Flush the queue, no nothing otherwise.
	vertex_queue.count = 0;
}

void GSInterface::drawing_kick(bool adc)
{
	(this->*draw_handler)(adc);
	post_draw_kick_handler();
}

template <PRIMType PRIM>
void GSInterface::drawing_kick(bool adc)
{
	// constexpr dispatch
	switch (PRIM)
	{
	case PRIMType::Point:
		drawing_kick_primitive<true, false, true, 1>(adc);
		break;

	case PRIMType::LineList:
		drawing_kick_primitive<true, false, false, 2>(adc);
		break;

	case PRIMType::LineStrip:
		drawing_kick_primitive<false, false, false, 2>(adc);
		break;

	case PRIMType::TriangleList:
		drawing_kick_primitive<true, false, false, 3>(adc);
		break;

	case PRIMType::TriangleStrip:
		drawing_kick_primitive<false, false, false, 3>(adc);
		break;

	case PRIMType::TriangleFan:
		drawing_kick_primitive<false, true, false, 3>(adc);
		break;

	case PRIMType::Sprite:
		drawing_kick_primitive<true, false, true, 2>(adc);
		break;

	default:
		break;
	}

	post_draw_kick_handler();
}

void GSInterface::post_draw_kick_handler()
{
	// If we have buffered up too much, flush out automatically now.
	if (render_pass.pending_palette_updates >= CLUTInstances ||
	    render_pass.primitive_count >= MaxPrimitivesPerFlush ||
		render_pass.tex_infos.size() >= MaxTextures ||
		render_pass.state_vectors.size() >= MaxStateVectors)
	{
		flush_pending_transfer(true);
		tracker.flush_render_pass(FlushReason::Overflow);
	}
}

void GSInterface::reset_vertex_queue()
{
	vertex_queue.count = 0;
}

template <int CTX>
void GSInterface::a_d_TEX2(uint64_t payload)
{
	auto &ctx = registers.ctx[CTX];
	auto &preserve = ctx.tex0.desc;

	Reg64<TEX0Bits> tex0{payload};
	tex0.desc.TBP0 = preserve.TBP0;
	tex0.desc.TBW = preserve.TBW;
	tex0.desc.TW = preserve.TW;
	tex0.desc.TH = preserve.TH;
	tex0.desc.TCC = preserve.TCC;
	tex0.desc.TFX = preserve.TFX;

	if (CTX == 0)
		a_d_TEX0_1(tex0.bits);
	else
		a_d_TEX0_2(tex0.bits);
}

void GSInterface::check_pending_transfer()
{
	if (transfer_state.host_to_local_active &&
	    transfer_state.host_to_local_payload.size() >= transfer_state.required_qwords)
	{
		flush_pending_transfer(false);
	}
}

void GSInterface::flush_pending_transfer(bool keep_alive)
{
	if (transfer_state.host_to_local_active &&
	    transfer_state.host_to_local_payload.size() > transfer_state.last_flushed_qwords)
	{
#ifdef PARALLEL_GS_DEBUG
		if (transfer_state.copy.bitbltbuf.bits != registers.bitbltbuf.bits)
			LOGW("Mismatch in bitbltbuf state.\n");
		if (transfer_state.copy.trxpos.bits != registers.trxpos.bits)
			LOGW("Mismatch in trxpos state.\n");
		if (transfer_state.copy.trxreg.bits != registers.trxreg.bits)
			LOGW("Mismatch in trxreg state.\n");
#endif

		auto dst_rect = compute_page_rect(transfer_state.copy.bitbltbuf.desc.DBP,
										  transfer_state.copy.trxpos.desc.DSAX,
										  transfer_state.copy.trxpos.desc.DSAY,
		                                  transfer_state.copy.trxreg.desc.RRW,
		                                  transfer_state.copy.trxreg.desc.RRH,
		                                  transfer_state.copy.bitbltbuf.desc.DBW,
		                                  transfer_state.copy.bitbltbuf.desc.DPSM);

		tracker.mark_transfer_write(dst_rect);
		if (tracker.invalidate_texture_cache(render_pass.clut_instance))
			mark_texture_state_dirty();

		transfer_state.copy.host_data = transfer_state.host_to_local_payload.data();
		transfer_state.copy.host_data_size = transfer_state.host_to_local_payload.size() * sizeof(uint64_t);
		transfer_state.copy.host_data_size_offset = transfer_state.last_flushed_qwords * sizeof(uint64_t);
		transfer_state.copy.host_data_size_required = transfer_state.required_qwords * sizeof(uint64_t);
		renderer.copy_vram(transfer_state.copy);

		// Very possible we just have to flush early and we never receive more image data until
		// game kicks a new transfer.
		transfer_state.last_flushed_qwords = uint32_t(transfer_state.host_to_local_payload.size());

		TRACE_HEADER("VRAM COPY", transfer_state.copy);
	}

	if (!keep_alive)
	{
		transfer_state.host_to_local_payload.clear();
		transfer_state.last_flushed_qwords = 0;
		transfer_state.host_to_local_active = false;
	}
}

void GSInterface::init_transfer()
{
	flush_pending_transfer(false);

	transfer_state.copy.trxdir = registers.trxdir;
	transfer_state.copy.trxreg = registers.trxreg;
	transfer_state.copy.trxpos = registers.trxpos;
	transfer_state.copy.bitbltbuf = registers.bitbltbuf;

	auto XDIR = transfer_state.copy.trxdir.desc.XDIR;

	if (XDIR == LOCAL_TO_LOCAL)
	{
		auto dst_rect = compute_page_rect(transfer_state.copy.bitbltbuf.desc.DBP,
		                                  transfer_state.copy.trxpos.desc.DSAX,
		                                  transfer_state.copy.trxpos.desc.DSAY,
		                                  transfer_state.copy.trxreg.desc.RRW,
		                                  transfer_state.copy.trxreg.desc.RRH,
		                                  transfer_state.copy.bitbltbuf.desc.DBW,
		                                  transfer_state.copy.bitbltbuf.desc.DPSM);

		auto src_rect = compute_page_rect(transfer_state.copy.bitbltbuf.desc.SBP,
		                                  transfer_state.copy.trxpos.desc.SSAX,
		                                  transfer_state.copy.trxpos.desc.SSAY,
		                                  transfer_state.copy.trxreg.desc.RRW,
		                                  transfer_state.copy.trxreg.desc.RRH,
		                                  transfer_state.copy.bitbltbuf.desc.SBW,
		                                  transfer_state.copy.bitbltbuf.desc.SPSM);

		tracker.mark_transfer_copy(dst_rect, src_rect);
		renderer.copy_vram(transfer_state.copy);
	}
	else if (XDIR == HOST_TO_LOCAL)
	{
		transfer_state.required_qwords =
				(transfer_state.copy.trxreg.desc.RRW *
				transfer_state.copy.trxreg.desc.RRH *
				get_bits_per_pixel(transfer_state.copy.bitbltbuf.desc.DPSM)) / 64;

		transfer_state.host_to_local_active = transfer_state.required_qwords != 0;
		// Await writes to HWREG.
	}
	else if (XDIR == LOCAL_TO_HOST)
	{
		// FIFO? TODO.
	}
}

void GSInterface::update_draw_handler()
{
	switch (PRIMType(registers.prim.desc.PRIM))
	{
	case PRIMType::Point:
		draw_handler = &GSInterface::drawing_kick_primitive<true, false, true, 1>;
		break;

	case PRIMType::LineList:
		draw_handler = &GSInterface::drawing_kick_primitive<true, false, false, 2>;
		break;

	case PRIMType::LineStrip:
		draw_handler = &GSInterface::drawing_kick_primitive<false, false, false, 2>;
		break;

	case PRIMType::TriangleList:
		draw_handler = &GSInterface::drawing_kick_primitive<true, false, false, 3>;
		break;

	case PRIMType::TriangleStrip:
		draw_handler = &GSInterface::drawing_kick_primitive<false, false, false, 3>;
		break;

	case PRIMType::TriangleFan:
		draw_handler = &GSInterface::drawing_kick_primitive<false, true, false, 3>;
		break;

	case PRIMType::Sprite:
		draw_handler = &GSInterface::drawing_kick_primitive<true, false, true, 2>;
		break;

	case PRIMType::Invalid:
		draw_handler = &GSInterface::drawing_kick_invalid;
		break;
	}
}

void GSInterface::update_optimized_gif_handler(uint32_t path)
{
	auto &hand = optimized_draw_handler[path];
	hand = nullptr;

	auto &gif_path = paths[path];

	// Only care about PACKED
	if (gif_path.tag.FLG != GIFTagBits::PACKED || gif_path.tag.NLOOP == 0)
		return;

	static const OptimizedPacketHandler STQRGBAXYZHandlers[] = {
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(0), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(1), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(2), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(3), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(4), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(5), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(6), 1>,
		&GSInterface::packed_STQRGBAXYZ<false, PRIMType(7), 1>,
	};

	static const OptimizedPacketHandler STQRGBAXYZFHandlers[] = {
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(0), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(1), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(2), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(3), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(4), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(5), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(6), 1>,
		&GSInterface::packed_STQRGBAXYZ<true, PRIMType(7), 1>,
	};

	static const OptimizedPacketHandler UVRGBAXYZHandlers[] = {
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(0), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(1), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(2), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(3), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(4), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(5), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(6), 1>,
		&GSInterface::packed_UVRGBAXYZ<false, PRIMType(7), 1>,
	};

	static const OptimizedPacketHandler UVRGBAXYZFHandlers[] = {
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(0), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(1), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(2), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(3), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(4), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(5), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(6), 1>,
		&GSInterface::packed_UVRGBAXYZ<true, PRIMType(7), 1>,
	};

	static const OptimizedPacketHandler ADONLYHandlers[] = {
		&GSInterface::packed_ADONLY<16>,
		&GSInterface::packed_ADONLY<1>,
		&GSInterface::packed_ADONLY<2>,
		&GSInterface::packed_ADONLY<3>,
		&GSInterface::packed_ADONLY<4>,
		&GSInterface::packed_ADONLY<5>,
		&GSInterface::packed_ADONLY<6>,
		&GSInterface::packed_ADONLY<7>,
		&GSInterface::packed_ADONLY<8>,
		&GSInterface::packed_ADONLY<9>,
		&GSInterface::packed_ADONLY<10>,
		&GSInterface::packed_ADONLY<11>,
		&GSInterface::packed_ADONLY<12>,
		&GSInterface::packed_ADONLY<13>,
		&GSInterface::packed_ADONLY<14>,
		&GSInterface::packed_ADONLY<15>,
	};

	constexpr uint64_t STQRGBAXYZ2_Mask =
			(uint32_t(GIFAddr::ST) << 0) |
			(uint32_t(GIFAddr::RGBAQ) << 4) |
			(uint32_t(GIFAddr::XYZ2) << 8);

	constexpr uint64_t STQRGBAXYZF2_Mask =
			(uint32_t(GIFAddr::ST) << 0) |
			(uint32_t(GIFAddr::RGBAQ) << 4) |
			(uint32_t(GIFAddr::XYZF2) << 8);

	constexpr uint64_t STQRGBAXYZ2_TriList_Mask =
			STQRGBAXYZ2_Mask | (STQRGBAXYZ2_Mask << 12) | (STQRGBAXYZ2_Mask << 24);
	constexpr uint64_t STQRGBAXYZF2_TriList_Mask =
			STQRGBAXYZF2_Mask | (STQRGBAXYZF2_Mask << 12) | (STQRGBAXYZF2_Mask << 24);

	constexpr uint64_t STQRGBAXYZ2_LineList_Mask =
			STQRGBAXYZ2_Mask | (STQRGBAXYZ2_Mask << 12);
	constexpr uint64_t STQRGBAXYZF2_LineList_Mask =
			STQRGBAXYZF2_Mask | (STQRGBAXYZF2_Mask << 12);

	constexpr uint64_t UVRGBAXYZ2_Mask =
			(uint32_t(GIFAddr::UV) << 0) |
			(uint32_t(GIFAddr::RGBAQ) << 4) |
			(uint32_t(GIFAddr::XYZ2) << 8);

	constexpr uint64_t UVRGBAXYZF2_Mask =
			(uint32_t(GIFAddr::UV) << 0) |
			(uint32_t(GIFAddr::RGBAQ) << 4) |
			(uint32_t(GIFAddr::XYZF2) << 8);

	constexpr uint32_t STXYZFSTRGBAXYZF_Mask =
			(uint32_t(GIFAddr::ST) << 0) |
			(uint32_t(GIFAddr::XYZF2) << 4) |
			(uint32_t(GIFAddr::ST) << 8) |
			(uint32_t(GIFAddr::RGBAQ) << 12) |
			(uint32_t(GIFAddr::XYZF2) << 16);

	constexpr uint32_t STXYZFSTRGBAXYZ_Mask =
			(uint32_t(GIFAddr::ST) << 0) |
			(uint32_t(GIFAddr::XYZ2) << 4) |
			(uint32_t(GIFAddr::ST) << 8) |
			(uint32_t(GIFAddr::RGBAQ) << 12) |
			(uint32_t(GIFAddr::XYZ2) << 16);

	uint32_t nreg = gif_path.tag.NREG;

	if (nreg == 3 && (gif_path.tag.REGS & 0xfff) == STQRGBAXYZ2_Mask)
	{
		// STQRGBAXYZ2 - Super common STQ comes before RGBA since that's how you update Q correctly,
		// and obviously XYZ2 is the vert/draw kick, so it has to be last.
		hand = STQRGBAXYZHandlers[registers.prim.desc.PRIM];
	}
	else if (nreg == 3 && (gif_path.tag.REGS & 0xfff) == STQRGBAXYZF2_Mask)
	{
		// STQRGBAXYZF2 - Super common STQ comes before RGBA since that's how you update Q correctly,
		// and obviously XYZ2 is the vert/draw kick, so it has to be last.
		hand = STQRGBAXYZFHandlers[registers.prim.desc.PRIM];
	}
	else if (nreg == 3 && (gif_path.tag.REGS & 0xfff) == UVRGBAXYZ2_Mask)
	{
		hand = UVRGBAXYZHandlers[registers.prim.desc.PRIM];
	}
	else if (nreg == 3 && (gif_path.tag.REGS & 0xfff) == UVRGBAXYZF2_Mask)
	{
		hand = UVRGBAXYZFHandlers[registers.prim.desc.PRIM];
	}
	else if (nreg == 5 &&
	         (gif_path.tag.REGS & 0xfffff) == STXYZFSTRGBAXYZF_Mask &&
	         PRIMType(registers.prim.desc.PRIM) == PRIMType::Sprite)
	{
		// Makes sense for sprite rendering. No need to specify RGBA twice. Seen in Legaia 2.
		hand = &GSInterface::packed_STXYZSTRGBAXYZ_sprite<true>;
	}
	else if (nreg == 5 &&
	         (gif_path.tag.REGS & 0xfffff) == STXYZFSTRGBAXYZ_Mask &&
	         PRIMType(registers.prim.desc.PRIM) == PRIMType::Sprite)
	{
		// Makes sense for sprite rendering. No need to specify RGBA twice. Seen in Legaia 2.
		hand = &GSInterface::packed_STXYZSTRGBAXYZ_sprite<false>;
	}
	else if (nreg == 6 &&
	         (gif_path.tag.REGS & 0xffffffull) == STQRGBAXYZ2_LineList_Mask &&
	         PRIMType(registers.prim.desc.PRIM) == PRIMType::LineList)
	{
		// Makes sense for linelist.
		hand = &GSInterface::packed_STQRGBAXYZ<false, PRIMType::LineList, 2>;
	}
	else if (nreg == 6 &&
	         (gif_path.tag.REGS & 0xffffffull) == STQRGBAXYZF2_LineList_Mask &&
	         PRIMType(registers.prim.desc.PRIM) == PRIMType::LineList)
	{
		// Makes sense for linelist.
		hand = &GSInterface::packed_STQRGBAXYZ<true, PRIMType::LineList, 2>;
	}
	else if (nreg == 9 &&
	         (gif_path.tag.REGS & 0xfffffffffull) == STQRGBAXYZ2_TriList_Mask &&
	         PRIMType(registers.prim.desc.PRIM) == PRIMType::TriangleList)
	{
		// Makes sense for trilist.
		hand = &GSInterface::packed_STQRGBAXYZ<false, PRIMType::TriangleList, 3>;
	}
	else if (nreg == 9 &&
	         (gif_path.tag.REGS & 0xfffffffffull) == STQRGBAXYZF2_TriList_Mask &&
	         PRIMType(registers.prim.desc.PRIM) == PRIMType::TriangleList)
	{
		// Makes sense for trilist.
		hand = &GSInterface::packed_STQRGBAXYZ<true, PRIMType::TriangleList, 3>;
	}
	else
	{
		constexpr uint64_t ad_only_mask = uint64_t(GIFAddr::A_D) * 0x1111111111111111ull;
		uint64_t reg_mask = nreg == 0 ? UINT64_MAX : ((1ull << (gif_path.tag.NREG * 4)) - 1);
		if ((gif_path.tag.REGS & reg_mask) == (ad_only_mask & reg_mask))
			hand = ADONLYHandlers[gif_path.tag.NREG];
	}
}

void GSInterface::a_d_PRIM(uint64_t payload)
{
	Reg64<PRIMBits> prim(payload);
	bool prim_delta = registers.prim.desc.PRIM != prim.desc.PRIM;

	if (registers.prmodecont.desc.AC)
	{
		if (registers.prim.desc.CTXT != prim.desc.CTXT)
		{
			state_tracker.dirty_flags |= STATE_DIRTY_DEGENERATE_BIT |
			                             STATE_DIRTY_PRIM_TEMPLATE_BIT |
			                             STATE_DIRTY_TEX_BIT |
			                             STATE_DIRTY_FB_BIT |
			                             STATE_DIRTY_FEEDBACK_BIT;
		}

		update_internal_register(registers.prim.bits, payload,
		                         STATE_DIRTY_FEEDBACK_BIT |
		                         STATE_DIRTY_PRIM_TEMPLATE_BIT |
		                         STATE_DIRTY_TEX_BIT |
		                         STATE_DIRTY_STATE_BIT);

		if (!registers.prim.desc.TME)
			state_tracker.dirty_flags &= ~STATE_DIRTY_TEX_BIT;
	}
	else
		registers.prim.desc.PRIM = prim.desc.PRIM;

	if (prim_delta)
	{
		update_draw_handler();
		// If we're updating PRIM, optimized draw handler is either nullptr anyway,
		// or we're in ADONLY, in which case the optimized handler
		// does not care about PRIM register at all.
		// We don't really know (or should need to know) which GIFPath we're executing in here,
		// so don't try to be clever.
	}

	reset_vertex_queue();
	registers.internal_q = 1.0f;

	TRACE("PRIM", registers.prim);
}

void GSInterface::a_d_RGBAQ(uint64_t payload)
{
	registers.rgbaq.bits = payload;
	TRACE("RGBAQ", registers.rgbaq);
}

void GSInterface::a_d_RGBAQUndocumented(uint64_t payload)
{
	// Ridge Racer V.
	a_d_RGBAQ(payload);
}

void GSInterface::a_d_ST(uint64_t payload)
{
	registers.st.bits = payload;
	TRACE("ST", registers.st);
}

void GSInterface::a_d_UV(uint64_t payload)
{
	registers.uv.bits = payload;
	TRACE("UV", registers.uv);
}

void GSInterface::a_d_XYZF2(uint64_t payload)
{
	vertex_kick_xyzf(payload);
	drawing_kick(false);
}

void GSInterface::a_d_XYZ2(uint64_t payload)
{
	vertex_kick_xyz(payload);
	drawing_kick(false);
}

void GSInterface::a_d_TEX0_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].tex0.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_STATE_BIT |
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("TEX0_1", registers.ctx[0].tex0);
	handle_tex0_write(0);
	handle_miptbl_gen(0);
}

void GSInterface::a_d_TEX0_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].tex0.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_STATE_BIT |
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("TEX0_2", registers.ctx[1].tex0);
	handle_tex0_write(1);
	handle_miptbl_gen(1);
}

void GSInterface::a_d_CLAMP_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].clamp.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("CLAMP_1", registers.ctx[0].clamp);
}

void GSInterface::a_d_CLAMP_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].clamp.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("CLAMP_2", registers.ctx[1].clamp);
}

void GSInterface::a_d_FOG(uint64_t payload)
{
	registers.fog.bits = payload;
	TRACE("FOG", registers.fog);
}

void GSInterface::a_d_XYZF3(uint64_t payload)
{
	vertex_kick_xyzf(payload);
}

void GSInterface::a_d_XYZ3(uint64_t payload)
{
	vertex_kick_xyz(payload);
}

void GSInterface::a_d_TEX1_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].tex1.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("TEX1_1", registers.ctx[0].tex1);
}

void GSInterface::a_d_TEX1_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].tex1.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("TEX1_2", registers.ctx[1].tex1);
}

void GSInterface::a_d_TEX2_1(uint64_t payload) { a_d_TEX2<0>(payload); }
void GSInterface::a_d_TEX2_2(uint64_t payload) { a_d_TEX2<1>(payload); }
void GSInterface::a_d_XYOFFSET_1(uint64_t payload)
{
	registers.ctx[0].xyoffset.bits = payload;
	TRACE("XYOFFSET_1", registers.ctx[0].xyoffset);
}

void GSInterface::a_d_XYOFFSET_2(uint64_t payload)
{
	registers.ctx[1].xyoffset.bits = payload;
	TRACE("XYOFFSET_2", registers.ctx[1].xyoffset);
}

void GSInterface::a_d_PRMODECONT(uint64_t payload)
{
	registers.prmodecont.bits = payload;
	TRACE("PRMODECONT", registers.prmodecont);
}

void GSInterface::a_d_PRMODE(uint64_t payload)
{
	if (!registers.prmodecont.desc.AC)
	{
		Reg64<PRIMBits> prim{payload};
		prim.desc.PRIM = registers.prim.desc.PRIM;

		if (registers.prim.desc.CTXT != prim.desc.CTXT)
		{
			state_tracker.dirty_flags |= STATE_DIRTY_DEGENERATE_BIT |
			                             STATE_DIRTY_PRIM_TEMPLATE_BIT |
			                             STATE_DIRTY_TEX_BIT |
			                             STATE_DIRTY_FB_BIT |
			                             STATE_DIRTY_FEEDBACK_BIT;
		}

		update_internal_register(registers.prim.bits, prim.bits,
		                         STATE_DIRTY_FEEDBACK_BIT |
		                         STATE_DIRTY_PRIM_TEMPLATE_BIT |
		                         STATE_DIRTY_TEX_BIT |
		                         STATE_DIRTY_STATE_BIT);

		if (!registers.prim.desc.TME)
			state_tracker.dirty_flags &= ~STATE_DIRTY_TEX_BIT;

		TRACE("PRMODE", registers.prim);
	}
}
void GSInterface::a_d_TEXCLUT(uint64_t payload)
{
	registers.texclut.bits = payload;
	TRACE("TEXCLUT", registers.texclut);
}

void GSInterface::a_d_SCANMSK(uint64_t payload)
{
	update_internal_register(registers.scanmsk.bits, payload,
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("SCANMSK", registers.scanmsk);
}

void GSInterface::a_d_MIPTBP1_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].miptbl_1_3.bits, payload,
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("MIPTBP1_1", registers.ctx[0].miptbl_1_3);
}

void GSInterface::a_d_MIPTBP1_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].miptbl_1_3.bits, payload,
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("MIPTBP1_2", registers.ctx[1].miptbl_1_3);
}

void GSInterface::a_d_MIPTBP2_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].miptbl_4_6.bits, payload,
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("MIPTBP2_1", registers.ctx[0].miptbl_4_6);
}

void GSInterface::a_d_MIPTBP2_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].miptbl_4_6.bits, payload,
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("MIPTBP2_2", registers.ctx[1].miptbl_4_6);
}

void GSInterface::a_d_TEXA(uint64_t payload)
{
	update_internal_register(registers.texa.bits, payload,
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT | STATE_DIRTY_TEX_BIT);
	TRACE("TEXA", registers.texa);
}

void GSInterface::a_d_FOGCOL(uint64_t payload)
{
	registers.fogcol.bits = payload;
	TRACE("FOGCOL", registers.fogcol);
}

void GSInterface::a_d_TEXFLUSH(uint64_t)
{
	// We cannot rely on TEXFLUSH unfortunately.
	// We'll have to rely on our own tracking.
	TRACE("TEXFLUSH", Reg64<DummyBits>{0});
}

void GSInterface::a_d_SCISSOR_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].scissor.bits, payload, STATE_DIRTY_DEGENERATE_BIT);
	TRACE("SCISSOR_1", registers.ctx[0].scissor);
}

void GSInterface::a_d_SCISSOR_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].scissor.bits, payload, STATE_DIRTY_DEGENERATE_BIT);
	TRACE("SCISSOR_2", registers.ctx[1].scissor);
}

void GSInterface::a_d_ALPHA_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].alpha.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("ALPHA_1", registers.ctx[0].alpha);
}

void GSInterface::a_d_ALPHA_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].alpha.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("ALPHA_2", registers.ctx[1].alpha);
}

void GSInterface::a_d_DIMX(uint64_t payload)
{
	update_internal_register(registers.dimx.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("DIMX", registers.dimx);
}

void GSInterface::a_d_DTHE(uint64_t payload)
{
	update_internal_register(registers.dthe.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("DTHE", registers.dthe);
}

void GSInterface::a_d_COLCLAMP(uint64_t payload)
{
	update_internal_register(registers.colclamp.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("COLCLAMP", registers.colclamp);
}

void GSInterface::a_d_TEST_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].test.bits, payload,
	                         STATE_DIRTY_DEGENERATE_BIT |
	                         STATE_DIRTY_STATE_BIT |
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("TEST_1", registers.ctx[0].test);
}

void GSInterface::a_d_TEST_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].test.bits, payload,
	                         STATE_DIRTY_DEGENERATE_BIT |
	                         STATE_DIRTY_STATE_BIT |
	                         STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("TEST_2", registers.ctx[1].test);
}

void GSInterface::a_d_PABE(uint64_t payload)
{
	update_internal_register(registers.pabe.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("PABE", registers.pabe);
}

void GSInterface::a_d_FBA_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].fba.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("FBA_1", registers.ctx[0].fba);
}

void GSInterface::a_d_FBA_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].fba.bits, payload, STATE_DIRTY_STATE_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("FBA_2", registers.ctx[1].fba);
}

void GSInterface::update_internal_register(uint64_t &reg, uint64_t value, StateDirtyFlags flags)
{
	if (reg != value)
	{
		reg = value;
		TRACE("DIRTY", flags);
		state_tracker.dirty_flags |= flags;
	}
}

void GSInterface::a_d_FRAME_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].frame.bits, payload,
	                         STATE_DIRTY_DEGENERATE_BIT | STATE_DIRTY_FEEDBACK_BIT |
	                         STATE_DIRTY_FB_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("FRAME_1", registers.ctx[0].frame);
}

void GSInterface::a_d_FRAME_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].frame.bits, payload,
	                         STATE_DIRTY_DEGENERATE_BIT | STATE_DIRTY_FEEDBACK_BIT |
	                         STATE_DIRTY_FB_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("FRAME_2", registers.ctx[1].frame);
}

void GSInterface::a_d_ZBUF_1(uint64_t payload)
{
	update_internal_register(registers.ctx[0].zbuf.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_DEGENERATE_BIT |
	                         STATE_DIRTY_FB_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("ZBUF_1", registers.ctx[0].zbuf);
}

void GSInterface::a_d_ZBUF_2(uint64_t payload)
{
	update_internal_register(registers.ctx[1].zbuf.bits, payload,
	                         STATE_DIRTY_FEEDBACK_BIT | STATE_DIRTY_DEGENERATE_BIT |
	                         STATE_DIRTY_FB_BIT | STATE_DIRTY_PRIM_TEMPLATE_BIT);
	TRACE("ZBUF_2", registers.ctx[1].zbuf);
}

void GSInterface::a_d_BITBLTBUF(uint64_t payload)
{
	registers.bitbltbuf.bits = payload;
	TRACE("BITBLTBUF", registers.bitbltbuf);
}

void GSInterface::a_d_TRXPOS(uint64_t payload)
{
	registers.trxpos.bits = payload;
	TRACE("TRXPOS", registers.trxpos);
}

void GSInterface::a_d_TRXREG(uint64_t payload)
{
	registers.trxreg.bits = payload;
	TRACE("TRXREG", registers.trxreg);
}

void GSInterface::a_d_TRXDIR(uint64_t payload)
{
	registers.trxdir.bits = payload;
	TRACE("TRXDIR", registers.trxdir);
	init_transfer();
}

// Normally this is written by GIFTag + IMAGE, which effectively spams HWREG with data,
// but nothing stops application from writing HWREG on its own.
void GSInterface::a_d_HWREG(uint64_t payload)
{
	if (transfer_state.host_to_local_active)
	{
		transfer_state.host_to_local_payload.push_back(payload);
		// Flush out transfer if enough data has been received.
		check_pending_transfer();
	}
}

void GSInterface::a_d_HWREG_multi(const uint64_t *payload, size_t count)
{
	if (transfer_state.host_to_local_active)
	{
		transfer_state.host_to_local_payload.insert(transfer_state.host_to_local_payload.end(),
													payload, payload + count);
		// Flush out transfer if enough data has been received.
		check_pending_transfer();
	}
}

// For debugging?
void GSInterface::a_d_SIGNAL(uint64_t) {}
void GSInterface::a_d_FINISH(uint64_t) {}
void GSInterface::a_d_LABEL(uint64_t) {}

void GSInterface::reglist_nop(uint64_t) {}
void GSInterface::packed_nop(const void *) {}

template <GSInterface::RegListHandler Handler>
void GSInterface::packed_a_d_forward(const void *words)
{
	(this->*Handler)(*static_cast<const uint64_t *>(words));
}

void GSInterface::packed_RGBAQ(const void *words)
{
	auto &rgba = *static_cast<const PackedRGBAQBits *>(words);
	registers.rgbaq.desc.R = rgba.R;
	registers.rgbaq.desc.G = rgba.G;
	registers.rgbaq.desc.B = rgba.B;
	registers.rgbaq.desc.A = rgba.A;
	registers.rgbaq.desc.Q = registers.internal_q;

	TRACE("RGBAQ", registers.rgbaq);
}

void GSInterface::packed_ST(const void *words)
{
	auto &st = *static_cast<const PackedSTBits *>(words);
	registers.st.desc.S = st.S;
	registers.st.desc.T = st.T;
	registers.internal_q = st.Q;

	TRACE("ST", registers.st);
}

void GSInterface::packed_UV(const void *words)
{
	auto &uv = *static_cast<const PackedUVBits *>(words);
	registers.uv.desc.U = uv.U;
	registers.uv.desc.V = uv.V;
	TRACE("UV", registers.uv);
}

template <bool ADC>
void GSInterface::packed_XYZF(const void *words)
{
	auto &xyzf = *static_cast<const PackedXYZFBits *>(words);
	bool adc = ADC || xyzf.ADC;

	Reg64<XYZFBits> bits = {};
	bits.desc.X = xyzf.X;
	bits.desc.Y = xyzf.Y;
	bits.desc.Z = xyzf.Z;
	bits.desc.F = xyzf.F;
	vertex_kick_xyzf(bits);

	TRACE("ADC", adc);
	drawing_kick(adc);
}

template <bool ADC>
void GSInterface::packed_XYZ(const void *words)
{
	auto &xyz = *static_cast<const PackedXYZBits *>(words);
	bool adc = ADC || xyz.ADC;

	Reg64<XYZBits> bits = {};
	bits.desc.X = xyz.X;
	bits.desc.Y = xyz.Y;
	bits.desc.Z = xyz.Z;
	vertex_kick_xyz(bits);

	TRACE("ADC", adc);
	drawing_kick(adc);
}

template <bool ADC, bool FOG, PRIMType PRIM>
void GSInterface::packed_XYZ(const void *words)
{
	bool adc;

	if (FOG)
	{
		auto &xyzf = *static_cast<const PackedXYZFBits *>(words);
		Reg64<XYZFBits> bits = {};
		bits.desc.X = xyzf.X;
		bits.desc.Y = xyzf.Y;
		bits.desc.Z = xyzf.Z;
		bits.desc.F = xyzf.F;
		vertex_kick_xyzf(bits);
		adc = ADC || xyzf.ADC;
	}
	else
	{
		auto &xyz = *static_cast<const PackedXYZBits *>(words);
		Reg64<XYZBits> bits = {};
		bits.desc.X = xyz.X;
		bits.desc.Y = xyz.Y;
		bits.desc.Z = xyz.Z;
		vertex_kick_xyz(bits);
		adc = ADC || xyz.ADC;
	}

	TRACE("ADC", adc);
	drawing_kick<PRIM>(adc);
}

template <bool FOG, PRIMType PRIM, int factor>
void GSInterface::packed_STQRGBAXYZ(const void *words_, uint32_t num_vertices)
{
	const auto *words = static_cast<const uint8_t *>(words_);
	num_vertices *= factor;

	for (uint32_t i = 0; i < num_vertices; i++, words += 48)
	{
		packed_ST(words + 0);
		packed_RGBAQ(words + 16);
		packed_XYZ<false, FOG, PRIM>(words + 32);
	}
}

template <bool FOG, PRIMType PRIM, int factor>
void GSInterface::packed_UVRGBAXYZ(const void *words_, uint32_t num_vertices)
{
	const auto *words = static_cast<const uint8_t *>(words_);
	num_vertices *= factor;

	for (uint32_t i = 0; i < num_vertices; i++, words += 48)
	{
		packed_UV(words + 0);
		packed_RGBAQ(words + 16);
		packed_XYZ<false, FOG, PRIM>(words + 32);
	}
}

template <bool FOG>
void GSInterface::packed_STXYZSTRGBAXYZ_sprite(const void *words_, uint32_t num_sprites)
{
	const auto *words = static_cast<const uint8_t *>(words_);
	for (uint32_t i = 0; i < num_sprites; i++, words += 80)
	{
		packed_ST(words + 0);
		packed_XYZ<false, FOG, PRIMType::Sprite>(words + 16);
		packed_ST(words + 32);
		packed_RGBAQ(words + 48);
		packed_XYZ<false, FOG, PRIMType::Sprite>(words + 64);
	}
}

void GSInterface::packed_FOG(const void *words)
{
	auto &fog = *static_cast<const PackedFOGBits *>(words);
	registers.fog.desc.FOG = fog.F;
	TRACE("FOG", registers.fog);
}

void GSInterface::setup_handlers()
{
	for (auto &h : ad_handlers)
		h = &GSInterface::reglist_nop;
	for (auto &h : reglist_handlers)
		h = &GSInterface::reglist_nop;
	for (auto &h : packed_handlers)
		h = &GSInterface::packed_nop;

	draw_handler = &GSInterface::drawing_kick_invalid;

#define DECL_REG(reg, addr) ad_handlers[addr] = &GSInterface::a_d_##reg;
#include "gs_register_addr.hpp"
#undef DECL_REG

	reglist_handlers[int(GIFAddr::PRIM)] = &GSInterface::a_d_PRIM;
	reglist_handlers[int(GIFAddr::RGBAQ)] = &GSInterface::a_d_RGBAQ;
	reglist_handlers[int(GIFAddr::ST)] = &GSInterface::a_d_ST;
	reglist_handlers[int(GIFAddr::UV)] = &GSInterface::a_d_UV;
	reglist_handlers[int(GIFAddr::XYZF2)] = &GSInterface::a_d_XYZF2;
	reglist_handlers[int(GIFAddr::XYZ2)] = &GSInterface::a_d_XYZ2;
	reglist_handlers[int(GIFAddr::TEX0_1)] = &GSInterface::a_d_TEX0_1;
	reglist_handlers[int(GIFAddr::TEX0_2)] = &GSInterface::a_d_TEX0_2;
	reglist_handlers[int(GIFAddr::CLAMP_1)] = &GSInterface::a_d_CLAMP_1;
	reglist_handlers[int(GIFAddr::CLAMP_2)] = &GSInterface::a_d_CLAMP_2;
	reglist_handlers[int(GIFAddr::FOG)] = &GSInterface::a_d_FOG;
	reglist_handlers[int(GIFAddr::XYZF3)] = &GSInterface::a_d_XYZF3;
	reglist_handlers[int(GIFAddr::XYZ3)] = &GSInterface::a_d_XYZ3;

	packed_handlers[int(GIFAddr::PRIM)] = &GSInterface::packed_a_d_forward<&GSInterface::a_d_PRIM>;
	packed_handlers[int(GIFAddr::RGBAQ)] = &GSInterface::packed_RGBAQ;
	packed_handlers[int(GIFAddr::ST)] = &GSInterface::packed_ST;
	packed_handlers[int(GIFAddr::UV)] = &GSInterface::packed_UV;
	packed_handlers[int(GIFAddr::TEX0_1)] = &GSInterface::packed_a_d_forward<&GSInterface::a_d_TEX0_1>;
	packed_handlers[int(GIFAddr::TEX0_2)] = &GSInterface::packed_a_d_forward<&GSInterface::a_d_TEX0_2>;
	packed_handlers[int(GIFAddr::CLAMP_1)] = &GSInterface::packed_a_d_forward<&GSInterface::a_d_CLAMP_1>;
	packed_handlers[int(GIFAddr::CLAMP_2)] = &GSInterface::packed_a_d_forward<&GSInterface::a_d_CLAMP_2>;
	packed_handlers[int(GIFAddr::FOG)] = &GSInterface::packed_FOG;
	packed_handlers[int(GIFAddr::XYZF2)] = &GSInterface::packed_XYZF<false>;
	packed_handlers[int(GIFAddr::XYZ2)] = &GSInterface::packed_XYZ<false>;
	packed_handlers[int(GIFAddr::XYZF3)] = &GSInterface::packed_XYZF<true>;
	packed_handlers[int(GIFAddr::XYZ3)] = &GSInterface::packed_XYZ<true>;
}

void *GSInterface::map_vram_write(size_t offset, size_t size)
{
	if (!size)
		return nullptr;

	size_t begin_page = offset / PageSize;
	size_t end_page = (offset + size - 1) / PageSize;

	PageRect page_rect = {};
	page_rect.base_page = begin_page;
	page_rect.page_width = end_page - begin_page + 1;
	page_rect.page_height = 1;

	uint64_t host_write_timeline = tracker.get_host_write_timeline(page_rect);
	if (host_write_timeline == UINT64_MAX)
	{
		host_write_timeline = tracker.mark_submission_timeline();
		renderer.flush_submit(host_write_timeline);
	}

	renderer.wait_timeline(host_write_timeline);

	return static_cast<uint8_t *>(renderer.begin_host_vram_access()) + offset;
}

void GSInterface::end_vram_write(size_t offset, size_t size)
{
	if (!size)
		return;

	size_t begin_page = offset / PageSize;
	size_t end_page = (offset + size - 1) / PageSize;

	PageRect page_rect = {};
	page_rect.base_page = begin_page;
	page_rect.page_width = end_page - begin_page + 1;
	page_rect.page_height = 1;

	renderer.end_host_write_vram_access();
	tracker.commit_host_write(page_rect);
}

const void *GSInterface::map_vram_read(size_t offset, size_t size)
{
	if (!size)
		return nullptr;

	size_t begin_page = offset / PageSize;
	size_t end_page = (offset + size - 1) / PageSize;

	PageRect page_rect = {};
	page_rect.base_page = begin_page;
	page_rect.page_width = end_page - begin_page + 1;
	page_rect.page_height = 1;

	uint64_t host_read_timeline = tracker.get_host_read_timeline(page_rect);
	if (host_read_timeline == UINT64_MAX)
	{
		host_read_timeline = tracker.mark_submission_timeline();
		renderer.flush_submit(host_read_timeline);
	}

	renderer.wait_timeline(host_read_timeline);

	return static_cast<const uint8_t *>(renderer.begin_host_vram_access()) + offset;
}

void GSInterface::flush()
{
	flush_pending_transfer(true);
	renderer.flush_submit(tracker.mark_submission_timeline());
}

void GSInterface::clobber_register_state()
{
	state_tracker.dirty_flags = STATE_DIRTY_ALL_BITS;
	update_draw_handler();
	// We don't know which path will start executing so we cannot infer anything from pending GIFTags.
	// Defer until we receive a fresh GIFTag header.
	for (uint32_t i = 0; i < 4; i++)
		update_optimized_gif_handler(i);
}

void GSInterface::write_register(RegisterAddr addr, uint64_t payload)
{
	(this->*ad_handlers[int(addr)])(payload);
}

template <int count>
void GSInterface::packed_ADONLY(const void *words, uint32_t num_loops)
{
	auto *ad = static_cast<const Reg128<PackedADBits> *>(words);
	for (uint32_t i = 0; i < num_loops; i++)
		for (int j = 0; j < count; j++, ad++)
			write_register(RegisterAddr(ad->desc.ADDR), ad->desc.data);
}

void GSInterface::gif_transfer(uint32_t path_index, const void *data, size_t size)
{
	// Transfers are in units of 128 bits.
	assert(path_index < 4);
	assert(size % 16 == 0);
	size /= 16;
	auto &path = paths[path_index];

	if (size == 0)
		return;

	const auto *qwords = static_cast<const GIFTagBits *>(data);
	const auto *word64 = static_cast<const uint64_t *>(data);

	// This can be optimized a lot, but keep it simple for now.

	uint32_t nreg = path.tag.NREG == 0 ? 16 : path.tag.NREG;

	for (size_t i = 0; i < size; )
	{
		bool needs_gif_tag = path.loop == path.tag.NLOOP;
		if (needs_gif_tag)
		{
			path.tag = qwords[i];
			TRACE_HEADER("GIFTag", path.tag);
			if (path.tag.FLG == GIFTagBits::PACKED && path.tag.PRE != 0)
			{
				// Set PRIM register.
				a_d_PRIM(path.tag.PRIM);
			}

			update_optimized_gif_handler(path_index);

			path.loop = 0;
			path.reg = 0;
			i++;
			nreg = path.tag.NREG == 0 ? 16 : path.tag.NREG;
		}
		else
		{
			if (path.reg == 0 && optimized_draw_handler[path_index])
			{
				// Should this divide be optimized to use divide by constant trick?
				uint32_t nloops_to_run = std::min<uint32_t>(size / nreg, path.tag.NLOOP - path.loop);
				(this->*optimized_draw_handler[path_index])(&qwords[i], nloops_to_run);
				i += nloops_to_run * nreg;
				path.loop += nloops_to_run;
			}
			else if (path.tag.FLG == GIFTagBits::PACKED)
			{
				auto addr = uint32_t(path.tag.REGS >> (4 * path.reg)) & 0xf;
				path.reg++;

				if (GIFAddr(addr) == GIFAddr::A_D)
				{
					auto *ad = reinterpret_cast<const Reg128<PackedADBits> *>(&qwords[i]);
					write_register(RegisterAddr(ad->desc.ADDR), ad->desc.data);
				}
				else
					(this->*packed_handlers[addr])(&qwords[i]);

				i++;

				bool end_of_loop = path.reg == nreg;
				if (end_of_loop)
				{
					path.loop++;
					path.reg = 0;
				}
			}
			else if (path.tag.FLG == GIFTagBits::REGLIST)
			{
				// Number of 128-bit words is ceil(NLOOP * NREG / 2).
				// Loops can be tightly packed if NREG is odd.

				for (uint32_t j = 0; j < 2; j++)
				{
					auto addr = uint32_t(path.tag.REGS >> (4 * path.reg)) & 0xf;
					path.reg++;
					(this->*reglist_handlers[addr])(word64[2 * i + j]);

					bool end_of_loop = path.reg == nreg;
					if (end_of_loop)
					{
						path.loop++;
						path.reg = 0;
						if (path.loop == path.tag.NLOOP)
							break;
					}
				}

				i++;
			}
			else
			{
				// IMAGE
				// Spam HWREG.
				auto num_loops = std::min<size_t>(size - i, path.tag.NLOOP - path.loop);
				a_d_HWREG_multi(word64 + 2 * i, num_loops * 2);
				i += num_loops;
				path.loop += num_loops;
			}
		}
	}
}

RegisterState &GSInterface::get_register_state()
{
	return registers;
}

const RegisterState &GSInterface::get_register_state() const
{
	return registers;
}

PrivRegisterState &GSInterface::get_priv_register_state()
{
	return priv_registers;
}

const PrivRegisterState &GSInterface::get_priv_register_state() const
{
	return priv_registers;
}

GIFPath &GSInterface::get_gif_path(uint32_t path)
{
	return paths[path];
}

const GIFPath &GSInterface::get_gif_path(uint32_t path) const
{
	return paths[path];
}

void GSInterface::set_debug_mode(const DebugMode &mode)
{
	debug_mode = mode;
}

ScanoutResult GSInterface::vsync(const VSyncInfo &info)
{
	return renderer.vsync(priv_registers, info);
}

FlushStats GSInterface::consume_flush_stats()
{
	return renderer.consume_flush_stats();
}

double GSInterface::get_accumulated_timestamps(TimestampType type) const
{
	return renderer.get_accumulated_timestamps(type);
}
}