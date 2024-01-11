#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>

#include <stdio.h>
#include <SDL2/SDL.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

SDL_Renderer *g_renderer;

static void decode(AVCodecContext *dec_ctx, AVPacket *pkt,
                   SDL_Texture *texture)
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
        // Allocate video frame
        AVFrame *frame = av_frame_alloc();

        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0)
        {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        // 将YUV数据填充到SDL纹理中
        SDL_UpdateYUVTexture(texture, NULL,
                             frame->data[0], frame->linesize[0],
                             frame->data[1], frame->linesize[1],
                             frame->data[2], frame->linesize[2]);

        // 清空渲染器
        SDL_RenderClear(g_renderer);

        // 将纹理复制到渲染器
        SDL_RenderCopy(g_renderer, texture, NULL, NULL);

        // 刷新屏幕
        SDL_RenderPresent(g_renderer);

        av_frame_free(&frame);
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
    AVPacket *packet;
    int frameFinished;
    int numBytes;
    int ret;

    if (argc < 2)
    {
        printf("Please provide a movie file\n");
        return -1;
    }

    // 初始化SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        fprintf(stderr, "无法初始化SDL - %s\n", SDL_GetError());
        return -1;
    }

    // 创建SDL窗口
    SDL_Window *window = SDL_CreateWindow("Windows", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN);
    if (!window)
    {
        fprintf(stderr, "无法创建窗口 - %s\n", SDL_GetError());
        return -1;
    }

    // 创建SDL渲染器
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
    {
        fprintf(stderr, "无法创建渲染器 - %s\n", SDL_GetError());
        return -1;
    }
    g_renderer = renderer;

    // 创建SDL纹理
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    if (!texture)
    {
        fprintf(stderr, "无法创建纹理 - %s\n", SDL_GetError());
        return -1;
    }

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

    packet = av_packet_alloc();
    // Read frames and save first five frames to disk
    i = 0;
    SDL_bool quit = SDL_FALSE;
    SDL_Event e;
    while (!quit)
    {
        while (SDL_PollEvent(&e) != 0)
        {
            if (e.type == SDL_QUIT)
            {
                quit = SDL_TRUE;
                break;
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SPACE)
            {
                if (SDL_GetAudioStatus() == SDL_AUDIO_PAUSED)
                    SDL_PauseAudio(0);
                else
                    SDL_PauseAudio(1);
                break;
            }
        }

        if (av_read_frame(pFormatCtx, packet) >= 0)
        {
            // Is this a packet from the video stream?
            if (packet->stream_index == videoStream)
            {
                decode(pCodecCtx, packet, texture);
            }

            // Free the packet that was allocated by av_read_frame
            av_packet_unref(packet);
        }
        else
        {
            decode(pCodecCtx, NULL, texture);
            quit = SDL_TRUE;
        }
    }
    

    av_packet_free(&packet);

    // Close the codecs
    avcodec_close(pCodecCtx);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    // 销毁SDL纹理、渲染器和窗口
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    // 退出SDL
    SDL_Quit();

    return 0;
}
