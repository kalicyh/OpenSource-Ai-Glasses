
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>
#include <stdatomic.h>

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_adec.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_ivs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"

static FILE *venc0_file;
static RK_S32 g_s32FrameCnt = -1;
static bool quit = false;

// IMU logging globals
static pthread_t g_imu_thread;
static bool g_imu_running = false;
static FILE *g_imu_file = NULL;

// Shared atomic frame counter
static atomic_uint_fast64_t g_frame_counter = 0;

// IMU sysfs paths (gyro=device1, accel=device2)
static const char *IMU_GYRO_X = "/sys/bus/iio/devices/iio:device1/in_anglvel_x_raw";
static const char *IMU_GYRO_Y = "/sys/bus/iio/devices/iio:device1/in_anglvel_y_raw";
static const char *IMU_GYRO_Z = "/sys/bus/iio/devices/iio:device1/in_anglvel_z_raw";
static const char *IMU_ACCEL_X = "/sys/bus/iio/devices/iio:device2/in_accel_x_raw";
static const char *IMU_ACCEL_Y = "/sys/bus/iio/devices/iio:device2/in_accel_y_raw";
static const char *IMU_ACCEL_Z = "/sys/bus/iio/devices/iio:device2/in_accel_z_raw";
static const char *IMU_ACCEL_FREQ = "/sys/bus/iio/devices/iio:device2/sampling_frequency";
static const char *IMU_GYRO_FREQ = "/sys/bus/iio/devices/iio:device1/sampling_frequency";

static void sigterm_handler(int sig) {
	fprintf(stderr, "signal %d\n", sig);
	quit = true;
}

RK_U64 TEST_COMM_GetNowUs() {
	struct timespec time = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000; /* microseconds */
}

static int write_sysfs_str(const char *path, const char *buf) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, buf, strlen(buf));
    close(fd);
    return (n == (ssize_t)strlen(buf)) ? 0 : -1;
}

static int read_sysfs_int_fd(int fd, int *out) {
    char buf[64];
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    *out = atoi(buf);
    return 0;
}

static void *imu_logger_thread(void *arg) {
    (void)arg;
    // Open all sensors once
    int fd_ax = open(IMU_ACCEL_X, O_RDONLY | O_CLOEXEC);
    int fd_ay = open(IMU_ACCEL_Y, O_RDONLY | O_CLOEXEC);
    int fd_az = open(IMU_ACCEL_Z, O_RDONLY | O_CLOEXEC);
    int fd_gx = open(IMU_GYRO_X,  O_RDONLY | O_CLOEXEC);
    int fd_gy = open(IMU_GYRO_Y,  O_RDONLY | O_CLOEXEC);
    int fd_gz = open(IMU_GYRO_Z,  O_RDONLY | O_CLOEXEC);
    if (fd_ax < 0 || fd_ay < 0 || fd_az < 0 || fd_gx < 0 || fd_gy < 0 || fd_gz < 0) {
        fprintf(stderr, "IMU: failed to open sensor nodes\n");
        if (fd_ax >= 0) close(fd_ax); if (fd_ay >= 0) close(fd_ay); if (fd_az >= 0) close(fd_az);
        if (fd_gx >= 0) close(fd_gx); if (fd_gy >= 0) close(fd_gy); if (fd_gz >= 0) close(fd_gz);
        g_imu_running = false;
        return NULL;
    }

    // Track last seen frame number
    uint64_t last_seen_frame = atomic_load_explicit(&g_frame_counter, memory_order_acquire);

    while (!quit && g_imu_running) {
        uint64_t current_frame = atomic_load_explicit(&g_frame_counter, memory_order_acquire);
        if (current_frame != last_seen_frame) {
            int ax=0, ay=0, az=0, gx=0, gy=0, gz=0;
            (void)read_sysfs_int_fd(fd_ax, &ax);
            (void)read_sysfs_int_fd(fd_ay, &ay);
            (void)read_sysfs_int_fd(fd_az, &az);
            (void)read_sysfs_int_fd(fd_gx, &gx);
            (void)read_sysfs_int_fd(fd_gy, &gy);
            (void)read_sysfs_int_fd(fd_gz, &gz);

            if (g_imu_file) {
                // CSV: frame_no,ax,ay,az,gx,gy,gz
                fprintf(g_imu_file, "%llu,%d,%d,%d,%d,%d,%d\n",
                        (unsigned long long)current_frame, ax, ay, az, gx, gy, gz);
            }
            last_seen_frame = current_frame;
        } else {
            struct timespec req = {0, 1000000L}; // ~1ms sleep to avoid busy spin
            nanosleep(&req, NULL);
        }
    }

    if (g_imu_file) { fflush(g_imu_file); }
    close(fd_ax); close(fd_ay); close(fd_az);
    close(fd_gx); close(fd_gy); close(fd_gz);
    return NULL;
}

