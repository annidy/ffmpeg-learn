extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

#include <stdio.h>
#include <SDL2/SDL.h>
#include <list>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 1024 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 5

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

static int decode(AVCodecContext *dec_ctx, AVPacket *pkt, std::function<void(AVFrame *)> onFrame);
void audio_callback(void *userdata, Uint8 *stream, int len);

struct PacketQueue
{
    std::list<AVPacket *> plist;
    int nb_packets = 0;
    int size = 0;
    std::mutex mutex;
    std::condition_variable cond;
    bool eof = false;

    PacketQueue()
    {

    }

    int Put(const AVPacket *pkt)
    {
        std::unique_lock<std::mutex> lock(mutex);
        plist.push_back(av_packet_clone(pkt));
        nb_packets++;
        size += pkt->size;
        cond.notify_one();
        return 0;
    }

    AVPacket *Get(bool block = true)
    {
        AVPacket *ret = nullptr;
        std::unique_lock<std::mutex> lock(mutex);
        for(;;)
        {
            if (!plist.empty())
            {
                ret = plist.front();
                plist.pop_front();
                size -= ret->size;
                nb_packets--;
                break;
            }
            else if (!block || eof)
            {
                break;
            }
            cond.wait(lock, [&]() { return !plist.empty() || eof; });
        }
        return ret;
    }
};

struct VideoPicture
{
    AVFrame *frame;
    double pts;
};

struct VideoState
{
    AVFormatContext *pFormatCtx = nullptr;
    int videoStream = -1, audioStream = -1;
    AVStream *audio_st = nullptr;
    AVCodecContext *audio_ctx = nullptr;
    PacketQueue audioq;
    AVStream *video_st = nullptr;
    AVCodecContext *video_ctx = nullptr;
    PacketQueue videoq;

    VideoPicture *pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int pictq_size = 0, pictq_rindex = 0, pictq_windex = 0;
    std::mutex pictq_mutex;
    std::condition_variable pictq_cond;

    std::thread parse_thread;
    std::thread video_thread;

    bool quit = false;

