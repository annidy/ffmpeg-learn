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