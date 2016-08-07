// g++ -std=c++11 -o ge2dplayer main.cpp -L/usr/lib/aml_libs/ -lamcodec -lamadec -lamavutils -lasound -lpthread

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>	//mmap
#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <cstdlib>	//rand
#include <errno.h>
//#include <linux/videodev2.h> // V4L
#include <map>
#include <queue>
#include "ge2d.h"
#include "ge2d_cmd.h"


// The headers are not aware C++ exists
extern "C"
{
	//#include <amcodec/codec.h>
#include <codec.h>
}

// Codec parameter flags
//    size_t is used to make it 
//    64bit safe for use on Odroid C2
const size_t EXTERNAL_PTS = 0x01;
const size_t SYNC_OUTSIDE = 0x02;
const size_t USE_IDR_FRAMERATE = 0x04;
const size_t UCODE_IP_ONLY_PARAM = 0x08;
const size_t MAX_REFER_BUF = 0x10;
const size_t ERROR_RECOVERY_MODE_IN = 0x20;

// Buffer size
const int BUFFER_SIZE = 1024 * 64;	// 4K video expected
const int MAX_SCREEN_BUFFERS = 2;

									// vfm_grabber experimental support
#define MAX_PLANE_COUNT 3

typedef unsigned int u32;

typedef struct
{
	u32 index;
	ulong addr;
	u32 width;
	u32 height;
} canvas_info_t;

typedef struct
{
	int dma_fd[MAX_PLANE_COUNT];
	int width;
	int height;
	int stride;
	void *priv;
	int cropWidth;
	int cropHeight;
	canvas_info_t canvas_plane0;
	canvas_info_t canvas_plane1;
	canvas_info_t canvas_plane2;
} vfm_grabber_frame;

typedef struct
{
	int frames_decoded;
	int frames_ready;
} vfm_grabber_info;

#define VFM_GRABBER_GET_FRAME   _IOWR('V', 0x01, vfm_grabber_frame)
#define VFM_GRABBER_GET_INFO    _IOWR('V', 0x02, vfm_grabber_info)
#define VFM_GRABBER_PUT_FRAME   _IOWR('V', 0x03, vfm_grabber_frame)


// Global variable(s)
bool isRunning;
//int dmabuf_fd = -1;
timeval startTime;
timeval endTime;


void ResetTime()
{
	gettimeofday(&startTime, NULL);
	endTime = startTime;
}

float GetTime()
{
	gettimeofday(&endTime, NULL);
	float seconds = (endTime.tv_sec - startTime.tv_sec);
	float milliseconds = (float(endTime.tv_usec - startTime.tv_usec)) / 1000000.0f;

	startTime = endTime;

	return seconds + milliseconds;
}



// Signal handler for Ctrl-C
void SignalHandler(int s)
{
	isRunning = false;
}

