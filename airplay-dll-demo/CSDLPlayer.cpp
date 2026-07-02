#include "CSDLPlayer.h"
#include <stdio.h>
#include "CAutoLock.h"

/* This function may run in a separate event thread */
int FilterEvents(const SDL_Event* event) {
// 	static int boycott = 1;
// 
// 	/* This quit event signals the closing of the window */
// 	if ((event->type == SDL_QUIT) && boycott) {
// 		printf("Quit event filtered out -- try again.\n");
// 		boycott = 0;
// 		return(0);
// 	}
// 	if (event->type == SDL_MOUSEMOTION) {
// 		printf("Mouse moved to (%d,%d)\n",
// 			event->motion.x, event->motion.y);
// 		return(0);    /* Drop it, we've handled it */
// 	}
	return(1);
}

CSDLPlayer::CSDLPlayer()
	: m_surface(NULL)
	, m_yuv(NULL)
	, m_bAudioInited(false)
	, m_bDumpAudio(false)
	, m_fileWav(NULL)
	, m_sAudioFmt()
	, m_rect()
	, m_server()
	, m_fRatio(1.0f)
	, m_nOriginalWidth(0)
	, m_nOriginalHeight(0)
	, m_pSwsCtx(NULL)
	, m_pScaledBuf(NULL)
	, m_nScaledWidth(0)
	, m_nScaledHeight(0)
{
	ZeroMemory(&m_sAudioFmt, sizeof(SFgAudioFrame));
	ZeroMemory(&m_rect, sizeof(SDL_Rect));
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
}

CSDLPlayer::~CSDLPlayer()
{
	unInit();

	CloseHandle(m_mutexAudio);
	CloseHandle(m_mutexVideo);
}

bool CSDLPlayer::init()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return false;
	}

	/* Clean up on exit, exit on window close and interrupt */
	atexit(SDL_Quit);

	SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_IGNORE);

	initVideo(600, 400);

	/* Filter quit and mouse motion events */
	SDL_SetEventFilter(FilterEvents);

	m_server.start(this);

	return true;
}

void CSDLPlayer::unInit()
{
	m_nOriginalWidth = 0;
	m_nOriginalHeight = 0;
	m_fRatio = 1.0f;

	if (m_pSwsCtx) {
		sws_freeContext(m_pSwsCtx);
		m_pSwsCtx = NULL;
	}
	if (m_pScaledBuf) {
		delete[] m_pScaledBuf;
		m_pScaledBuf = NULL;
	}
	m_nScaledWidth = 0;
	m_nScaledHeight = 0;

	unInitVideo();
	unInitAudio();

	SDL_Quit();
}

void CSDLPlayer::loopEvents()
{
	SDL_Event event;

	BOOL bEndLoop = FALSE;
	/* Loop waiting for ESC+Mouse_Button */
	while (SDL_WaitEvent(&event) >= 0) {
		switch (event.type) {
		case SDL_USEREVENT: {
			if (event.user.code == VIDEO_SIZE_CHANGED_CODE) {
				unsigned int width = (unsigned int)event.user.data1;
				unsigned int height = (unsigned int)event.user.data2;
				printf("AirPlay Video Stream Resolution: %dx%d\n", width, height);
				if (m_surface == NULL || m_yuv == NULL || width != m_yuv->w || height != m_yuv->h) {
					unInitVideo();
					initVideo(width, height);
				}
			}
			break;
		}
		case SDL_VIDEORESIZE: {
			m_surface = SDL_SetVideoMode(event.resize.w, event.resize.h, 0, SDL_SWSURFACE | SDL_RESIZABLE);
			SDL_FillRect(m_surface, NULL, 0);
			break;
		}
		case SDL_VIDEOEXPOSE: {
			break;
		}
		case SDL_ACTIVEEVENT: {
			if (event.active.state & SDL_APPACTIVE) {
				if (event.active.gain) {
					//printf("App activated\n");
				}
				else {
					//printf("App iconified\n");
				}
			}
			break;
		}
		case SDL_KEYUP: {
			switch (event.key.keysym.sym)
			{
				case SDLK_q: {
					printf("key down");
					m_server.stop();
					SDL_WM_SetCaption("AirPlay Demo - Stopped [s - start server, q - stop server]", NULL);
					break;
				}
				case SDLK_s: {
					printf("key down");
					m_server.start(this);
					SDL_WM_SetCaption("AirPlay Demo - Started [s - start server, q - stop server]", NULL);
					break;
				}
				case SDLK_EQUALS: {
					m_fRatio *= 2;
					m_fRatio = m_server.setVideoScale(m_fRatio);
					break;
				}
				case SDLK_MINUS: {
					m_fRatio /= 2;
					m_fRatio = m_server.setVideoScale(m_fRatio);
					break;
				}
			}
				break;
		}

		case SDL_QUIT: {
			printf("Quit requested, quitting.\n");
			m_server.stop();
			bEndLoop = TRUE;
			break;
		}
		}
		if (bEndLoop)
		{
			break;
		}
	}
}

