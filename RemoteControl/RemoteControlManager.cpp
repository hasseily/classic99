/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2019, Tom Charlesworth, Michael Pohoreski, Nick Westgate

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: Remote Control Manager
 *
 * Handles remote control and interfaces with GameLink and potentially other interfaces
 *
 * Author: Henri Asseily
 *
 */

#include "RemoteControlManager.h"
#include <Windows.h>		// to inject the incoming Gamelink inputs into Classic99
#include "Memory.h"
#include "../console/tiemul.h"
#include "../console/sound.h"
#include "Gamelink.h"
#include <unordered_set>
#include "../addons/ams.h"

extern int max_volume;	// maximum volume, 0-100 (percentage)

RemoteControlManager RCManager = RemoteControlManager();	// Handles all remote control features

#define UNKNOWN_VOLUME_NAME "Unknown Volume"

 // The GameLink I/O structure
struct Gamelink_Block {
	// UINT pitch;	// not implemented here
	GameLink::sSharedMMapInput_R2 input_prev;
	GameLink::sSharedMMapInput_R2 input;
	GameLink::sSharedMMapAudio_R1 audio;
	UINT repeat_last_tick[256];
	bool want_mouse;
};
Gamelink_Block g_gamelink;

struct Info_HDV {
	std::string VolumeName;
	UINT32 sig;
};
Info_HDV g_infoHdv = { std::string(UNKNOWN_VOLUME_NAME), 0 };

UINT8* framedataRC;	// reversed framebuffer

UINT64 iCurrentTicks;						// Used to check the repeat interval
static std::unordered_set<UINT8> exclusionSet;		// list of VK codes that will not be passed through to the emulator

static bool bVideoNativeFormat = false;

bool bHardDiskIsLoaded = false;			// If HD is loaded, use it instead of floppy
bool bFloppyIsLoaded = false;

// Private Prototypes
static void reverseScanlines(uint8_t* destination, uint8_t* source, uint32_t width, uint32_t height, uint8_t depth);

//===========================================================================
// Global functions

bool RemoteControlManager::isRemoteControlEnabled()
{
	return GameLink::GetGameLinkEnabled();
}

//===========================================================================

void RemoteControlManager::setRemoteControlEnabled(bool bEnabled)
{
	return GameLink::SetGameLinkEnabled(bEnabled);
}

//===========================================================================

bool RemoteControlManager::isTrackOnlyEnabled()
{
	return GameLink::GetTrackOnlyEnabled();
}

//===========================================================================

void RemoteControlManager::setTrackOnlyEnabled(bool bEnabled)
{
	return GameLink::SetTrackOnlyEnabled(bEnabled);
}

//===========================================================================
LPBYTE RemoteControlManager::initializeMem(UINT size)
{
	if (GameLink::GetGameLinkEnabled())
	{
		if (framedataRC == NULL)
		{
			framedataRC = (UINT8*)malloc((256 + 16) * 4 * (192 + 16) * 4 * 4);
		}
		LPBYTE _mem = (LPBYTE)GameLink::AllocRAM(size);

		// initialize the gamelink previous input to 0
		memset(&g_gamelink.input_prev, 0, sizeof(GameLink::sSharedMMapInput_R2));

		(GameLink::Init(isTrackOnlyEnabled()));
		setKeypressExclusionList(aDefaultKeyExclusionList, sizeof(aDefaultKeyExclusionList));
		updateRunningProgramInfo();	// Disks might have been loaded before the shm was ready
		g_gamelink.audio.master_vol_l = g_gamelink.audio.master_vol_r = max_volume;
		return _mem;
	}
	return NULL;
}

//===========================================================================

bool RemoteControlManager::destroyMem()
{
	if (GameLink::GetGameLinkEnabled())
	{
		GameLink::Term();
		free(framedataRC);
		return true;
	}
	return false;
}

//===========================================================================

void RemoteControlManager::setKeypressExclusionList(UINT8 _exclusionList[], UINT8 length)
{
	exclusionSet.clear();
	for (UINT8 i = 0; i < length; i++)
	{
		exclusionSet.insert(_exclusionList[i]);
	}
}

//===========================================================================

void RemoteControlManager::setLoadedProgram(char* name, UINT8 length)
{
	// pass in length = 0 to remove it
	if (length > 0)
	{
		bHardDiskIsLoaded = true;
		g_infoHdv.VolumeName = std::string(name, length);
		g_infoHdv.sig = crc32buf(name, length);
	}
	else {
		bHardDiskIsLoaded = false;
		g_infoHdv.VolumeName = "";
		g_infoHdv.sig = 0;
	}
	updateRunningProgramInfo();
}

