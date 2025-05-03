#include "rtsp2ws_video.h"
#include "video_ws.h"       // Provides g_video_clients, g_video_clients_mutex, etc.
#include "websocket.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PKT 2000000

static const char *g_rtsp_url = NULL;
static volatile int g_video_shutdown = 0;
static pthread_t g_video_thread;

//
// Broadcast a WebSocket frame to all video clients (managed by video_ws.c)
//
static void broadcast_video_frame(uint8_t *wsbuf, size_t wslen) {
    pthread_mutex_lock(&g_video_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_video_clients[i].fd != -1 && g_video_clients[i].handshake_done) {
            if (send(g_video_clients[i].fd, wsbuf, wslen, 0) < 0) {
                perror("send() video client");
            }
        }
    }
    pthread_mutex_unlock(&g_video_clients_mutex);
}

//
// Attempt to extract SPS (NAL type 7) and PPS (NAL type 8) from a keyframe packet
// and send them as a configuration record (avcC) to the clients.
// Returns 0 on success, -1 on failure.
//
static int send_config_from_packet(AVPacket *pkt, AVCodecContext *ctx, uint8_t *wsbuf, size_t *wslen) {
    // Assume pkt->data is in Annex B format.
    uint8_t *data = pkt->data;
    int size = pkt->size;
    uint8_t *sps = NULL, *pps = NULL;
    int sps_size = 0, pps_size = 0;
    
    // Search for start code "00 00 00 01" and then check the NAL type.
    for (int i = 0; i < size - 4; i++) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            int nal_type = data[i+4] & 0x1F;
            if (nal_type == 7 && sps == NULL) {
                sps = &data[i+4];
                // Find length by scanning for next start code
                int j = i+4;
                while(j < size-4 && !(data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1))
                    j++;
                sps_size = j - (i+4);
            } else if (nal_type == 8 && pps == NULL) {
                pps = &data[i+4];
                int j = i+4;
                while(j < size-4 && !(data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1))
                    j++;
                pps_size = j - (i+4);
            }
            if (sps && pps)
                break;
        }
    }
    
    if (!sps || !pps || sps_size <= 0 || pps_size <= 0) {
        fprintf(stderr, "[ERROR] Could not extract SPS/PPS from packet\n");
        return -1;
    }
    
    // Build an avcC configuration record.
    int avcc_size = 5 + 1 + 2 + sps_size + 1 + 2 + pps_size;
    uint8_t *avcc = malloc(avcc_size);
    if (!avcc) {
        perror("malloc");
        return -1;
    }
    int offset = 0;
    avcc[offset++] = 1; // configurationVersion
    avcc[offset++] = sps[1]; // AVCProfileIndication
    avcc[offset++] = sps[2]; // profile_compatibility
    avcc[offset++] = sps[3]; // AVCLevelIndication
    avcc[offset++] = 0xFF;   // reserved + lengthSizeMinusOne

    avcc[offset++] = 0xE1;   // reserved + number of SPS (1)
    avcc[offset++] = (sps_size >> 8) & 0xFF;
    avcc[offset++] = sps_size & 0xFF;
    memcpy(&avcc[offset], sps, sps_size);
    offset += sps_size;

    avcc[offset++] = 1;      // number of PPS
    avcc[offset++] = (pps_size >> 8) & 0xFF;
    avcc[offset++] = pps_size & 0xFF;
    memcpy(&avcc[offset], pps, pps_size);
    offset += pps_size;

    // Create a WebSocket binary frame from the configuration.
    ws_create_binary_frame(avcc, offset, wsbuf, wslen);
    free(avcc);
    return 0;
}

