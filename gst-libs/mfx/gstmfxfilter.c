#include "sysdeps.h"
#include "gstmfxfilter.h"
#include "gstmfxtaskaggregator.h"
#include "gstmfxtask.h"
#include "gstmfxsurfacepool.h"
#include "gstmfxsurfaceproxy.h"

#include <mfxvideo.h>

#define DEBUG 1
#include "gstmfxdebug.h"

#define GST_MFX_FILTER(obj) \
	((GstMfxFilter *)(obj))

typedef struct _GstMfxFilterOpData GstMfxFilterOpData;

typedef struct
{
    GstMfxFilterType type;
    mfxU32 filter;
    gchar desc[64];
} GstMfxFilterMap;

struct _GstMfxFilterOpData
{
    GstMfxFilterType type;
    gpointer filter;
    gsize size;
};

struct _GstMfxFilter
{
	/*< private >*/
	GstMfxMiniObject        parent_instance;
	GstMfxTaskAggregator   *aggregator;
	GstMfxTask             *vpp[2];
	GstMfxSurfacePool      *vpp_pool[2];
	mfxSession              session;
	mfxVideoParam           params;
	mfxFrameInfo            frame_info;
    mfxFrameAllocRequest   *vpp_request[2];

	/* VPP output parameters */
	mfxU32                  fourcc;
	mfxU16                  width;
	mfxU16                  height;
    mfxU16                  fps_n;
    mfxU16                  fps_d;

    /* FilterType */
    guint                   supported_filters;
    guint                   filter_op;
    GPtrArray              *filter_op_data;

	mfxExtBuffer          **ext_buffer;
    mfxExtVPPDoUse          vpp_use;
};

static const GstMfxFilterMap filter_map[] = {
    { GST_MFX_FILTER_DEINTERLACING, MFX_EXTBUFF_VPP_DEINTERLACING, "Deinterlacing filter" },
    { GST_MFX_FILTER_PROCAMP, MFX_EXTBUFF_VPP_PROCAMP, "ProcAmp filter" },
    { GST_MFX_FILTER_DETAIL, MFX_EXTBUFF_VPP_DETAIL, "Detail filter" },
    { GST_MFX_FILTER_DENOISE, MFX_EXTBUFF_VPP_DENOISE, "Denoise filter" },
    { GST_MFX_FILTER_FRAMERATE_CONVERSION, MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION, "Framerate conversion filter" },
    { GST_MFX_FILTER_FIELD_PROCESSING, MFX_EXTBUFF_VPP_FIELD_PROCESSING, "Field processing filter" },
    { GST_MFX_FILTER_IMAGE_STABILIZATION, MFX_EXTBUFF_VPP_IMAGE_STABILIZATION, "Image stabilization filter" },
    { GST_MFX_FILTER_ROTATION, MFX_EXTBUFF_VPP_ROTATION, "Rotation filter" },
    {0, }
};

void
gst_mfx_filter_set_request(GstMfxFilter * filter,
    mfxFrameAllocRequest * request, guint flags)
{
    filter->vpp_request[flags & GST_MFX_TASK_VPP_OUT] =
        g_slice_dup(mfxFrameAllocRequest, request);

    filter->frame_info = request->Info;

    if (flags & GST_MFX_TASK_VPP_IN)
        filter->vpp_request[0]->Type |= MFX_MEMTYPE_FROM_VPPIN;
    else
        filter->vpp_request[1]->Type |= MFX_MEMTYPE_FROM_VPPOUT;
}

void
gst_mfx_filter_set_frame_info(GstMfxFilter * filter, GstVideoInfo * info)
{
	filter->frame_info.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	filter->frame_info.FourCC = gst_video_format_to_mfx_fourcc(
        GST_VIDEO_INFO_FORMAT(info));
	filter->frame_info.PicStruct = GST_VIDEO_INFO_IS_INTERLACED(info) ?
        (GST_VIDEO_INFO_FLAG_IS_SET(info, GST_VIDEO_FRAME_FLAG_TFF) ?
            MFX_PICSTRUCT_FIELD_TFF : MFX_PICSTRUCT_FIELD_BFF)
        : MFX_PICSTRUCT_PROGRESSIVE;

	filter->frame_info.CropX = 0;
	filter->frame_info.CropY = 0;
	filter->frame_info.CropW = info->width;
	filter->frame_info.CropH = info->height;
	filter->frame_info.FrameRateExtN = info->fps_n ? info->fps_n : 30;
	filter->frame_info.FrameRateExtD = info->fps_d;
	filter->frame_info.AspectRatioW = info->par_n;
	filter->frame_info.AspectRatioH = info->par_d;
	filter->frame_info.BitDepthChroma = 8;
	filter->frame_info.BitDepthLuma = 8;

	filter->frame_info.Width = GST_ROUND_UP_16(info->width);
	filter->frame_info.Height =
		(MFX_PICSTRUCT_PROGRESSIVE == filter->frame_info.PicStruct) ?
		GST_ROUND_UP_16(info->height) :
		GST_ROUND_UP_32(info->height);
}

