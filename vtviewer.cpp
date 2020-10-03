extern "C"
{
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/eval.h>
}
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <array>
#include <portaudio.h>
#include <pthread.h>
#include <termbox.h>

// Ugly globals but they'll do for now
uint8_t* pixels;
bool done = false;
int frame_width = 0;
int frame_height = 0;

SwrContext* swr_ctx;
struct SwsContext *sws_ctx = NULL;
AVFrame           *pFrameRGB = NULL;
AVCodecContext* video_codec_ctx = NULL;

PaStreamParameters outputParameters;
PaStream *paStream;
PaError err;

typedef struct LockedFrame LockedFrame;

std::mutex init_lock;
std::condition_variable cv;
std::condition_variable ca;
std::mutex cv_m;
std::mutex ca_m;
std::mutex video_lock;

struct LockedFrame {
    int id;
    AVFrame* audio_frame = 0;
    uint8_t* audio_buffer = 0;
    size_t audio_buffer_size = 0;
    std::mutex* mtx;
    LockedFrame *next = 0;
    int got_samples = 0;
};

void playSound(LockedFrame* frame){
    init_lock.lock();
    init_lock.unlock();

    pthread_setname_np(pthread_self(), "audio");

    while(frame->got_samples >= 0){
        frame->mtx->lock();


        if(Pa_IsStreamStopped(paStream)){
            std::unique_lock<std::mutex> lk(ca_m);
            cv.wait(lk, []{return true;});
        }

        // for non-planar audio we could just as well use
        // frame->audio_frame->linesize[0], but this might be a bit more clear
        err = Pa_WriteStream(paStream, frame->audio_frame->data[0], frame->audio_frame->nb_samples);
        if(err != paNoError){
            std::cerr << "Failed writing to stream" << std::endl;
            std::cerr << Pa_GetErrorText(err) << std::endl;

            // this error seems to be triggered when the stream is stopped at
            // the end of a song, strange
            /*if(err == paUnanticipatedHostError)
                break;*/

            // underflow is alright, shouldn't cause any issues
            if(err != paOutputUnderflowed)
                exit(1);
        }

        frame->mtx->unlock();
        frame = frame->next;

        if(done)
            break;
    }
}

int conv(u_char c){
    if(c < 48)
        return 0;
    if(c < 114)
        return 1;
    return ((c - 35)/40);
}

int rgb2xterm(u_char r, u_char g, u_char b){
    int xr, xg, xb;

    xr = conv(r);
    xg = conv(g);
    xb = conv(b);

    return 16 + (36 * xr) + (6 * xg) + xb;
}

