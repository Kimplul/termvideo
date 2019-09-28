extern "C"
{
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/eval.h>
#include <ao/ao.h>
}
#include <thread>
#include <iostream>
#include <string>
#include <thread>
#include <array>
#include <termbox.h>

// Ugly globals but they'll do for now
uint8_t* pixels;
bool done = false;
int frame_width = 0;
int frame_height = 0;

bool audio_ready = false;
bool audio_decode_ready = false;

SwrContext* swr_ctx;
AVFrame* audio_frame;
ao_device* device;
uint8_t* audio_buffer;
const int         out_channels = 2;
const int         out_samples = 512;
const int         sample_rate = 44100;

void playSound(){
    AVFrame* laudio_frame = av_frame_alloc();
    while(!done){

        if(audio_decode_ready){
            audio_decode_ready = false;
             // Deep copy frame to make sure the decoding can take place in main
            laudio_frame->format = audio_frame->format;
            laudio_frame->width = audio_frame->width;
            laudio_frame->height = audio_frame->height;
            laudio_frame->channels = audio_frame->channels;
            laudio_frame->channel_layout = audio_frame->channel_layout;
            laudio_frame->nb_samples = audio_frame->nb_samples;
            av_frame_get_buffer(laudio_frame, 1);
            int ret = av_frame_copy(laudio_frame, audio_frame);
            if(ret < 0){
                exit(1);
            }

            int got_samples = swr_convert(
                    swr_ctx,
                    &audio_buffer, out_samples,
                    (const uint8_t **)laudio_frame->data, laudio_frame->nb_samples);

            if(got_samples < 0){
                fprintf(stderr, "error_ swr_convert()\n");
                exit(1);
            }

            // Play sounds until samples from packet run dry
            while(got_samples > 0){
                int audio_buffer_size = av_samples_get_buffer_size(NULL, out_channels, got_samples, AV_SAMPLE_FMT_S16, 1);
                ao_play(device, (char*)audio_buffer, audio_buffer_size);

                got_samples = swr_convert(swr_ctx, &audio_buffer, out_samples, NULL, 0);
                if(got_samples < 0){
                    fprintf(stderr, "error: swr_convert()\n");
                    exit(1);
                }
            }


            audio_ready = true;
        }
    }
    av_frame_free(&laudio_frame);
}

void renderImage(int w, int h, int imgwidth, int imgheight){
    int widthsize = imgwidth / w;
    int heightsize = imgheight / h;

    while(!done){
        w = tb_width();
        h = tb_height();
        widthsize = imgwidth / w;
        heightsize = imgheight / h;

        int i = 0;
        int j = 0;
        for(int x = 0; x < h; x++){
            for(int y = 0; y < w; y++){
                int vi = 3*(i + 1)*imgwidth + j*widthsize;
                int vj = 3*i*imgwidth + j*widthsize;
                tb_truecolor fg = {
                    pixels[vi],
                    pixels[vi + 1],
                    pixels[vi + 2]};
                tb_truecolor bg = {
                    pixels[vj],
                    pixels[vj + 1],
                    pixels[vj + 2] };
                tb_change_cell(y, x, L'\u2584', 0, 0, fg, bg);

                j += 3;
            }
            i += heightsize - 1;
            j = 0;
        }
        tb_present();
    }

}
// ffmpeg probably has a similar function built in, but I couldn't find it.
double get_fps(AVCodecContext *avctx){
    return 1.0 / av_q2d(avctx->time_base) / FFMAX(avctx->ticks_per_frame, 1);
}

//TODO: add error checking
void parseWH(std::string s, int& w, int& h){
    std::string temp = "";
    for(auto c : s){
        if(c == ' '){
            h = std::stoi(temp);
            temp = "";
        }
        temp += c;
    }
    w = std::stoi(temp);
}