static void check_supported_filters(GstMfxFilter *filter)
{
    mfxVideoParam param;
    mfxExtVPPDoUse vpp_use;
    mfxExtBuffer *extbuf[1];
    mfxStatus sts;
    const GstMfxFilterMap *m;

    filter->supported_filters = GST_MFX_FILTER_NONE;
    memset(&vpp_use, 0, sizeof(mfxExtVPPDoUse));

    vpp_use.NumAlg = 1;
    vpp_use.AlgList = g_slice_alloc(sizeof(mfxExtVPPDoUse));
    vpp_use.Header.BufferId = MFX_EXTBUFF_VPP_DOUSE;
    param.NumExtParam = 1;

    extbuf[0] = (mfxExtBuffer *)&vpp_use;
    param.ExtParam = (mfxExtBuffer **)&extbuf[0];

    /* check filters */
    for(m = filter_map; m->type; m++) {
        vpp_use.AlgList[0] = m->filter;
        sts = MFXVideoVPP_Query(filter->session, NULL, &param);
        if (MFX_ERR_NONE == sts)
            filter->supported_filters |= m->type;
        else
            g_print("%s is not supported in this platform!\n", m->desc);
    }

    /* Release the resource */
    g_slice_free(mfxExtVPPDoUse, vpp_use.AlgList);
}

static gboolean
init_filters(GstMfxFilter * filter)
{
    GstMfxFilterOpData *op;
    mfxExtBuffer *ext_buf;
    guint i;

    check_supported_filters(filter);

    memset(&filter->vpp_use, 0, sizeof(mfxExtVPPDoUse));

    filter->vpp_use.Header.BufferId = MFX_EXTBUFF_VPP_DOUSE;
    filter->vpp_use.Header.BufferSz = sizeof(mfxExtVPPDoUse);
    filter->vpp_use.NumAlg = filter->filter_op_data->len;
	filter->vpp_use.AlgList = g_slice_alloc(
        filter->vpp_use.NumAlg * sizeof(mfxU32));
	if (!filter->vpp_use.AlgList)
		return FALSE;

    filter->ext_buffer = g_slice_alloc(
        (filter->vpp_use.NumAlg + 1) * sizeof(mfxExtBuffer*));
    if (!filter->ext_buffer)
        return FALSE;

    for(i = 0; i < filter->filter_op_data->len; i++) {
        op = (GstMfxFilterOpData *)g_ptr_array_index(filter->filter_op_data, i);
        ext_buf = (mfxExtBuffer *)op->filter;
        filter->vpp_use.AlgList[i] = ext_buf->BufferId;
        filter->ext_buffer[i+1] = (mfxExtBuffer *)op->filter;
    }

    filter->ext_buffer[0] = (mfxExtBuffer*)&filter->vpp_use;

    filter->params.NumExtParam = filter->vpp_use.NumAlg+1;
	filter->params.ExtParam = (mfxExtBuffer **) &filter->ext_buffer[0];

	return TRUE;
}

static gboolean
init_params(GstMfxFilter * filter)
{
    filter->params.vpp.In = filter->frame_info;
    filter->params.vpp.Out = filter->params.vpp.In;

    if (filter->fourcc)
        filter->params.vpp.Out.FourCC = filter->fourcc;

    if (filter->width) {
        filter->params.vpp.Out.CropW = filter->width;
        filter->params.vpp.Out.Width = GST_ROUND_UP_16(filter->width);
    }
    if (filter->height) {
        filter->params.vpp.Out.CropH = filter->height;
        filter->params.vpp.Out.Height = GST_ROUND_UP_16(filter->height);
    }
    if (filter->filter_op & GST_MFX_FILTER_DEINTERLACING) {
        filter->params.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        filter->params.vpp.Out.Height = GST_ROUND_UP_16(filter->height);
    }
    if(filter->filter_op & GST_MFX_FILTER_FRAMERATE_CONVERSION &&
            (filter->fps_n && filter->fps_d)) {
        filter->params.vpp.Out.FrameRateExtN = filter->fps_n;
        filter->params.vpp.Out.FrameRateExtD = filter->fps_d;
    }

    return TRUE;
}

