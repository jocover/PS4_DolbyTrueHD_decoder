#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdlib.h>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <orbis/libkernel.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <orbis/Audiodec.h>
#include <orbis/Sysmodule.h>

#include "circularBuffer.h"


#define ORBIS_AUDIO_DEC_CODEC_DolbyTrueHD	(0x0005U)
#define ORBIS_AUDIO_DEC_CODEC_MPEG2BC		(0x0006U)
#define ORBIS_AUDIO_DEC_CODEC_Ogg_Vorbis	(0x0007U)
#define ORBIS_AUDIO_DEC_CODEC_Opus_CELT		(0x0008U)
#define ORBIS_AUDIO_DEC_CODEC_Opus_SILK		(0x0009U)
//#define ORBIS_AUDIO_DEC_CODEC_FLAC		(0x0010U)


#define ORBIS_AUDIODEC_DolbyTrueHD_MAX_FRAME_SIZE	(4000)
#define ORBIS_AUDIODEC_DolbyTrueHD_MAX_FRAME_SAMPLES	(160)
#define ORBIS_AUDIODEC_DolbyTrueHD_MAX_CHANNELS_FOR_8CH	(8)
#define ORBIS_AUDIODEC_DolbyTrueHD_MAX_NFRAME		(40)

#define ORBIS_AUDIO_OUT_PARAM_FORMAT_FLOAT_8CH		(5)


size_t sceUserMainThreadStackSize = 512 * 1024;
size_t sceLibcHeapSize = 8 * 1024 * 1024;

static unsigned const samplingFrequencyTable[16] = {
	96000, 88200, 64000, 48000,
	44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000,
	7350, 0, 0, 0
};

typedef struct SceAudiodecParamDolbyTrueHD {
	uint32_t uiSize;
	int32_t  iBwPcm;//0 or 2  0 is s32 24bit  2 is float       0x1000000C1 
	uint32_t presentationMode;// value <= 2  0:2ch 1:6ch 2:8ch    SCE_AUDIODEC_ERROR_TRHD_INVALID_PRESENTATION_MODE
	uint32_t lossless;// value 0 or 1 SCE_AUDIODEC_ERROR_TRHD_INVALID_LOSSLESS
	uint32_t enDrcMode;// value <= 2 drc enable??   SCE_AUDIODEC_ERROR_TRHD_INVALID_DRC_ENABLE  
	uint32_t drcCut;// value <= 0x64 SCE_AUDIODEC_ERROR_TRHD_INVALID_DRC_CUT
	uint32_t drcBoost;//value <= 0x64 SCE_AUDIODEC_ERROR_TRHD_INVALID_DRC_BOOST
	uint32_t nFrame;//// value 1-40 SCE_AUDIODEC_ERROR_TRHD_INVALID_NFRAME_DECODE

} SceAudiodecParamDolbyTrueHD;



//R14[10] sample rate
typedef struct SceAudiodecDolbyTrueHDInfo {
	uint32_t uiSize;
	uint32_t unk[36];
	uint32_t  iResult;

} SceAudiodecDolbyTrueHDInfo;


typedef struct SceAudiodecAuInfo {
	uint32_t uiSize;
	void *pAuAddr;
	uint32_t uiAuSize;
} SceAudiodecAuInfo;

typedef struct SceAudiodecPcmItem {
	uint32_t uiSize;
	void *pPcmAddr;
	uint32_t uiPcmSize;
} SceAudiodecPcmItem;

typedef struct SceAudiodecCtrl {
	void *pParam;
	void *pBsiInfo;
	SceAudiodecAuInfo *pAuInfo;
	SceAudiodecPcmItem *pPcmItem;
} SceAudiodecCtrl;


typedef struct TrueHDHeader {
	uint32_t samples_per_frame;
	uint32_t samplingFrequency;
	uint32_t numChannels;
	uint32_t is_truehd;
} TrueHDHeader;

#ifndef AV_RB32
#   define AV_RB32(x)                                \
	(((uint32_t)((const uint8_t*)(x))[0] << 24) |    \
	 (((const uint8_t*)(x))[1] << 16) |    \
	 (((const uint8_t*)(x))[2] <<  8) |    \
	 ((const uint8_t*)(x))[3])
#endif

#ifndef AV_RB16
#   define AV_RB16(x)                           \
	((((const uint8_t*)(x))[0] << 8) |          \
	 ((const uint8_t*)(x))[1])
#endif

