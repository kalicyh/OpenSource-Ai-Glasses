#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

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
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"

static FILE *venc0_file = NULL;
static RK_S32 g_s32FrameCnt = -1;
static bool quit = false;
static pid_t ffmpeg_pid = -1;
static char *ffmpeg_path = "./usr/bin/ffmpeg";
static RK_U32 target_fps = 30;
static int low_latency_mode = 1;

static void sigterm_handler(int sig) {
    fprintf(stderr, "Signal %d received, exiting...\n", sig);
    quit = true;
}

RK_U64 TEST_COMM_GetNowUs() {
    struct timespec time = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000;
}

static int ffmpeg_running() {
    if (ffmpeg_pid <= 0) return 0;
    
    int status;
    pid_t result = waitpid(ffmpeg_pid, &status, WNOHANG);
    if (result == 0) return 1;
    if (result < 0) {
        if (errno == ECHILD) return 0;
        perror("waitpid error");
    }
    return 0;
}

static void *GetMediaBuffer0(void *arg) {
    (void)arg;
    RK_LOGI("Start VENC stream receiver thread");
    
    int loopCount = 0;
    int s32Ret;

    VENC_STREAM_S stFrame;
    stFrame.pstPack = malloc(sizeof(VENC_PACK_S));
    if (!stFrame.pstPack) {
        RK_LOGE("Failed to allocate memory for VENC_PACK_S");
        return NULL;
    }

    while (!quit) {
        s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, 10); // 10ms超时
        if (s32Ret != RK_SUCCESS) {
            if (s32Ret == RK_ERR_VENC_BUF_EMPTY) {
                continue; // 非阻塞，继续循环
            } else if (s32Ret == RK_ERR_VENC_BUF_FULL) {
                RK_LOGW("VENC buffer full, retrying...");
                usleep(1000);
                continue;
            }
            RK_LOGE("RK_MPI_VENC_GetStream failed: 0x%X", s32Ret);
            break;
        }

        if (venc0_file) {
            if (!ffmpeg_running()) {
                RK_LOGE("FFmpeg process is not running!");
                quit = true;
                s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
                if (s32Ret != RK_SUCCESS) {
                    RK_LOGE("RK_MPI_VENC_ReleaseStream fail: 0x%X", s32Ret);
                }
                break;
            }
            
            void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
            if (!pData) {
                RK_LOGE("RK_MPI_MB_Handle2VirAddr returned NULL");
                s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
                if (s32Ret != RK_SUCCESS) {
                    RK_LOGE("RK_MPI_VENC_ReleaseStream fail: 0x%X", s32Ret);
                }
                continue;
            }
            
            size_t written = fwrite(pData, 1, stFrame.pstPack->u32Len, venc0_file);
            if (written != stFrame.pstPack->u32Len) {
                RK_LOGE("fwrite error: %s", strerror(ferror(venc0_file)));
                
                if (!ffmpeg_running()) {
                    RK_LOGE("FFmpeg process exited unexpectedly");
                    quit = true;
                    s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
                    if (s32Ret != RK_SUCCESS) {
                        RK_LOGE("RK_MPI_VENC_ReleaseStream fail: 0x%X", s32Ret);
                    }
                    break;
                }
            } else {
                fflush(venc0_file); // 强制刷新
            }
        }

        s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VENC_ReleaseStream fail: 0x%X", s32Ret);
        }
        
        loopCount++;
        if ((g_s32FrameCnt >= 0) && (loopCount >= g_s32FrameCnt)) {
            quit = true;
            break;
        }
    }

    if (ffmpeg_pid > 0) {
        RK_LOGI("Terminating FFmpeg process (PID: %d)", ffmpeg_pid);
        kill(ffmpeg_pid, SIGKILL); // 使用SIGKILL强制终止
        
        int status;
        waitpid(ffmpeg_pid, &status, 0);
        ffmpeg_pid = -1;
    }

    if (venc0_file) {
        fclose(venc0_file);
        venc0_file = NULL;
    }

    free(stFrame.pstPack);
    RK_LOGI("VENC stream receiver thread exited");
    return NULL;
}

