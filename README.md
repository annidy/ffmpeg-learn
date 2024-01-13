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