/*
 * VA drops HEVC's original SPS reference-picture sets. Rebuild parameter sets
 * and put the effective DPB and reference lists into each independent slice.
 * The remaining slice header (including WPP/tile entry points) and the CABAC
 * payload stay intact.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

struct reader {
	const uint8_t *data;
	size_t size, bit;
	int error;
};

struct writer {
	uint8_t *data;
	size_t capacity, bit;
	int error;
};

struct bytes {
	uint8_t *data;
	size_t size, capacity;
};

static unsigned log_bits(unsigned value)
{
	unsigned bits = 0;
	while (value > 1) {
		value = (value + 1) / 2;
		bits++;
	}
	return bits;
}

static uint32_t get(struct reader *r, unsigned count)
{
	uint32_t value = 0;
	if (count > 32 || r->bit > r->size * 8 || count > r->size * 8 - r->bit) {
		r->error = 1;
		return 0;
	}
	while (count--) {
		value = (value << 1) | ((r->data[r->bit / 8] >> (7 - r->bit % 8)) & 1);
		r->bit++;
	}
	return value;
}

static uint32_t get_ue(struct reader *r)
{
	unsigned zeros = 0;
	while (!get(r, 1) && !r->error)
		if (++zeros == 32) {
			r->error = 1;
			return 0;
		}
	return ((1u << zeros) - 1) + get(r, zeros);
}

static void skip(struct reader *r, size_t count)
{
	if (r->bit > r->size * 8 || count > r->size * 8 - r->bit)
		r->error = 1;
	else
		r->bit += count;
}

static void put(struct writer *w, unsigned count, uint32_t value)
{
	if (count > 32 || w->bit > w->capacity * 8 || count > w->capacity * 8 - w->bit) {
		w->error = 1;
		return;
	}
	while (count--) {
		if ((value >> count) & 1)
			w->data[w->bit / 8] |= 0x80 >> (w->bit % 8);
		w->bit++;
	}
}

static void put_ue(struct writer *w, uint32_t value)
{
	if (value == UINT32_MAX) {
		w->error = 1;
		return;
	}
	uint32_t code = value + 1;
	unsigned bits = 0;
	for (uint32_t n = code; n; n >>= 1)
		bits++;
	put(w, bits - 1, 0);
	put(w, bits, code);
}

static void put_se(struct writer *w, int32_t value)
{
	int64_t code = value > 0 ? 2ll * value - 1 : -2ll * value;
	if (code >= UINT32_MAX)
		w->error = 1;
	else
		put_ue(w, (uint32_t)code);
}

static void copy_bits(struct writer *w, const uint8_t *data, size_t begin, size_t end)
{
	for (size_t i = begin; i < end && !w->error; i++)
		put(w, 1, (data[i / 8] >> (7 - i % 8)) & 1);
}

static void align_bits(struct writer *w)
{
	put(w, 1, 1);
	while (w->bit % 8 && !w->error)
		put(w, 1, 0);
}

static int append(struct bytes *b, const void *data, size_t size)
{
	if (size > SIZE_MAX - b->size)
		return -1;
	size_t needed = b->size + size;
	if (needed > b->capacity) {
		size_t capacity = needed < SIZE_MAX / 2 ? needed * 2 : needed;
		void *next = realloc(b->data, capacity);
		if (!next)
			return -1;
		b->data = next;
		b->capacity = capacity;
	}
	memcpy(b->data + b->size, data, size);
	b->size += size;
	return 0;
}

static int encode_nal(struct bytes *b, const uint8_t header[2],
		      const uint8_t *payload, size_t size)
{
	static const uint8_t start[] = {0, 0, 0, 1};
	if (append(b, start, sizeof(start)) || append(b, header, 2))
		return -1;
	unsigned zeros = 0;
	for (size_t i = 0; i < size; i++) {
		uint8_t value = payload[i];
		if (zeros >= 2 && value <= 3) {
			static const uint8_t escape = 3;
			if (append(b, &escape, 1))
				return -1;
			zeros = 0;
		}
		if (append(b, &value, 1))
			return -1;
		zeros = value ? 0 : zeros + 1;
	}
	if (zeros >= 2) {
		static const uint8_t escape = 3;
		if (append(b, &escape, 1))
			return -1;
	}
	return 0;
}

static uint8_t *unescape(const uint8_t *data, size_t *size)
{
	uint8_t *raw = malloc(*size);
	if (!raw)
		return NULL;
	size_t output = 0;
	unsigned zeros = 0;
	for (size_t i = 0; i < *size; i++) {
		uint8_t value = data[i];
		if (zeros >= 2 && value == 3) {
			if (i + 1 < *size && data[i + 1] > 3) {
				free(raw);
				return NULL;
			}
			zeros = 0;
			continue;
		}
		raw[output++] = value;
		zeros = value ? 0 : zeros + 1;
	}
	*size = output;
	return raw;
}

int hevc_validate(const struct context *c)
{
	const VAPictureParameterBufferHEVC *p = &c->hevc;
	return !p->pic_width_in_luma_samples || p->pic_width_in_luma_samples > 8192 ||
		!p->pic_height_in_luma_samples || p->pic_height_in_luma_samples > 8192 ||
		p->pic_fields.bits.chroma_format_idc != 1 ||
		p->pic_fields.bits.separate_colour_plane_flag ||
		(p->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) ||
		(p->bit_depth_luma_minus8 != 0 && p->bit_depth_luma_minus8 != 2) ||
		p->bit_depth_luma_minus8 != p->bit_depth_chroma_minus8 ||
		(c->cfg.profile == VAProfileHEVCMain && p->bit_depth_luma_minus8) ||
		p->log2_min_luma_coding_block_size_minus3 > 3 ||
		p->log2_diff_max_min_luma_coding_block_size >
			3 - p->log2_min_luma_coding_block_size_minus3 ||
		p->log2_min_transform_block_size_minus2 > 3 ||
		p->log2_diff_max_min_transform_block_size >
			3 - p->log2_min_transform_block_size_minus2 ||
		p->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
		p->sps_max_dec_pic_buffering_minus1 > 15 ||
		p->num_short_term_ref_pic_sets > 64 || p->num_long_term_ref_pic_sps > 32 ||
		p->num_ref_idx_l0_default_active_minus1 > 14 ||
		p->num_ref_idx_l1_default_active_minus1 > 14 ||
		p->num_extra_slice_header_bits > 7 ||
		p->num_tile_columns_minus1 > 19 || p->num_tile_rows_minus1 > 21;
}

struct refs {
	uint8_t before[15], after[15], lt[15];
	unsigned nb, na, nl, used;
	uint32_t cycles[15];
	uint8_t msb_present[15];
};

static int used(const VAPictureHEVC *p)
{
	return !!(p->flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
			     VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR));
}

static int collect_refs(const VAPictureParameterBufferHEVC *p, struct refs *r)
{
	memset(r, 0, sizeof(*r));
	int64_t modulus = 1ll << (p->log2_max_pic_order_cnt_lsb_minus4 + 4);
	int64_t current_msb = p->CurrPic.pic_order_cnt & ~(modulus - 1);
	for (unsigned i = 0; i < 15; i++) {
		const VAPictureHEVC *ref = &p->ReferenceFrames[i];
		if (ref->picture_id == VA_INVALID_ID || (ref->flags & VA_PICTURE_HEVC_INVALID))
			continue;
		if ((ref->flags & VA_PICTURE_HEVC_FIELD_PIC) ||
		    ref->pic_order_cnt == p->CurrPic.pic_order_cnt)
			return -1;
		r->used += used(ref);
		if (ref->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE) {
			int64_t msb = ref->pic_order_cnt & ~(modulus - 1);
			int64_t cycles = (current_msb - msb) / modulus;
			if (cycles >= UINT32_MAX)
				return -1;
			if (cycles < 0) {
				for (unsigned k = 0; k < 15; k++) {
					const VAPictureHEVC *other = &p->ReferenceFrames[k];
					if (k != i && other->picture_id != VA_INVALID_ID &&
					    !(other->flags & VA_PICTURE_HEVC_INVALID) &&
					    !((ref->pic_order_cnt ^ other->pic_order_cnt) & (modulus - 1)))
						return -1;
				}
			} else {
				r->cycles[i] = (uint32_t)cycles;
				r->msb_present[i] = 1;
			}
			unsigned j = r->nl++;
			while (j && r->cycles[r->lt[j - 1]] > r->cycles[i]) {
				r->lt[j] = r->lt[j - 1];
				j--;
			}
			r->lt[j] = (uint8_t)i;
		} else {
			int before = ref->pic_order_cnt < p->CurrPic.pic_order_cnt;
			uint8_t *list = before ? r->before : r->after;
			unsigned j = before ? r->nb++ : r->na++;
			while (j && (before ?
			       p->ReferenceFrames[list[j - 1]].pic_order_cnt < ref->pic_order_cnt :
			       p->ReferenceFrames[list[j - 1]].pic_order_cnt > ref->pic_order_cnt)) {
				list[j] = list[j - 1];
				j--;
			}
			list[j] = (uint8_t)i;
		}
	}
	return 0;
}

static void write_refs(struct writer *w, const VAPictureParameterBufferHEVC *p,
		       const struct refs *r)
{
	put(w, 1, 0); /* short_term_ref_pic_set_sps_flag */
	put_ue(w, r->nb);
	put_ue(w, r->na);
	for (unsigned list = 0; list < 2; list++) {
		int64_t previous = p->CurrPic.pic_order_cnt;
		unsigned count = list ? r->na : r->nb;
		const uint8_t *indices = list ? r->after : r->before;
		for (unsigned i = 0; i < count; i++) {
			const VAPictureHEVC *ref = &p->ReferenceFrames[indices[i]];
			int64_t delta = list ? ref->pic_order_cnt - previous :
					      previous - ref->pic_order_cnt;
			if (delta <= 0 || delta > UINT32_MAX) {
				w->error = 1;
				return;
			}
			put_ue(w, (uint32_t)(delta - 1));
			put(w, 1, used(ref));
			previous = ref->pic_order_cnt;
		}
	}
	put_ue(w, r->nl);
	uint32_t previous = 0;
	unsigned bits = p->log2_max_pic_order_cnt_lsb_minus4 + 4;
	for (unsigned i = 0; i < r->nl; i++) {
		unsigned index = r->lt[i];
		put(w, bits, (uint32_t)p->ReferenceFrames[index].pic_order_cnt);
		put(w, 1, used(&p->ReferenceFrames[index]));
		put(w, 1, r->msb_present[index]);
		if (r->msb_present[index])
			put_ue(w, r->cycles[index] - previous);
		previous = r->cycles[index];
	}
}

