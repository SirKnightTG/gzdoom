/*
** i_music.cpp
** Plays music
**
**---------------------------------------------------------------------------
** Copyright 1998-2001 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** I appologize for the mess that this file has become. It works, but it's
** not pretty. The SPC, Timidity, and OPL playback routines are also too
** FMOD-specific. I should have made some generic streaming class for them
** to utilize so that they can work with other sound systems, too.
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
#include <SDL/SDL.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <wordexp.h>
#include <stdio.h>
#include "mus2midi.h"
#define FALSE 0
#define TRUE 1
#endif

#include <ctype.h>
#include <assert.h>
#include <stdio.h>

#include "doomtype.h"
#include "m_argv.h"
#include "i_music.h"
#include "i_musicinterns.h"
#include "w_wad.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_swap.h"
#include "i_cd.h"
#include "tempfiles.h"
#include "templates.h"

#include <fmod.h>

EXTERN_CVAR (Int, snd_samplerate)
EXTERN_CVAR (Int, snd_mididevice)

void Enable_FSOUND_IO_Loader ();
void Disable_FSOUND_IO_Loader ();

static bool MusicDown = true;

extern bool nofmod;

MusInfo *currSong;
int		nomusic = 0;

//==========================================================================
//
// CVAR snd_musicvolume
//
// Maximum volume of MOD/stream music.
//==========================================================================

CUSTOM_CVAR (Float, snd_musicvolume, 0.3f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (self < 0.f)
		self = 0.f;
	else if (self > 1.f)
		self = 1.f;
	else if (currSong != NULL && !currSong->IsMIDI ())
		currSong->SetVolume (self);
}

MusInfo::~MusInfo ()
{
}

bool MusInfo::SetPosition (int order)
{
	return false;
}

void I_InitMusic (void)
{
	static bool setatterm = false;

	snd_musicvolume.Callback ();

	nomusic = !!Args.CheckParm("-nomusic") || !!Args.CheckParm("-nosound");

#ifdef _WIN32
	I_InitMusicWin32 ();
#endif // _WIN32
	
	if (!setatterm)
	{
		setatterm = true;
		atterm (I_ShutdownMusic);
	
#ifndef _WIN32
		signal (SIGCHLD, ChildSigHandler);
#endif
	}
	MusicDown = false;
}


void STACK_ARGS I_ShutdownMusic(void)
{
	if (MusicDown)
		return;
	MusicDown = true;
	if (currSong)
	{
		S_StopMusic (true);
		assert (currSong == NULL);
	}
#ifdef _WIN32
	I_ShutdownMusicWin32 ();
#endif // _WIN32
}


void I_PlaySong (void *handle, int _looping)
{
	MusInfo *info = (MusInfo *)handle;

	if (!info || nomusic)
		return;

	info->Stop ();
	info->Play (_looping ? true : false);
	
	if (info->m_Status == MusInfo::STATE_Playing)
		currSong = info;
	else
		currSong = NULL;
}


void I_PauseSong (void *handle)
{
	MusInfo *info = (MusInfo *)handle;

	if (info)
		info->Pause ();
}

void I_ResumeSong (void *handle)
{
	MusInfo *info = (MusInfo *)handle;

	if (info)
		info->Resume ();
}

void I_StopSong (void *handle)
{
	MusInfo *info = (MusInfo *)handle;
	
	if (info)
		info->Stop ();

	if (info == currSong)
		currSong = NULL;
}

void I_UnRegisterSong (void *handle)
{
	MusInfo *info = (MusInfo *)handle;

	if (info)
	{
		delete info;
	}
}

void *I_RegisterSong (FileReader *mem)
{
	// When is the FileReader deleted? If the song loads all its data
	// immediately, this function will delete it before returning.
	// If the song streams its data while playing, the song object
	// will delete it when it gets destroyed.

	int len;
	MusInfo *info = NULL;
	DWORD id;
	bool needdelete = true;

	if (nomusic)
	{
		delete mem;
		return 0;
	}

	len = mem->GetLength();
	*mem >> id;
	mem->Seek (-4, SEEK_CUR);

	if (id == MAKE_ID('M','U','S',0x1a))
	{
		// This is a mus file
		if (!nofmod && opl_enable)
		{
			info = new OPLMUSSong (mem);
		}
		if (info == NULL)
		{
#ifdef _WIN32
			if (snd_mididevice != -2)
			{
				info = new MUSSong2 (mem);
			}
			else if (!nofmod)
#endif // _WIN32
			{
				info = new TimiditySong (mem);
			}
		}
	}
	else if (id == MAKE_ID('M','T','h','d'))
	{
		// This is a midi file
#ifdef _WIN32
		if (snd_mididevice != -2)
		{
			info = new MIDISong2 (mem);
		}
		else if (!nofmod)
#endif // _WIN32
		{
			info = new TimiditySong (mem);
		}
	}
	else if (id == MAKE_ID('S','N','E','S') && len >= 66048)
	{
		char header[0x23];

		mem->Read (header, 0x23);
		mem->Seek (-4, SEEK_CUR);
		if (strncmp (header+4, "-SPC700 Sound File Data", 23) == 0 &&
			header[0x21] == '\x1a' &&
			header[0x22] == '\x1a')
		{
			info = new SPCSong (mem);
		}
	}
	else if (id == MAKE_ID('f','L','a','C'))
	{
		needdelete = false;
		info = new FLACSong (mem);
	}

	if (info == NULL)
	{
		if (id == (('R')|(('I')<<8)|(('F')<<16)|(('F')<<24)))
		{
			DWORD subid;

			mem->Seek (8, SEEK_CUR);
			*mem >> subid;
			mem->Seek (-12, SEEK_CUR);

			if (subid == (('C')|(('D')<<8)|(('D')<<16)|(('A')<<24)))
			{
				// This is a CDDA file
				info = new CDDAFile (mem);
			}
		}
		if (info == NULL && !nofmod)	// no FMOD => no modules/streams
		{
			// First try loading it as MOD, then as a stream
			info = new MODSong (mem);
			if (!info->IsValid ())
			{
				delete info;
				needdelete = false;
				info = new StreamSong (mem);
			}
		}
	}

	if (info && !info->IsValid ())
	{
		delete info;
		info = NULL;
		needdelete = true;
	}

	if (needdelete)
	{
		delete mem;
	}

	return info;
}

void *I_RegisterCDSong (int track, int id)
{
	MusInfo *info = new CDSong (track, id);

	if (info && !info->IsValid ())
	{
		delete info;
		info = NULL;
	}

	return info;
}

// Is the song playing?
bool I_QrySongPlaying (void *handle)
{
	MusInfo *info = (MusInfo *)handle;

	return info ? info->IsPlaying () : false;
}

// Change to a different part of the song
bool I_SetSongPosition (void *handle, int order)
{
	MusInfo *info = (MusInfo *)handle;
	return info ? info->SetPosition (order) : false;
}