int main(int argc, char *argv[]) {
    // Termbox doesn't play well with outputting to std(in|err)
    av_log_set_level(AV_LOG_QUIET);


    // Initalizing these to NULL prevents segfaults
    AVFormatContext   *pFormatCtx = NULL;
    int               videoStream, audioStream;
    AVCodecContext    *pCodecCtxOrig = NULL;
    AVCodecContext    *pCodecCtx = NULL;
    AVCodec           *pCodec = NULL;
    AVFrame           *pFrame = NULL;
    AVFrame           *pFrameRGB = NULL;

    AVPacket          packet;
        const int         max_audio_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_samples, AV_SAMPLE_FMT_S16, 1);
    int               numBytes;
    double            deltaFrameTime = 0.0;
    int               width;
    int               height;
    uint8_t           *buffer = NULL;
    tb_event*         key_event = (tb_event*)malloc(sizeof(tb_event));
    struct SwsContext *sws_ctx = NULL;

    // Initialize audio player
    ao_initialize();

    int default_driver = ao_default_driver_id();
    ao_sample_format format;

    memset(&format, 0, sizeof(format));
    format.bits = 16;
    format.channels = out_channels;
    format.rate = sample_rate;
    format.byte_format = AO_FMT_NATIVE;
    format.matrix = (char*)"L,R"; // Front left and front right

    device = ao_open_live(default_driver, &format, NULL);

    if(argc < 2) {
        printf("Please provide a movie file\n");
        return -1;
    }
    // Rather primitive and POSIX-dependent, but I wanted to try out pipes.
    // Sue me.
    std::string command = "stty size";
    std::array<char, 32> bufferArray;
    std::string result = "";
    FILE* pipe = popen(command.c_str(), "r");
    if(!pipe){
        std::cerr << "Couldn't start command!" << std::endl;
        return 1;
    }
    while(fgets(bufferArray.data(), 32, pipe)){
        result += bufferArray.data();
    }
    pclose(pipe);
    parseWH(result, width, height);

    // Register all formats and codecs

    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        return -1; // Couldn't open file

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    // Find the audio/video streams
    videoStream=-1;
    audioStream=-1;
    for(unsigned int i = 0; i<pFormatCtx->nb_streams; i++){
        if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream=i;
        }

        if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && audioStream < 0){
            audioStream=i;
        }
    }
    if(videoStream==-1)
        return -1; // Didn't find a video stream
    if(audioStream==-1)
        return -1; // Didn't find an audio stream

    // Allocate audio context and find decoder
    AVCodecContext* audio_codec_ctx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(audio_codec_ctx, pFormatCtx->streams[audioStream]->codecpar);
    if(audio_codec_ctx->channel_layout == 0){
        audio_codec_ctx->channel_layout = AV_CH_FRONT_LEFT | AV_CH_FRONT_LEFT;
    }

    AVCodec* audioCodec = avcodec_find_decoder(audio_codec_ctx->codec_id);
    if(!audioCodec){
        fprintf(stderr, "error: audio avcodec_find_decoder()\n");
    }

    if(avcodec_open2(audio_codec_ctx, audioCodec, NULL) < 0){
        fprintf(stderr, "error: avcodec_open2()\n");
        exit(1);
    }

    // Allocate software resampling context
    swr_ctx =
        swr_alloc_set_opts(NULL,
                AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT,
                AV_SAMPLE_FMT_S16,
                sample_rate,
                audio_codec_ctx->channel_layout,
                audio_codec_ctx->sample_fmt,
                audio_codec_ctx->sample_rate,
                0,
                NULL);

    if(!swr_ctx){
        fprintf(stderr, "error: swr_alloc_set_opts()\n");
        exit(1);
    }

    // Initiate termbox for drawing to the screen. USE MY FORK, MUY IMPORTANTE!
    tb_select_output_mode(TB_OUTPUT_TRUECOLOR);
    tb_init();

    // Initiate resampling context
    swr_init(swr_ctx);
    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig=avcodec_alloc_context3(NULL);
    // Contexts can't be directly transferred anymore, a temporary container is
    // needed. I think.
    AVCodecParameters* tempparams = avcodec_parameters_alloc();
    avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if(pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    avcodec_parameters_from_context(tempparams, pCodecCtxOrig);
    if(avcodec_parameters_to_context(pCodecCtx, tempparams) != 0){
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
        return -1; // Could not open codec

    deltaFrameTime = 1 / get_fps(pCodecCtx);
    // Allocate video frame
    pFrame=av_frame_alloc();

    // Allocate an AVFrame structure
    pFrameRGB=av_frame_alloc();
    if(pFrameRGB==NULL)
        return -1;

    // Allocate audio frame
    audio_frame = av_frame_alloc();
    // Allocate the audio buffer that we'll use to pass data to AO
    audio_buffer = (uint8_t*)av_malloc(max_audio_buffer_size);

    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);
    //pCodecCtx->height);
    buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
    pixels=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);    // Where the fuck is alignment defined? Someone told me to just use 1, seems to work

    // Initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
            pCodecCtx->height,
            pCodecCtx->pix_fmt,
            pCodecCtx->width,
            pCodecCtx->height,
            AV_PIX_FMT_RGB24,
            SWS_BILINEAR,
            NULL,
            NULL,
            NULL
            );

    // Initiate new thread for drawing the image
    std::thread trd(renderImage, width, height, pCodecCtx->width, pCodecCtx->height);
    std::thread aud(playSound);

    // Main loop
    while(av_read_frame(pFormatCtx, &packet)>=0) {

        // If user presses ESC or Q, quit
        if(tb_peek_event(key_event, 1) && (key_event->key == TB_KEY_ESC || key_event->ch == 'q')){
            break;
        }

        // Is this a packet from the video stream?
        if(packet.stream_index == videoStream) {
            // Decode video frame
            avcodec_send_packet(pCodecCtx, &packet);
            avcodec_receive_frame(pCodecCtx, pFrame);

            // Convert the image from its native format to RGB
            sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
                    pFrame->linesize, 0, pCodecCtx->height,
                    pFrameRGB->data, pFrameRGB->linesize);

            memcpy((uint8_t*)pixels, (uint8_t*)*pFrameRGB->data, frame_width*frame_height*3);
            frame_width = pCodecCtx->width;
            frame_height = pCodecCtx->height;

        }
        if(packet.stream_index == audioStream){
            // Decode packet
            avcodec_send_packet(audio_codec_ctx, &packet);
            avcodec_receive_frame(audio_codec_ctx, audio_frame);

            audio_decode_ready = true;
            while(!audio_ready){
                continue;
            }
            audio_ready = false;




            /*int got_samples = swr_convert(
                    swr_ctx,
                    &audio_buffer, out_samples,
                    (const uint8_t **)audio_frame->data, audio_frame->nb_samples);

            if(got_samples < 0){
                fprintf(stderr, "error_ swr_convert()\n");
                exit(1);
            }

            // Play sounds until samples from packet run dry
            while(got_samples > 0){
                int audio_buffer_size = av_samples_get_buffer_size(NULL, out_channels, got_samples, AV_SAMPLE_FMT_S16, 1);
                AVPacket pock;
                av_init_packet(&pock);
                pock.data =  audio_buffer;
                pock.size = audio_buffer_size;
                ao_play(device, (char*)pock.data, pock.size);

                got_samples = swr_convert(swr_ctx, &audio_buffer, out_samples, NULL, 0);
                if(got_samples < 0){
                    fprintf(stderr, "error: swr_convert()\n");
                    exit(1);
                }
            }*/
        }
        // Free the packet that was allocated by av_read_frame
        av_packet_unref(&packet);
    }
    // Tell other thread to stop rendering
    done = true;
    trd.join();
    aud.join();

    // Free everything
    ao_close(device);
    ao_shutdown();
    tb_shutdown();


    swr_free(&swr_ctx);
    sws_freeContext(sws_ctx);
    av_free(buffer);
    av_free(pixels);
    av_free(audio_buffer);
    av_frame_free(&pFrameRGB);
    av_frame_free(&pFrame);
    av_frame_free(&audio_frame);
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);
    avcodec_free_context(&pCodecCtx);
    avcodec_close(audio_codec_ctx);
    avcodec_free_context(&audio_codec_ctx);
    avcodec_free_context(&pCodecCtxOrig);
    avcodec_parameters_free(&tempparams);
    avformat_close_input(&pFormatCtx);
    free(key_event);

    return 0;
}