    double audio_clock = 0.0;
    double video_clock = 0.0;
    double frame_timer = 0.0;
    double frame_last_pts = 0.0;
    double frame_last_delay = 0.0;
    size_t audio_buf_size = 0;
    size_t audio_buf_index = 0;
    uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];

    void Open(const std::string &filename)
    {
        // Open video file
        if (avformat_open_input(&pFormatCtx, filename.c_str(), NULL, NULL) != 0)
            throw std::runtime_error("Couldn't open input stream");

        // Retrieve stream information
        if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
            throw std::runtime_error("Couldn't find stream information");

        // Dump information about file onto standard error
        av_dump_format(pFormatCtx, 0, filename.c_str(), 0);

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
            throw std::runtime_error("Didn't find a video or audio stream");

        parse_thread = std::thread([&]
                                   { decode_thread(); });
    }

    ~VideoState()
    {
        avformat_close_input(&pFormatCtx);
        avformat_free_context(pFormatCtx);

        avcodec_free_context(&audio_ctx);
        avcodec_free_context(&video_ctx);
        for (auto &p : pictq)
        {
            av_frame_free(&p->frame);
            delete p;
        }
    }

    void decode_thread()
    {
        stream_componet_open(audioStream);
        stream_componet_open(videoStream);

        while (!quit)
        {
            if (audioq.size > MAX_AUDIOQ_SIZE || videoq.size > MAX_VIDEOQ_SIZE)
            {
                SDL_Delay(10);
                continue;
            }

            auto packet = av_packet_alloc();

            if (av_read_frame(pFormatCtx, packet) >= 0)
            {
                // Is this a packet from the video stream?
                if (packet->stream_index == videoStream)
                {
                    videoq.Put(packet);
                }
                else if (packet->stream_index == audioStream)
                {
                    audioq.Put(packet);
                }

                // Free the packet that was allocated by av_read_frame
                av_packet_unref(packet);
            }
            else
            {
                break;
            }

            av_packet_free(&packet);
        }
        // 设置结束保证，防止Get无限等待
        audioq.eof = true;
        videoq.eof = true;
    }

    void decode_video_thread()
    {
        for(;;)
        {
            auto packet = videoq.Get();
            if (!packet)
            {
                break;
            }
            
            decode(video_ctx, packet, [&](AVFrame *frame) {     
                double pts = 0;
                if (packet->dts != AV_NOPTS_VALUE)
                    pts = frame->best_effort_timestamp * av_q2d(video_st->time_base); // av_frame_get_best_effort_timestamp() 被移除了，使用best_effort_timestamp

                pts = synchorize_video(frame, pts); // 更新视频时钟
                frame = av_frame_clone(frame);
                auto vp = new VideoPicture();
                vp->frame = frame;
                vp->pts = pts;          
                push_video_picture(vp);
            });

            av_packet_free(&packet);
        }
        quit = true;
    }

    int decode_audio(uint8_t *audio_buf, int buf_size)
    {
        AVPacket *pkt;
        int data_size = 0;

        if ((pkt = audioq.Get(false)) == nullptr)
            return -1;  // 声音不能block，否则SDL的音频会停止工作
        
        decode(audio_ctx, pkt, [&](AVFrame *frame)
               {
                auto sample_size = av_get_bytes_per_sample(audio_ctx->sample_fmt);
                if (sample_size < 0)
                {
                    /* This should not occur, checking just for paranoia */
                    fprintf(stderr, "Failed to calculate data size\n");
                    return;
                }
                for (auto i = 0; i < frame->nb_samples; i++)
                {
                    for (auto ch = 0; ch < audio_ctx->ch_layout.nb_channels; ch++)
                    {
                        memcpy(audio_buf + data_size, frame->data[ch] + sample_size * i, sample_size);
                        data_size += sample_size;
                    }
                } 
                // 音频就直接使用pts
                if(pkt->pts != AV_NOPTS_VALUE) {
                    audio_clock = av_q2d(audio_st->time_base)*pkt->pts;
                }});
        av_packet_free(&pkt);

        return data_size;
    }

    int push_video_picture(VideoPicture *pict)
    {
        std::unique_lock lk(pictq_mutex);
        pictq_cond.wait(lk, [&]
                { return quit || pictq_size < VIDEO_PICTURE_QUEUE_SIZE; });

        if (quit)
            return -1;
        // TODO: B帧排序
        pictq[pictq_windex] = pict;
        pictq_windex = (pictq_windex + 1) % VIDEO_PICTURE_QUEUE_SIZE;
        ++pictq_size;

        lk.unlock();
        pictq_cond.notify_one();
        return 0;
    }

    int pop_video_picture(VideoPicture *&pict)
    {
        std::unique_lock lk(pictq_mutex);
        if (quit || pictq_size == 0)
            return -1;

        pict = pictq[pictq_rindex];
        pictq[pictq_rindex] = nullptr;
        pictq_rindex = (pictq_rindex + 1) % VIDEO_PICTURE_QUEUE_SIZE;
        --pictq_size;
        lk.unlock();
        pictq_cond.notify_one();
        return 0;
    }

    int stream_componet_open(int stream_index)
    {
        if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams)
            return -1;

        auto codecPar = pFormatCtx->streams[stream_index]->codecpar;
        auto codec = avcodec_find_decoder(codecPar->codec_id);
        if (codec == NULL)
        {
            fprintf(stderr, "Unsupported codec!\n");
            return -1; // Codec not found
        }
        auto codecCtx = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(codecCtx, codecPar) < 0)
        {
            fprintf(stderr, "Couldn't copy codec context");
            return -1; // Error copying codec context
        }
        // 打开解码器
        if (avcodec_open2(codecCtx, codec, NULL) < 0)
            return -1;

        switch (codecCtx->codec_type)
        {
        case AVMEDIA_TYPE_AUDIO:
        {
            // Set audio settings from codec info
            SDL_AudioSpec wanted_spec, spec;
            wanted_spec.freq = codecCtx->sample_rate;
            if (codecPar->format == AV_SAMPLE_FMT_FLTP)
                wanted_spec.format = AUDIO_F32SYS;
            else if (codecPar->format == AV_SAMPLE_FMT_S16P)
                wanted_spec.format = AUDIO_S16SYS;
            else
            {
                fprintf(stderr, "Unsupport format %d", codecPar->format);
                return -1;
            }

            wanted_spec.channels = codecCtx->ch_layout.nb_channels;
            wanted_spec.silence = 0;
            wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
            wanted_spec.callback = audio_callback;
            wanted_spec.userdata = this;

            if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
            {
                fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
                return -1;
            }

            audioStream = stream_index;
            audio_st = pFormatCtx->streams[stream_index];
            audio_ctx = codecCtx;

            SDL_PauseAudio(0);
            break;
        }
        case AVMEDIA_TYPE_VIDEO:
        {
            videoStream = stream_index;
            video_st = pFormatCtx->streams[stream_index];
            video_ctx = codecCtx;
            frame_timer = av_gettime() / 1000000.0; // Get the current time in seconds.
            frame_last_delay = 40e-3;

            video_thread = std::thread(&VideoState::decode_video_thread, this);

            break;
        }
        default:
            break;
        }
        return 0;
    }

    double synchorize_video(AVFrame *frame, double pts)
    {
        if (pts != 0)
            video_clock = pts;
        else
            pts = video_clock;

        auto frame_delay = av_q2d(video_st->time_base) * (frame->repeat_pict - 0.5);
        video_clock += frame_delay;
        return pts;
    }

    double get_audio_clock()
    {
        double pts = audio_clock;
        // 处理还没投喂给SDL的缓存数据长度
        double hw_buf_size = audio_buf_size - audio_buf_index;
        double bytes_per_sec = audio_st->codecpar->sample_rate * audio_st->codecpar->channels * 2;
        pts -= hw_buf_size / bytes_per_sec;
        if (pts < 0.0)
            pts = 0.0;
        return pts;
    }
};

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



