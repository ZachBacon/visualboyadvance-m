#include "ffmpeg.h"

#define STREAM_FRAME_RATE 60
#define STREAM_PIXEL_FORMAT AV_PIX_FMT_YUV420P


MediaRecorder::MediaRecorder() : isRecording(false), sws(NULL), st(NULL),
    enc(NULL), npts(0), frameIn(NULL), frameOut(NULL), fmt(NULL),
    oc(NULL), vcodec(NULL)
{

    // audio
    swr = NULL;
    acodec = NULL;
    ast = NULL;
    aenc = NULL;
    next_pts = 0;
    samples_count = 0;
    audioframe = NULL;
    audioframeTmp = NULL;
}

// video : calls setup_audio, setup_video, finish_setup
MediaRet MediaRecorder::Record(const char *fname, int width, int height, int depth)
{
    if (isRecording) return MRET_ERR_RECORDING;
    isRecording = true;

    switch (depth)
    {
        case 16:
            // FIXME: test & make endian-neutral
            pixfmt = AV_PIX_FMT_RGB565LE;
            break;
        case 24:
            pixfmt = AV_PIX_FMT_RGB24;
            break;
        case 32:
        default: // should never be anything else
            pixfmt = AV_PIX_FMT_RGBA;
            break;
    }


    sws = sws_getContext(width, height, pixfmt, // from
                         width, height, STREAM_PIXEL_FORMAT, // to
                         SWS_BICUBIC, NULL, NULL, NULL); // params
    if (!sws) return MRET_ERR_BUFSIZE;



    pixsize = depth >> 3;
    linesize = pixsize * width;
    switch(pixsize) {
    case 2:
        //    16-bit: 2 @ right, 1 @ top
        tbord = 1; rbord = 2; break;
    case 3:
        //    24-bit: no border
        tbord = rbord = 0; break;
    case 4:
        //    32-bit: 1 @ right, 1 @ top
        tbord = 1; rbord = 1; break;
    }


    avformat_alloc_output_context2(&oc, NULL, NULL, fname);
    if (!oc) return MRET_ERR_BUFSIZE;
    fmt = oc->oformat;





    ////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////
    // audio
    acodec = avcodec_find_encoder(fmt->audio_codec);
    if (!acodec) return MRET_ERR_FMTGUESS;
    ast = avformat_new_stream(oc, NULL);
    if (!ast) return MRET_ERR_NOMEM;
    ast->id = oc->nb_streams - 1;
    aenc = avcodec_alloc_context3(acodec);
    //add_stream
    aenc->sample_fmt = (acodec)->sample_fmts ? (acodec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    aenc->bit_rate = 128000;
    aenc->sample_rate = 44100;

    if (acodec->supported_samplerates)
    {
        aenc->sample_rate = acodec->supported_samplerates[0];
        for (int i = 0; acodec->supported_samplerates[i]; ++i)
        {
            if (acodec->supported_samplerates[i] == 44100)
                aenc->sample_rate = 44100;
        }
    }
    aenc->channels = av_get_channel_layout_nb_channels(aenc->channel_layout);
    aenc->channel_layout = AV_CH_LAYOUT_STEREO;
    if (acodec->channel_layouts) {
        aenc->channel_layout = acodec->channel_layouts[0];
        for (int i = 0; acodec->channel_layouts[i]; ++i)
        {
            if (acodec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                aenc->channel_layout = AV_CH_LAYOUT_STEREO;
        }
    }
    aenc->channels = av_get_channel_layout_nb_channels(aenc->channel_layout);
    aenc->time_base = (AVRational){ 1, aenc->sample_rate };
    ast->time_base = (AVRational){ 1, aenc->sample_rate };
    // open_audio

    AVCodecContext *c;
    int nb_samples;
    AVDictionary *opt = NULL;

    c = aenc;
    if (avcodec_open2(c, acodec, &opt) < 0)
    {
        fprintf(stderr, "Could not open audio codec\n");
        return MRET_ERR_NOCODEC;
    }
    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;

    // alloc_audio_frame
    audioframe = av_frame_alloc();
    if (!audioframe) {
        fprintf(stderr, "Error allocating an audio frame\n");
        return MRET_ERR_NOMEM;
    }
    audioframe->format = c->sample_fmt;
    audioframe->channel_layout = c->channel_layout;
    audioframe->sample_rate = c->sample_rate;
    audioframe->nb_samples = nb_samples;
    if (nb_samples) {
        if (av_frame_get_buffer(audioframe, 0) < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            return MRET_ERR_NOMEM;
        }
    }
    // alloc_audio_frame
    audioframeTmp = av_frame_alloc();
    if (!audioframeTmp) {
        fprintf(stderr, "Error allocating an audio frame\n");
        return MRET_ERR_NOMEM;
    }
    audioframeTmp->format = AV_SAMPLE_FMT_S16;
    audioframeTmp->channel_layout = c->channel_layout;
    audioframeTmp->sample_rate = c->sample_rate;
    audioframeTmp->nb_samples = nb_samples;
    if (nb_samples) {
        if (av_frame_get_buffer(audioframeTmp, 0) < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            return MRET_ERR_NOMEM;
        }
    }
    // end

    if (avcodec_parameters_from_context(ast->codecpar, c) < 0)
    {
        fprintf(stderr, "Could not copy the stream parameters\n");
        return MRET_ERR_BUFSIZE;
    }

    swr = swr_alloc();
    if (!swr) {
        fprintf(stderr, "Could not allocate resampler context\n");
        return MRET_ERR_BUFSIZE;
    }

    /* set options */
    av_opt_set_int       (swr, "in_channel_count",   c->channels,       0);
    av_opt_set_int       (swr, "in_sample_rate",     c->sample_rate,    0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
    av_opt_set_int       (swr, "out_channel_count",  c->channels,       0);
    av_opt_set_int       (swr, "out_sample_rate",    c->sample_rate,    0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt",     c->sample_fmt,     0);

    /* initialize the resampling context */
    if ((swr_init(swr)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        return MRET_ERR_BUFSIZE;
    }

    ////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////

    vcodec = avcodec_find_encoder(fmt->video_codec);
    if (!vcodec) return MRET_ERR_FMTGUESS;
    st = avformat_new_stream(oc, NULL);
    if (!st) return MRET_ERR_NOMEM;
    st->id = oc->nb_streams - 1;
    enc = avcodec_alloc_context3(vcodec);
    enc->codec_id = fmt->video_codec;
    enc->bit_rate = 400000;
    enc->width = width;
    enc->height = height;
    st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
    enc->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
    enc->gop_size = 12;
    enc->pix_fmt = STREAM_PIXEL_FORMAT;
    if (enc->codec_id == AV_CODEC_ID_MPEG2VIDEO)
    {
        enc->max_b_frames = 2;
    }
    if (enc->codec_id == AV_CODEC_ID_MPEG1VIDEO)
    {
        enc->mb_decision = 2;
    }


    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;




    opt = NULL;

    if (avcodec_open2(enc, vcodec, &opt) < 0)
    {
        fprintf(stderr, "Could not open video codec...\n");
        return MRET_ERR_NOCODEC;
    }


    frameIn = av_frame_alloc();
    if (!frameIn) {
        fprintf(stderr, "Could not allocate video frame\n");
        return MRET_ERR_NOMEM;
    }
    frameIn->format = pixfmt;
    frameIn->width  = width;
    frameIn->height = height;
    if (av_frame_get_buffer(frameIn, 32) < 0)
    {
        fprintf(stderr, "Could not av_frame_get_buffer for frameIn\n");
        return MRET_ERR_NOMEM;
    }

    frameOut = av_frame_alloc();
    if (!frameOut) {
        fprintf(stderr, "Could not allocate video frame\n");
        return MRET_ERR_NOMEM;
    }
    frameOut->format = STREAM_PIXEL_FORMAT;
    frameOut->width  = width;
    frameOut->height = height;
    if (av_frame_get_buffer(frameOut, 32) < 0)
    {
        fprintf(stderr, "Could not av_frame_get_buffer for frameOut\n");
        return MRET_ERR_NOMEM;
    }


    if (avcodec_parameters_from_context(st->codecpar, enc) < 0)
    {
        fprintf(stderr, "Could not copy the stream parameters\n");
        return MRET_ERR_BUFSIZE;
    }




    av_dump_format(oc, 0, fname, 1);

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&oc->pb, fname, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s'\n", fname);
            return MRET_ERR_FERR;
        }
    }


    /* Write the stream header, if any. */
    if (avformat_write_header(oc, &opt) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return MRET_ERR_FERR;
    }





    return MRET_OK;
}

MediaRet MediaRecorder::AddFrame(const uint8_t *vid)
{
    if (!isRecording) return MRET_OK;



    AVCodecContext *c = enc;
    int got_packet = 0, ret = 0;
    AVPacket pkt = { 0 };


    if (av_frame_make_writable(frameOut) < 0)
    {
        return MRET_ERR_RECORDING;
    }

/***************************/

if (av_image_fill_arrays(frameIn->data, frameIn->linesize,
    (uint8_t *)vid + tbord * (linesize + pixsize * rbord),
    pixfmt, c->width + rbord, c->height, 1) < 0)
{
    fprintf(stderr, "Error av_image_fill_arrays\n");
    return MRET_ERR_RECORDING;
}

/***************************/

    sws_scale(sws, (const uint8_t * const *) frameIn->data,
              frameIn->linesize, 0, c->height, frameOut->data,
              frameOut->linesize);

    frameOut->pts = npts++;


    av_init_packet(&pkt);

    got_packet = avcodec_receive_packet(c, &pkt);
    ret = avcodec_send_frame(c, frameOut);
    if (ret < 0) {
        fprintf(stderr, "Error encoding video frame\n");
        if (ret == AVERROR(EAGAIN))
        {
            fprintf(stderr, "input is not accepted in the current state - user must read output with avcodec_receive_packet()\n");
        }
        else if (ret == AVERROR(EOF))
        {
            fprintf(stderr, "the encoder has been flushed, and no new frames can be sent to it\n");
        }
        else if (ret == AVERROR(EINVAL))
        {
            fprintf(stderr, "codec not opened, refcounted_frames not set, it is a decoder, or requires flush\n");
        }
        else if (ret == AVERROR(ENOMEM))
        {
            fprintf(stderr, "failed to add packet to internal queue, or similar other errors: legitimate decoding errors\n");
        }
        return MRET_ERR_RECORDING;
    }

    if (!got_packet)
    {
        /* rescale output packet timestamp values from codec to stream timebase */
        //av_packet_rescale_ts(&pkt, (AVRational)&c->time_base, st->time_base);
        av_packet_rescale_ts(&pkt, (AVRational)c->time_base, st->time_base);
        pkt.stream_index = st->index;
        /* Write the compressed frame to the media file. */
        //log_packet(oc, &pkt);
        ret = av_interleaved_write_frame(oc, &pkt);
        if (ret < 0)
        {
            fprintf(stderr, "failed to av_interleaved_write_frame\n");
            return MRET_ERR_RECORDING;
        }
    }


    return MRET_OK;
}

void MediaRecorder::Stop()
{
    isRecording = false;
    if (sws)
    {
        sws_freeContext(sws);
        sws = NULL;
    }
    if (st)
    {
        st = NULL;
    }
    if (enc)
    {
        avcodec_free_context(&enc);
        avcodec_close(enc);
        enc = NULL;
    }
    if (frameIn)
    {
        av_frame_free(&frameIn);
        frameIn = NULL;
    }
    if (frameOut)
    {
        av_frame_free(&frameOut);
        frameOut = NULL;
    }
    if (vcodec)
    {
        vcodec = NULL;
    }
    npts = 0;
    if (oc)
    {
        /* Write the trailer, if any. The trailer must be written before you
         * close the CodecContexts open when you wrote the header; otherwise
         * av_write_trailer() may try to use memory that was freed on
         * av_codec_close(). */
        av_write_trailer(oc);

        /* Close the output file. */
        if (!(fmt->flags & AVFMT_NOFILE))
        {
            avio_closep(&oc->pb);
        }
        fmt = NULL;

        avformat_free_context(oc);
        oc = NULL;
    }
}

// audio
MediaRet MediaRecorder::Record(const char *fname)
{
    if (isRecording) return MRET_ERR_RECORDING;
    return MRET_OK;
}

MediaRet MediaRecorder::AddFrame(const uint16_t *aud)
{
    if (!isRecording) return MRET_OK;

    // write_audio_frame
    AVCodecContext *c;
    AVPacket pkt = { 0 }; // data and size must be 0;
    AVFrame *frame;
    int ret;
    int got_packet;
    int dst_nb_samples;

    av_init_packet(&pkt);
    c = aenc;
    //// get_audio_frame

/*
    if (av_samples_fill_arrays(audioframeTmp->data, NULL, (uint8_t *)aud, aenc->channels, audioframeTmp->nb_samples, AV_SAMPLE_FMT_S16, 0) < 0)
    {
        fprintf(stderr, "Fail on av_samples_fill_arrays\n");
        return MRET_ERR_RECORDING;
    }
*/
    int samples_size = av_samples_get_buffer_size(NULL, aenc->channels, audioframeTmp->nb_samples, c->sample_fmt, 1);
    if (avcodec_fill_audio_frame(audioframeTmp, aenc->channels, aenc->sample_fmt, (uint8_t *)aud, samples_size, 0) < 0)
    {
        fprintf(stderr, "Fail on avcodec_fill_audio_frame\n");
        return MRET_ERR_RECORDING;
    }
    ////

    dst_nb_samples = av_rescale_rnd(swr_get_delay(swr, c->sample_rate) + audioframeTmp->nb_samples, c->sample_rate, c->sample_rate, AV_ROUND_UP);
    av_assert0(dst_nb_samples == audioframeTmp->nb_samples);

    if (av_frame_make_writable(audioframe) < 0)
    {
        fprintf(stderr, "Fail on av_frame_make_writable(audioframe)\n");
        return MRET_ERR_RECORDING;
    }

    if (swr_convert(swr, audioframe->data, dst_nb_samples, (const uint8_t **)audioframeTmp->data, audioframeTmp->nb_samples) < 0)
    {
        fprintf(stderr, "Fail on swr_convert\n");
        return MRET_ERR_RECORDING;
    }

    frame = audioframe;
    frame->pts = av_rescale_q(samples_count, ast->time_base, aenc->time_base);
    samples_count += dst_nb_samples;


    got_packet = avcodec_receive_packet(c, &pkt);
    if (avcodec_send_frame(c, frame) < 0) {
        fprintf(stderr, "Error encoding audio frame\n");
        return MRET_ERR_RECORDING;
    }

    if (!got_packet) {
        av_packet_rescale_ts(&pkt, c->time_base, ast->time_base);
        pkt.stream_index = ast->index;
        ret = av_interleaved_write_frame(oc, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error while writing audio frame\n");
            return MRET_ERR_RECORDING;
        }
    }
    //
    return MRET_OK;
}

MediaRecorder::~MediaRecorder()
{
    Stop();
}

MediaRet MediaRecorder::setup_sound_stream(const char *fname, AVOutputFormat *fmt)
{
    return MRET_OK;
}

MediaRet MediaRecorder::setup_video_stream(const char *fname, int w, int h, int d)
{
    return MRET_OK;
}

MediaRet MediaRecorder::finish_setup(const char *fname)
{
    return MRET_OK;
}