static int imu_start_logging(const char *video_path) {
    // Set sampling frequency before logging
    (void)write_sysfs_str(IMU_ACCEL_FREQ, "104");
    (void)write_sysfs_str(IMU_GYRO_FREQ,  "104");

    // Build imu log path: <video_path>.imu.txt
    char imu_path[512];
    snprintf(imu_path, sizeof(imu_path), "%s.imu.txt", video_path ? video_path : "/tmp/imu");
    g_imu_file = fopen(imu_path, "w");
    if (!g_imu_file) {
        fprintf(stderr, "IMU: failed to open log file: %s\n", imu_path);
        // still try to run to not block video
    } else {
        fprintf(g_imu_file, "frame_no,ax,ay,az,gx,gy,gz\n");
        fflush(g_imu_file);
    }

    g_imu_running = true;
    if (pthread_create(&g_imu_thread, NULL, imu_logger_thread, NULL) != 0) {
        fprintf(stderr, "IMU: failed to start thread\n");
        g_imu_running = false;
        if (g_imu_file) { fclose(g_imu_file); g_imu_file = NULL; }
        return -1;
    }
    return 0;
}

static void imu_stop_logging(void) {
    if (g_imu_running) {
        g_imu_running = false;
        pthread_join(g_imu_thread, NULL);
    }
    if (g_imu_file) { fclose(g_imu_file); g_imu_file = NULL; }
}

static void *GetMediaBuffer0(void *arg) {
	(void)arg;
	printf("========%s========\n", __func__);
	void *pData = RK_NULL;
	int loopCount = 0;
	int s32Ret;

	VENC_STREAM_S stFrame;
	stFrame.pstPack = malloc(sizeof(VENC_PACK_S));

	while (!quit) {
		s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
		if (s32Ret == RK_SUCCESS) {
			if (venc0_file) {
				pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
				fwrite(pData, 1, stFrame.pstPack->u32Len, venc0_file);
				fflush(venc0_file);
			}
			RK_U64 nowUs = TEST_COMM_GetNowUs();

			RK_LOGD("chn:0, loopCount:%d enc->seq:%d wd:%d pts=%lld delay=%lldus\n",
			        loopCount, stFrame.u32Seq, stFrame.pstPack->u32Len,
			        stFrame.pstPack->u64PTS, nowUs - stFrame.pstPack->u64PTS);

			s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
			if (s32Ret != RK_SUCCESS) {
				RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
			}

			// Increase global frame counter once per encoded frame
			atomic_fetch_add_explicit(&g_frame_counter, 1, memory_order_release);

			loopCount++;
		} else {
			RK_LOGE("RK_MPI_VI_GetChnFrame fail %x", s32Ret);
		}

		if ((g_s32FrameCnt >= 0) && (loopCount > g_s32FrameCnt)) {
			quit = true;
			break;
		}
	}

	if (venc0_file)
		fclose(venc0_file);

	free(stFrame.pstPack);
	return NULL;
}

