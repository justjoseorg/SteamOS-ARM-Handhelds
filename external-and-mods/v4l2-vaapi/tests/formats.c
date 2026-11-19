#define _GNU_SOURCE
#include <assert.h>

#include "../src/driver.c"

static void test_config(VADriverContextP ctx)
{
	struct drv *d = DRV(ctx);
	VAConfigAttrib a = {.type = VAConfigAttribRTFormat};
	assert(GetConfigAttributes(ctx, VAProfileAV1Profile0, VAEntrypointVLD, &a, 1) == VA_STATUS_SUCCESS);
	assert(a.value == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10));
	assert(GetConfigAttributes(ctx, VAProfileH264High, VAEntrypointVLD, &a, 1) == VA_STATUS_SUCCESS);
	assert(a.value == VA_RT_FORMAT_YUV420);
	VAConfigID id;
	a.value = VA_RT_FORMAT_YUV420_10;
	assert(CreateConfig(ctx, VAProfileH264High, VAEntrypointVLD, &a, 1, &id) ==
	       VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
	d->av1_p010 = 0;
	assert(CreateConfig(ctx, VAProfileAV1Profile0, VAEntrypointVLD, &a, 1, &id) ==
	       VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
	d->av1_p010 = 1;
	assert(CreateConfig(ctx, VAProfileAV1Profile0, VAEntrypointVLD, &a, 1, &id) == VA_STATUS_SUCCESS);
	VAProfile profile;
	VAEntrypoint entry;
	int count;
	assert(QueryConfigAttributes(ctx, id, &profile, &entry, &a, &count) == VA_STATUS_SUCCESS);
	assert(count == 1 && a.value == VA_RT_FORMAT_YUV420_10);
	unsigned n = 0;
	assert(QuerySurfaceAttributes(ctx, id, NULL, &n) == VA_STATUS_SUCCESS && n == 7);
	VASurfaceAttrib attrs[8];
	assert(QuerySurfaceAttributes(ctx, id, attrs, &n) == VA_STATUS_SUCCESS);
	assert(attrs[0].value.value.i == VA_FOURCC_P010);
	assert(DestroyConfig(ctx, id) == VA_STATUS_SUCCESS);
	assert(CreateConfig(ctx, VAProfileAV1Profile0, VAEntrypointVLD, NULL, 0, &id) == VA_STATUS_SUCCESS);
	n = 8;
	assert(QuerySurfaceAttributes(ctx, id, attrs, &n) == VA_STATUS_SUCCESS && n == 8);
	assert(attrs[0].value.value.i == VA_FOURCC_NV12 && attrs[1].value.value.i == VA_FOURCC_P010);
	assert(DestroyConfig(ctx, id) == VA_STATUS_SUCCESS);
	assert(GetConfigAttributes(ctx, VAProfileHEVCMain, VAEntrypointVLD, &a, 1) == VA_STATUS_SUCCESS);
	assert(a.value == VA_RT_FORMAT_YUV420);
	assert(GetConfigAttributes(ctx, VAProfileHEVCMain10, VAEntrypointVLD, &a, 1) == VA_STATUS_SUCCESS);
	assert(a.value == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10));
	d->hevc = 0;
	assert(GetConfigAttributes(ctx, VAProfileHEVCMain, VAEntrypointVLD, &a, 1) ==
	       VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
	d->hevc = 1;
	d->hevc_p010 = 0;
	assert(GetConfigAttributes(ctx, VAProfileHEVCMain10, VAEntrypointVLD, &a, 1) ==
	       VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
	d->hevc_p010 = 1;
	VAProfile profiles[MAX_PROFILES];
	assert(QueryConfigProfiles(ctx, profiles, &count) == VA_STATUS_SUCCESS && count == 6);
}

static void test_hevc_buffers(void)
{
	struct surface surface = {.fourcc = VA_FOURCC_NV12};
	struct context c = {.cfg = {.profile = VAProfileHEVCMain10}, .target = &surface};
	VAPictureParameterBufferHEVC picture = {
		.pic_width_in_luma_samples = 64, .pic_height_in_luma_samples = 64,
		.bit_depth_luma_minus8 = 2, .bit_depth_chroma_minus8 = 2,
	};
	picture.pic_fields.bits.chroma_format_idc = 1;
	struct buffer b = {.type = VAPictureParameterBufferType, .data = (uint8_t *)&picture,
			   .size = sizeof(picture) - 1, .num = 1};
	assert(render_hevc(&c, &b) == VA_STATUS_ERROR_INVALID_PARAMETER && !c.have_pic);
	b.size++;
	assert(render_hevc(&c, &b) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT && !c.have_pic);
	surface.fourcc = VA_FOURCC_P010;
	assert(render_hevc(&c, &b) == VA_STATUS_SUCCESS && c.have_pic);
	c.cfg.profile = VAProfileHEVCMain;
	assert(render_hevc(&c, &b) == VA_STATUS_ERROR_INVALID_PARAMETER);
	c.cfg.profile = VAProfileHEVCMain10;
	picture.pic_fields.bits.chroma_format_idc = 3;
	assert(render_hevc(&c, &b) == VA_STATUS_ERROR_INVALID_PARAMETER);
	VASliceParameterBufferHEVC slice = {0};
	b = (struct buffer){.type = VASliceParameterBufferType, .data = (uint8_t *)&slice,
			    .size = sizeof(slice), .num = 0};
	assert(render_hevc(&c, &b) == VA_STATUS_ERROR_INVALID_PARAMETER);
	b.num = 1;
	b.size--;
	assert(render_hevc(&c, &b) == VA_STATUS_ERROR_INVALID_PARAMETER);
	b.size++;
	assert(render_hevc(&c, &b) == VA_STATUS_SUCCESS && c.hevc_nslices == 1);
	free(c.hevc_slices);
}

static void test_surface(VADriverContextP ctx, uint32_t format)
{
	struct drv *d = DRV(ctx);
	unsigned bytes = video_sample_bytes(format);
	uint32_t pitch, uv, size;
	assert(!video_layout(format, 641, 361, &pitch, &uv, &size));
	assert(pitch == 768 * bytes && uv == pitch * 384 && !(size & 4095));
	int fd = memfd_create("p010-surface-test", MFD_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, size));
	VADRMPRIMESurfaceDescriptor descriptor = {
		.fourcc = format, .width = 641, .height = 361, .num_objects = 1, .num_layers = 1,
		.objects = {{.fd = fd, .size = size, .drm_format_modifier = DRM_FORMAT_MOD_LINEAR}},
		.layers = {{.drm_format = format, .num_planes = 2,
			    .offset = {0, uv}, .pitch = {pitch, pitch}}},
	};
	VASurfaceAttrib attrs[] = {
		{.type = VASurfaceAttribPixelFormat,
		 .value = {.type = VAGenericValueTypeInteger, .value.i = format}},
		{.type = VASurfaceAttribMemoryType,
		 .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
		{.type = VASurfaceAttribExternalBufferDescriptor,
		 .value = {.type = VAGenericValueTypePointer, .value.p = &descriptor}},
	};
	VASurfaceID sid;
	uint32_t rt = surface_rt_format(format);
	assert(CreateSurfaces2(ctx, rt, 641, 361, &sid, 1, attrs, 3) == VA_STATUS_SUCCESS);
	struct surface *s = obj_get(d, O_SURFACE, sid);
	assert(s && s->fourcc == format && s->pitch == pitch);
	assert(!surf_map(s));
	for (unsigned i = 0; i < size; i++)
		((uint8_t *)s->map)[i] = (uint8_t)(i * 37 + i / 256);

	VAImage derived, copy;
	assert(DeriveImage(ctx, sid, &derived) == VA_STATUS_SUCCESS);
	assert(derived.format.fourcc == format && derived.format.bits_per_pixel == 12 * bytes);
	assert(derived.pitches[0] == pitch && derived.offsets[1] == uv);
	assert(DestroyImage(ctx, derived.image_id) == VA_STATUS_SUCCESS);
	VAImageFormat fmt = {.fourcc = format, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 12 * bytes};
	assert(CreateImage(ctx, &fmt, 639, 359, &copy) == VA_STATUS_SUCCESS);
	assert(GetImage(ctx, sid, -1, 0, 639, 359, copy.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER);
	assert(GetImage(ctx, sid, 0, 0, UINT32_MAX, 359, copy.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER);
	assert(GetImage(ctx, sid, 2, 2, 639, 359, copy.image_id) == VA_STATUS_SUCCESS);
	struct buffer *b = obj_get(d, O_BUFFER, copy.buf);
	for (unsigned plane = 0; plane < 2; plane++) {
		unsigned rows = plane ? 180 : 359;
		unsigned width = (plane ? 640 : 639) * bytes;
		unsigned source_y = plane ? 1 : 2;
		unsigned source_offset = plane ? uv : 0;
		for (unsigned y = 0; y < rows; y++)
			assert(!memcmp(b->data + copy.offsets[plane] + y * copy.pitches[plane],
				       (uint8_t *)s->map + source_offset + (source_y + y) * pitch + 2 * bytes,
				       width));
	}
	assert(GetImage(ctx, sid, 1, 1, 2, 2, copy.image_id) == VA_STATUS_SUCCESS);
	for (unsigned y = 0; y < 2; y++)
		assert(!memcmp(b->data + copy.offsets[0] + y * copy.pitches[0],
			       (uint8_t *)s->map + (y + 1) * pitch + bytes, 2 * bytes));
	assert(!memcmp(b->data + copy.offsets[1], (uint8_t *)s->map + uv, 2 * bytes));
	assert(DestroyImage(ctx, copy.image_id) == VA_STATUS_SUCCESS);
	fmt.fourcc = format == VA_FOURCC_P010 ? VA_FOURCC_NV12 : VA_FOURCC_P010;
	fmt.bits_per_pixel = 12 * video_sample_bytes(fmt.fourcc);
	assert(CreateImage(ctx, &fmt, 641, 361, &copy) == VA_STATUS_SUCCESS);
	assert(GetImage(ctx, sid, 0, 0, 641, 361, copy.image_id) == VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
	assert(DestroyImage(ctx, copy.image_id) == VA_STATUS_SUCCESS);

	VADRMPRIMESurfaceDescriptor exported;
	assert(ExportSurfaceHandle(ctx, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				   VA_EXPORT_SURFACE_SEPARATE_LAYERS, &exported) == VA_STATUS_SUCCESS);
	assert(exported.fourcc == format && exported.num_layers == 2);
	assert(exported.layers[0].drm_format == (bytes == 2 ? DRM_FORMAT_R16 : DRM_FORMAT_R8));
	assert(exported.layers[1].drm_format == (bytes == 2 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88));
	assert(exported.layers[1].offset[0] == uv && exported.layers[1].pitch[0] == pitch);
	close(exported.objects[0].fd);
	assert(ExportSurfaceHandle(ctx, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				   VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported) == VA_STATUS_SUCCESS);
	assert(exported.num_layers == 1 && exported.layers[0].drm_format == format);
	close(exported.objects[0].fd);
	assert(DestroySurfaces(ctx, &sid, 1) == VA_STATUS_SUCCESS);

	descriptor.objects[0].size = uv;
	assert(CreateSurfaces2(ctx, rt, 641, 361, &sid, 1, attrs, 3) == VA_STATUS_ERROR_INVALID_PARAMETER);
	descriptor.objects[0].size = size;
	descriptor.objects[0].drm_format_modifier = 1;
	assert(CreateSurfaces2(ctx, rt, 641, 361, &sid, 1, attrs, 3) == VA_STATUS_ERROR_INVALID_PARAMETER);
	close(fd);
}

static void test_decode_recovery(VADriverContextP ctx)
{
	struct drv *d = DRV(ctx);
	struct context c = {.drv = d, .cfg.profile = VAProfileAV1Profile0, .failed = 1, .fd = -1};
	struct surface s = {.ctx = &c, .fourcc = VA_FOURCC_NV12, .error = 1};
	uint32_t sid = obj_add(d, O_SURFACE, &s);
	assert(SyncSurface(ctx, sid) == VA_STATUS_ERROR_DECODING_ERROR);
	c.failed = 0;
	assert(SyncSurface(ctx, sid) == VA_STATUS_ERROR_DECODING_ERROR);
	assert(!c.failed);
	s.error = 0;
	assert(SyncSurface(ctx, sid) == VA_STATUS_SUCCESS);
	assert(!c.failed);
	s.pending = 1;
	assert(SyncSurface(ctx, sid) == VA_STATUS_ERROR_DECODING_ERROR);
	assert(c.failed && s.error);
	s.pending = 0;
	s.error = 0;

	c.target = &s;
	VADecPictureParameterBufferAV1 pic = {0};
	struct buffer b = {.type = VAPictureParameterBufferType, .data = (uint8_t *)&pic,
			   .size = sizeof(pic)};
	pic.pic_info_fields.bits.frame_type = 1;
	pic.pic_info_fields.bits.show_frame = 1;
	assert(render_av1(&c, &b) == VA_STATUS_SUCCESS && c.failed);
	pic.pic_info_fields.bits.frame_type = 0;
	pic.pic_info_fields.bits.show_frame = 0;
	assert(render_av1(&c, &b) == VA_STATUS_SUCCESS && c.failed);
	pic.pic_info_fields.bits.show_frame = 1;
	assert(render_av1(&c, &b) == VA_STATUS_SUCCESS && c.failed);

	pic.current_frame = sid;
	pic.current_display_picture = sid + 1;
	pic.seq_info_fields.fields.film_grain_params_present = 1;
	pic.film_grain_info.film_grain_info_fields.bits.apply_grain = 1;
	c.failed = 1;
	assert(render_av1(&c, &b) == VA_STATUS_ERROR_UNIMPLEMENTED);
	assert(!c.have_pic && c.failed);
	pic.film_grain_info.film_grain_info_fields.bits.apply_grain = 0;
	assert(render_av1(&c, &b) == VA_STATUS_SUCCESS && c.have_pic && c.failed);

	c.cfg.profile = VAProfileH264High;
	c.failed = 1;
	uint32_t cid = obj_add(d, O_CONTEXT, &c);
	assert(EndPicture(ctx, cid) == VA_STATUS_ERROR_DECODING_ERROR && c.failed);
	c.idr = 1;
	assert(EndPicture(ctx, cid) == VA_STATUS_ERROR_DECODING_ERROR && c.failed);
	obj_del(d, O_CONTEXT, cid);
	obj_del(d, O_SURFACE, sid);
	assert(SyncSurface(ctx, sid) == VA_STATUS_ERROR_INVALID_SURFACE);
}

static void test_failed_picture(VADriverContextP ctx)
{
	struct drv *d = DRV(ctx);
	struct context c = {.drv = d, .cfg = {.profile = VAProfileHEVCMain,
		.rt_format = VA_RT_FORMAT_YUV420}, .fd = -1};
	struct surface s = {.ctx = &c, .fourcc = VA_FOURCC_NV12};
	uint32_t cid = obj_add(d, O_CONTEXT, &c), sid = obj_add(d, O_SURFACE, &s);
	assert(BeginPicture(ctx, cid, sid) == VA_STATUS_SUCCESS);
	c.hevc.pic_width_in_luma_samples = c.hevc.pic_height_in_luma_samples = 96;
	c.hevc.pic_fields.bits.chroma_format_idc = 1;
	c.have_pic = 1;
	uint8_t nal[] = {0x26, 1, 0xa0};
	VASliceParameterBufferHEVC slice = {.slice_data_size = sizeof(nal)};
	c.hevc_slices = &slice;
	c.hevc_nslices = 1;
	struct buffer data = {.type = VASliceDataBufferType,
		.data = nal, .size = sizeof(nal), .num = 1};
	VABufferID bid = obj_add(d, O_BUFFER, &data);
	assert(RenderPicture(ctx, cid, &bid, 1) == VA_STATUS_SUCCESS);
	assert(c.idr && c.nal_len && !c.picture_failed);
	VABufferID invalid = VA_INVALID_ID;
	assert(RenderPicture(ctx, cid, &invalid, 1) == VA_STATUS_ERROR_INVALID_BUFFER);
	assert(c.picture_failed);
	assert(EndPicture(ctx, cid) == VA_STATUS_ERROR_DECODING_ERROR);
	assert(s.error && !s.pending && c.failed && !c.target);
	assert(BeginPicture(ctx, cid, sid) == VA_STATUS_SUCCESS);
	assert(!c.picture_failed && !c.nal_len && !c.idr && c.failed);
	free(c.nal);
	obj_del(d, O_BUFFER, bid);
	obj_del(d, O_CONTEXT, cid);
	obj_del(d, O_SURFACE, sid);
}

static void test_unconsumed_slices(VADriverContextP ctx, VAProfile profile, unsigned size)
{
	struct drv *d = DRV(ctx);
	struct context *c = calloc(1, sizeof(*c));
	assert(c);
	c->drv = d;
	c->cfg = (struct config){.profile = profile, .rt_format = VA_RT_FORMAT_YUV420};
	c->fd = -1;
	struct surface s = {.ctx = c, .fourcc = VA_FOURCC_NV12, .slot = -1};
	VAContextID cid = obj_add(d, O_CONTEXT, c);
	VASurfaceID sid = obj_add(d, O_SURFACE, &s);
	assert(BeginPicture(ctx, cid, sid) == VA_STATUS_SUCCESS);
	int decoder_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	assert(decoder_fd >= 0);
	c->fd = decoder_fd;
	c->have_pic = c->idr = c->failed = 1;
	c->nal = malloc(1);
	assert(c->nal);
	c->nal[0] = 0x80;
	c->nal_len = 1;
	void *parameters = calloc(1, size);
	assert(parameters);
	VABufferID bid;
	assert(CreateBuffer(ctx, cid, VASliceParameterBufferType, size, 1, parameters, &bid) ==
	       VA_STATUS_SUCCESS);
	free(parameters);
	assert(RenderPicture(ctx, cid, &bid, 1) == VA_STATUS_SUCCESS && !c->picture_failed);
	assert(EndPicture(ctx, cid) == VA_STATUS_ERROR_DECODING_ERROR);
	assert(c->picture_failed && c->failed && s.error && !s.pending);
	assert(c->fd == decoder_fd && fcntl(decoder_fd, F_GETFD) >= 0);
	assert(DestroyBuffer(ctx, bid) == VA_STATUS_SUCCESS);
	assert(DestroyContext(ctx, cid) == VA_STATUS_SUCCESS);
	obj_del(d, O_SURFACE, sid);
}

int main(void)
{
	struct drv d = {.av1 = 1, .hevc = 1, .p010 = 1,
		.av1_p010 = 1, .hevc_p010 = 1, .render_fd = -1};
	pthread_mutex_init(&d.lock, NULL);
	struct VADriverContext ctx = {.pDriverData = &d};
	uint32_t pitch, uv, size;
	assert(!video_layout(VIDEO_NV12, 1920, 1080, &pitch, &uv, &size));
	assert(pitch == 1920 && uv == 2088960 && size == 3133440);
	assert(!video_layout(VIDEO_P010, 1920, 1080, &pitch, &uv, &size));
	assert(pitch == 3840 && uv == 4177920 && size == 6266880);
	assert(video_layout(0, 1920, 1080, &pitch, &uv, &size) == -1);
	assert(video_layout(VIDEO_P010, UINT32_MAX, 1080, &pitch, &uv, &size) == -1);
	assert(video_layout(VIDEO_P010, 1920, 0, &pitch, &uv, &size) == -1);
	test_config(&ctx);
	test_hevc_buffers();
	test_surface(&ctx, VA_FOURCC_NV12);
	d.av1 = 0;
	test_surface(&ctx, VA_FOURCC_P010);
	d.av1 = 1;
	test_decode_recovery(&ctx);
	test_failed_picture(&ctx);
	test_unconsumed_slices(&ctx, VAProfileHEVCMain, sizeof(VASliceParameterBufferHEVC));
	test_unconsumed_slices(&ctx, VAProfileH264High, sizeof(VASliceParameterBufferH264));
	test_unconsumed_slices(&ctx, VAProfileAV1Profile0, sizeof(VASliceParameterBufferAV1));
	VAImageFormat images[2];
	int count;
	assert(QueryImageFormats(&ctx, images, &count) == VA_STATUS_SUCCESS && count == 2);
	assert(images[1].fourcc == VA_FOURCC_P010 && images[1].bits_per_pixel == 24);
	d.p010 = 0;
	assert(QueryImageFormats(&ctx, images, &count) == VA_STATUS_SUCCESS && count == 1);
	for (int i = 0; i < O_MAX; i++)
		free(d.tab[i].items);
	pthread_mutex_destroy(&d.lock);
	puts("NV12/P010 configuration, surface, image and export tests passed");
	return 0;
}