//
// The video stream thread: opens the RTSP stream, sets up the decoder,
// sends SPS/PPS configuration immediately if possible (or on first keyframe),
// and then continuously broadcasts video frames.
//
static void *video_stream_thread(void *arg) {
    (void)arg;
    int ret;
    int video_stream_index = -1;
    int sent_sps = 0;
    uint8_t wsbuf[10 + MAX_PKT];
    size_t wslen = 0;
    
    AVDictionary *opts = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVPacket *packet = NULL;
    
    while (!g_video_shutdown) {
        printf("[VIDEO] Opening RTSP stream: %s\n", g_rtsp_url);
        
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "max_delay", "500000", 0);  // 500ms delay
        
        ret = avformat_open_input(&fmt_ctx, g_rtsp_url, NULL, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            fprintf(stderr, "[ERROR] avformat_open_input failed (%d)\n", ret);
            sleep(1);
            continue;
        }
        
        if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
            fprintf(stderr, "[ERROR] avformat_find_stream_info failed (%d)\n", ret);
            avformat_close_input(&fmt_ctx);
            sleep(1);
            continue;
        }
        
        video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (video_stream_index < 0) {
            fprintf(stderr, "[ERROR] No video stream found\n");
            avformat_close_input(&fmt_ctx);
            sleep(1);
            continue;
        }
        printf("[DEBUG] Found video stream index: %d\n", video_stream_index);
        
        AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
        const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder) {
            fprintf(stderr, "[ERROR] Unsupported codec (%d)\n", codecpar->codec_id);
            avformat_close_input(&fmt_ctx);
            sleep(1);
            continue;
        }
        
        codec_ctx = avcodec_alloc_context3(decoder);
        if (!codec_ctx || avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
            fprintf(stderr, "[ERROR] Failed to copy codec parameters to context\n");
            if (codec_ctx)
                avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            sleep(1);
            continue;
        }
        
        // Force global header so that extradata (SPS/PPS) is included if available.
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        
        if (avcodec_open2(codec_ctx, decoder, NULL) < 0) {
            fprintf(stderr, "[ERROR] Failed to open decoder\n");
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            sleep(1);
            continue;
        }
        
        // Immediately send extradata if available.
        if (codec_ctx->extradata && codec_ctx->extradata_size > 0) {
            printf("[DEBUG] Sending extradata immediately (size: %d)\n", codec_ctx->extradata_size);
            ws_create_binary_frame(codec_ctx->extradata, codec_ctx->extradata_size, wsbuf, &wslen);
            broadcast_video_frame(wsbuf, wslen);
            sent_sps = 1;
        } else {
            fprintf(stderr, "[WARN] extradata missing or empty after decoder init\n");
            sent_sps = 0;
        }
        
        packet = av_packet_alloc();
        if (!packet) {
            fprintf(stderr, "[ERROR] av_packet_alloc failed\n");
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            sleep(1);
            continue;
        }
        
        // Read packets and broadcast video frames.
        while (!g_video_shutdown && (ret = av_read_frame(fmt_ctx, packet)) >= 0) {
            if (packet->stream_index != video_stream_index) {
                av_packet_unref(packet);
                continue;
            }
            
            printf("[DEBUG] Packet received: size=%d, keyframe=%d\n", packet->size, packet->flags & AV_PKT_FLAG_KEY);
            
            // If extradata hasn't been sent, try to send it from the keyframe.
            if (!sent_sps && (packet->flags & AV_PKT_FLAG_KEY)) {
                if (codec_ctx->extradata && codec_ctx->extradata_size > 0) {
                    printf("[DEBUG] Sending extradata after first keyframe (size: %d)\n", codec_ctx->extradata_size);
                    ws_create_binary_frame(codec_ctx->extradata, codec_ctx->extradata_size, wsbuf, &wslen);
                    broadcast_video_frame(wsbuf, wslen);
                    sent_sps = 1;
                } else {
                    printf("[DEBUG] extradata missing; attempting extraction from keyframe packet\n");
                    if (send_config_from_packet(packet, codec_ctx, wsbuf, &wslen) == 0) {
                        broadcast_video_frame(wsbuf, wslen);
                        sent_sps = 1;
                    }
                }
            }
            
            // Broadcast the current video frame.
            ws_create_binary_frame(packet->data, packet->size, wsbuf, &wslen);
            broadcast_video_frame(wsbuf, wslen);
            
            av_packet_unref(packet);
        }
        
        if (ret != AVERROR_EOF)
            fprintf(stderr, "[ERROR] av_read_frame failed (%d)\n", ret);
        
        av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        
        sleep(1);
    }
    
    return NULL;
}

int rtsp_video_init(const char *rtsp_url) {
    if (!rtsp_url)
        return -1;
    g_rtsp_url = rtsp_url;
    g_video_shutdown = 0;
    avformat_network_init();
    return 0;
}

int rtsp_video_start(void) {
    if (pthread_create(&g_video_thread, NULL, video_stream_thread, NULL) != 0) {
        perror("pthread_create() video");
        return -1;
    }
    return 0;
}

void rtsp_video_stop(void) {
    g_video_shutdown = 1;
    pthread_join(g_video_thread, NULL);
}