void renderImage(int w, int h, int imgwidth, int imgheight, AVFrame* pFrame){
    int widthsize = imgwidth / w;
    int heightsize = imgheight / h;

    pthread_setname_np(pthread_self(), "video");

    while(!done){
        video_lock.lock(); 

        avcodec_receive_frame(video_codec_ctx, pFrame);

        // Convert the image from its native format to RGB
        sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
                pFrame->linesize, 0, imgheight,
                pFrameRGB->data, pFrameRGB->linesize);

        video_lock.unlock();

        std::unique_lock<std::mutex> lk(cv_m);
        cv.wait(lk, []{return true;});

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
                    pFrameRGB->data[0][vi],
                    pFrameRGB->data[0][vi + 1],
                    pFrameRGB->data[0][vi + 2]};
                tb_truecolor bg = {
                    pFrameRGB->data[0][vj],
                    pFrameRGB->data[0][vj + 1],
                    pFrameRGB->data[0][vj + 2]};

                // Approximate xterm values
                uint16_t r_fg = rgb2xterm(fg.r, fg.g, fg.b);
                uint16_t r_bg = rgb2xterm(bg.r, bg.g, bg.b);

                tb_change_cell(y, x, L'\u2584', r_fg, r_bg, fg, bg);

                j += 3;
            }
            i += heightsize - 1;
            j = 0;
        }
        tb_present();
    }

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
    AVFrame           *pFrame = NULL;

    AVPacket          packet;
    int               numBytes;
    int               width;
    int               height;
    uint8_t           *buffer = NULL;
    tb_event*         key_event = (tb_event*)malloc(sizeof(tb_event));

    LockedFrame* frame1 = (LockedFrame*)calloc(1, sizeof(LockedFrame));
    LockedFrame* frame2 = (LockedFrame*)calloc(1, sizeof(LockedFrame));
    LockedFrame* frame3 = (LockedFrame*)calloc(1, sizeof(LockedFrame));

    frame1->audio_frame = av_frame_alloc();
    frame2->audio_frame = av_frame_alloc();
    frame3->audio_frame = av_frame_alloc();

    frame1->id = 1;
    frame2->id = 2;
    frame3->id = 3;

    frame1->next = frame2;
    frame2->next = frame3;
    frame3->next = frame1;

    frame1->mtx = new std::mutex;
    frame2->mtx = new std::mutex;
    frame3->mtx = new std::mutex;

    if(argc < 2) {
        printf("Please provide a movie file\n");
        return -1;
    }

    err = Pa_Initialize();
    if(err != paNoError){
        std::cout << "Failed initializing PA" << std::endl;
        exit(1);
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

    // Alloc format context
    pFormatCtx = avformat_alloc_context();

    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        return -1; // Couldn't open file

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    // Allocate audio codec and find the best stream
    AVCodec* audioCodec;
    if((audioStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0)) < 0)
        return -1;

    AVCodecContext* audio_codec_ctx = avcodec_alloc_context3(audioCodec);
    avcodec_parameters_to_context(audio_codec_ctx, pFormatCtx->streams[audioStream]->codecpar);

    if(avcodec_open2(audio_codec_ctx, audioCodec, NULL) < 0){
        fprintf(stderr, "error: avcodec_open2()\n");
        exit(1);
    }

    outputParameters.device = Pa_GetDefaultOutputDevice();
    if(outputParameters.device == paNoDevice){
        std::cerr << "No default output device" << std::endl;
        exit(1);
    }

    outputParameters.channelCount = audio_codec_ctx->channels;
    outputParameters.sampleFormat = paInt16;
    outputParameters.suggestedLatency = 0.050;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(&paStream,
            NULL,
            &outputParameters,
            audio_codec_ctx->sample_rate,
            // Random value large enough?
            1024,
            paClipOff,
            NULL,
            NULL);

    if(err != paNoError){
        std::cerr << "Failed opening PA stream" << std::endl;
        exit(1);
    }

    // Allocate software resampling context
    swr_ctx =
        swr_alloc_set_opts(NULL,
                AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT,
                AV_SAMPLE_FMT_S16,
                audio_codec_ctx->sample_rate,
                audio_codec_ctx->channel_layout,
                audio_codec_ctx->sample_fmt,
                audio_codec_ctx->sample_rate,
                0,
                NULL);

    if(!swr_ctx){
        fprintf(stderr, "error: swr_alloc_set_opts()\n");
        exit(1);
    }

    // Initiate termbox for drawing to the screen. My fork is needed.
    if(argc == 2){
        tb_select_output_mode(TB_OUTPUT_256);
    } else {
        tb_select_output_mode(TB_OUTPUT_TRUECOLOR);
    }
    tb_init();

    // Initiate resampling context
    swr_init(swr_ctx);

    AVCodec* videoCodec;
    if((videoStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &videoCodec, 0)) < 0)
        return -1;

    video_codec_ctx = avcodec_alloc_context3(videoCodec);
    avcodec_parameters_to_context(video_codec_ctx, pFormatCtx->streams[videoStream]->codecpar);

    // Open video codec
    if(avcodec_open2(video_codec_ctx, videoCodec, NULL)<0)
        return -1; // Could not open codec

    // Allocate video frame
    pFrame=av_frame_alloc();

    // Allocate an AVFrame structure
    pFrameRGB=av_frame_alloc();
    if(pFrameRGB==NULL)
        return -1;

    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_codec_ctx->width, video_codec_ctx->height, 1);
    //pCodecCtx->height);
    buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
    pixels=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, video_codec_ctx->width, video_codec_ctx->height, 1);    // Where the fuck is alignment defined? Someone told me to just use 1, seems to work

    // Initialize SWS context for software scaling
    sws_ctx = sws_getContext(video_codec_ctx->width,
            video_codec_ctx->height,
            video_codec_ctx->pix_fmt,
            video_codec_ctx->width,
            video_codec_ctx->height,
            AV_PIX_FMT_RGB24,
            SWS_BILINEAR,
            NULL,
            NULL,
            NULL
            );

    LockedFrame *frame = frame1;
    init_lock.lock();

    // Initiate new thread for drawing the image
    std::thread trd(renderImage, width, height, video_codec_ctx->width, video_codec_ctx->height, pFrame);
    std::thread aud(playSound, frame);

    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    AVFrame* output = av_frame_alloc();

    err = Pa_StartStream(paStream);
    if(err != paNoError){
        std::cerr << "Failed starting stream" << std::endl;
        exit(1);
    }

    // Main loop
    int frames = 0;
    int ret = 0;
    while(av_read_frame(pFormatCtx, &packet)>=0) {

        // If user presses ESC or Q, quit
        if(tb_peek_event(key_event, 1) && (key_event->key == TB_KEY_ESC || key_event->ch == 'q')){
            break;
        }

        // Is this a packet from the video stream?
        if(packet.stream_index == videoStream) {
            video_lock.lock();
            // Decode video frame
            avcodec_send_packet(video_codec_ctx, &packet);
            video_lock.unlock();

            cv.notify_all();
        }

        if(packet.stream_index == audioStream){
            // Decode packet

            frame->mtx->lock();
            avcodec_send_packet(audio_codec_ctx, &packet);

            avcodec_receive_frame(audio_codec_ctx, output);
            //frame->audio_frame->nb_samples = output->nb_samples;

            if(frame->audio_frame->nb_samples != output->nb_samples){
                av_frame_unref(frame->audio_frame);

                // get new buffers and stuff
                frame->audio_frame->format = AV_SAMPLE_FMT_S16;
                frame->audio_frame->channels = output->channels;
                frame->audio_frame->channel_layout = output->channel_layout;
                frame->audio_frame->nb_samples = output->nb_samples;

                ret = av_frame_get_buffer(frame->audio_frame, 32);
                if(ret < 0){
                    std::cerr << "failed getting new buffer" << std::endl;
                    exit(1);
                }

                if(frames != output->nb_samples){

                    /*err = Pa_StopStream(paStream);
                    if(err != paNoError){
                        std::cerr << "Failed stopping PA stream after update" << std::endl;
                        exit(1);
                    }*/

                    err = Pa_OpenStream(&paStream,
                            NULL,
                            &outputParameters,
                            audio_codec_ctx->sample_rate,
                            output->nb_samples,
                            paClipOff,
                            NULL,
                            NULL);
                    if(err != paNoError){
                        std::cerr << "Failed opening PA stream after update" << std::endl;
                        exit(1);
                    }

                    err = Pa_StartStream(paStream);
                    if(err != paNoError){
                        std::cerr << "Failed starting PA stream after update" << std::endl;
                        exit(1);
                    }

                    ca.notify_all();
                    frames = output->nb_samples;
                }

            }
            // we should get all samples because our buffer should be large
            // enough, hopefully
            frame->got_samples = swr_convert(
                    swr_ctx,
                    frame->audio_frame->data, frame->audio_frame->nb_samples,
                    (const uint8_t **)output->data, output->nb_samples
                    );

            if(frame->got_samples < 0){
                std::cerr << "error: swr_convert" << std::endl;
                exit(1);
            }

            frame->audio_buffer_size = av_samples_get_buffer_size(NULL,
                    frame->audio_frame->channels,
                    frame->audio_frame->nb_samples,
                    AV_SAMPLE_FMT_S16, 32);

            frame->mtx->unlock();

            frame = frame->next;
            init_lock.unlock();

        }
        // Free the packet that was allocated by av_read_frame
        av_packet_unref(&packet);
    }
    //Pa_StopStream(paStream);
    // Tell other threads to stop
    frame->got_samples = -1;
    done = true;

    aud.join();
    trd.join();

    // Free everything
    err = Pa_CloseStream(paStream);
    if(err != paNoError){
        std::cerr << "Failed closing PA stream in cleanup" << std::endl;
        exit(1);
    }
    Pa_Terminate();
    tb_shutdown();


    swr_free(&swr_ctx);
    sws_freeContext(sws_ctx);
    av_free(buffer);
    av_free(pixels);
    av_frame_free(&pFrameRGB);
    av_frame_free(&pFrame);
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);
    avcodec_free_context(&pCodecCtx);
    avcodec_close(audio_codec_ctx);
    avcodec_free_context(&audio_codec_ctx);
    avcodec_free_context(&pCodecCtxOrig);
    avformat_close_input(&pFormatCtx);
    free(key_event);

    return 0;
}