void CSDLPlayer::outputVideo(SFgVideoFrame* data) 
{
	if (data->width == 0 || data->height == 0) {
		return;
	}

	if (m_surface == NULL) {
		return;
	}

	// Compute the target scaled dimensions to fit the current window
	float aspect = (float)data->width / data->height;
	int target_w = m_surface->w;
	int target_h = (int)(target_w / aspect);
	if (target_h > m_surface->h) {
		target_h = m_surface->h;
		target_w = (int)(target_h * aspect);
	}
	// Ensure even dimensions for YUV420
	target_w &= ~1;
	target_h &= ~1;
	if (target_w <= 0 || target_h <= 0) return;

	// Recreate SwsContext if source or target dimensions changed
	if (m_nScaledWidth != target_w || m_nScaledHeight != target_h) {
		if (m_pSwsCtx) {
			sws_freeContext(m_pSwsCtx);
			m_pSwsCtx = NULL;
		}
		if (m_pScaledBuf) {
			delete[] m_pScaledBuf;
			m_pScaledBuf = NULL;
		}
		m_nScaledWidth = target_w;
		m_nScaledHeight = target_h;
	}

	if (!m_pSwsCtx) {
		m_pSwsCtx = sws_getContext(
			data->width, data->height, AV_PIX_FMT_YUV420P,
			target_w, target_h, AV_PIX_FMT_YUV420P,
			SWS_BICUBIC, NULL, NULL, NULL);
		if (!m_pSwsCtx) return;
	}

	// Allocate scaled buffer if needed
	int scaledYPitch = ((target_w + 15) >> 4) << 4;
	int scaledUVPitch = (((target_w >> 1) + 15) >> 4) << 4;
	int scaledYSize = scaledYPitch * target_h;
	int scaledUVSize = scaledUVPitch * (target_h >> 1);
	int totalScaledSize = scaledYSize + scaledUVSize * 2;
	if (!m_pScaledBuf) {
		m_pScaledBuf = new uint8_t[totalScaledSize];
	}

	// Set up source pointers and strides from the incoming frame
	const uint8_t* srcSlice[3] = {
		data->data,
		data->data + data->dataLen[0],
		data->data + data->dataLen[0] + data->dataLen[1]
	};
	int srcStride[3] = { (int)data->pitch[0], (int)data->pitch[1], (int)data->pitch[2] };

	// Set up destination pointers and strides for the scaled buffer
	uint8_t* dstSlice[3] = {
		m_pScaledBuf,
		m_pScaledBuf + scaledYSize,
		m_pScaledBuf + scaledYSize + scaledUVSize
	};
	int dstStride[3] = { scaledYPitch, scaledUVPitch, scaledUVPitch };

	// Bicubic scale!
	sws_scale(m_pSwsCtx, srcSlice, srcStride, 0, data->height, dstSlice, dstStride);

	// Now check if the YUV overlay matches the scaled dimensions
	if (m_yuv == NULL || target_w != m_yuv->w || target_h != m_yuv->h) {
		{
			CAutoLock oLock(m_mutexVideo, "unInitVideo");
			if (NULL != m_yuv) {
				SDL_FreeYUVOverlay(m_yuv);
				m_yuv = NULL;
			}
		}
		m_evtVideoSizeChange.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.code = VIDEO_SIZE_CHANGED_CODE;
		m_evtVideoSizeChange.user.data1 = (void*)(uintptr_t)target_w;
		m_evtVideoSizeChange.user.data2 = (void*)(uintptr_t)target_h;

		SDL_PushEvent(&m_evtVideoSizeChange);
		return;
	}

	CAutoLock oLock(m_mutexVideo, "outputVideo");
	if (m_yuv == NULL) {
		return;
	}

	SDL_LockYUVOverlay(m_yuv);

	// Copy scaled YUV data into the overlay
	for (int i = 0; i < target_h; i++) {
		memcpy(m_yuv->pixels[0] + i * m_yuv->pitches[0],
			dstSlice[0] + i * scaledYPitch, min((int)m_yuv->pitches[0], scaledYPitch));
		if (i % 2 == 0) {
			memcpy(m_yuv->pixels[1] + (i >> 1) * m_yuv->pitches[1],
				dstSlice[1] + (i >> 1) * scaledUVPitch, min((int)m_yuv->pitches[1], scaledUVPitch));
			memcpy(m_yuv->pixels[2] + (i >> 1) * m_yuv->pitches[2],
				dstSlice[2] + (i >> 1) * scaledUVPitch, min((int)m_yuv->pitches[2], scaledUVPitch));
		}
	}

	SDL_UnlockYUVOverlay(m_yuv);

	// Display 1:1 centered in the window
	m_rect.x = (m_surface->w - target_w) / 2;
	m_rect.y = (m_surface->h - target_h) / 2;
	m_rect.w = target_w;
	m_rect.h = target_h;

	SDL_FillRect(m_surface, NULL, 0);
	SDL_DisplayYUVOverlay(m_yuv, &m_rect);
}