void* VideoDecoderThread(void* argument)
{
	// Initialize the codec
	codec_para_t codecContext = { 0 };

#if 0

	//const char* fileName = "test.h264";
	//codecContext.stream_type = STREAM_TYPE_ES_VIDEO;
	//codecContext.video_type = VFORMAT_H264;
	//codecContext.has_video = 1;
	//codecContext.noblock = 0;
	//codecContext.am_sysinfo.format = VIDEO_DEC_FORMAT_H264;
	////codecContext.am_sysinfo.rate = (96000.0 / (24000.0 / 1001.0));	// 24 fps
	//codecContext.am_sysinfo.param = (void*)(SYNC_OUTSIDE);


	// 4K
	const char* fileName = "test.h264";
	codecContext.stream_type = STREAM_TYPE_ES_VIDEO;
	codecContext.video_type = VFORMAT_H264_4K2K;
	codecContext.has_video = 1;
	codecContext.noblock = 0;
	codecContext.am_sysinfo.format = VIDEO_DEC_FORMAT_H264_4K2K;
	//codecContext.am_sysinfo.rate = (96000.0 / (24000.0 / 1001.0));	// 24 fps
	codecContext.am_sysinfo.param = (void*)(SYNC_OUTSIDE);

#else

	const char* fileName = "test.hevc";
	codecContext.stream_type = STREAM_TYPE_ES_VIDEO;
	codecContext.video_type = VFORMAT_HEVC;
	codecContext.has_video = 1;
	codecContext.noblock = 0;
	codecContext.am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
	//codecContext.am_sysinfo.rate = (96000.0 / (24000.0 / 1001.0));	
	codecContext.am_sysinfo.param = (void*)(SYNC_OUTSIDE);

#endif


	int api = codec_init(&codecContext);
	if (api != 0)
	{
		printf("codec_init failed (%x).\n", api);
		exit(1);
	}

	//codec_set_cntl_avthresh(&codecContext, AV_SYNC_THRESH);
	//codec_set_cntl_mode(&codecContext, TRICKMODE_NONE);
	//codec_set_cntl_syncthresh(&codecContext, 0);

	// Open the media file
	int fd = open(fileName, O_RDONLY);
	if (fd < 0)
	{
		printf("test file could not be opened.");
		exit(1);
	}


	unsigned char buffer[BUFFER_SIZE];

	while (isRunning)
	{
		// Read the ES video data from the file
		int bytesRead;
		while (true)
		{
			bytesRead = read(fd, &buffer, BUFFER_SIZE);
			if (bytesRead > 0)
			{
				break;
			}

			// Loop the video when the end is reached
			lseek(fd, 0, SEEK_SET);
		}

		// Send the data to the codec
		int api = 0;
		int offset = 0;
		do
		{
			api = codec_write(&codecContext, &buffer + offset, bytesRead - offset);
			if (api == -EAGAIN)
			{
				usleep(100);
			}
			else if (api == -1)
			{
				// TODO: Sometimes this error is returned.  Ignoring it
				// does not seem to have any impact on video display
			}
			else if (api < 0)
			{
				printf("codec_write error: %x\n", api);
				//codec_reset(&codecContext);
			}
			else if (api > 0)
			{
				offset += api;
			}

		} while (api == -EAGAIN || offset < bytesRead);
	}


	// Close the codec and media file
	codec_close(&codecContext);
	close(fd);


	return NULL;
}

void WriteToFile(const char* path, const char* value)
{
	int fd = open(path, O_RDWR | O_TRUNC, 0644);
	if (fd < 0)
	{
		printf("WriteToFile open failed: %s = %s\n", path, value);
		exit(1);
	}

	if (write(fd, value, strlen(value)) < 0)
	{
		printf("WriteToFile write failed: %s = %s\n", path, value);
		exit(1);
	}

	close(fd);
}

void SetVfmState()
{
	// vfm_grabber
	WriteToFile("/sys/class/vfm/map", "rm default");
	WriteToFile("/sys/class/vfm/map", "add default decoder vfm_grabber");

	// Use NV21 instead of compressed format for hevc
	WriteToFile("/sys/module/amvdec_h265/parameters/double_write_mode", "1");
}

void ResetVfmState()
{
	// TODO
	WriteToFile("/sys/class/vfm/map", "rm default");
	WriteToFile("/sys/class/vfm/map", "add default decoder ppmgr deinterlace amvideo");

	WriteToFile("/sys/module/amvdec_h265/parameters/double_write_mode", "0");
}

int OpenVfmGrabber()
{
	int fd = open("/dev/vfm_grabber", O_RDWR); //| O_NONBLOCK
	if (fd < 0)
	{
		printf("open vfm_grabber failed.");
		exit(1);
	}

	printf("vfm_grabber file handle: %x\n", fd);

	return fd;
}

int OpenDisplay()
{
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0)
	{
		printf("open /dev/fb0 failed.");
		exit(1);
	}

	printf("/dev/fb0 file handle: %x\n", fd);

	return fd;
}

int OpenGe2d()
{
	int fd = open("/dev/ge2d", O_RDWR);
	if (fd < 0)
	{
		printf("open /dev/ge2d failed.");
		exit(1);
	}

	printf("/dev/ge2d file handle: %x\n", fd);

	return fd;
}

void EnableDisplay()
{
	WriteToFile("/sys/class/graphics/fb0/blank", "0");
}