static RK_S32 test_venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType) {
	printf("========%s========\n", __func__);
	VENC_RECV_PIC_PARAM_S stRecvParam;
	VENC_CHN_ATTR_S stAttr;
	memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

	if (enType == RK_VIDEO_ID_AVC) {
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
		stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;
		stAttr.stRcAttr.stH264Cbr.u32Gop = 60;
	} else if (enType == RK_VIDEO_ID_HEVC) {
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
		stAttr.stRcAttr.stH265Cbr.u32BitRate = 10 * 1024;
		stAttr.stRcAttr.stH265Cbr.u32Gop = 60;
	} else if (enType == RK_VIDEO_ID_MJPEG) {
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
		stAttr.stRcAttr.stMjpegCbr.u32BitRate = 10 * 1024;
	}

	stAttr.stVencAttr.enType = enType;
	stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	if (enType == RK_VIDEO_ID_AVC)
		stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
	stAttr.stVencAttr.u32PicWidth = width;
	stAttr.stVencAttr.u32PicHeight = height;
	stAttr.stVencAttr.u32VirWidth = width;
	stAttr.stVencAttr.u32VirHeight = height;
	stAttr.stVencAttr.u32StreamBufCnt = 2;
	stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
	stAttr.stVencAttr.enMirror = MIRROR_NONE;

	RK_MPI_VENC_CreateChn(chnId, &stAttr);

	// stRecvParam.s32RecvPicNum = 100;		//recv 100 slice
	// RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	stRecvParam.s32RecvPicNum = -1;
	RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

	return 0;
}

// demo板dev默认都是0，根据不同的channel 来选择不同的vi节点
int vi_dev_init() {
	printf("%s\n", __func__);
	int ret = 0;
	int devId = 0;
	int pipeId = devId;

	VI_DEV_ATTR_S stDevAttr;
	VI_DEV_BIND_PIPE_S stBindPipe;
	memset(&stDevAttr, 0, sizeof(stDevAttr));
	memset(&stBindPipe, 0, sizeof(stBindPipe));
	// 0. get dev config status
	ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		// 0-1.config dev
		ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevAttr %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_SetDevAttr already\n");
	}
	// 1.get dev enable status
	ret = RK_MPI_VI_GetDevIsEnable(devId);
	if (ret != RK_SUCCESS) {
		// 1-2.enable dev
		ret = RK_MPI_VI_EnableDev(devId);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_EnableDev %x\n", ret);
			return -1;
		}
		// 1-3.bind dev/pipe
		stBindPipe.u32Num = 1;
		stBindPipe.PipeId[0] = pipeId;
		ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_EnableDev already\n");
	}

	return 0;
}

int vi_chn_init(int channelId, int width, int height) {
	int ret;
	int buf_cnt = 2;
	// VI init
	VI_CHN_ATTR_S vi_chn_attr;
	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
	vi_chn_attr.stIspOpt.enMemoryType =
	    VI_V4L2_MEMORY_TYPE_DMABUF; // VI_V4L2_MEMORY_TYPE_MMAP;
	vi_chn_attr.stSize.u32Width = width;
	vi_chn_attr.stSize.u32Height = height;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE; // COMPRESS_AFBC_16x16;
	vi_chn_attr.u32Depth = 0; //0, get fail, 1 - u32BufCount, can get, if bind to other device, must be < u32BufCount
	ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(0, channelId);
	if (ret) {
		printf("ERROR: create VI error! ret=%d\n", ret);
		return ret;
	}

	return ret;
}

static RK_CHAR optstr[] = "?::w:h:c:I:e:o:";
static void print_usage(const RK_CHAR *name) {
	printf("usage example:\n");
	printf("\t%s -I 0 -w 1920 -h 1080 -o /tmp/venc.h264\n", name);
	printf("\t-w | --width: VI width, Default:1920\n");
	printf("\t-h | --heght: VI height, Default:1080\n");
	printf("\t-c | --frame_cnt: frame number of output, Default:-1\n");
	printf("\t-I | --camid: camera ctx id, Default 0. "
	       "0:rkisp_mainpath,1:rkisp_selfpath,2:rkisp_bypasspath\n");
	printf("\t-e | --encode: encode type, Default:h264, Value:h264, h265, mjpeg\n");
	printf("\t-o: output path, Default:NULL\n");
}