gboolean
gst_mfx_filter_start(GstMfxFilter * filter)
{
    mfxFrameAllocRequest vpp_request[2];
    mfxFrameAllocResponse response;
    mfxStatus sts;
    gboolean mapped;
    guint i;

    if (!filter->session) {
        mapped = filter->params.IOPattern & MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        filter->vpp[1] = gst_mfx_task_new(filter->aggregator, GST_MFX_TASK_VPP_OUT, mapped);
        filter->session = gst_mfx_task_get_session(filter->vpp[1]);
        gst_mfx_task_aggregator_set_current_task(filter->aggregator, filter->vpp[1]);
    }

    if (!init_params(filter))
        return FALSE;

    sts = MFXVideoVPP_QueryIOSurf(filter->session, &filter->params, &vpp_request);
    if (sts < 0) {
        GST_ERROR("Unable to query VPP allocation request %d", sts);
        return FALSE;
    }

    /* Initialize VPP surface pools */
    for (i = 0; i < 2; i++) {
        GstMfxTaskType type = i == 0 ? GST_MFX_TASK_VPP_IN : GST_MFX_TASK_VPP_OUT;
		guint io_pattern = i == 0 ? MFX_IOPATTERN_IN_VIDEO_MEMORY :
			MFX_IOPATTERN_OUT_VIDEO_MEMORY;

        /* No need for input VPP pool when shared alloc request is not set */
        if (GST_MFX_TASK_VPP_IN == type && !filter->vpp_request[0])
            continue;

        if (!gst_mfx_task_aggregator_find_task(filter->aggregator, filter->vpp[i])) {
            mapped = filter->params.IOPattern & MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
            filter->vpp[i] = gst_mfx_task_new_with_session(filter->aggregator,
                &filter->session, type, mapped);
        }

        if (!filter->vpp_request[i]) {
            filter->vpp_request[i] =
                g_slice_dup(mfxFrameAllocRequest, &vpp_request[i]);
        }
        else {
            filter->vpp_request[i]->NumFrameSuggested +=
                vpp_request[i].NumFrameSuggested;
            filter->vpp_request[i]->NumFrameMin =
                filter->vpp_request[i]->NumFrameSuggested;
        }

        gst_mfx_task_set_request(filter->vpp[i], filter->vpp_request[i]);

        if (!gst_mfx_task_has_mapped_surface(filter->vpp[i])) {
            filter->vpp_request[i]->Type |= MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

            sts = gst_mfx_task_frame_alloc(filter->vpp[i], filter->vpp_request[i],
                    &response);
            if (MFX_ERR_NONE != sts)
                return FALSE;
        }

        filter->vpp_pool[i] = gst_mfx_surface_pool_new_with_task(filter->vpp[i]);
        if (!filter->vpp_pool[i])
            return FALSE;
    }

    init_filters(filter);

    sts = MFXVideoVPP_Init(filter->session, &filter->params);
    if (sts < 0) {
		GST_ERROR("Error initializing MFX VPP %d", sts);
		return FALSE;
	}

    return TRUE;
}

static GstMfxFilterOpData *
find_filter_op_data(GstMfxFilter *filter, GstMfxFilterType type)
{
    guint i;
    GstMfxFilterOpData *op;

    for(i = 0; i < filter->filter_op_data->len; i++) {
        op = (GstMfxFilterOpData *)g_ptr_array_index(filter->filter_op_data, i);
        if(type == op->type)
            return op;
    }

    return NULL;
}

static void
free_filter_op_data(gpointer data)
{
    GstMfxFilterOpData *op = (GstMfxFilterOpData *)data;
    g_slice_free1(op->size, op->filter);
    g_slice_free(GstMfxFilterOpData, op);
}