void CSDLPlayer::outputAudio(SFgAudioFrame* data)
{
	if (data->channels == 0) {
		return;
	}

	initAudio(data);

	if (m_bDumpAudio) {
		if (m_fileWav != NULL) {
			fwrite(data->data, data->dataLen, 1, m_fileWav);
		}
	}

	SDemoAudioFrame* dataClone = new SDemoAudioFrame();
	dataClone->dataTotal = data->dataLen;
	dataClone->pts = data->pts;
	dataClone->dataLeft = dataClone->dataTotal;
	dataClone->data = new uint8_t[dataClone->dataTotal];
	memcpy(dataClone->data, data->data, dataClone->dataTotal);

	{
		CAutoLock oLock(m_mutexAudio, "outputAudio");
		m_queueAudio.push(dataClone);
	}
}

void CSDLPlayer::initVideo(int width, int height)
{
	int win_width = width;
	int win_height = height;
	m_surface = SDL_SetVideoMode(win_width, win_height, 0, SDL_SWSURFACE | SDL_RESIZABLE);
	SDL_WM_SetCaption("AirPlay Demo [s - start server, q - stop server]", NULL);

	{
		CAutoLock oLock(m_mutexVideo, "initVideo");
		// Create the YUV overlay at the WINDOW size (scaled size)
		// so SDL displays it 1:1 with no additional scaling
		m_yuv = SDL_CreateYUVOverlay(win_width, win_height, SDL_IYUV_OVERLAY, m_surface);

		memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
		memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * m_yuv->h >> 1);
		memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * m_yuv->h >> 1);

		m_rect.x = 0;
		m_rect.y = 0;
		m_rect.w = win_width;
		m_rect.h = win_height;

		SDL_DisplayYUVOverlay(m_yuv, &m_rect);
	}
}

void CSDLPlayer::unInitVideo()
{
	if (NULL != m_surface) {
		SDL_FreeSurface(m_surface);
		m_surface = NULL;
	}

	{
		CAutoLock oLock(m_mutexVideo, "unInitVideo");
		if (NULL != m_yuv) {
			SDL_FreeYUVOverlay(m_yuv);
			m_yuv = NULL;
		}
		m_rect.w = 0;
		m_rect.h = 0;
	}

	unInitAudio();
}

void CSDLPlayer::initAudio(SFgAudioFrame* data)
{
	if ((data->sampleRate != m_sAudioFmt.sampleRate || data->channels != m_sAudioFmt.channels)) {
		unInitAudio();
	}
	if (!m_bAudioInited) {
		SDL_AudioSpec wanted_spec, obtained_spec;
		wanted_spec.freq = data->sampleRate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = data->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = 1920;
		wanted_spec.callback = sdlAudioCallback;
		wanted_spec.userdata = this;

		if (SDL_OpenAudio(&wanted_spec, &obtained_spec) < 0)
		{
			printf("can't open audio.\n");
			return;
		}

		SDL_PauseAudio(1);

		m_sAudioFmt.bitsPerSample = data->bitsPerSample;
		m_sAudioFmt.channels = data->channels;
		m_sAudioFmt.sampleRate = data->sampleRate;
		m_bAudioInited = true;

		if (m_bDumpAudio) {
			m_fileWav = fopen("demo-audio.wav", "wb");
		}
	}
	if (m_queueAudio.size() > 5) {
		SDL_PauseAudio(0);
	}
}

void CSDLPlayer::unInitAudio()
{
	SDL_CloseAudio();
	m_bAudioInited = false;
	memset(&m_sAudioFmt, 0, sizeof(m_sAudioFmt));

	{
		CAutoLock oLock(m_mutexAudio, "unInitAudio");
		while (!m_queueAudio.empty())
		{
			SDemoAudioFrame* pAudioFrame = m_queueAudio.front();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
			m_queueAudio.pop();
		}
	}

	if (m_fileWav != NULL) {
		fclose(m_fileWav);
		m_fileWav = NULL;
	}
}

void CSDLPlayer::sdlAudioCallback(void* userdata, Uint8* stream, int len)
{
	CSDLPlayer* pThis = (CSDLPlayer*)userdata;
	int needLen = len;
	int streamPos = 0;
	memset(stream, 0, len);

	CAutoLock oLock(pThis->m_mutexAudio, "sdlAudioCallback");
	while (!pThis->m_queueAudio.empty())
	{
		SDemoAudioFrame* pAudioFrame = pThis->m_queueAudio.front();
		int pos = pAudioFrame->dataTotal - pAudioFrame->dataLeft;
		int readLen = min(pAudioFrame->dataLeft, needLen);

		//SDL_MixAudio(stream + streamPos, pAudioFrame->data + pos, readLen, 100);
		memcpy(stream + streamPos, pAudioFrame->data + pos, readLen);

		pAudioFrame->dataLeft -= readLen;
		needLen -= readLen;
		streamPos += readLen;
		if (pAudioFrame->dataLeft <= 0) {
			pThis->m_queueAudio.pop();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
		}
		if (needLen <= 0) {
			break;
		}
	}
}