static int start_ffmpeg(const char *rtsp_url) {
    int pipefd[2];
    if (pipe(pipefd)) {
        perror("pipe creation failed");
        return -1;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    
    if (pid == 0) { // 子进程
        close(pipefd[1]);
        
        if (dup2(pipefd[0], STDIN_FILENO) < 0) {
            perror("dup2 failed");
            exit(EXIT_FAILURE);
        }
        close(pipefd[0]);
        
        char *args[] = {
                    "ffmpeg",
                    "-loglevel", "warning",
                    "-use_wallclock_as_timestamps", "1",
                    "-fflags", "+nobuffer+fastseek", // 最小化缓冲
                    "-flags", "low_delay",      // 低延迟标志
                    "-strict", "experimental",
                    "-avioflags", "direct",     // 直接模式
                    "-f", "h264",
                    "-i", "pipe:0",
                    "-c", "copy",               // 直接复制流，不重新编码
                    "-f", "flv",
                    // "-rtsp_transport", "tcp",
                    "-muxdelay", "0",           // 最小化复用延迟
                    "-muxpreload", "0",
                    "-flush_packets", "1", // 立即刷新包
                    "-avoid_negative_ts", "make_zero",



                    (char *)rtsp_url,
                    NULL
                };
        
        execv(ffmpeg_path, args);
        fprintf(stderr, "execv failed for %s: %s\n", ffmpeg_path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    // 父进程
    close(pipefd[0]);
    ffmpeg_pid = pid;
    
    venc0_file = fdopen(pipefd[1], "w");
    if (!venc0_file) {
        perror("fdopen failed");
        close(pipefd[1]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    
    // 设置无缓冲模式
    setvbuf(venc0_file, NULL, _IONBF, 0);
    
    RK_LOGI("FFmpeg started with PID %d", pid);
    return 0;
}

static RK_S32 test_venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType) {
    RK_LOGI("Initialize VENC channel %d (low latency)", chnId);
    
    VENC_RECV_PIC_PARAM_S stRecvParam;
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

    RK_U32 bitrate = (width * height * target_fps) / (1920 * 1080 / 4000);
    if (bitrate < 512) bitrate = 512;
    if (bitrate > 8192) bitrate = 8192;

    RK_LOGI("Encoder settings: %dx%d@%dfps, bitrate: %d Kbps", 
            width, height, target_fps, bitrate);

    if (enType == RK_VIDEO_ID_AVC) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        stAttr.stRcAttr.stH264Cbr.u32BitRate = bitrate;
        stAttr.stRcAttr.stH264Cbr.u32Gop = target_fps; // 1秒GOP
    } else if (enType == RK_VIDEO_ID_HEVC) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        stAttr.stRcAttr.stH265Cbr.u32BitRate = bitrate;
        stAttr.stRcAttr.stH265Cbr.u32Gop = target_fps;
    } else if (enType == RK_VIDEO_ID_MJPEG) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
        stAttr.stRcAttr.stMjpegCbr.u32BitRate = bitrate;
    }

    stAttr.stVencAttr.enType = enType;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    if (enType == RK_VIDEO_ID_AVC)
        stAttr.stVencAttr.u32Profile = H264E_PROFILE_MAIN; // 使用Main Profile降低复杂度
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 1; // 最小化缓冲区数量以降低延迟
    stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
    stAttr.stVencAttr.enMirror = MIRROR_NONE;

    RK_S32 ret = RK_MPI_VENC_CreateChn(chnId, &stAttr);
    if (ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VENC_CreateChn failed: 0x%X", ret);
        return ret;
    }

    memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
    stRecvParam.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);
    if (ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VENC_StartRecvFrame failed: 0x%X", ret);
        return ret;
    }

    return RK_SUCCESS;
}

int vi_dev_init() {
    RK_LOGI("Initialize VI device (low latency)");
    
    int ret = 0;
    int devId = 0;
    int pipeId = devId;

    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    memset(&stBindPipe, 0, sizeof(stBindPipe));
    
    ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
        if (ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevAttr failed: 0x%X", ret);
            return -1;
        }
    } else if (ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_GetDevAttr failed: 0x%X", ret);
        return -1;
    }
    
    ret = RK_MPI_VI_GetDevIsEnable(devId);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(devId);
        if (ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_EnableDev failed: 0x%X", ret);
            return -1;
        }
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevBindPipe failed: 0x%X", ret);
            return -1;
        }
    }

    return 0;
}

int vi_chn_init(int channelId, int width, int height) {
    RK_LOGI("Initialize VI channel %d (low latency)", channelId);
    
    int ret;
    int buf_cnt = 2; // 减少缓冲区数量
    
    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
    vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi_chn_attr.stSize.u32Width = width;
    vi_chn_attr.stSize.u32Height = height;
    vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    vi_chn_attr.u32Depth = 1; // 最小化深度以降低延迟
    
    ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
    if (ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_SetChnAttr failed: 0x%X", ret);
        return ret;
    }
    
    ret = RK_MPI_VI_EnableChn(0, channelId);
    if (ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_EnableChn failed: 0x%X", ret);
        return ret;
    }

    return RK_SUCCESS;
}

static RK_CHAR optstr[] = "?::w:h:c:I:e:o:u:f:F:l";
static void print_usage(const RK_CHAR *name) {
    printf("usage example:\n");
    printf("\t%s -I 0 -w 1920 -h 1080 -u rtsp://192.168.2.56:7/live/streamName -f ./ffmpeg\n", name);
    printf("\t-w | --width: VI width, Default:1920\n");
    printf("\t-h | --height: VI height, Default:1080\n");
    printf("\t-c | --frame_cnt: frame number of output, Default:-1\n");
    printf("\t-I | --camid: camera ctx id, Default 0\n");
    printf("\t-e | --encode: encode type, Default:h264, Value:h264, h265, mjpeg\n");
    printf("\t-o: output file path, Default:NULL\n");
    printf("\t-u: RTSP push URL, Default:NULL\n");
    printf("\t-f: FFmpeg executable path, Default:./ffmpeg\n");
    printf("\t-F: Target frame rate, Default:30\n");
    printf("\t-l: Enable ultra low latency mode\n");
}