gboolean
gst_mfx_filter_has_filter(GstMfxFilter * filter, guint flags)
{
    g_return_val_if_fail(filter != NULL, FALSE);

    return (filter->supported_filters & flags) != 0;
}

static void
gst_mfx_filter_init(GstMfxFilter * filter,
	GstMfxTaskAggregator * aggregator,
	gboolean mapped_in, gboolean mapped_out)
{
    filter->params.IOPattern |= mapped_in ?
        MFX_IOPATTERN_IN_SYSTEM_MEMORY : MFX_IOPATTERN_IN_VIDEO_MEMORY;
    filter->params.IOPattern |= mapped_out ?
        MFX_IOPATTERN_OUT_SYSTEM_MEMORY : MFX_IOPATTERN_OUT_VIDEO_MEMORY;
	filter->aggregator = gst_mfx_task_aggregator_ref(aggregator);

    /* Initialize the array of operation data */
    filter->filter_op_data =
        g_ptr_array_new_with_free_func(free_filter_op_data);

    /* Initialize the filter flag */
    filter->filter_op = GST_MFX_FILTER_NONE;
}

static void
gst_mfx_filter_finalize(GstMfxFilter * filter)
{
    guint i;

	for (i = 0; i < 2; i++) {
        if (!filter->vpp_request[0])
            continue;

        gst_mfx_task_replace(&filter->vpp[i], NULL);
        gst_mfx_surface_pool_replace(&filter->vpp_pool[i], NULL);
	}

    /* Free allocated memory for filters */
    g_slice_free1((sizeof(mfxU32) * filter->vpp_use.NumAlg),
            filter->vpp_use.AlgList);

    g_slice_free1((sizeof(mfxExtBuffer *) * filter->params.NumExtParam),
            filter->ext_buffer);

    g_ptr_array_free(filter->filter_op_data, TRUE);

    MFXVideoVPP_Close(filter->session);
	gst_mfx_task_aggregator_unref(filter->aggregator);
}

static inline const GstMfxMiniObjectClass *
gst_mfx_filter_class(void)
{
	static const GstMfxMiniObjectClass GstMfxFilterClass = {
		sizeof (GstMfxFilter),
		(GDestroyNotify)gst_mfx_filter_finalize
	};
	return &GstMfxFilterClass;
}

GstMfxFilter *
gst_mfx_filter_new(GstMfxTaskAggregator * aggregator,
	gboolean mapped_in, gboolean mapped_out)
{
	GstMfxFilter *filter;

	g_return_val_if_fail(aggregator != NULL, NULL);

	filter = (GstMfxFilter *)
		gst_mfx_mini_object_new0(gst_mfx_filter_class());
	if (!filter)
		return NULL;

	gst_mfx_filter_init(filter, aggregator, mapped_in, mapped_out);
	return filter;
}

GstMfxFilter *
gst_mfx_filter_new_with_task(GstMfxTaskAggregator * aggregator,
	GstMfxTask * task, GstMfxTaskType type,
	gboolean mapped_in, gboolean mapped_out)
{
	GstMfxFilter *filter;

	g_return_val_if_fail(aggregator != NULL, NULL);
	g_return_val_if_fail(task != NULL, NULL);

	filter = (GstMfxFilter *)
		gst_mfx_mini_object_new0(gst_mfx_filter_class());
	if (!filter)
		return NULL;

    filter->session = gst_mfx_task_get_session(task);
    filter->vpp[type & GST_MFX_TASK_VPP_OUT] = gst_mfx_task_ref(task);

    gst_mfx_task_set_task_type(task, type);

	gst_mfx_filter_init(filter, aggregator, mapped_in, mapped_out);
	return filter;
}


GstMfxFilter *
gst_mfx_filter_ref(GstMfxFilter * filter)
{
	g_return_val_if_fail(filter != NULL, NULL);

	return
		GST_MFX_FILTER(gst_mfx_mini_object_ref(GST_MFX_MINI_OBJECT (filter)));
}

void
gst_mfx_filter_unref(GstMfxFilter * filter)
{
	g_return_if_fail(filter != NULL);

	gst_mfx_mini_object_unref(GST_MFX_MINI_OBJECT(filter));
}


void
gst_mfx_filter_replace(GstMfxFilter ** old_filter_ptr,
    GstMfxFilter * new_filter)
{
	g_return_if_fail(old_filter_ptr != NULL);

	gst_mfx_mini_object_replace((GstMfxMiniObject **)old_filter_ptr,
		GST_MFX_MINI_OBJECT(new_filter));
}