int main(int argc, char *argv[]) {
	RK_S32 s32Ret = RK_FAILURE;
	RK_U32 u32Width = 1920;
	RK_U32 u32Height = 1080;
	RK_CHAR *pOutPath = NULL;
	RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
	RK_CHAR *pCodecName = "H264";
	RK_S32 s32chnlId = 0;
	int c;
	int ret = -1;

	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 'w':
			u32Width = atoi(optarg);
			break;
		case 'h':
			u32Height = atoi(optarg);
			break;
		case 'I':
			s32chnlId = atoi(optarg);
			break;
		case 'c':
			g_s32FrameCnt = atoi(optarg);
			break;
		case 'e':
			if (!strcmp(optarg, "h264")) {
				enCodecType = RK_VIDEO_ID_AVC;
				pCodecName = "H264";
			} else if (!strcmp(optarg, "h265")) {
				enCodecType = RK_VIDEO_ID_HEVC;
				pCodecName = "H265";
			} else if (!strcmp(optarg, "mjpeg")) {
				enCodecType = RK_VIDEO_ID_MJPEG;
				pCodecName = "MJPEG";
			} else {
				printf("ERROR: Invalid encoder type.\n");
				return -1;
			}
			break;
		case 'o':
			pOutPath = optarg;
			break;
		case '?':
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	printf("#CodecName:%s\n", pCodecName);
	printf("#Resolution: %dx%d\n", u32Width, u32Height);
	printf("#Output Path: %s\n", pOutPath);
	printf("#CameraIdx: %d\n\n", s32chnlId);
	printf("#Frame Count to save: %d\n", g_s32FrameCnt);

	if (pOutPath) {
		venc0_file = fopen(pOutPath, "w");
		if (!venc0_file) {
			printf("ERROR: open file: %s fail, exit\n", pOutPath);
			return 0;
		}
	}
	signal(SIGINT, sigterm_handler);

	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		RK_LOGE("rk mpi sys init fail!");
		goto __FAILED;
	}

	vi_dev_init();
	vi_chn_init(s32chnlId, u32Width, u32Height);

	// venc  init
	test_venc_init(0, u32Width, u32Height,
	               enCodecType); // RK_VIDEO_ID_AVC RK_VIDEO_ID_HEVC

	// Start IMU logging (before frames start)
	imu_start_logging(pOutPath);

	MPP_CHN_S stSrcChn, stDestChn;
	// bind vi to venc
	stSrcChn.enModId = RK_ID_VI;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = s32chnlId;

	stDestChn.enModId = RK_ID_VENC;
	stDestChn.s32DevId = 0;
	stDestChn.s32ChnId = 0;
	printf("====RK_MPI_SYS_Bind vi0 to venc0====\n");
	s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("bind 0 ch venc failed");
		goto __FAILED;
	}

	pthread_t main_thread;
	pthread_create(&main_thread, NULL, GetMediaBuffer0, NULL);

	while (!quit) {
		usleep(50000);
	}
	pthread_join(main_thread, NULL);

	// Stop IMU logging
	imu_stop_logging();

	s32Ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_SYS_UnBind fail %x", s32Ret);
	}

	s32Ret = RK_MPI_VI_DisableChn(0, s32chnlId);
	RK_LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

	s32Ret = RK_MPI_VENC_StopRecvFrame(0);
	if (s32Ret != RK_SUCCESS) {
		return s32Ret;
	}

	s32Ret = RK_MPI_VENC_DestroyChn(0);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VDEC_DestroyChn fail %x", s32Ret);
	}

	s32Ret = RK_MPI_VI_DisableDev(0);
	RK_LOGE("RK_MPI_VI_DisableDev %x", s32Ret);
	ret = 0;
__FAILED:
	RK_LOGE("test running exit:%d", s32Ret);
	RK_MPI_SYS_Exit();

	return ret;
}