static const int sampling_rates[] = { 48000, 96000, 192000, 0, 0, 0, 0, 0, 44100, 88200, 176400, 0, 0, 0, 0, 0 };

int decode_channel_map(int channel_map) {
	static const int channel_count[13] = {
		//  LR    C   LFE  LRs LRvh  LRc LRrs  Cs   Ts  LRsd  LRw  Cvh  LFE2
		2,   1,   1,   2,   2,   2,   2,   1,   1,   2,   2,   1,   1
	};

	int channels = 0, i;

	for (i = 0; 13 > i; ++i)
		channels += channel_count[i] * ((channel_map >> i) & 1);

	return channels;
}


TrueHDHeader truehd_parse(const uint8_t* in_buf, int buf_size)
{
	TrueHDHeader head;
	memset(&head, 0, sizeof(TrueHDHeader));

	uint32_t sync = 0xf8726fba;
	const uint8_t* buf, * last_buf = in_buf, * end = in_buf + buf_size;
	int frames = 0, valid = 0, size = 0;
	int nsubframes = 0;

	for (buf = in_buf; buf + 8 <= end; buf++) {
		if (AV_RB32(buf + 4) == sync) {
			frames++;
			if (last_buf + size == buf) {
				valid += 1 + nsubframes / 8;
			}
			nsubframes = 0;
			last_buf = buf;
			size = (AV_RB16(buf) & 0xfff) * 2;

			int sampling_rate = sampling_rates[(buf + 8)[0] >> 4];

			int m_samples_per_frame = 40 << (((buf + 8)[0] >> 4) & 0x07);
			int chanmap_substream_1 = (((buf + 9)[0] & 0x0f) << 1) | ((buf + 10)[0] >> 7);
			int chanmap_substream_2 = (((buf + 10)[0] & 0x0f) << 8) | ((buf + 11)[0]);
			int channels = decode_channel_map(chanmap_substream_2 ? chanmap_substream_2 : chanmap_substream_1);

			head.is_truehd = 1;
			head.samplingFrequency = sampling_rate;
			head.numChannels = channels;
			head.samples_per_frame = m_samples_per_frame;


			break;
		}
		else if (buf - last_buf == size) {
			nsubframes++;
			size += (AV_RB16(buf) & 0xfff) * 2;
		}
	}


	if (valid >= 100)
		return head;
	return head;
}




#define USER_GRAIN 1024

//audiobuf size = channel * USER_GRAIN * sizeof(float)
#define AUDIO_BUF_LEN  (8 * USER_GRAIN * 4)

std::mutex buf_mutex;
std::condition_variable buf_cond;

CircularBuffer audio_cb;


void playthread(void){

	int rc;

	rc = sceAudioOutInit();
	if (rc != 0)
	{
		printf("[ERROR] Failed to initialize audio output\n");
		return;
	}


	OrbisUserServiceUserId userId = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
	int audio=  sceAudioOutOpen(userId, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, USER_GRAIN, 48000,ORBIS_AUDIO_OUT_PARAM_FORMAT_FLOAT_8CH);

	if(audio<0){
		printf("sceAudioOutOpen failed\n");
		return ;
	}

	uint8_t* audiobuf=(uint8_t* )malloc(AUDIO_BUF_LEN*sizeof(uint8_t));



	while(true){



		int size=CircularBufferGetDataSize(audio_cb); 

		if(size<AUDIO_BUF_LEN){
			std::unique_lock <std::mutex> lck(buf_mutex);
			buf_cond.wait(lck);	
		
		}

			CircularBufferPop(audio_cb, AUDIO_BUF_LEN, audiobuf);

			sceAudioOutOutput(audio, audiobuf);

	}

	sceAudioOutClose(audio);

	free(audiobuf);

}