GstMfxSurfacePool *
gst_mfx_filter_get_pool(GstMfxFilter * filter, guint flags)
{
    return filter->vpp_pool[flags & GST_MFX_TASK_VPP_OUT];
}

gboolean
gst_mfx_filter_set_format(GstMfxFilter * filter, GstVideoFormat fmt)
{
    g_return_val_if_fail (filter != NULL, FALSE);

    if (GST_VIDEO_FORMAT_NV12 == fmt || GST_VIDEO_FORMAT_BGRA == fmt)
        filter->fourcc = gst_video_format_to_mfx_fourcc(fmt);
    else
        return FALSE;

	return TRUE;
}

gboolean
gst_mfx_filter_set_size(GstMfxFilter * filter, mfxU16 width, mfxU16 height)
{
    g_return_val_if_fail (filter != NULL, FALSE);

	filter->width = width;
	filter->height = height;

	return TRUE;
}

static gpointer init_deinterlacing_default()
{
    mfxExtVPPDeinterlacing *ext_deinterlacing;
    ext_deinterlacing = g_slice_alloc0(sizeof(mfxExtVPPDeinterlacing));
    if (!ext_deinterlacing)
        return NULL;
    ext_deinterlacing->Header.BufferId = MFX_EXTBUFF_VPP_DEINTERLACING;
    ext_deinterlacing->Header.BufferSz = sizeof(mfxExtVPPDeinterlacing);
    ext_deinterlacing->Mode = MFX_DEINTERLACING_ADVANCED; //Set as default

    return ext_deinterlacing;
}

static gpointer init_procamp_default()
{
    mfxExtVPPProcAmp *ext_procamp;
    ext_procamp = g_slice_alloc0(sizeof(mfxExtVPPProcAmp));
    if (!ext_procamp)
        return NULL;
    ext_procamp->Header.BufferId = MFX_EXTBUFF_VPP_PROCAMP;
    ext_procamp->Header.BufferSz = sizeof(mfxExtVPPProcAmp);
    ext_procamp->Brightness = 0.0;
    ext_procamp->Contrast = 1.0;
    ext_procamp->Hue = 0.0;
    ext_procamp->Saturation = 1.0;

    return ext_procamp;
}

static gpointer init_denoise_default()
{
    mfxExtVPPDenoise *ext_denoise;
    ext_denoise = g_slice_alloc0(sizeof(mfxExtVPPDenoise));
    if (!ext_denoise)
        return NULL;
    ext_denoise->Header.BufferId = MFX_EXTBUFF_VPP_DENOISE;
    ext_denoise->Header.BufferSz = sizeof(mfxExtVPPDenoise);
    ext_denoise->DenoiseFactor = 0;

    return ext_denoise;
}

static gpointer init_detail_default()
{
    mfxExtVPPDetail *ext_detail;
    ext_detail = g_slice_alloc0(sizeof(mfxExtVPPDetail));
    if (!ext_detail)
        return NULL;
    ext_detail->Header.BufferId = MFX_EXTBUFF_VPP_DETAIL;
    ext_detail->Header.BufferSz = sizeof(mfxExtVPPDetail);
    ext_detail->DetailFactor = 0;

    return ext_detail;
}

static gpointer init_rotation_default()
{
    mfxExtVPPRotation *ext_rotation;
    ext_rotation = g_slice_alloc0(sizeof(mfxExtVPPRotation));
    if (!ext_rotation)
        return NULL;
    ext_rotation->Header.BufferId = MFX_EXTBUFF_VPP_ROTATION;
    ext_rotation->Header.BufferSz = sizeof(mfxExtVPPRotation);
    ext_rotation->Angle = MFX_ANGLE_0;

    return ext_rotation;
}

static gpointer init_frc_default()
{
    mfxExtVPPFrameRateConversion *ext_frc;
    ext_frc = g_slice_alloc0(sizeof(mfxExtVPPFrameRateConversion));
    if (!ext_frc)
        return NULL;
    ext_frc->Header.BufferId = MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION;
    ext_frc->Header.BufferSz = sizeof(mfxExtVPPFrameRateConversion);
    ext_frc->Algorithm = 0;

    return ext_frc;

}

