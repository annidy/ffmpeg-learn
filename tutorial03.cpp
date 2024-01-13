extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

#include <stdio.h>
#include <SDL2/SDL.h>
#include <list>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

bool g_quit = false;

/*
typedef struct AVPacketList
{
    AVPacket pkt;
    struct AVPacketList *next;
} AVPacketList;
*/
struct PacketQueue 
{
    std::list<AVPacket *> plist;
    int nb_packets = 0;
    int size = 0;
    SDL_mutex *mutex;
    SDL_cond *cond;

    PacketQueue()
    {
        mutex = SDL_CreateMutex();
        cond = SDL_CreateCond();
    }

    int Put(AVPacket *pkt)
    {
        SDL_LockMutex(mutex);
        plist.push_back(av_packet_clone(pkt));
        nb_packets++;
        size += pkt->size;
        SDL_CondSignal(cond);
        SDL_UnlockMutex(mutex);
        return 0;
    }

    AVPacket *Get(bool block)
    {
        AVPacket *ret = nullptr;
        SDL_LockMutex(mutex);
        for(;;)
        {
            if (g_quit)
                break;
            if (!plist.empty())
            {
                ret = plist.front();
                plist.pop_front();
                break;
            }
            else if (!block)
            {
                break;
            }
            SDL_CondWait(cond, mutex);
        }

        SDL_UnlockMutex(mutex);
        return ret;
    }
} g_audioq;

static int decode(AVCodecContext *dec_ctx, AVPacket *pkt, std::function<void(AVFrame *)> onFrame)
{
    int ret = AVERROR(1);

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0)
    {
        fprintf(stderr, "Error submitting the packet to the decoder\n");
        return ret;
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0)
    {
        auto frame = av_frame_alloc();
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret < 0)
        {
            break;
        }
        onFrame(frame);
        av_frame_free(&frame);
    }
    return ret == AVERROR(EAGAIN) ? 0 : ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size)
{
    AVPacket *pkt;
    int data_size = 0;

    for (;;)
    {
        if (g_quit)
        {
            return -1;
        }

        if ((pkt = g_audioq.Get(true)) == nullptr)
        {
            return -1;
        }
        auto ret = decode(aCodecCtx, pkt, [&](AVFrame *frame)
                          {
            auto sample_size = av_get_bytes_per_sample(aCodecCtx->sample_fmt);
            if (sample_size < 0)
            {
                /* This should not occur, checking just for paranoia */
                fprintf(stderr, "Failed to calculate data size\n");
                exit(1);
            }
            for (auto i = 0; i < frame->nb_samples; i++)
            {
                for (auto ch = 0; ch < aCodecCtx->ch_layout.nb_channels; ch++)
                {
                    memcpy(audio_buf + data_size, frame->data[ch] + sample_size * i, sample_size);
                    data_size += sample_size;
                }
            } });
        av_packet_free(&pkt);
        if (ret == 0)
            break;
    }
    return data_size;
}

void video_callback(AVCodecContext *codecCtx, AVPacket *pkt, SDL_Renderer *renderer, SDL_Texture *texture)
{
    decode(codecCtx, pkt, [&](AVFrame *frame)
           {

    // 将YUV数据填充到SDL纹理中
    SDL_UpdateYUVTexture(texture, NULL,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);

    // 清空渲染器
    SDL_RenderClear(renderer);

    // 将纹理复制到渲染器
    SDL_RenderCopy(renderer, texture, NULL, NULL);

    // 刷新屏幕
    SDL_RenderPresent(renderer); });
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0)
    {
        if (audio_buf_index >= audio_buf_size)
        {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0)
            {
                /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                memset(audio_buf, 0, audio_buf_size);
            }
            else
            {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

int main(int argc, char *argv[])
{
    // Initalizing these to NULL prevents segfaults!
    AVFormatContext *pFormatCtx = NULL;
    int videoStream, audioStream;
    AVCodecParameters *pCodecPar = NULL;
    AVCodecContext *vCodecCtx = NULL, *aCodecCtx;
    AVPacket *packet;
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

    // Find the first video/audio stream
    videoStream = -1;
    audioStream = -1;
    for (auto i = 0; i < pFormatCtx->nb_streams; i++)
    {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = i;
        }
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioStream = i;
        }
    }
    if (videoStream == -1 || audioStream == -1)
        return -1;

    // 音频解码器
    auto aCodecPar = pFormatCtx->streams[audioStream]->codecpar;
    auto aCodec = avcodec_find_decoder(aCodecPar->codec_id);
    if (aCodec == NULL)
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    aCodecCtx = avcodec_alloc_context3(aCodec);
    if ((ret = avcodec_parameters_to_context(aCodecCtx, aCodecPar)) < 0)
    {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }
    // 打开解码器
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0)
        return -1;

    // Set audio settings from codec info
    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.freq = aCodecCtx->sample_rate;
    if (aCodecPar->format == AV_SAMPLE_FMT_FLTP)
        wanted_spec.format = AUDIO_F32SYS;
    else if (aCodecPar->format == AV_SAMPLE_FMT_S16P)
        wanted_spec.format = AUDIO_S16SYS;
    else
    {
        fprintf(stderr, "Unsupport format %d", aCodecPar->format);
        return -1;
    }

    wanted_spec.channels = aCodecCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
    {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudio(0);

    // -----------------------------------------

    // Get a pointer to the codec context for the video stream
    pCodecPar = pFormatCtx->streams[videoStream]->codecpar;
    // Find the decoder for the video stream
    auto vCodec = avcodec_find_decoder(pCodecPar->codec_id);
    if (vCodec == NULL)
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    vCodecCtx = avcodec_alloc_context3(vCodec);

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(vCodecCtx, pCodecPar)) < 0)
    {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        return -1;
    }

    // Open codec
    if (avcodec_open2(vCodecCtx, vCodec, NULL) < 0)
        return -1; // Could not open codec

    packet = av_packet_alloc();
    SDL_Event e;
    while (!g_quit)
    {
        while (SDL_PollEvent(&e) != 0)
        {
            if (e.type == SDL_QUIT)
            {
                g_quit = SDL_TRUE;
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
                video_callback(vCodecCtx, packet, renderer, texture);
            }
            else if (packet->stream_index == audioStream)
            {
                g_audioq.Put(packet);
            }

            // Free the packet that was allocated by av_read_frame
            av_packet_unref(packet);
        }
        else
        {
            video_callback(vCodecCtx, NULL, renderer, texture);
            g_quit = SDL_TRUE;
        }
    }
    

    av_packet_free(&packet);

    // Close the codecs
    avcodec_close(vCodecCtx);
    avcodec_close(aCodecCtx);

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