int main()
{
	// Intialize
	isRunning = true;

	// Trap signal to clean up
	signal(SIGINT, SignalHandler);

	// Ionvideo will not generate frames until connected
	SetVfmState();


	// Display
	EnableDisplay();

	int fb_fd = OpenDisplay();
	
	fb_var_screeninfo var_info;
	int fbCall = ioctl(fb_fd, FBIOGET_VSCREENINFO, &var_info);
	if (fbCall < 0)
	{
		printf("FBIOGET_VSCREENINFO failed.\n");
		exit(1);
	}

	printf("var_info.xres = %d\n", var_info.xres);
	printf("var_info.yres = %d\n", var_info.yres);
	
	//printf("var_info.width = %d\n", var_info.width);
	//printf("var_info.height = %d\n", var_info.height);
	
	printf("var_info.xres_virtual = %d\n", var_info.xres_virtual);
	printf("var_info.yres_virtual = %d\n", var_info.yres_virtual);


	// GE2D
	int ge2d_fd = OpenGe2d();


	// vfm_grabber support
	int vfmFD = OpenVfmGrabber();


	// ----- start decoder -----
	pthread_t thread;
	int result_code = pthread_create(&thread, NULL, VideoDecoderThread, NULL);
	if (result_code != 0)
	{
		printf("pthread_create failed.\n");
		exit(1);
	}


	// ----- RENDERING -----
	int frames = 0;
	float totalTime = 0;

	ResetTime();

	int currentBuffer = 0;


	while (isRunning)
	{
		// Grab a frame
		vfm_grabber_frame frame;
		int vfmCall = ioctl(vfmFD, VFM_GRABBER_GET_FRAME, &frame);
		if (vfmCall < 0)
		{
			printf("VFM_GRABBER_GET_FRAME ioctl failed. (%x)\n", vfmCall);
			//exit(1);
		}


		// Blit
		// Configure GE2D
		int src_index = ((frame.canvas_plane0.index & 0xff) | ((frame.canvas_plane1.index << 8) & 0x0000ff00));
		struct config_para_ex_s configex = { 0 };

		configex.src_para.mem_type = CANVAS_TYPE_INVALID;
		configex.src_para.format = GE2D_FORMAT_M24_NV21;
		configex.src_para.canvas_index = src_index;
		configex.src_para.left = 0;
		configex.src_para.top = 0;
		configex.src_para.width = frame.cropWidth;
		configex.src_para.height = frame.cropHeight;

		configex.src2_para.mem_type = CANVAS_TYPE_INVALID;

		configex.dst_para.mem_type = CANVAS_OSD0;
		configex.dst_para.format = GE2D_FORMAT_S32_ARGB;
		configex.dst_para.left = 0;
		configex.dst_para.top = 0;
		configex.dst_para.width = var_info.xres;
		configex.dst_para.height = var_info.yres_virtual;

		int ge2dCall = ioctl(ge2d_fd, GE2D_CONFIG_EX, &configex);
		if (ge2dCall < 0)
		{
			printf("GE2D_CONFIG_EX failed.\n");
		}


		// Perform the blit operation
		struct ge2d_para_s blitRect = { 0 };

		blitRect.src1_rect.x = 0;
		blitRect.src1_rect.y = 0;
		blitRect.src1_rect.w = configex.src_para.width;
		blitRect.src1_rect.h = configex.src_para.height;
		
		blitRect.dst_rect.x = 0;
		blitRect.dst_rect.y = var_info.yres * currentBuffer;
		blitRect.dst_rect.w = var_info.xres;
		blitRect.dst_rect.h = var_info.yres;

		ge2dCall = ioctl(ge2d_fd, GE2D_STRETCHBLIT_NOALPHA, &blitRect);
		if (ge2dCall < 0)
		{
			printf("GE2D_STRETCHBLIT_NOALPHA failed.\n");
		}


		// Return frame
		vfmCall = ioctl(vfmFD, VFM_GRABBER_PUT_FRAME, &frame);
		if (vfmCall < 0)
		{
			printf("VFM_GRABBER_PUT_FRAME ioctl failed.\n");
			exit(1);
		}


		// Flip
		var_info.yoffset = var_info.yres * currentBuffer;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var_info);

		// Wait for vsync
		// The normal order is to wait for vsync and then pan,
		// but its done backwards due to the wait its implemented
		// by Amlogic (non-syncronous).
		ioctl(fb_fd, FBIO_WAITFORVSYNC, 0);


		++currentBuffer;
		if (currentBuffer >= MAX_SCREEN_BUFFERS)
		{
			currentBuffer = 0;
		}


		// FPS
		float deltaTime = GetTime();

		totalTime += deltaTime;
		++frames;

		if (totalTime >= 1.0f)
		{
			int fps = (int)(frames / totalTime);
			printf("FPS: %i\n", fps);

			frames = 0;
			totalTime = 0;
		}
	}

	void *retval;
	pthread_join(thread, &retval);

	ResetVfmState();

	return 0;
}