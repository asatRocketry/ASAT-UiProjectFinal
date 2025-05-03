#ifndef RTSP2WS_VIDEO_H
#define RTSP2WS_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the video streaming module with the RTSP URL.
int rtsp_video_init(const char *rtsp_url);

// Start the video streaming thread.
int rtsp_video_start(void);

// Stop the video streaming thread and clean up resources.
void rtsp_video_stop(void);

#ifdef __cplusplus
}
#endif

#endif // RTSP2WS_VIDEO_H