int main(int argc, char *argv[]) {
    RK_S32 s32Ret = RK_FAILURE;
    RK_U32 u32Width = 1920;
    RK_U32 u32Height = 1080;
    RK_CHAR *pOutPath = NULL;
    RK_CHAR *rtsp_url = NULL;
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
        case 'u':
            rtsp_url = optarg;
            break;
        case 'f':
            ffmpeg_path = optarg;
            break;
        case 'F':
            target_fps = atoi(optarg);
            if (target_fps < 1 || target_fps > 60) {
                fprintf(stderr, "Invalid frame rate: %d, using default 30\n", target_fps);
                target_fps = 30;
            }
            break;
        case 'l':
            low_latency_mode = 1; // 启用超低延迟模式
            break;
        case '?':
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    printf("\n===== Configuration Summary =====\n");
    printf("# Codec: %s\n", pCodecName);
    printf("# Resolution: %dx%d\n", u32Width, u32Height);
    printf("# Frame Rate: %d fps\n", target_fps);
    printf("# Output Path: %s\n", pOutPath ? pOutPath : "None");
    printf("# RTSP URL: %s\n", rtsp_url ? rtsp_url : "None");
    printf("# FFmpeg Path: %s\n", ffmpeg_path);
    printf("# Camera Index: %d\n", s32chnlId);
    printf("# Ultra Low Latency Mode: %s\n", low_latency_mode ? "ENABLED" : "DISABLED");
    printf("# Frame Count: %d\n\n", g_s32FrameCnt);

    if (pOutPath && rtsp_url) {
        printf("ERROR: -o and -u cannot be used at the same time\n");
        return -1;
    }

    if (rtsp_url && enCodecType != RK_VIDEO_ID_AVC) {
        printf("WARNING: RTSP streaming requires H.264, auto switching to H.264\n");
        enCodecType = RK_VIDEO_ID_AVC;
        pCodecName = "H264";
    }

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    if (rtsp_url) {
        if (access(ffmpeg_path, X_OK) != 0) {
            fprintf(stderr, "FFmpeg not found or not executable at: %s\n", ffmpeg_path);
            return -1;
        }
        
        if (start_ffmpeg(rtsp_url)) {
            fprintf(stderr, "Failed to start FFmpeg\n");
            return -1;
        }
    } else if (pOutPath) {
        venc0_file = fopen(pOutPath, "wb");
        if (!venc0_file) {
            perror("Failed to open output file");
            return -1;
        }
    }

    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("rk mpi sys init fail!");
        goto __FAILED;
    }

    ret = vi_dev_init();
    if (ret != 0) {
        RK_LOGE("vi_dev_init failed: %d", ret);
        goto __FAILED;
    }

    ret = vi_chn_init(s32chnlId, u32Width, u32Height);
    if (ret != RK_SUCCESS) {
        RK_LOGE("vi_chn_init failed: 0x%X", ret);
        goto __FAILED;
    }

    if (test_venc_init(0, u32Width, u32Height, enCodecType) != RK_SUCCESS) {
        RK_LOGE("test_venc_init failed");
        goto __FAILED;
    }

    MPP_CHN_S stSrcChn, stDestChn;
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = 0;
    stSrcChn.s32ChnId = s32chnlId;

    stDestChn.enModId = RK_ID_VENC;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = 0;
    
    RK_LOGI("Binding VI channel %d to VENC channel 0", s32chnlId);
    s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("bind vi to venc failed: 0x%X", s32Ret);
        goto __FAILED;
    }

    pthread_t main_thread;
    if (pthread_create(&main_thread, NULL, GetMediaBuffer0, NULL) != 0) {
        RK_LOGE("Failed to create stream receiver thread");
        goto __FAILED;
    }

    RK_LOGI("Ultra low latency streaming started, press Ctrl+C to stop...");
    
    while (!quit) {
        usleep(1000); // 1ms检查频率提高
    }
    
    RK_LOGI("Stopping stream...");
    pthread_join(main_thread, NULL);

    RK_LOGI("Unbinding VI and VENC...");
    RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);

    RK_LOGI("Disabling VI channel...");
    RK_MPI_VI_DisableChn(0, s32chnlId);

    RK_LOGI("Stopping VENC receive frame...");
    RK_MPI_VENC_StopRecvFrame(0);

    RK_LOGI("Destroying VENC channel...");
    RK_MPI_VENC_DestroyChn(0);

    RK_LOGI("Disabling VI device...");
    RK_MPI_VI_DisableDev(0);

    ret = 0;
    RK_LOGI("Cleanup complete, exiting...");

__FAILED:
    if (ret != 0) {
        RK_LOGE("Application exited with errors");
    }
    RK_MPI_SYS_Exit();

    return ret; 
}
