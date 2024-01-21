## ffmpeg-learn

My code for learn
http://dranger.com/ffmpeg/ffmpeg.html

### tutorial01

大量API已被废弃

1. avcodec_copy_context已废弃，改为avcodec_parameters_to_context
2. avpicture_get_size和avpicture_fill已废弃，改为使用av_frame_get_buffer
3. avcodec_decode_video2已废弃，改为avcodec_send_packet+avcodec_receive_frame

https://www.ffmpeg.org/doxygen/trunk/demux_decode_8c-example.html
https://www.ffmpeg.org/doxygen/trunk/decode_video_8c-example.html

### tutorial02

SDL2相比之前，很多API都已经改变了

最主要的，显示的画面是通过`SDL_Texture`纹理来处理的。
老版本使用是`SDL_Overlay`，这个和`AVFrame`类似，需要先创建固定大小，还要通过swsCtx转换到对于的大小才可以用。

而纹理比较简单, 只需要`SDL_UpdateYUVTexture`一次调用。

### tutorial03

音频和视频一样，也要创建解码器，调用的过程和视频一样。
新的解码过程和视频也是一样，因此我们重构了一下解码函数，以便复用整个流程。

不同于视频解码后可以直接渲染，音频需要SDL主动来拉。这里我们的做法是先把packet保存到队列中，等到拉的时候再调用decode获取pcm数据。

由于每次decode得到的长度和SDL回调要求的不一样，所以需要把没用完的pcm保存下来。

https://www.ffmpeg.org/doxygen/trunk/decode_audio_8c-example.html

## tutorial04

对代码进行重构，主要引入了2个新线程。

1. decode_thread：解包线程，并且新增了videoq队列，解码后packet放到队列中，同时如果队列满了就先不调用av_read_frame了
2. decode_vodeo_thread：视频解码线程。从videoq读包，解码后的YUV先放到pictq队列中。它既是vidoq的消费者，又是pictq的生成者

至于每一帧显示多久，目前是靠主线程的**定时器**来实现的，VideoState内部没有延迟。重构后，把formatContext等上下文相关的都封装起来，代码相对清晰。

目前开始，VideoState基本有了播放器雏形。

## tutorial05

音画同步有很多方式，比如视频同步音频、使用系统时钟、使用视频自己的等。
最简单的方案就是用当前帧P减去上一帧P'，其差值就是当前帧显示的时间（严谨的做法是用当前帧减下一帧，但是这样必须要求至少有2帧缓存，现实中都是减上一帧，实现更简单）。这种方案就是使用视频自己的pts同步，由于误差大，实际在实现是还要考虑音频的pts。

这里还要提一下，获取到的包顺序一般是和dts一致的，但是显示的pts由于B帧可能不同。所以必须缓存一定数量的帧

```
   PTS: 1 4 2 3
   DTS: 1 2 3 4
Stream: I P B B
```

audio_clock记录的是当前音频的pts（注意，很多地方提到pts是浮点秒数，packet中的pts其实是序号，需要用time_base相乘得到绝对时间），因为音频也有缓存，实际在用的时候`get_audio_clock`还要减去SDL没取出的缓存剩余的时间。

> 所有播放器必须要有一个缓冲队列，缓存解码后的视频和音频数据

视频定时器计算伪代码

```
frame_timer = 系统时间

timer():
    P = 帧缓存取出当前帧的pts
    P' = 上一帧的pts
    delay = P - P'

    # 对齐音频
    diff = P - 音频PTS
    if diff < 10:
        if diff < -0.1:
            delay = 0  # P落后太多，加快显示
        else if diff > 0.1:
            delay = delay * 2 # P超前了，减慢一点

    frame_timer += delay
    delay2 = frame_timer - 系统时间
    next_timer(delay2) # 定时器设置下一帧

    display(P)
```

上面的代码有一些小技巧在里面
1. 每一帧的delay不是`P - 音频PTS`，而是先`P - P'`，然后再看与音频的diff，加快或减慢，如果超过10秒，ffplay就不管音频了，摆烂只使用视频的pts
2. frame_timer记录实际视频显示的*绝对时间*。为了规避定时器误差，特地把初始值设为系统时间，在设置定时器时再减去当前系统时间，这样就很准确了

## tutorial06

本节把节的时钟改为可配置为外部系统时钟。考虑到这不是主流实现方案，就不实现了

## tutorial07

FFmpeg的seek非常简单，api调用主要就2个
```c++
auto seek_target = av_rescale_q(pos, AV_TIME_BASE_Q,
                            pFormatCtx->streams[stream_index]->time_base);
if (av_seek_frame(pFormatCtx, stream_index, seek_target, seek_flags) < 0)
{
    std::cerr << "seek error\n";
}
```
`AV_TIME_BASE_Q`是单位为秒的分数，它主要是把pos转换为stream_index对应的时间单位。
最后调用`av_seek_frame`方法即可，stream_index可以是任意一个，另一个seek_flags是标记向前或向后seek

seek的另一个必要步骤是清空播放器的缓冲和解码器缓冲。包缓冲直接清空链表，而清空解码器是在包里放入一个特殊的pkt，当解码线程读到这个特殊的包时，就调用`avcodec_flush_buffers`刷新缓冲。

> FFmpeg里，flush操作是evacuate意思

---

## 后记

正如教程所言，这个小型的练手项目不到1000行。我本人曾多次翻阅过ffplay的代码，如果不是照着教程重新实现了一遍，很多设计的考量并不清楚，甚至认为这是自然而然的。正是知其然不知其所以然。
重新动手实现一遍后，有以下收获：

1. 虽然FFmpeg的API变化很快，但是打开文件、创建formatCtx/codecCtx等基本流程没太大变化。`AVRational`这个结构以前比较陌生，这次发现很多处理时间的函数都用到这个类型。
2. VideoState这个类型是任何播放器必要的结构。初学者很容易把做播放器等同于实现渲染输出，实际上st里的缓存非常重要，即便是在线流媒体这种收到数据立即播放，也需要维护包和帧缓冲，且视频和音频各需要一份
3. 音画同步应该多参考前人的经验。主要是现实中情况过于复杂，无法按照理想简单的定时渲染，即便ffplay的同步算法也不能100%适用。在做同步时关注好两点：a. 理解FFmpeg的pts是什么意思，如何换算到实际的时间；b. 熟悉你所在平台是如何播放音频的。视频一般比较通用，最好使用定时器而不是在线程中sleep。