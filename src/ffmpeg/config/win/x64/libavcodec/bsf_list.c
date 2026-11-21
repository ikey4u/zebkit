static const FFBitStreamFilter * const bitstream_filters[] = {
    &ff_aac_adtstoasc_bsf,
    &ff_extract_extradata_bsf,
    &ff_h264_mp4toannexb_bsf,
    &ff_pgs_frame_merge_bsf,
    &ff_vp9_superframe_bsf,
    &ff_source_bsf,
    &ff_sink_bsf,
    NULL };
