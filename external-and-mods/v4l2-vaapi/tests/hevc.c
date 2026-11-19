#include <assert.h>
#include <stdio.h>

#include "../src/hevc.c"

static void setup(struct context *c)
{
	memset(c, 0, sizeof(*c));
	c->cfg.profile = VAProfileHEVCMain;
	c->width = 1280;
	c->height = 720;
	c->hevc.pic_width_in_luma_samples = c->width;
	c->hevc.pic_height_in_luma_samples = c->height;
	c->hevc.pic_fields.bits.chroma_format_idc = 1;
	c->hevc.log2_diff_max_min_luma_coding_block_size = 3;
	c->hevc.log2_max_pic_order_cnt_lsb_minus4 = 4;
	c->hevc.sps_max_dec_pic_buffering_minus1 = 5;
	c->hevc.CurrPic.pic_order_cnt = 300;
	for (unsigned i = 0; i < 15; i++)
		c->hevc.ReferenceFrames[i].picture_id = VA_INVALID_ID;
}

static const uint8_t cabac[] = {0x35, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0x93, 0, 0};

static struct bytes source_slice(struct context *c, VASliceParameterBufferHEVC *s,
				int from_sps)
{
	VAPictureParameterBufferHEVC *p = &c->hevc;
	p->num_short_term_ref_pic_sets = from_sps ? 2 : 0;
	p->slice_parsing_fields.bits.long_term_ref_pics_present_flag = 1;
	p->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
	p->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
	p->slice_parsing_fields.bits.lists_modification_present_flag = 1;
	p->ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 10, .pic_order_cnt = 302,
		.flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER};
	p->ReferenceFrames[1] = (VAPictureHEVC){.picture_id = 11, .pic_order_cnt = 299,
		.flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
	p->ReferenceFrames[2] = (VAPictureHEVC){.picture_id = 12, .pic_order_cnt = 298};
	p->ReferenceFrames[3] = (VAPictureHEVC){.picture_id = 13, .pic_order_cnt = 12,
		.flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR};
	p->ReferenceFrames[4] = (VAPictureHEVC){.picture_id = 14, .pic_order_cnt = -250,
		.flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE};
	uint8_t raw[256] = {0};
	struct writer w = {.data = raw, .capacity = sizeof(raw)};
	put(&w, 1, 1);
	put_ue(&w, 5);
	put_ue(&w, 1);
	put(&w, 8, 300);
	put(&w, 1, from_sps);
	if (from_sps) {
		put(&w, 1, 1);
	} else {
		size_t begin = w.bit;
		put_ue(&w, 2);
		put_ue(&w, 1);
		put_ue(&w, 0);
		put(&w, 1, 1);
		put_ue(&w, 0);
		put(&w, 1, 0);
		put_ue(&w, 1);
		put(&w, 1, 1);
		p->st_rps_bits = w.bit - begin;
	}
	put_ue(&w, 2);
	put(&w, 8, 12);
	put(&w, 1, 1);
	put(&w, 1, 1);
	put_ue(&w, 1);
	put(&w, 8, 6);
	put(&w, 1, 0);
	put(&w, 1, 1);
	put_ue(&w, 1);
	put(&w, 1, 1);
	put(&w, 2, 2);
	put(&w, 1, 1);
	put_ue(&w, 1);
	put(&w, 1, 0);
	put(&w, 12, 0x6da);
	put_ue(&w, 2);
	put_ue(&w, 5);
	put(&w, 32, 0xa1234567);
	align_bits(&w);
	assert(!w.error);
	s->slice_data_byte_offset = 2 + w.bit / 8;
	s->RefPicList[0][0] = 3;
	s->RefPicList[0][1] = 1;
	memcpy(raw + w.bit / 8, cabac, sizeof(cabac));
	struct bytes nal = {0};
	const uint8_t header[] = {2, 1};
	assert(!encode_nal(&nal, header, raw, w.bit / 8 + sizeof(cabac)));
	s->slice_data_size = nal.size;
	return nal;
}