static void write_lists(struct writer *w, const VAPictureParameterBufferHEVC *p,
			const VASliceParameterBufferHEVC *s, const struct refs *r,
			unsigned slice_type, const unsigned active[2])
{
	for (unsigned list = 0; list < (slice_type ? 1u : 2u); list++) {
		uint8_t indices[15];
		unsigned count = 0;
		for (unsigned group = 0; group < 3; group++) {
			const uint8_t *source = group == 2 ? r->lt :
				(group == list ? r->before : r->after);
			unsigned size = group == 2 ? r->nl : group == list ? r->nb : r->na;
			for (unsigned i = 0; i < size; i++)
				if (used(&p->ReferenceFrames[source[i]]))
					indices[count++] = source[i];
		}
		if (!count || count != r->used) {
			w->error = 1;
			return;
		}
		if (count > 1)
			put(w, 1, 1);
		for (unsigned i = 0; i < active[list]; i++) {
			unsigned index = 0;
			while (index < count && indices[index] != s->RefPicList[list][i])
				index++;
			if (index == count) {
				w->error = 1;
				return;
			}
			if (count > 1)
				put(w, log_bits(count), index);
		}
	}
}

static int rewrite_slice(struct context *c, const VASliceParameterBufferHEVC *s,
			 const uint8_t *nal, size_t size, struct bytes *output, unsigned *pps)
{
	const VAPictureParameterBufferHEVC *p = &c->hevc;
	if (size < 3 || (nal[0] & 0x81) || (nal[1] >> 3) || !(nal[1] & 7) ||
	    (nal[0] >> 1) > 31 || s->slice_data_flag != VA_SLICE_DATA_FLAG_ALL)
		return -1;
	unsigned type = nal[0] >> 1;
	size_t raw_size = size;
	uint8_t *raw = unescape(nal, &raw_size);
	if (!raw)
		return -1;
	struct reader br = {.data = raw, .size = raw_size, .bit = 16};
	unsigned first = get(&br, 1);
	if (type >= 16 && type <= 23)
		get(&br, 1);
	*pps = get_ue(&br);
	unsigned dependent = 0;
	if (!first) {
		if (p->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
			dependent = get(&br, 1);
		unsigned block = 1u << (p->log2_min_luma_coding_block_size_minus3 + 3 +
					p->log2_diff_max_min_luma_coding_block_size);
		unsigned blocks = ((p->pic_width_in_luma_samples + block - 1) / block) *
			((p->pic_height_in_luma_samples + block - 1) / block);
		get(&br, log_bits(blocks));
	}
	if (br.error || *pps > 63)
		goto fail;
	if (type >= 16 && type <= 21)
		c->idr = 1;
	if (dependent || type == 19 || type == 20) {
		int status = encode_nal(output, nal, raw + 2, raw_size - 2);
		free(raw);
		return status;
	}
	skip(&br, p->num_extra_slice_header_bits);
	unsigned slice_type = get_ue(&br);
	if (slice_type > 2)
		goto fail;
	if (p->slice_parsing_fields.bits.output_flag_present_flag)
		get(&br, 1);
	skip(&br, p->log2_max_pic_order_cnt_lsb_minus4 + 4);
	size_t rps_begin = br.bit;
	if (get(&br, 1)) {
		if (!p->num_short_term_ref_pic_sets)
			goto fail;
		if (get(&br, log_bits(p->num_short_term_ref_pic_sets)) >=
		    p->num_short_term_ref_pic_sets)
			goto fail;
	} else {
		if (!p->st_rps_bits)
			goto fail;
		skip(&br, p->st_rps_bits);
	}
	if (p->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
		unsigned nsps = p->num_long_term_ref_pic_sps ? get_ue(&br) : 0;
		unsigned count = get_ue(&br);
		if (nsps > p->num_long_term_ref_pic_sps || count > 15 || nsps > 15 - count)
			goto fail;
		for (unsigned i = 0; i < nsps + count; i++) {
			if (i < nsps) {
				if (get(&br, log_bits(p->num_long_term_ref_pic_sps)) >=
				    p->num_long_term_ref_pic_sps)
					goto fail;
			} else {
				skip(&br, p->log2_max_pic_order_cnt_lsb_minus4 + 5);
			}
			if (get(&br, 1))
				get_ue(&br);
		}
	}
	size_t rps_end = br.bit;
	struct refs refs;
	if (collect_refs(p, &refs))
		goto fail;
	unsigned active[2] = {p->num_ref_idx_l0_default_active_minus1 + 1u,
			      p->num_ref_idx_l1_default_active_minus1 + 1u};
	size_t lists_begin = rps_end, lists_end = rps_end;
	if (slice_type != 2) {
		if (p->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
			get(&br, 1);
		if (p->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag)
			skip(&br, 2);
		if (get(&br, 1)) {
			unsigned l0 = get_ue(&br);
			unsigned l1 = slice_type == 0 ? get_ue(&br) : 0;
			if (l0 > 14 || l1 > 14)
				goto fail;
			active[0] = l0 + 1;
			active[1] = l1 + 1;
		}
		lists_begin = br.bit;
		if (p->slice_parsing_fields.bits.lists_modification_present_flag && refs.used > 1)
			for (unsigned list = 0; list < (slice_type ? 1u : 2u); list++)
				if (get(&br, 1))
					for (unsigned i = 0; i < active[list]; i++)
						if (get(&br, log_bits(refs.used)) >= refs.used)
							goto fail;
		lists_end = br.bit;
	}
	size_t offset = s->slice_data_byte_offset;
	if (br.error || offset < 3 || offset > raw_size || !raw[offset - 1])
		goto fail;
	unsigned alignment = 1;
	for (uint8_t last = raw[offset - 1]; !(last & 1); last >>= 1)
		alignment++;
	size_t header_end = offset * 8 - alignment;
	if (lists_end > header_end || offset > SIZE_MAX - 256)
		goto fail;
	struct writer bw = {.capacity = offset + 256};
	bw.data = calloc(1, bw.capacity);
	if (!bw.data)
		goto fail;
	copy_bits(&bw, raw, 16, rps_begin);
	write_refs(&bw, p, &refs);
	copy_bits(&bw, raw, rps_end, lists_begin);
	if (slice_type != 2)
		write_lists(&bw, p, s, &refs, slice_type, active);
	copy_bits(&bw, raw, lists_end, header_end);
	align_bits(&bw);
	struct bytes payload = {0};
	int status = bw.error || append(&payload, bw.data, bw.bit / 8) ||
		append(&payload, raw + offset, raw_size - offset) ||
		encode_nal(output, nal, payload.data, payload.size);
	free(payload.data);
	free(bw.data);
	free(raw);
	return status ? -1 : 0;
fail:
	free(raw);
	return -1;
}

int hevc_add_slices(struct context *c, const struct buffer *data)
{
	if (c->picture_failed || !c->hevc_nslices || hevc_validate(c)) {
		c->picture_failed = 1;
		c->nal_len = 0;
		return -1;
	}
	struct bytes output = {.data = c->nal, .size = c->nal_len, .capacity = c->nal_cap};
	int status = 0;
	for (unsigned i = 0; i < c->hevc_nslices; i++) {
		const VASliceParameterBufferHEVC *s = &c->hevc_slices[i];
		size_t total = (size_t)data->size * data->num;
		if (s->slice_data_offset > total || s->slice_data_size > total - s->slice_data_offset) {
			status = -1;
			break;
		}
		const uint8_t *nal = data->data + s->slice_data_offset;
		size_t size = s->slice_data_size;
		if (size > 4 && !nal[0] && !nal[1] && !nal[2] && nal[3] == 1)
			nal += 4, size -= 4;
		else if (size > 3 && !nal[0] && !nal[1] && nal[2] == 1)
			nal += 3, size -= 3;
		unsigned pps = 0;
		if (rewrite_slice(c, s, nal, size, &output, &pps)) {
			status = -1;
			break;
		}
		c->pps_used[pps / 8] |= 1u << (pps % 8);
	}
	c->nal = output.data;
	c->nal_len = status ? 0 : output.size;
	c->nal_cap = output.capacity;
	c->hevc_nslices = 0;
	if (status)
		c->picture_failed = 1;
	return status;
}

static void profile_tier_level(struct writer *w, const struct context *c)
{
	unsigned profile = c->cfg.profile == VAProfileHEVCMain10 ? 2 : 1;
	put(w, 2, 0);
	put(w, 1, 0);
	put(w, 5, profile);
	put(w, 32, 1u << (31 - profile));
	put(w, 4, 9); /* progressive_source, frame_only */
	put(w, 32, 0);
	put(w, 12, 0);
	put(w, 8, (uint32_t)c->width * c->height > 8912896 ? 186 : 156);
	for (unsigned i = 0; i < 6; i++)
		put(w, 2, 0);
	put(w, 4, 0);
}

static void ordering(struct writer *w, const struct context *c)
{
	put(w, 1, 1);
	for (unsigned i = 0; i < 7; i++) {
		put_ue(w, c->hevc.sps_max_dec_pic_buffering_minus1);
		put_ue(w, c->hevc.sps_max_dec_pic_buffering_minus1);
		put_ue(w, 0);
	}
}

static void write_vps(struct writer *w, const struct context *c)
{
	put(w, 4, 0);
	put(w, 2, 3);
	put(w, 6, 0);
	put(w, 3, 6);
	put(w, 1, 1);
	put(w, 16, 0xffff);
	profile_tier_level(w, c);
	ordering(w, c);
	put(w, 6, 0);
	put_ue(w, 0);
	put(w, 1, 0);
	put(w, 1, 0);
}

static void write_sps(struct writer *w, const struct context *c)
{
	const VAPictureParameterBufferHEVC *p = &c->hevc;
	put(w, 4, 0);
	put(w, 3, 6);
	put(w, 1, 1);
	profile_tier_level(w, c);
	put_ue(w, 0);
	put_ue(w, 1);
	put_ue(w, p->pic_width_in_luma_samples);
	put_ue(w, p->pic_height_in_luma_samples);
	put(w, 1, 0);
	put_ue(w, p->bit_depth_luma_minus8);
	put_ue(w, p->bit_depth_chroma_minus8);
	put_ue(w, p->log2_max_pic_order_cnt_lsb_minus4);
	ordering(w, c);
	put_ue(w, p->log2_min_luma_coding_block_size_minus3);
	put_ue(w, p->log2_diff_max_min_luma_coding_block_size);
	put_ue(w, p->log2_min_transform_block_size_minus2);
	put_ue(w, p->log2_diff_max_min_transform_block_size);
	put_ue(w, p->max_transform_hierarchy_depth_inter);
	put_ue(w, p->max_transform_hierarchy_depth_intra);
	put(w, 1, p->pic_fields.bits.scaling_list_enabled_flag);
	if (p->pic_fields.bits.scaling_list_enabled_flag)
		put(w, 1, 0);
	put(w, 1, p->pic_fields.bits.amp_enabled_flag);
	put(w, 1, p->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
	put(w, 1, p->pic_fields.bits.pcm_enabled_flag);
	if (p->pic_fields.bits.pcm_enabled_flag) {
		put(w, 4, p->pcm_sample_bit_depth_luma_minus1);
		put(w, 4, p->pcm_sample_bit_depth_chroma_minus1);
		put_ue(w, p->log2_min_pcm_luma_coding_block_size_minus3);
		put_ue(w, p->log2_diff_max_min_pcm_luma_coding_block_size);
		put(w, 1, p->pic_fields.bits.pcm_loop_filter_disabled_flag);
	}
	put_ue(w, 0); /* num_short_term_ref_pic_sets */
	put(w, 1, 1); /* long_term_ref_pics_present_flag */
	put_ue(w, 0); /* num_long_term_ref_pics_sps */
	put(w, 1, p->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
	put(w, 1, p->pic_fields.bits.strong_intra_smoothing_enabled_flag);
	put(w, 1, 0);
	put(w, 1, 0);
}

static void scaling_lists(struct writer *w, const VAIQMatrixBufferHEVC *iq)
{
	for (unsigned size = 0; size < 4; size++) {
		for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
			const uint8_t *list = size == 0 ? iq->ScalingList4x4[matrix] :
				size == 1 ? iq->ScalingList8x8[matrix] :
				size == 2 ? iq->ScalingList16x16[matrix] :
					    iq->ScalingList32x32[matrix / 3];
			put(w, 1, 1);
			int previous = 8;
			if (size > 1) {
				previous = size == 2 ? iq->ScalingListDC16x16[matrix] :
						      iq->ScalingListDC32x32[matrix / 3];
				if (!previous)
					w->error = 1;
				put_se(w, previous - 8);
			}
			unsigned side = size ? 8 : 4;
			for (unsigned diagonal = 0; diagonal < side * 2 - 1; diagonal++) {
				for (int y = (int)(diagonal < side ? diagonal : side - 1); y >= 0; y--) {
					unsigned x = diagonal - (unsigned)y;
					if (x >= side)
						continue;
					int value = list[(unsigned)y * side + x];
					int delta = value - previous;
					if (delta > 127)
						delta -= 256;
					if (delta < -128)
						delta += 256;
					put_se(w, delta);
					previous = value;
				}
			}
		}
	}
}

static void write_pps(struct writer *w, const struct context *c, unsigned id)
{
	const VAPictureParameterBufferHEVC *p = &c->hevc;
	put_ue(w, id);
	put_ue(w, 0);
	put(w, 1, p->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag);
	put(w, 1, p->slice_parsing_fields.bits.output_flag_present_flag);
	put(w, 3, p->num_extra_slice_header_bits);
	put(w, 1, p->pic_fields.bits.sign_data_hiding_enabled_flag);
	put(w, 1, p->slice_parsing_fields.bits.cabac_init_present_flag);
	put_ue(w, p->num_ref_idx_l0_default_active_minus1);
	put_ue(w, p->num_ref_idx_l1_default_active_minus1);
	put_se(w, p->init_qp_minus26);
	put(w, 1, p->pic_fields.bits.constrained_intra_pred_flag);
	put(w, 1, p->pic_fields.bits.transform_skip_enabled_flag);
	put(w, 1, p->pic_fields.bits.cu_qp_delta_enabled_flag);
	if (p->pic_fields.bits.cu_qp_delta_enabled_flag)
		put_ue(w, p->diff_cu_qp_delta_depth);
	put_se(w, p->pps_cb_qp_offset);
	put_se(w, p->pps_cr_qp_offset);
	put(w, 1, p->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
	put(w, 1, p->pic_fields.bits.weighted_pred_flag);
	put(w, 1, p->pic_fields.bits.weighted_bipred_flag);
	put(w, 1, p->pic_fields.bits.transquant_bypass_enabled_flag);
	put(w, 1, p->pic_fields.bits.tiles_enabled_flag);
	put(w, 1, p->pic_fields.bits.entropy_coding_sync_enabled_flag);
	if (p->pic_fields.bits.tiles_enabled_flag) {
		put_ue(w, p->num_tile_columns_minus1);
		put_ue(w, p->num_tile_rows_minus1);
		put(w, 1, 0);
		for (unsigned i = 0; i < p->num_tile_columns_minus1; i++)
			put_ue(w, p->column_width_minus1[i]);
		for (unsigned i = 0; i < p->num_tile_rows_minus1; i++)
			put_ue(w, p->row_height_minus1[i]);
		put(w, 1, p->pic_fields.bits.loop_filter_across_tiles_enabled_flag);
	}
	put(w, 1, p->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);
	put(w, 1, 1); /* deblocking_filter_control_present_flag */
	put(w, 1, p->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag);
	put(w, 1, p->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
	if (!p->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
		put_se(w, p->pps_beta_offset_div2);
		put_se(w, p->pps_tc_offset_div2);
	}
	put(w, 1, p->pic_fields.bits.scaling_list_enabled_flag);
	if (p->pic_fields.bits.scaling_list_enabled_flag)
		scaling_lists(w, &c->hevc_iq);
	put(w, 1, 1); /* lists_modification_present_flag */
	put_ue(w, p->log2_parallel_merge_level_minus2);
	put(w, 1, p->slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
	put(w, 1, 0);
}

size_t hevc_build(struct context *c, uint8_t **out)
{
	*out = NULL;
	if (c->picture_failed || hevc_validate(c) || !c->nal_len ||
	    (c->hevc.pic_fields.bits.scaling_list_enabled_flag && !c->have_iq))
		return 0;
	struct bytes frame = {0};
	for (unsigned type = 32; type <= 34; type++) {
		for (unsigned id = 0; id < (type == 34 ? 64u : 1u); id++) {
			if (type == 34 && !(c->pps_used[id / 8] & (1u << (id % 8))))
				continue;
			uint8_t raw[8192] = {0};
			struct writer bw = {.data = raw, .capacity = sizeof(raw)};
			if (type == 32)
				write_vps(&bw, c);
			else if (type == 33)
				write_sps(&bw, c);
			else
				write_pps(&bw, c, id);
			align_bits(&bw);
			const uint8_t header[] = {(uint8_t)(type << 1), 1};
			if (bw.error || encode_nal(&frame, header, raw, bw.bit / 8))
				goto fail;
		}
	}
	static const uint8_t aud[] = {0, 0, 0, 1, 0x46, 1, 0x50};
	if (append(&frame, c->nal, c->nal_len) || append(&frame, aud, sizeof(aud)))
		goto fail;
	*out = frame.data;
	return frame.size;
fail:
	free(frame.data);
	return 0;
}