void recvthread(void){

	socklen_t addrLen;

	struct sockaddr_in serverAddr;

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

	if (sockfd < 0)
	{
		printf("Failed to create socket:");
		return;
	}

	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(7721);

	if (bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) != 0)
	{
		printf("Failed to bind to 0.0.0.0\n");
		return;
	}



	int read_byte = 0;
	int rc;


	rc = sceAudiodecInitLibrary(ORBIS_AUDIO_DEC_CODEC_DolbyTrueHD);
        if(rc<0){
                printf("sceAudiodecInitLibrary DolbyTrueHD failed\n");
                return;
        }


	SceAudiodecParamDolbyTrueHD param_DolbyTrueHD;
	memset(&param_DolbyTrueHD,0,sizeof(SceAudiodecParamDolbyTrueHD));

	SceAudiodecDolbyTrueHDInfo info_DolbyTrueHD;
	memset(&info_DolbyTrueHD,0,sizeof(SceAudiodecDolbyTrueHDInfo));


	SceAudiodecCtrl ctrl;
	ctrl.pParam=(void*)&param_DolbyTrueHD;

	param_DolbyTrueHD.uiSize=sizeof(SceAudiodecParamDolbyTrueHD);
	param_DolbyTrueHD.iBwPcm=2;
	param_DolbyTrueHD.nFrame=1;
	param_DolbyTrueHD.lossless=1;
	param_DolbyTrueHD.presentationMode=2;


	ctrl.pBsiInfo=(void*)&info_DolbyTrueHD;
	info_DolbyTrueHD.uiSize=sizeof(SceAudiodecDolbyTrueHDInfo);

	SceAudiodecAuInfo au;
	memset(&au,0,sizeof(SceAudiodecAuInfo));

	SceAudiodecPcmItem pcm;
	memset(&pcm,0,sizeof(SceAudiodecPcmItem));

	ctrl.pAuInfo=&au;
	au.uiSize=sizeof(au);
	ctrl.pPcmItem = &pcm;
	pcm.uiSize = sizeof(pcm);

	rc = sceAudiodecCreateDecoder((OrbisAudiodecCtrl *)&ctrl, ORBIS_AUDIO_DEC_CODEC_DolbyTrueHD);


	if(rc<0){
		printf("sceAudiodecCreateDecoder failed. ret:%08x\n",rc);
		return ;
	}

	int32_t handle=rc;
	const uint32_t maxAuSize = ORBIS_AUDIODEC_DolbyTrueHD_MAX_FRAME_SIZE;
	const uint32_t maxPcmSize =sizeof(float)* ORBIS_AUDIODEC_DolbyTrueHD_MAX_NFRAME * ORBIS_AUDIODEC_DolbyTrueHD_MAX_CHANNELS_FOR_8CH * ORBIS_AUDIODEC_DolbyTrueHD_MAX_FRAME_SAMPLES;

	uint8_t* pcm_buf=(uint8_t*)malloc(maxPcmSize*sizeof(uint8_t));
	uint8_t* recv_buf=(uint8_t*)malloc(maxAuSize*sizeof(uint8_t));

	while(1){

		memset(recv_buf,0,ORBIS_AUDIODEC_DolbyTrueHD_MAX_FRAME_SIZE);
		memset(pcm_buf,0,maxPcmSize);

		read_byte = recv(sockfd, recv_buf,maxAuSize, 0);

		if(read_byte<=0){
			continue;
		}

		au.pAuAddr = recv_buf;
		au.uiAuSize = read_byte;
		pcm.pPcmAddr = pcm_buf;
		pcm.uiPcmSize = maxPcmSize;

		rc = sceAudiodecDecode(handle, (OrbisAudiodecCtrl *)&ctrl);
		if(rc<0){
			printf("sceAudiodecDecode failed. ret:%08x\n",rc);

			break;
		}


		CircularBufferPush(audio_cb,pcm_buf,pcm.uiPcmSize);

		int size=CircularBufferGetDataSize(audio_cb);
		 if(size>=AUDIO_BUF_LEN){
			buf_cond.notify_one();
                }



	}

	sceAudiodecDeleteDecoder(handle);
	sceAudiodecTermLibrary(ORBIS_AUDIO_DEC_CODEC_DolbyTrueHD);
	free(pcm_buf);
	free(recv_buf);


}

//ffmpeg -re -i filename -map 0:a:0 -c:a copy -bsf:a truehd_core -f truehd udp://xxx.xxx.xxx.xxx:7721

int main(void)
{


	sceUserServiceInitialize(NULL);

	int rc;
	rc = sceSysmoduleLoadModule(0x0088);

	if (rc < 0)
	{
		printf("[ERROR] Failed to load audiodec module\n");
		return rc;
	}

	printf("sceSysmoduleLoadModule ok\n");


	audio_cb = CircularBufferCreate(AUDIO_BUF_LEN*3);
	//recv_cb= CircularBufferCreate(ORBIS_AUDIODEC_DolbyTrueHD_MAX_FRAME_SIZE*3);

	std::thread play_thread(playthread);
	std::thread recv_thread(recvthread);


	while(true){

		sleep(1);
		

	}



}