gboolean
gst_mfx_filter_set_saturation(GstMfxFilter * filter, gfloat value)
{
    GstMfxFilterOpData *op;
    mfxExtVPPProcAmp *ext_procamp;

    g_return_val_if_fail(filter !=  NULL, FALSE);
    g_return_val_if_fail(value <=  10.0, FALSE);
    g_return_val_if_fail(value >=  0.0, FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_PROCAMP);
    if( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_PROCAMP;
        filter->filter_op |= GST_MFX_FILTER_PROCAMP;
        op->size = sizeof(mfxExtVPPProcAmp);
        op->filter = init_procamp_default();
        if( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }
    ext_procamp = (mfxExtVPPProcAmp *)op->filter;
    ext_procamp->Saturation = value;

    return TRUE;
}

gboolean
gst_mfx_filter_set_brightness(GstMfxFilter * filter, gfloat value)
{
    GstMfxFilterOpData *op;
    mfxExtVPPProcAmp *ext_procamp;

    g_return_val_if_fail(filter !=  NULL, FALSE);
    g_return_val_if_fail(value <=  100.0, FALSE);
    g_return_val_if_fail(value >=  -100.0, FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_PROCAMP);
    if( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_PROCAMP;
        filter->filter_op |= GST_MFX_FILTER_PROCAMP;
        op->size = sizeof(mfxExtVPPProcAmp);
        op->filter = init_procamp_default();
        if( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_procamp = (mfxExtVPPProcAmp *)op->filter;
    ext_procamp->Brightness = value;

    return TRUE;
}

gboolean
gst_mfx_filter_set_contrast(GstMfxFilter * filter, gfloat value)
{
    GstMfxFilterOpData *op;
    mfxExtVPPProcAmp *ext_procamp;

    g_return_val_if_fail(filter !=  NULL, FALSE);
    g_return_val_if_fail(value <=  10.0, FALSE);
    g_return_val_if_fail(value >=  0.0, FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_PROCAMP);
    if( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_PROCAMP;
        filter->filter_op |= GST_MFX_FILTER_PROCAMP;
        op->size = sizeof(mfxExtVPPProcAmp);
        op->filter = init_procamp_default();
        if( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_procamp = (mfxExtVPPProcAmp *)op->filter;
    ext_procamp->Contrast = value;

    return TRUE;
}

gboolean
gst_mfx_filter_set_hue(GstMfxFilter * filter, gfloat value)
{
    GstMfxFilterOpData *op;
    mfxExtVPPProcAmp *ext_procamp;

    g_return_val_if_fail(filter !=  NULL, FALSE);
    g_return_val_if_fail(value <=  180.0, FALSE);
    g_return_val_if_fail(value >=  -180.0, FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_PROCAMP);
    if( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_PROCAMP;
        filter->filter_op |= GST_MFX_FILTER_PROCAMP;
        op->size = sizeof(mfxExtVPPProcAmp);
        op->filter = init_procamp_default();
        if( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_procamp = (mfxExtVPPProcAmp *)op->filter;
    ext_procamp->Hue = value;

    return TRUE;
}

gboolean
gst_mfx_filter_set_denoising_level(GstMfxFilter * filter, guint level)
{
    GstMfxFilterOpData *op;
    mfxExtVPPDenoise *ext_denoise;

    g_return_val_if_fail(filter != NULL, FALSE);
    g_return_val_if_fail(level <= 100, FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_DENOISE);
    if ( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_DENOISE;
        filter->filter_op |= GST_MFX_FILTER_DENOISE;
        op->size = sizeof(mfxExtVPPDenoise);
        op->filter = init_denoise_default();
        if( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_denoise = (mfxExtVPPDenoise *)op->filter;
    ext_denoise->DenoiseFactor = level;

    return TRUE;
}

gboolean
gst_mfx_filter_set_detail_level(GstMfxFilter * filter, guint level)
{
    GstMfxFilterOpData *op;
    mfxExtVPPDetail *ext_detail;
    g_return_val_if_fail(filter != NULL, FALSE);
    g_return_val_if_fail(level <= 100, FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_DETAIL);
    if ( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_DETAIL;
        filter->filter_op |= GST_MFX_FILTER_DETAIL;
        op->size = sizeof(mfxExtVPPDetail);
        op->filter = init_detail_default();
        if ( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_detail = (mfxExtVPPDetail *)op->filter;
    ext_detail->DetailFactor = level;

    return TRUE;
}

gboolean
gst_mfx_filter_set_rotation(GstMfxFilter * filter, GstMfxRotation angle)
{
    GstMfxFilterOpData *op;
    mfxExtVPPRotation *ext_rotation;

    g_return_val_if_fail(filter != NULL, FALSE);
    g_return_val_if_fail((angle == 0 ||
                angle == 90 ||
                angle == 180 ||
                angle == 270), FALSE);

    op = find_filter_op_data(filter, GST_MFX_FILTER_ROTATION);
    if ( NULL == op ) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_ROTATION;
        filter->filter_op |= GST_MFX_FILTER_ROTATION;
        op->size = sizeof(mfxExtVPPRotation);
        op->filter = init_rotation_default();
        if ( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_rotation = (mfxExtVPPRotation *)op->filter;
    ext_rotation->Angle = angle;

    return TRUE;
}

gboolean
gst_mfx_filter_set_deinterlace_mode(GstMfxFilter *filter,
        GstMfxDeinterlaceMode mode)
{
    GstMfxFilterOpData *op;
    mfxExtVPPDeinterlacing *ext_deinterlacing;
    guint16 alg;

    g_return_val_if_fail(filter != NULL, FALSE);
    g_return_val_if_fail((GST_MFX_DEINTERLACE_MODE_NONE == mode ||
            GST_MFX_DEINTERLACE_MODE_BOB == mode ||
            GST_MFX_DEINTERLACE_MODE_ADVANCED == mode ||
            GST_MFX_DEINTERLACE_MODE_ADVANCED_NOREF == mode), FALSE);

    switch(mode) {
        case GST_MFX_DEINTERLACE_MODE_NONE:
            alg = 0;
            break;
        case GST_MFX_DEINTERLACE_MODE_BOB:
            alg = MFX_DEINTERLACING_BOB;
            break;
        case GST_MFX_DEINTERLACE_MODE_ADVANCED:
            alg = MFX_DEINTERLACING_ADVANCED;
            break;
        case GST_MFX_DEINTERLACE_MODE_ADVANCED_NOREF:
            alg = MFX_DEINTERLACING_ADVANCED_NOREF;
            break;
        default:
            alg =0;
            break;
    }

    op = find_filter_op_data(filter, GST_MFX_FILTER_DEINTERLACING);
    if (NULL == op) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op )
            return FALSE;
        op->type = GST_MFX_FILTER_DEINTERLACING;
        filter->filter_op |= GST_MFX_FILTER_DEINTERLACING;
        op->size = sizeof(mfxExtVPPDeinterlacing);
        op->filter = init_deinterlacing_default();
        if ( NULL == op->filter ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_deinterlacing = (mfxExtVPPDeinterlacing *)op->filter;
    ext_deinterlacing->Mode = alg;

    return TRUE;
}

gboolean
gst_mfx_filter_set_framerate(GstMfxFilter *filter,
        guint16 fps_n, guint16 fps_d)
{
    g_return_val_if_fail(filter != NULL, FALSE);
    g_return_val_if_fail((0 != fps_n && 0 != fps_d), FALSE);

    filter->fps_n = fps_n;
    filter->fps_d = fps_d;

    return TRUE;
}

gboolean
gst_mfx_filter_set_frc_algorithm(GstMfxFilter *filter,
        GstMfxFrcAlgorithm alg)
{
    GstMfxFilterOpData *op;
    mfxExtVPPFrameRateConversion *ext_frc;
    guint16 mode;

    g_return_val_if_fail(filter != NULL, FALSE);
    g_return_val_if_fail((GST_MFX_FRC_NONE == alg||
                GST_MFX_FRC_PRESERVE_TIMESTAMP == alg ||
                GST_MFX_FRC_DISTRIBUTED_TIMESTAMP == alg),
            FALSE);
    /*g_return_val_if_fail((GST_MFX_FRC_NONE == alg||
                GST_MFX_FRC_PRESERVE_TIMESTAMP == alg ||
                GST_MFX_FRC_DISTRIBUTED_TIMESTAMP == alg ||
                GST_MFX_FRC_FRAME_INTERPOLATION == alg ||
                GST_MFX_FRC_FI_PRESERVE_TIMESTAMP == alg ||
                GST_MFX_FRC_FI_DISTRIBUTED_TIMESTAMP == alg),
            FALSE);*/

    switch(alg) {
        case GST_MFX_FRC_NONE:
            mode = 0;
            break;
        case GST_MFX_FRC_PRESERVE_TIMESTAMP:
            mode = MFX_FRCALGM_PRESERVE_TIMESTAMP;
            break;
        case GST_MFX_FRC_DISTRIBUTED_TIMESTAMP:
            mode = MFX_FRCALGM_DISTRIBUTED_TIMESTAMP;
            break;
        /*case GST_MFX_FRC_FRAME_INTERPOLATION:
            mode = MFX_FRCALGM_FRAME_INTERPOLATION;
            break;
        case GST_MFX_FRC_FI_PRESERVE_TIMESTAMP:
            mode = MFX_FRCALGM_PRESERVE_TIMESTAMP ||
                MFX_FRCALGM_FRAME_INTERPOLATION;
            break;
        case GST_MFX_FRC_FI_DISTRIBUTED_TIMESTAMP:
            mode = MFX_FRCALGM_DISTRIBUTED_TIMESTAMP ||
                MFX_FRCALGM_FRAME_INTERPOLATION;
            break;*/
        default:
            mode = 0;
            break;
    }
    op = find_filter_op_data(filter, GST_MFX_FILTER_FRAMERATE_CONVERSION);
    if (NULL == op) {
        op = g_slice_alloc(sizeof(GstMfxFilterOpData));
        if ( NULL == op  )
            return FALSE;
        op->type = GST_MFX_FILTER_FRAMERATE_CONVERSION;
        filter->filter_op = GST_MFX_FILTER_FRAMERATE_CONVERSION;
        op->size = sizeof(mfxExtVPPFrameRateConversion);
        op->filter = init_frc_default();
        if ( NULL == op->filter  ) {
            g_slice_free(GstMfxFilterOpData, op);
            return FALSE;
        }
        g_ptr_array_add(filter->filter_op_data, op);
    }

    ext_frc = (mfxExtVPPFrameRateConversion *)op->filter;
    ext_frc->Algorithm = mode;
    return TRUE;
}

GstMfxFilterStatus
gst_mfx_filter_process(GstMfxFilter * filter, GstMfxSurfaceProxy *proxy,
	GstMfxSurfaceProxy ** out_proxy)
{
	mfxFrameSurface1 *insurf, *outsurf = NULL;
	mfxSyncPoint syncp;
	mfxStatus sts = MFX_ERR_NONE;
    gboolean more_surface = FALSE;

	do {
		*out_proxy = gst_mfx_surface_proxy_new_from_pool(filter->vpp_pool[1]);
		if (!*out_proxy)
			return GST_MFX_FILTER_STATUS_ERROR_ALLOCATION_FAILED;

		insurf = gst_mfx_surface_proxy_get_frame_surface(proxy);
		outsurf = gst_mfx_surface_proxy_get_frame_surface(*out_proxy);
		sts = MFXVideoVPP_RunFrameVPPAsync(filter->session, insurf, outsurf, NULL, &syncp);
		if (MFX_WRN_DEVICE_BUSY == sts)
			g_usleep(500);
	} while (MFX_WRN_DEVICE_BUSY == sts);

    if (MFX_ERR_MORE_DATA == sts)
        return GST_MFX_FILTER_STATUS_ERROR_MORE_DATA;

    /* The current frame is ready. Hence treat it
     * as MFX_ERR_NONE and request for more surface
     */
    if (MFX_ERR_MORE_SURFACE == sts) {
         sts = MFX_ERR_NONE;
         more_surface = TRUE;
    }

	if (MFX_ERR_NONE != sts) {
		GST_ERROR("Error during MFX filter process.");
		return GST_MFX_FILTER_STATUS_ERROR_OPERATION_FAILED;
	}

	if (syncp) {
		do {
			sts = MFXVideoCORE_SyncOperation(filter->session, syncp, 1000);
		} while (MFX_WRN_IN_EXECUTION == sts);

        *out_proxy = gst_mfx_surface_pool_find_proxy(filter->vpp_pool[1], outsurf);
	}

    if(more_surface)
        return GST_MFX_FILTER_STATUS_ERROR_MORE_SURFACE;
	return GST_MFX_FILTER_STATUS_SUCCESS;
}