void audio_callback(void *userdata, Uint8 *stream, int len)
{
    VideoState *is = (VideoState *)userdata;
    int len1, audio_size;


    while (len > 0)
    {
        if (is->audio_buf_index >= is->audio_buf_size)
        {
            /* We have already sent all our data; get more */
            audio_size = is->decode_audio(is->audio_buf, sizeof(is->audio_buf));
            if (audio_size < 0)
            {
                /* If error, output silence */
                is->audio_buf_size = 1024; // arbitrary?
                memset(is->audio_buf, 0, is->audio_buf_size);
            }
            else
            {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}



static uint32_t sdl_refresh_timer_cb(uint32_t interval, void *opaque)
{
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

void schedule_refresh(VideoState *is, int delay)
{
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_refresh_timer(VideoState *is, std::function<void(AVFrame *)> onDisplay)
{
    if (!is)
    {
        schedule_refresh(is, 100);
        return;
    }
    VideoPicture *vp = nullptr;
    if (is->pop_video_picture(vp) < 0)
    {
        schedule_refresh(is, 1);
        return;
    }

    auto delay = vp->pts - is->frame_last_pts;
    if (delay <= 0 || delay >= 1)
        delay = is->frame_last_delay;

    is->frame_last_delay = delay;
    is->frame_last_pts = vp->pts;

    auto ref_clock = is->get_audio_clock();
    auto diff = vp->pts - ref_clock;

    // ffplay的策略：如果同步的差值在 [0.01, 10] 范围内，如果小于 -0.01，直接延迟0秒加速播； 如果大于 0.01，delay加倍，放慢视频播放 
    auto sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
    if (fabs(diff) < AV_NOSYNC_THRESHOLD) 
    {
        if (diff <= -sync_threshold)
            delay = 0;
        else if (diff >= sync_threshold)
            delay = 2 * delay;
    }

    is->frame_timer += delay;
    auto actual_dealy = is->frame_timer - (av_gettime() / 1000000.0); // 跟实际时钟求时间差，应该是为了消除误差
    if (actual_dealy < 0.010) 
        actual_dealy = 0.010;

    schedule_refresh(is, (int)(actual_dealy * 1000.0 + 0.5));

    onDisplay(vp->frame);

    av_frame_unref(vp->frame);
    delete vp;
}

int main(int argc, char *argv[])
{
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
    SDL_Window *window = SDL_CreateWindow("Windows", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 720, 560, SDL_WINDOW_SHOWN);
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
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, 720, 560);
    if (!texture)
    {
        fprintf(stderr, "无法创建纹理 - %s\n", SDL_GetError());
        return -1;
    }

    auto is = std::make_shared<VideoState>();
    is->Open(argv[1]);

    schedule_refresh(is.get(), 40);


    SDL_Event e;
    while (!is->quit)
    {
        while (SDL_PollEvent(&e) != 0)
        {
            if (e.type == SDL_QUIT)
            {
                is->quit = true;
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
            if (e.type == FF_REFRESH_EVENT)
            {

                video_refresh_timer(is.get(), [&](AVFrame *frame)
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
                    SDL_RenderPresent(renderer);
                });
            }
        }
    }

    // 销毁SDL纹理、渲染器和窗口
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    // 退出SDL
    SDL_Quit();

    return 0;
}