static void test_rewrite(int from_sps)
{
	struct context c;
	setup(&c);
	VASliceParameterBufferHEVC slice = {0};
	struct bytes source = source_slice(&c, &slice, from_sps);
	c.hevc_slices = &slice;
	c.hevc_nslices = 1;
	struct buffer data = {.data = source.data, .size = source.size, .num = 1};
	assert(!hevc_add_slices(&c, &data));
	assert(!c.hevc_nslices && c.pps_used[0] == (1 << 5));
	size_t size = c.nal_len - 4;
	uint8_t *raw = unescape(c.nal + 4, &size);
	assert(raw);
	struct reader r = {.data = raw, .size = size, .bit = 16};
	assert(get(&r, 1) == 1 && get_ue(&r) == 5 && get_ue(&r) == 1);
	assert(get(&r, 8) == (300 & 255));
	assert(get(&r, 1) == 0);
	assert(get_ue(&r) == 2 && get_ue(&r) == 1);
	assert(get_ue(&r) == 0 && get(&r, 1) == 1);
	assert(get_ue(&r) == 0 && get(&r, 1) == 0);
	assert(get_ue(&r) == 1 && get(&r, 1) == 1);
	assert(get_ue(&r) == 2);
	assert(get(&r, 8) == 12 && get(&r, 1) == 1 && get(&r, 1) == 1);
	assert(get_ue(&r) == 1);
	assert(get(&r, 8) == 6 && get(&r, 1) == 0 && get(&r, 1) == 1);
	assert(get_ue(&r) == 1);
	assert(get(&r, 1) == 1 && get(&r, 2) == 2);
	assert(get(&r, 1) == 1 && get_ue(&r) == 1);
	assert(get(&r, 1) == 1 && get(&r, 2) == 2 && get(&r, 2) == 0);
	assert(get(&r, 12) == 0x6da);
	assert(get_ue(&r) == 2 && get_ue(&r) == 5);
	assert(get(&r, 32) == 0xa1234567);
	assert(get(&r, 1) == 1);
	while (r.bit % 8)
		assert(get(&r, 1) == 0);
	assert(!r.error && size - r.bit / 8 == sizeof(cabac));
	assert(!memcmp(raw + r.bit / 8, cabac, sizeof(cabac)));
	uint8_t *frame = NULL;
	assert(hevc_build(&c, &frame) > c.nal_len && frame);
	free(frame);
	c.hevc.pic_fields.bits.scaling_list_enabled_flag = 1;
	assert(!hevc_build(&c, &frame) && !frame);
	free(source.data);
	free(raw);
	free(c.nal);
}

static void test_invalid_slice(void)
{
	struct context c;
	setup(&c);
	VASliceParameterBufferHEVC slice = {0};
	struct bytes source = source_slice(&c, &slice, 1);
	c.hevc_slices = &slice;
	struct buffer data = {.data = source.data, .size = source.size, .num = 1};
	for (unsigned i = 0; i < 4; i++) {
		c.picture_failed = 0;
		c.hevc_slices = &slice;
		c.hevc_nslices = 1;
		assert(!hevc_add_slices(&c, &data) && c.nal_len);
		size_t valid_size = c.nal_len;
		VASliceParameterBufferHEVC broken = slice;
		if (i == 0)
			broken.slice_data_offset = UINT32_MAX;
		if (i == 1)
			broken.slice_data_byte_offset = UINT32_MAX;
		if (i == 2)
			broken.slice_data_size = 6;
		if (i == 3)
			broken.RefPicList[0][0] = 14;
		c.hevc_slices = &broken;
		c.hevc_nslices = 1;
		assert(hevc_add_slices(&c, &data));
		assert(!c.nal_len && c.picture_failed);
		c.nal_len = valid_size;
		c.idr = 1;
		uint8_t *frame = NULL;
		assert(!hevc_build(&c, &frame) && !frame);
		c.nal_len = 0;
	}
	free(source.data);
	free(c.nal);
}

static void test_idr(void)
{
	struct context c;
	setup(&c);
	uint8_t payload[] = {0xae, 0x35, 0, 0, 1, 0, 0};
	const uint8_t header[] = {19 << 1, 1};
	struct bytes source = {0};
	assert(!encode_nal(&source, header, payload, sizeof(payload)));
	VASliceParameterBufferHEVC slice = {.slice_data_size = source.size};
	c.hevc_slices = &slice;
	c.hevc_nslices = 1;
	struct buffer data = {.data = source.data, .size = source.size, .num = 1};
	assert(!hevc_add_slices(&c, &data));
	assert(c.idr && c.nal_len == source.size);
	assert(!memcmp(c.nal, source.data, source.size));
	free(source.data);
	free(c.nal);
}

static void test_terminal_escape(void)
{
	const uint8_t invalid[] = {0, 0, 3, 4};
	size_t size = sizeof(invalid);
	assert(!unescape(invalid, &size));
	for (unsigned words = 1; words <= 2; words++) {
		uint8_t payload[] = {0x80, 0, 0, 0, 0};
		const uint8_t header[] = {2, 1};
		struct bytes nal = {0};
		size_t payload_size = 1 + 2 * words;
		assert(!encode_nal(&nal, header, payload, payload_size));
		assert(nal.data[nal.size - 1] == 3);
		size = nal.size - 6;
		uint8_t *raw = unescape(nal.data + 6, &size);
		assert(raw && size == payload_size && !memcmp(raw, payload, size));
		free(raw);
		free(nal.data);
	}
}