//===========================================================================

void RemoteControlManager::updateRunningProgramInfo()
{
	// Updates which program is running
	// Should only be called on re/boot
	if (bHardDiskIsLoaded)
	{
		GameLink::SetProgramInfo(g_infoHdv.VolumeName, 0, 0, 0, g_infoHdv.sig);
	}
	else
	{
		g_infoHdv.VolumeName = "";
		GameLink::SetProgramInfo(g_infoHdv.VolumeName, 0, 0, 0, 0);
	}

}

//===========================================================================
void RemoteControlManager::getInput()
{
	if (
		GameLink::GetGameLinkEnabled()
		&& myWnd != GetFocus()
		&& GameLink::In(&g_gamelink.input, &g_gamelink.audio)
		) {

		// -- Audio input	
		if (max_volume != g_gamelink.audio.master_vol_l)
		{
			max_volume = g_gamelink.audio.master_vol_l;
			SetSoundVolumes();
		}

		// -- Keyboard input
		{
			// Gamelink sets in shm 8 UINT32s, for a total of $FF bits
			// Using some kid of DIK keycodes.
			// Each bit will state if the scancode at that position is pressed (1) or released (0)
			// We keep a cache of the previous state, so we'll know if a key has changed state
			// and trigger the event
			// This is a map from the custom DIK scancodes to VK codes
			//HKL hKeyboardLayout = GetKeyboardLayout(0);
			UINT8 aDIKtoVK[256] = { 0x00, 0x1B, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0xBD, 0xBB,
									0x08, 0x09, 0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49, 0x4F, 0x50, 0xDB, 0xDD, 0x0D,
									0xA2, 0x41, 0x53, 0x44, 0x46, 0x47, 0x48, 0x4A, 0x4B, 0x4C, 0xBA, 0xDE, 0xC0, 0xA0, 0xDC,
									0x5A, 0x58, 0x43, 0x56, 0x42, 0x4E, 0x4D, 0xBC, 0xBE, 0xBF, 0xA1, 0x6A, 0xA4, 0x20, 0x14,
									0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x13, 0x91, 0x24, 0x26, 0x21,
									0x6D, 0x25, 0x0C, 0x27, 0x6B, 0x23, 0x28, 0x22, 0x2D, 0x2E, 0x2C, 0x00, 0xE2, 0x7A, 0x7B,
									0x0C, 0xEE, 0xF1, 0xEA, 0xF9, 0xF5, 0xF3, 0x00, 0x00, 0xFB, 0x2F, 0x7C, 0x7D, 0x7E, 0x7F,
									0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xED, 0x00, 0xE9, 0x00, 0xC1, 0x00, 0x00, 0x87,
									0x00, 0x00, 0x00, 0x00, 0xEB, 0x09, 0x00, 0xC2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
									0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB1, 0x00, 0x00, 0x00, 0x00,
									0x00, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x0D, 0xA3, 0x00, 0x00, 0xAD, 0xB6, 0xB3, 0x00,
									0xB2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAE, 0x00, 0xAF, 0x00, 0xB7,
									0x00, 0x00, 0xBF, 0x00, 0x2A, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
									0x00, 0x00, 0x00, 0x90, 0x00, 0x24, 0x26, 0x21, 0x00, 0x25, 0x00, 0x27, 0x00, 0x23, 0x28,
									0x22, 0x2D, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0x5C, 0x5D, 0x00, 0x5F,
									0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xAB, 0xA8, 0xA9, 0xA7, 0xA6, 0xAC, 0xB4, 0xB5, 0x00,
									0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
									0x00, 0x00 };

			// We need to handle long keypresses
			// We set up a do-nothing timer for each key so that the first repeat isn't too fast

			iCurrentTicks = GetTickCount();
			for (UINT8 blk = 0; blk < 8; ++blk)
			{
				const UINT old = g_gamelink.input_prev.keyb_state[blk];
				const UINT key = g_gamelink.input.keyb_state[blk];
				UINT8 scancode;
				UINT32 mask;
				UINT iKeyState;
				UINT8 iVK_Code;
				LPARAM lparam;
				UINT16 repeat;
				bool bCD, bPD;	// is current key down? is previous key down?

				for (UINT8 bit = 0; bit < 32; ++bit)
				{
					scancode = static_cast<UINT8>((blk * 32) + bit);
					mask = 1 << bit;
					bCD = (key & mask);	// key is down (bCD)
					bPD = (old & mask);	// key was down previously (bPD)
					if ((!bCD) && (!bPD))
						continue;	// the key was neither pressed previously nor now
					if (bCD && bPD)
					{
						// it's a repeat key
						if ((iCurrentTicks - g_gamelink.repeat_last_tick[scancode]) < kMinRepeatInterval)
							continue;	// drop repeat messages within kMinRepeatInterval ms
					}
					if (!bPD)	// This is the first time we're pressing the key, set the no-repeat interval
						g_gamelink.repeat_last_tick[scancode] = iCurrentTicks;
					else		// The key is already in repeat mode. Let it repeat as fast as it wants
						g_gamelink.repeat_last_tick[scancode] = 0;
					// Set up message
					iKeyState = bCD ? WM_KEYDOWN : WM_KEYUP;
					iVK_Code = aDIKtoVK[scancode];
					repeat = 1;		// Managed with ticks above
					{	// set up lparam
						lparam = repeat;
						lparam = lparam | ((LPARAM)scancode << 16);				// scancode
						lparam = lparam | ((LPARAM)(scancode > 0x7F) << 24);	// extended
						lparam = lparam | ((LPARAM)bPD << 30);				// previous key state
						lparam = lparam | ((LPARAM)!bCD << 31);		// transition state (1 for keyup)
					}
					// With PostMessage, the message goes to the highest level of AppleWin, hence controlling
					// all of AppleWin's behavior, including opening popups, etc...
					// It's not at all ideal, so we filter which keystrokes to pass in with a configurable exclusion list
					if (!exclusionSet.count(iVK_Code))
					{
						PostMessageW(myWnd, iKeyState, iVK_Code, lparam);
					}
				}
			}
		}

		// We're done parsing the input. Store it as the previous state
		memcpy(&g_gamelink.input_prev, &g_gamelink.input, sizeof(GameLink::sSharedMMapInput_R2));
	}
}


