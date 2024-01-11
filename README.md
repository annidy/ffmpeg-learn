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