static void test_long_term_wrap(void)
{
	struct context c;
	setup(&c);
	c.hevc.CurrPic.pic_order_cnt = 250;
	c.hevc.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = 260,
		.flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR};
	struct refs refs;
	assert(!collect_refs(&c.hevc, &refs));
	uint8_t raw[64] = {0};
	struct writer w = {.data = raw, .capacity = sizeof(raw)};
	write_refs(&w, &c.hevc, &refs);
	struct reader r = {.data = raw, .size = sizeof(raw)};
	assert(!get(&r, 1) && !get_ue(&r) && !get_ue(&r));
	assert(get_ue(&r) == 1 && get(&r, 8) == 4 && get(&r, 1) == 1);
	assert(!get(&r, 1) && r.bit == w.bit && !w.error);

	c.hevc.ReferenceFrames[1] = (VAPictureHEVC){.picture_id = 2, .pic_order_cnt = 4,
		.flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR};
	assert(collect_refs(&c.hevc, &refs));
	c.hevc.CurrPic.pic_order_cnt = 600;
	assert(!collect_refs(&c.hevc, &refs));
	memset(raw, 0, sizeof(raw));
	w.bit = 0;
	write_refs(&w, &c.hevc, &refs);
	r.bit = 0;
	assert(!get(&r, 1) && !get_ue(&r) && !get_ue(&r) && get_ue(&r) == 2);
	unsigned cycles = 0;
	for (unsigned i = 0; i < 2; i++) {
		unsigned lsb = get(&r, 8);
		assert(get(&r, 1) && get(&r, 1));
		cycles += get_ue(&r);
		assert(512 - (int)cycles * 256 + (int)lsb ==
		       c.hevc.ReferenceFrames[i].pic_order_cnt);
	}
	assert(!r.error && !w.error && r.bit == w.bit);
}

static void test_wpp(void)
{
	/* x265, 512x192 gray, wpp=1:bframes=0:info=0; POC 1, three CTU rows. */
	uint8_t source[] = {
		0x02, 0x01, 0xd0, 0x09, 0x7e, 0x10, 0xc6, 0x10,
		0xb6, 0xe4, 0xfa, 0x9d, 0x29, 0x70, 0xf4, 0x0c,
		0x55, 0xb1, 0xe9, 0xd3, 0x62, 0x09, 0xc8,
	};
	struct context c;
	setup(&c);
	c.width = c.hevc.pic_width_in_luma_samples = 512;
	c.height = c.hevc.pic_height_in_luma_samples = 192;
	c.hevc.CurrPic.pic_order_cnt = 1;
	c.hevc.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = 0,
		.flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
	c.hevc.st_rps_bits = 6;
	c.hevc.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
	c.hevc.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
	c.hevc.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
	c.hevc.pic_fields.bits.weighted_pred_flag = 1;
	VASliceParameterBufferHEVC slice = {
		.slice_data_size = sizeof(source), .slice_data_byte_offset = 10,
	};
	c.hevc_slices = &slice;
	c.hevc_nslices = 1;
	struct buffer data = {.data = source, .size = sizeof(source), .num = 1};
	assert(!hevc_add_slices(&c, &data));
	size_t size = c.nal_len - 4;
	uint8_t *raw = unescape(c.nal + 4, &size);
	assert(raw);
	struct reader r = {.data = raw, .size = size, .bit = 16};
	assert(get(&r, 1) && !get_ue(&r) && get_ue(&r) == 1 && get(&r, 8) == 1);
	assert(!get(&r, 1) && get_ue(&r) == 1 && !get_ue(&r));
	assert(!get_ue(&r) && get(&r, 1) && !get_ue(&r));
	assert(get(&r, 1) && get(&r, 2) == 3 && !get(&r, 1));
	assert(get_ue(&r) == 7 && get_ue(&r) == 2 && !get(&r, 2));
	assert(get_ue(&r) == 2 && get_ue(&r) == 15 && get(&r, 1));
	assert(get_ue(&r) == 2);
	unsigned bits = get_ue(&r) + 1;
	unsigned offsets[] = {get(&r, bits) + 1, get(&r, bits) + 1};
	assert(offsets[0] == 4 && offsets[1] == 5 && get(&r, 1));
	while (r.bit % 8)
		assert(!get(&r, 1));
	assert(!r.error && size - r.bit / 8 == sizeof(source) - 10);
	size_t start = 0;
	for (unsigned i = 0; i < 3; i++) {
		size_t length = i < 2 ? offsets[i] : sizeof(source) - 10 - start;
		assert(!memcmp(raw + r.bit / 8 + start, source + 10 + start, length));
		start += length;
	}
	free(raw);
	free(c.nal);
}

int main(void)
{
	test_rewrite(0);
	test_rewrite(1);
	test_invalid_slice();
	test_idr();
	test_terminal_escape();
	test_long_term_wrap();
	test_wpp();
	puts("HEVC explicit references, lists, WPP/CABAC preservation and malformed slices passed");
	return 0;
}
