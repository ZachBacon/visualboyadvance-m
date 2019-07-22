#ifndef WX_FFMPEG_H
#define WX_FFMPEG_H

// simplified interface for recording audio and/or video from emulator

// required for ffmpeg
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// return codes
// probably ought to put in own namespace, but this is good enough
enum MediaRet {
        MRET_OK,            // no errors
        MRET_ERR_NOMEM,     // error allocating buffers or structures
        MRET_ERR_NOCODEC,   // error opening codec
        MRET_ERR_FERR,      // error writing output file
        MRET_ERR_RECORDING, // attempt to start recording when already doing it
        MRET_ERR_FMTGUESS,  // can't guess format from file name
        MRET_ERR_BUFSIZE    // buffer overflow (fatal)
};

class MediaRecorder
{
        public:
        MediaRecorder();
        virtual ~MediaRecorder();

        // start audio+video (also video-only codecs)
        MediaRet Record(const char *fname, int width, int height, int depth);
        // start audio only
        MediaRet Record(const char *fname);
        // stop both
        void Stop();
        bool IsRecording()
        {
                return isRecording;
        }
        // add a frame of video; width+height+depth already given
        // assumes a 1-pixel border on top & right
        // always assumes being passed 1/60th of a second of video
        MediaRet AddFrame(const uint8_t *vid);
        // add a frame of audio; uses current sample rate to know length
        // always assumes being passed 1/60th of a second of audio.
        MediaRet AddFrame(const uint16_t *aud);

        private:
        bool isRecording;

        AVPixelFormat pixfmt;
        struct SwsContext *sws;


        AVStream *st;
        AVCodecContext *enc;


        int64_t npts;


        AVFrame *frameIn;
        AVFrame *frameOut;


        AVOutputFormat *fmt;
        AVFormatContext *oc;


        AVCodec *vcodec;


        int pixsize;
        int linesize;
        int tbord, rbord;


        // audio
        struct SwrContext *swr;
        AVCodec *acodec;
        AVStream *ast;
        AVCodecContext *aenc;
        int64_t next_pts;
        int samples_count;
        AVFrame *audioframe;
        AVFrame *audioframeTmp;

        int frame_len, sample_len;


        MediaRet setup_sound_stream(const char *fname, AVOutputFormat *fmt);
        MediaRet setup_video_stream(const char *fname, int w, int h, int d);
        MediaRet finish_setup(const char *fname);
};

#endif /* WX_FFMPEG_H */