//===========================================================================

void RemoteControlManager::sendOutput(UINT16 width, UINT16 height, UINT8 *pFramebufferbits)
{
	if (GameLink::GetGameLinkEnabled()) {
		// here send the last drawn frame to GameLink
		// We could efficiently send to GameLink g_pFramebufferbits with GetFrameBufferWidth/GetFrameBufferHeight, but the scanlines are reversed
		// We instead memcpy each scanline of the bitmap of the frame in reverse into another buffer, and pass that to GameLink.
		// When GridCartographer/GameLink allows to pass in flags specifying the x/y/w/h etc...,

		if (pFramebufferbits == NULL)
		{
			// Don't send out video, just handle out-of-band commands
			GameLink::Out(systemMemory);
			return;
		}

		g_gamelink.want_mouse = false;
		// TODO: only send the framebuffer out when not in trackonly_mode

		if (pFramebufferbits != NULL)
		{
			reverseScanlines(framedataRC, pFramebufferbits, width, height, 4);
			GameLink::Out(
				width,
				height,
				1.0,								// image ratio
				g_gamelink.want_mouse,
				framedataRC,
				systemMemory);					// Main memory pointer
		}
	}
}


//===========================================================================

// --------------------------------------------
// Utility
// --------------------------------------------

// The framebuffer might have its scanlines inverted, from bottom to top
// To send a correct bitmap out to a 3rd party program we need to reverse the scanlines
void reverseScanlines(uint8_t* destination, uint8_t* source, uint32_t width, uint32_t height, uint8_t depth)
{
	uint32_t linesize = width * depth;
	uint8_t* loln = source;
	uint8_t* hiln = destination + (height - 1) * (uint_fast64_t)linesize;	// first pixel of the last line
	for (size_t i = 0; i < height; i++)
	{
		memcpy(hiln, loln, linesize);
		loln = loln + linesize;
		hiln = hiln - linesize;
	}
}

// CRC32 implementation Copyright (C) 1986 Gary S. Brown
static uint32_t crc_32_tab[] = { /* CRC polynomial 0xedb88320 */
0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

#define UPDC32(octet,crc) (crc_32_tab[((crc) ^ ((uint8_t)octet)) & 0xff] ^ ((crc) >> 8))

uint32_t crc32buf(char* buf, size_t len)
{
	register uint32_t oldcrc32;

	oldcrc32 = 0xFFFFFFFF;

	for (; len; --len, ++buf)
	{
		oldcrc32 = UPDC32(*buf, oldcrc32);
	}

	return ~oldcrc32;
}