// tutorial01.c
// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101
// on GCC 4.7.2 in Debian February 2015

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use
//
// gcc -o tutorial01 tutorial01.c -lavformat -lavcodec -lswscale -lz
//
// to build (assuming libavformat and libavcodec are correctly installed
// your system).
//
// Run using
//
// tutorial01 myvideofile.mpg
//
// to write the first five frames from "myvideofile.mpg" to disk in PPM
// format.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>

#include <stdio.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename, "wb");
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}

static void ppm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename, "wb");
    fprintf(f, "P6\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize*3, f);
    fclose(f);
}

static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                    AVFrame *rgbFrame, struct SwsContext *swsCtx)
{
    char buf[1024];
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0)
    {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0)
        {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        printf("saving frame %3" PRId64 "\n", dec_ctx->frame_number);
        fflush(stdout);

        /* the picture is allocated by the decoder. no need to
           free it */
        // snprintf(buf, sizeof(buf), "%s-%" PRId64 ".pgm", filename, dec_ctx->frame_number);
        // pgm_save(frame->data[0], frame->linesize[0],
        //          frame->width, frame->height, buf);

        // 进行颜色空间转换
        sws_scale(swsCtx, frame->data[0], frame->linesize, 0, frame->height,
                rgbFrame->data, rgbFrame->linesize);
        snprintf(buf, sizeof(buf), "%s-%" PRId64 ".ppm", "frame", dec_ctx->frame_number);
        ppm_save(rgbFrame->data[0], rgbFrame->linesize[0],
                 rgbFrame->width, rgbFrame->height, buf);
    }
}

int main(int argc, char *argv[])
{
    // Initalizing these to NULL prevents segfaults!
    AVFormatContext *pFormatCtx = NULL;
    int i, videoStream;
    AVCodecParameters *pCodecPar = NULL;
    AVCodecContext *pCodecCtx = NULL;
    AVCodec *pCodec = NULL;
    AVFrame *pFrame = NULL;
    AVFrame *pRGBFrame = NULL;
    AVPacket *packet;
    int frameFinished;
    int numBytes;
    struct SwsContext *sws_ctx = NULL;
    int ret;

    if (argc < 2)
    {
        printf("Please provide a movie file\n");
        return -1;
    }
    // Register all formats and codecs
    // av_register_all();

    // Open video file
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
        return -1; // Couldn't open file

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    // Find the first video stream
    videoStream = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++)
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = i;
            break;
        }
    if (videoStream == -1)
        return -1; // Didn't find a video stream

    // Get a pointer to the codec context for the video stream
    pCodecPar = pFormatCtx->streams[videoStream]->codecpar;
    // Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecPar->codec_id);
    if (pCodec == NULL)
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    pCodecCtx = avcodec_alloc_context3(pCodec);

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(pCodecCtx, pCodecPar)) < 0)
    {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        return -1;
    }

    // Open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
        return -1; // Could not open codec

    // Allocate video frame
    pFrame = av_frame_alloc();

    // 创建一个新的AVFrame对象
    pRGBFrame = av_frame_alloc();
    // 设置新的AVFrame参数
    pRGBFrame->format = AV_PIX_FMT_RGB32;
    pRGBFrame->width = pCodecCtx->width;
    pRGBFrame->height = pCodecCtx->height;
    // 分配内存
    ret = av_frame_get_buffer(pRGBFrame, 0);
    if (ret < 0) {
        return -1;
    }

    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
                             pCodecCtx->height,
                             pCodecCtx->pix_fmt,
                             pCodecCtx->width,
                             pCodecCtx->height,
                             AV_PIX_FMT_RGB24,
                             SWS_BILINEAR,
                             NULL,
                             NULL,
                             NULL);

    packet = av_packet_alloc();
    // Read frames and save first five frames to disk
    i = 0;
    while (av_read_frame(pFormatCtx, packet) >= 0)
    {
        // Is this a packet from the video stream?
        if (packet->stream_index == videoStream)
        {
            decode(pCodecCtx, pFrame, packet, pRGBFrame, sws_ctx);
        }

        // Free the packet that was allocated by av_read_frame
        av_packet_unref(packet);
    }
    decode(pCodecCtx, pFrame, NULL, pRGBFrame, sws_ctx);

    av_packet_free(&packet);

    // Free the YUV frame
    av_frame_free(&pFrame);

    av_frame_free(&pRGBFrame);

    // Close the codecs
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecPar);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}
