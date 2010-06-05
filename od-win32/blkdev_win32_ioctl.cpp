/*
* UAE - The Un*x Amiga Emulator
*
* WIN32 CDROM/HD low level access code (IOCTL)
*
* Copyright 2002 Toni Wilen
*
*/

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WINDDK

#include "options.h"
#include "uae.h"
#include "threaddep/thread.h"
#include "blkdev.h"
#include "scsidev.h"
#include "gui.h"
#include "win32.h"

#include <devioctl.h>
#include <ntddcdrm.h>
#include <windows.h>
#include <mmsystem.h>
#include <winioctl.h>
#include <setupapi.h>   // for SetupDiXxx functions.
#include <stddef.h>
#ifdef RETROPLATFORM
#include "rp.h"
#endif

#include <ntddscsi.h>

#define IOCTL_DATA_BUFFER 8192
#define CDDA_BUFFERS 6

struct dev_info_ioctl {
	HANDLE h;
	uae_u8 *tempbuffer;
	TCHAR drvletter;
	TCHAR drvlettername[10];
	TCHAR devname[30];
	int mediainserted;
	int type;
	int blocksize;
	int mciid;
	int cdda;
	CDROM_TOC toc;
	UINT errormode;
	int playend;
	int fullaccess;
	int cdda_play_finished;
	int cdda_play;
	int cdda_paused;
	int cdda_volume;
	int cdda_volume_main;
	uae_u32 cd_last_pos;
	HWAVEOUT cdda_wavehandle;
	int cdda_start, cdda_end;
	uae_u8 subcode[96 * CDDA_BUFFERS];
};

static int MCICDA;

static struct dev_info_ioctl ciw32[MAX_TOTAL_DEVICES];

static void seterrormode (int unitnum)
{
	ciw32[unitnum].errormode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
}
static void reseterrormode (int unitnum)
{
	SetErrorMode (ciw32[unitnum].errormode);
}

static void close_device (int unitnum);
static int open_device (int unitnum);

static int mcierr (TCHAR *str, DWORD err)
{
	TCHAR es[1000];
	if (err == MMSYSERR_NOERROR)
		return MMSYSERR_NOERROR;
	if (mciGetErrorString (err, es, sizeof es))
		write_log (L"MCIErr: %s: %d = '%s'\n", str, err, es);
	return err;
}

static int win32_error (int unitnum, const TCHAR *format,...)
{
	LPVOID lpMsgBuf;
	va_list arglist;
	TCHAR buf[1000];
	DWORD err = GetLastError ();

	if (err == ERROR_WRONG_DISK) {
		write_log (L"IOCTL: media change, re-opening device\n");
		close_device (unitnum);
		if (!open_device (unitnum))
			write_log (L"IOCTL: re-opening failed!\n");
		return -1;
	}
	va_start (arglist, format);
	_vsntprintf (buf, sizeof buf / sizeof (TCHAR), format, arglist);
	FormatMessage (FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	if (log_scsi)
		write_log (L"IOCTL: unit=%d %s,%d: %s\n", unitnum, buf, err, (TCHAR*)lpMsgBuf);
	va_end (arglist);
	return err;
}

/* convert minutes, seconds and frames -> logical sector number */
static int msf2lsn (int	msf)
{
	int sector = (((msf >> 16) & 0xff) * 60 * 75 + ((msf >> 8) & 0xff) * 75 + ((msf >> 0) & 0xff));
	return sector;
}

/* convert logical sector number -> minutes, seconds and frames */
static int lsn2msf (int	sectors)
{
	int msf;
	msf = (sectors / (75 * 60)) << 16;
	msf |= ((sectors / 75) % 60) << 8;
	msf |= (sectors % 75) << 0;
	return msf;
}

static int close_createfile (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	ciw->fullaccess = 0;
	if (ciw->h != INVALID_HANDLE_VALUE) {
		if (log_scsi)
			write_log (L"IOCTL: IOCTL close\n");
		CloseHandle (ciw->h);
		if (log_scsi)
			write_log (L"IOCTL: IOCTL close completed\n");
		ciw->h = INVALID_HANDLE_VALUE;
		return 1;
	}
	return 0;
}

static int close_mci (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];
	MCI_GENERIC_PARMS gp = { 0 };

	ciw->playend = -1;
	if (ciw->mciid > 0) {
		if (log_scsi)
			write_log (L"IOCTL: MCI close\n");
		mcierr (L"MCI_STOP", mciSendCommand (ciw->mciid, MCI_STOP, MCI_WAIT, (DWORD_PTR)&gp));
		mcierr (L"MCI_CLOSE", mciSendCommand (ciw->mciid, MCI_CLOSE, MCI_WAIT, (DWORD_PTR)&gp));
		if (log_scsi)
			write_log (L"IOCTL: MCI close completed\n");
		ciw->mciid = 0;
		return 1;
	}
	return 0;
}

static int open_createfile (int unitnum, int fullaccess)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];
	int closed = 0;
	int cnt = 50;
	DWORD len;

	if (ciw->h != INVALID_HANDLE_VALUE) {
		if (fullaccess && ciw->fullaccess == 0) {
			close_createfile (unitnum);
		} else {
			return 1;
		}
	}
	closed = close_mci (unitnum);
	if (log_scsi)
		write_log (L"IOCTL: opening IOCTL %s\n", ciw->devname);
	for (;;) {
		if (fullaccess) {
			ciw->h = CreateFile (ciw->devname, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (ciw->h != INVALID_HANDLE_VALUE)
				ciw->fullaccess = 1;
		} else {
			DWORD flags = GENERIC_READ;
			ciw->h = CreateFile (ciw->devname, flags, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (ciw->h == INVALID_HANDLE_VALUE) {
				ciw->h = CreateFile (ciw->devname, flags, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (ciw->h == INVALID_HANDLE_VALUE) {
					flags |= GENERIC_WRITE;
					ciw->h = CreateFile (ciw->devname, flags, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				}
			}
		}
		if (ciw->h == INVALID_HANDLE_VALUE) {
			DWORD err = GetLastError ();
			if (err == ERROR_SHARING_VIOLATION) {
				if (closed && cnt > 0) {
					cnt--;
					Sleep (10);
					continue;
				}
			}
			if (closed)
				write_log (L"IOCTL: failed to re-open '%s', err=%d\n", ciw->devname, GetLastError ());
			return 0;
		}
		break;
	}
	if (!DeviceIoControl (ciw->h, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &len, NULL))
		write_log (L"IOCTL: FSCTL_ALLOW_EXTENDED_DASD_IO returned %d\n", GetLastError ());
	if (log_scsi)
		write_log (L"IOCTL: IOCTL open completed\n");
	return 1;
}

static int open_mci (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];
	TCHAR elname[10];
	TCHAR alname[100];
	MCI_OPEN_PARMS mciOpen;
	DWORD err, flags;
	int closed = 0;

	if (ciw->mciid > 0 || MCICDA == 0)
		return 1;
	ciw->playend = -1;
	closed = close_createfile (unitnum);
	if (log_scsi)
		write_log (L"IOCTL: MCI opening %s\n", ciw->drvlettername);
	memset (&mciOpen, 0, sizeof (mciOpen));
	mciOpen.lpstrDeviceType = (LPWSTR)MCI_DEVTYPE_CD_AUDIO;
	_stprintf (elname, L"%c:", ciw->drvletter);
	_stprintf (alname, L"CD%u:", GetCurrentTime ());
	mciOpen.lpstrElementName = elname;
	mciOpen.lpstrAlias = alname;
	flags = MCI_OPEN_ELEMENT | MCI_OPEN_SHAREABLE | MCI_OPEN_ALIAS | MCI_OPEN_TYPE | MCI_OPEN_TYPE_ID | MCI_WAIT;
	err = mciSendCommand (0, MCI_OPEN, flags, (DWORD_PTR)(LPVOID)&mciOpen);
	ciw->mciid = mciOpen.wDeviceID;
	if (err != MMSYSERR_NOERROR) {
		if (closed)
			mcierr (L"MCI_OPEN", err);
		return 0;
	}
	if (log_scsi)
		write_log (L"IOCTL: MCI open completed\n");
	return 1;
}

static void cdda_closewav (struct dev_info_ioctl *ciw)
{
	if (ciw->cdda_wavehandle != NULL)
		waveOutClose (ciw->cdda_wavehandle);
	ciw->cdda_wavehandle = NULL;
}

// DAE CDDA based on Larry Osterman's "Playing Audio CDs" blog series

static int cdda_openwav (struct dev_info_ioctl *ciw)
{
	WAVEFORMATEX wav = { 0 };
	MMRESULT mmr;

	wav.cbSize = 0;
	wav.nChannels = 2;
	wav.nSamplesPerSec = 44100;
	wav.wBitsPerSample = 16;
	wav.nBlockAlign = wav.wBitsPerSample / 8 * wav.nChannels;
	wav.nAvgBytesPerSec = wav.nBlockAlign * wav.nSamplesPerSec;
	wav.wFormatTag = WAVE_FORMAT_PCM;
	mmr = waveOutOpen (&ciw->cdda_wavehandle, WAVE_MAPPER, &wav, 0, 0, WAVE_ALLOWSYNC | WAVE_FORMAT_DIRECT);
	if (mmr != MMSYSERR_NOERROR) {
		write_log (L"CDDA: wave open %d\n", mmr);
		cdda_closewav (ciw);
		return 0;
	}
	return 1;
}

static void *cdda_play (void *v)
{
	DWORD len;
	struct dev_info_ioctl *ciw = (struct dev_info_ioctl*)v;
	int unitnum = ciw32 - ciw;
	int cdda_pos;
	int num_sectors = CDDA_BUFFERS;
	int bufnum;
	int buffered;
	uae_u8 *px[2], *p;
	int bufon[2];
	int i;
	WAVEHDR whdr[2];
	MMRESULT mmr;
	int volume, volume_main;
	int oldplay;


	for (i = 0; i < 2; i++) {
		memset (&whdr[i], 0, sizeof (WAVEHDR));
		whdr[i].dwFlags = WHDR_DONE;
	}

	while (ciw->cdda_play == 0)
		Sleep (10);
	oldplay = -1;

	p = (uae_u8*)VirtualAlloc (NULL, 2 * num_sectors * 4096, MEM_COMMIT, PAGE_READWRITE);
	px[0] = p;
	px[1] = p + num_sectors * 4096;
	bufon[0] = bufon[1] = 0;
	bufnum = 0;
	buffered = 0;
	volume = -1;
	volume_main = -1;

	if (cdda_openwav (ciw)) {

		for (i = 0; i < 2; i++) {
			memset (&whdr[i], 0, sizeof (WAVEHDR));
			whdr[i].dwBufferLength = 2352 * num_sectors;
			whdr[i].lpData = (LPSTR)px[i];
			mmr = waveOutPrepareHeader (ciw->cdda_wavehandle, &whdr[i], sizeof (WAVEHDR));
			if (mmr != MMSYSERR_NOERROR) {
				write_log (L"CDDA: waveOutPrepareHeader %d:%d\n", i, mmr);
				goto end;
			}
			whdr[i].dwFlags |= WHDR_DONE;
		}

		while (ciw->cdda_play > 0) {

			if (oldplay != ciw->cdda_play) {
				cdda_pos = ciw->cdda_start;
				oldplay = ciw->cdda_play;
			}

			while (!(whdr[bufnum].dwFlags & WHDR_DONE)) {
				Sleep (10);
				if (!ciw->cdda_play)
					goto end;
			}
			bufon[bufnum] = 0;

			if ((cdda_pos < ciw->cdda_end || ciw->cdda_end == 0xffffffff) && !ciw->cdda_paused && ciw->cdda_play) {
				RAW_READ_INFO rri;

				seterrormode (unitnum);
				rri.DiskOffset.QuadPart = 2048 * (cdda_pos - 150);
				rri.SectorCount = num_sectors;
				rri.TrackMode = RawWithSubCode;
				if (!DeviceIoControl (ciw->h, IOCTL_CDROM_RAW_READ, &rri, sizeof rri, px[bufnum], num_sectors * CD_RAW_SECTOR_WITH_SUBCODE_SIZE, &len, NULL)) {
					rri.TrackMode = CDDA;
					if (!DeviceIoControl (ciw->h, IOCTL_CDROM_RAW_READ, &rri, sizeof rri, px[bufnum], num_sectors * 2352, &len, NULL)) {
						DWORD err = GetLastError ();
						write_log (L"IOCTL_CDROM_RAW_READ CDDA sector %d returned %d\n", cdda_pos - 150, err);
						//ciw->cdda_play_finished = 1;
						//ciw->cdda_play = -1;
					}
				} else {
					for (i = 0; i < num_sectors; i++) {
						memcpy (ciw->subcode + i * 96, px[bufnum] + CD_RAW_SECTOR_WITH_SUBCODE_SIZE * i + 2352, 96);
					}
					for (i = 1; i < num_sectors; i++) {
						memmove (px[bufnum] + 2352 * i, px[bufnum] + CD_RAW_SECTOR_WITH_SUBCODE_SIZE * i, 2352);
					}
				}
				reseterrormode (unitnum);

				volume = ciw->cdda_volume;
				volume_main = currprefs.sound_volume;
				int vol_mult = (100 - volume_main) * volume / 100;
				if (vol_mult)
					vol_mult++;
				if (vol_mult >= 65536)
					vol_mult = 65536;
				uae_s16 *p = (uae_s16*)(px[bufnum]);
				for (i = 0; i < num_sectors * 2352 / 4; i++) {
					p[i * 2 + 0] = p[i * 2 + 0] * vol_mult / 65536;
					p[i * 2 + 1] = p[i * 2 + 1] * vol_mult / 65536;
				}
		
				bufon[bufnum] = 1;
				mmr = waveOutWrite (ciw->cdda_wavehandle, &whdr[bufnum], sizeof (WAVEHDR));
				if (mmr != MMSYSERR_NOERROR) {
					write_log (L"CDDA: waveOutWrite %d\n", mmr);
					break;
				}

				cdda_pos += num_sectors;
				if (cdda_pos - num_sectors < ciw->cdda_end && cdda_pos >= ciw->cdda_end) {
					ciw->cdda_play_finished = 1;
					ciw->cdda_play = -1;
					cdda_pos = ciw->cdda_end;
				}
				ciw->cd_last_pos = cdda_pos;

			}


			if (bufon[0] == 0 && bufon[1] == 0) {
				while (!(whdr[0].dwFlags & WHDR_DONE) || !(whdr[1].dwFlags & WHDR_DONE))
					Sleep (10);
				while (ciw->cdda_paused && ciw->cdda_play > 0)
					Sleep (10);
			}

			bufnum = 1 - bufnum;

		}
	}

end:
	while (!(whdr[0].dwFlags & WHDR_DONE) || !(whdr[1].dwFlags & WHDR_DONE))
		Sleep (10);
	for (i = 0; i < 2; i++)
		waveOutUnprepareHeader  (ciw->cdda_wavehandle, &whdr[i], sizeof (WAVEHDR));

	cdda_closewav (ciw);
	VirtualFree (p, 0, MEM_RELEASE);
	ciw->cdda_play = 0;
	write_log (L"CDDA: thread killed\n");
	return NULL;
}

static void cdda_stop (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (ciw->cdda_play > 0) {
		ciw->cdda_play = -1;
		while (ciw->cdda_play) {
			Sleep (10);
		}
	}
	ciw->cdda_play_finished = 0;
	ciw->cdda_paused = 0;
}

/* pause/unpause CD audio */
static int ioctl_command_pause (int unitnum, int paused)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (ciw->mciid > 0) {

		MCI_GENERIC_PARMS gp = { 0 };
		if (paused)
			mcierr(L"MCI_PAUSE", mciSendCommand (ciw->mciid, MCI_PAUSE, MCI_WAIT, (DWORD_PTR)&gp));
		else
			mcierr(L"MCI_RESUME", mciSendCommand (ciw->mciid, MCI_RESUME, MCI_WAIT, (DWORD_PTR)&gp));

	} else if (ciw->cdda) {

		ciw->cdda_paused = paused;
		
	} else {

		DWORD len;
		int command = paused ? IOCTL_CDROM_PAUSE_AUDIO : IOCTL_CDROM_RESUME_AUDIO;
		int cnt = 3;

		while (cnt-- > 0) {
			seterrormode (unitnum);
			if (!DeviceIoControl(ciw32[unitnum].h, command, NULL, 0, NULL, 0, &len, NULL)) {
				reseterrormode (unitnum);
				if (win32_error (unitnum, paused ? L"IOCTL_CDROM_PAUSE_AUDIO" : L"IOCTL_CDROM_RESUME_AUDIO") < 0)
					continue;
				return 0;
			}
			reseterrormode (unitnum);
			break;
		}
	}
	return 1;
}


/* stop CD audio */
static int ioctl_command_stop (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (ciw->mciid > 0) {

		MCI_GENERIC_PARMS gp = { 0 };
		mcierr (L"MCI_STOP", mciSendCommand (ciw->mciid, MCI_STOP, MCI_WAIT, (DWORD_PTR)&gp));
		ciw->playend = -1;

	} else if (ciw->cdda) {

		cdda_stop (unitnum);
		
	} else {

		DWORD len;
		int cnt = 3;

		while (cnt-- > 0) {
			seterrormode (unitnum);
			if(!DeviceIoControl (ciw32[unitnum].h, IOCTL_CDROM_STOP_AUDIO, NULL, 0, NULL, 0, &len, NULL)) {
				reseterrormode (unitnum);
				if (win32_error (unitnum, L"IOCTL_CDROM_STOP_AUDIO") < 0)
					continue;
				return 0;
			}
			reseterrormode (unitnum);
			break;
		}
	}
	return 1;
}

static void ioctl_command_volume (int unitnum, uae_u16 volume)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	ciw->cdda_volume = volume;
}

/* play CD audio */
static int ioctl_command_play (int unitnum, uae_u32 start, uae_u32 end, int scan)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (!ciw->cdda)
		open_mci (unitnum);

	if (ciw->mciid > 0) {

		MCI_SET_PARMS setParms = {0};
		MCI_PLAY_PARMS playParms = {0};

		setParms.dwTimeFormat = MCI_FORMAT_MSF;
		mcierr (L"MCI_SET", mciSendCommand (ciw->mciid, MCI_SET, MCI_SET_TIME_FORMAT | MCI_WAIT, (DWORD_PTR)&setParms));
		playParms.dwFrom = MCI_MAKE_MSF((start >> 16) & 0xff, (start >> 8) & 0xff, start & 0xff);
		playParms.dwTo = MCI_MAKE_MSF((end >> 16) & 0xff, (end >> 8) & 0xff, end & 0xff);
		mcierr (L"MCI_PLAY", mciSendCommand (ciw->mciid, MCI_PLAY, MCI_FROM | MCI_TO, (DWORD_PTR)&playParms));
		ciw->playend = end;

	} else if (ciw->cdda) {

		if (!open_createfile (unitnum, 1))
			return 0;
		ciw->cdda_paused = 0;
		ciw->cdda_play_finished = 0;
		if (!ciw->cdda_play) {
			uae_start_thread (L"cdda_play", cdda_play, ciw, NULL);
		}
		ciw->cdda_start = msf2lsn (start);
		ciw->cdda_end = msf2lsn (end);
		ciw->cd_last_pos = ciw->cdda_start;
		ciw->cdda_play++;

	} else {

		DWORD len;
		CDROM_PLAY_AUDIO_MSF pa;
		int cnt = 3;

#if 0
		{
			VOLUME_CONTROL vc;
			if (!DeviceIoControl(ciw32[unitnum].h, IOCTL_CDROM_GET_VOLUME, NULL, 0, &vc, sizeof(vc), &len, NULL))
				write_log (L"IOCTL_CDROM_GET_VOLUME %d\n", GetLastError());
			vc.PortVolume[0] = 0xff;
			vc.PortVolume[1] = 0xff;
			vc.PortVolume[2] = 0xff;
			vc.PortVolume[3] = 0xff;
			if (!DeviceIoControl(ciw32[unitnum].h, IOCTL_CDROM_SET_VOLUME, &vc, sizeof(vc), NULL, 0, &len, NULL))
				write_log (L"IOCTL_CDROM_SET_VOLUME %d\n", GetLastError());
		}
#endif

		while (cnt-- > 0) {
			pa.StartingM = start >> 16;
			pa.StartingS = start >> 8;
			pa.StartingF = start >> 0;
			pa.EndingM = end >> 16;
			pa.EndingS = end >> 8;
			pa.EndingF = end >> 0;
			seterrormode (unitnum);
			if (!DeviceIoControl (ciw32[unitnum].h, IOCTL_CDROM_PLAY_AUDIO_MSF, &pa, sizeof(pa), NULL, 0, &len, NULL)) {
				reseterrormode (unitnum);
				if (win32_error (unitnum, L"IOCTL_CDROM_PLAY_AUDIO_MSF %02.%02.%02-%02.%02.%02",
					pa.StartingM, pa.StartingS, pa.StartingF, pa.EndingM, pa.EndingS, pa.EndingF ) < 0) continue;
				return 0;
			}
			reseterrormode (unitnum);
			break;
		}
	}

	return 1;
}

/* read qcode */
static uae_u8 *ioctl_command_qcode (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (ciw->mciid > 0) {

		static uae_u8 buf[4 + 12];
		MCI_STATUS_PARMS mciStatusParms;
		DWORD err, mode;
		uae_u8 *p;
		uae_u32 pos, pos2;
		int trk;

		memset (buf, 0, sizeof buf);
		memset (&mciStatusParms, 0, sizeof mciStatusParms);
		mciStatusParms.dwItem = MCI_STATUS_MODE;
		err = mciSendCommand (ciw->mciid, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, (DWORD_PTR)&mciStatusParms);
		if (err != MMSYSERR_NOERROR)
			return 0;
		mode = mciStatusParms.dwReturn;
		mciStatusParms.dwItem = MCI_STATUS_CURRENT_TRACK;
		err = mciSendCommand (ciw->mciid, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, (DWORD_PTR)&mciStatusParms);
		if (err != MMSYSERR_NOERROR)
			return 0;
		trk = mciStatusParms.dwReturn - 1;
		if (trk < 0)
			trk = 0;
		mciStatusParms.dwItem = MCI_STATUS_POSITION;
		err = mciSendCommand (ciw->mciid, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, (DWORD_PTR)&mciStatusParms);
		if (err != MMSYSERR_NOERROR)
			return 0;
		pos = (((mciStatusParms.dwReturn >> 16) & 0xff) << 0) | (((mciStatusParms.dwReturn >> 8) & 0xff) << 8) | (((mciStatusParms.dwReturn >> 0) & 0xff) << 16);

		p = buf;
		p[1] = AUDIO_STATUS_NO_STATUS;
		if (mode == MCI_MODE_PLAY)
			p[1] = AUDIO_STATUS_IN_PROGRESS;
		else if (mode == MCI_MODE_PAUSE)
			p[1] = AUDIO_STATUS_PAUSED;
		p[3] = 12;

		p = buf + 4;
		p[1] = (ciw->toc.TrackData[trk].Control << 0) | (ciw->toc.TrackData[trk].Adr << 4);
		p[2] = trk + 1;
		p[3] = 1;

		p[5] = (pos >> 16) & 0xff;
		p[6] = (pos >> 8) & 0xff;
		p[7] = (pos >> 0) & 0xff;

		pos = msf2lsn (pos);
		pos2 = (ciw->toc.TrackData[trk].Address[1] << 16) | (ciw->toc.TrackData[trk].Address[2] << 8) | (ciw->toc.TrackData[trk].Address[3] << 0);
		pos -= msf2lsn (pos2);
		if (pos < 0)
			pos = 0;
		pos = lsn2msf (pos);

		p[9] = (pos >> 16) & 0xff;
		p[10] = (pos >> 8) & 0xff;
		p[11] = (pos >> 0) & 0xff;

		return buf;

	} else if (ciw->cdda) {

		static uae_u8 buf[4 + 12];
		uae_u8 *p;
		int trk;
		CDROM_TOC *toc = &ciw->toc;
		int pos;
		int msf;
		int start, end;
		int status;

		memset (buf, 0, sizeof buf);
		p = buf;

		status = AUDIO_STATUS_NO_STATUS;
		if (ciw->cdda_play) {
			status = AUDIO_STATUS_IN_PROGRESS;
			if (ciw->cdda_paused)
				status = AUDIO_STATUS_PAUSED;
		} else if (ciw->cdda_play_finished) {
			status = AUDIO_STATUS_PLAY_COMPLETE;
		}
		pos = ciw->cd_last_pos;
#if 0
		pos -= CDDA_BUFFERS * 2;
		if (ciw->cdda_play && pos < ciw->cdda_start) {
			pos = ciw->cdda_start;
			status = AUDIO_STATUS_NO_STATUS;
		}
#endif
		p[1] = status;
		p[3] = 12;

		p = buf + 4;

		if (pos >= 150)
			trk = 0;
		start = end = 0;
		for (trk = 0; trk <= toc->LastTrack; trk++) {
			TRACK_DATA *td = &toc->TrackData[trk];
			start = msf2lsn ((td->Address[1] << 16) | (td->Address[2] << 8) | td->Address[3]);
			end = msf2lsn ((td[1].Address[1] << 16) | (td[1].Address[2] << 8) | td[1].Address[3]);
			if (pos < start)
				break;
			if (pos >= start && pos < end)
				break;
		}
		p[1] = (toc->TrackData[trk].Control << 0) | (toc->TrackData[trk].Adr << 4);
		p[2] = trk + 1;
		p[3] = 1;
		msf = lsn2msf (pos);
		p[5] = (msf >> 16) & 0xff;
		p[6] = (msf >> 8) & 0xff;
		p[7] = (msf >> 0) & 0xff;
		pos -= start;
		if (pos < 0)
			pos = 0;
		msf = lsn2msf (pos);
		p[9] = (pos >> 16) & 0xff;
		p[10] = (pos >> 8) & 0xff;
		p[11] = (pos >> 0) & 0xff;

		return buf;

	} else {

		SUB_Q_CHANNEL_DATA qcd;
		DWORD len;
		ULONG in = 1;
		uae_u8 *p = ciw->tempbuffer;
		int cnt = 3;

		memset (p, 0, 4 + 12);
		p[1] = 0x15;
		p[3] = 12;
		while (cnt-- > 0) {
			reseterrormode (unitnum);
			if(!DeviceIoControl (ciw->h, IOCTL_CDROM_READ_Q_CHANNEL, &in, sizeof(in), &qcd, sizeof (qcd), &len, NULL)) {
				reseterrormode (unitnum);
				if (win32_error (unitnum, L"IOCTL_CDROM_READ_Q_CHANNEL") < 0)
					continue;
				return 0;
			}
			break;
		}
		reseterrormode (unitnum);
		p[1] = qcd.CurrentPosition.Header.AudioStatus;
		p += 4;
		p[1] = (qcd.CurrentPosition.Control << 0) | (qcd.CurrentPosition.ADR << 4);
		p[2] = qcd.CurrentPosition.TrackNumber;
		p[3] = qcd.CurrentPosition.IndexNumber;
		p[5] = qcd.CurrentPosition.AbsoluteAddress[1];
		p[6] = qcd.CurrentPosition.AbsoluteAddress[2];
		p[7] = qcd.CurrentPosition.AbsoluteAddress[3];
		p[9] = qcd.CurrentPosition.TrackRelativeAddress[1];
		p[10] = qcd.CurrentPosition.TrackRelativeAddress[2];
		p[11] = qcd.CurrentPosition.TrackRelativeAddress[3];
		return ciw->tempbuffer;
	}
}

typedef struct _SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER {
	SCSI_PASS_THROUGH_DIRECT spt;
	ULONG Filler;
	UCHAR SenseBuf[32];
} SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER;

static uae_u8 *spti_read (int unitnum, int sector, int sectorsize)
{
	DWORD status, returned;
	SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER swb;
	uae_u8 *p = ciw32[unitnum].tempbuffer;
	/* number of bytes returned depends on type of track:
	* CDDA = 2352
	* Mode1 = 2048
	* Mode2 = 2336
	* Mode2 Form 1 = 2048
	* Mode2 Form 2 = 2328
	*/
	uae_u8 cmd[12] = { 0xbe, 0, 0, 0, 0, 0, 0, 0, 1, 0x10, 0, 0 };
	int len = sizeof cmd;

	if (!open_createfile (unitnum, 1))
		return 0;
	ciw32[unitnum].cd_last_pos = sector + sectorsize;
	cmd[3] = (uae_u8)(sector >> 16);
	cmd[4] = (uae_u8)(sector >> 8);
	cmd[5] = (uae_u8)(sector >> 0);
	gui_flicker_led (LED_CD, unitnum, 1);
	memset (&swb, 0, sizeof (swb));
	memcpy (swb.spt.Cdb, cmd, len);
	swb.spt.Length = sizeof (SCSI_PASS_THROUGH);
	swb.spt.CdbLength = len;
	swb.spt.DataIn = SCSI_IOCTL_DATA_IN;
	swb.spt.DataTransferLength = IOCTL_DATA_BUFFER;
	swb.spt.DataBuffer = p;
	memset (p, 0, IOCTL_DATA_BUFFER);
	swb.spt.TimeOutValue = 80 * 60;
	swb.spt.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER, SenseBuf);
	swb.spt.SenseInfoLength = 32;

	seterrormode (unitnum);
	status = DeviceIoControl (ciw32[unitnum].h, IOCTL_SCSI_PASS_THROUGH_DIRECT,
		&swb, sizeof (SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER),
		&swb, sizeof (SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER),
		&returned, NULL);
	reseterrormode (unitnum);
	if (!status) {
		DWORD err = GetLastError ();
		write_log (L"IOCTL_RAW_SCSI unit %d, ERR=%d ", unitnum, err);
		return 0;
	}
	return p;
}

uae_u8 *ioctl_command_rawread (int unitnum, int sector, int sectorsize)
{
	int cnt = 3;
	RAW_READ_INFO rri;
	DWORD len;
	uae_u8 *p = ciw32[unitnum].tempbuffer;

	if (log_scsi)
		write_log (L"IOCTL rawread unit=%d sector=%d blocksize=%d\n", unitnum, sector, sectorsize);
	if (!os_vista)
		return spti_read (unitnum, sector, sectorsize);
	if (!open_createfile (unitnum, 1))
		return 0;
	cdda_stop (unitnum);
	if (sectorsize != 2336 && sectorsize != 2352 && sectorsize != 2048)
		return 0;
	while (cnt-- > 0) {
		gui_flicker_led (LED_CD, unitnum, 1);
		seterrormode (unitnum);
		rri.DiskOffset.QuadPart = sector * 2048;
		rri.SectorCount = 1;
		rri.TrackMode = RawWithSubCode;
		len = sectorsize;
		memset (p, 0, sectorsize);
		if (!DeviceIoControl (ciw32[unitnum].h, IOCTL_CDROM_RAW_READ, &rri, sizeof rri,
			p, IOCTL_DATA_BUFFER, &len, NULL)) {
				DWORD err = GetLastError ();
				write_log (L"IOCTL rawread unit=%d sector=%d blocksize=%d, ERR=%d\n", unitnum, sector, sectorsize, err);
		}
		reseterrormode (unitnum);
		ciw32[unitnum].cd_last_pos = sector + sectorsize;
		break;
	}
	if (sectorsize == 2352)
		return p;
	return p + 16;
}

static int ioctl_command_readwrite (int unitnum, int sector, int write, int blocksize, uae_u8 **ptr)
{
	DWORD dtotal;
	int cnt = 3;
	uae_u8 *p = ciw32[unitnum].tempbuffer;

	*ptr = NULL;
	if (!open_createfile (unitnum, 0))
		return 0;
	cdda_stop (unitnum);
	while (cnt-- > 0) {
		gui_flicker_led (LED_CD, unitnum, 1);
		seterrormode (unitnum);
		if (SetFilePointer (ciw32[unitnum].h, sector * ciw32[unitnum].blocksize, 0, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
			reseterrormode (unitnum);
			if (win32_error (unitnum, L"SetFilePointer") < 0)
				continue;
			return 0;
		}
		reseterrormode (unitnum);
		break;
	}
	cnt = 3;
	while (cnt-- > 0) {
		gui_flicker_led (LED_CD, unitnum, 1);
		seterrormode (unitnum);
		if (write) {
			if (!WriteFile (ciw32[unitnum].h, p, blocksize, &dtotal, 0)) {
				int err;
				reseterrormode (unitnum);
				err = win32_error (unitnum, L"WriteFile");
				if (err < 0)
					continue;
				if (err == ERROR_WRITE_PROTECT)
					return -1;
				return 0;
			}
		} else {
			dtotal = 0;
			if (!ReadFile (ciw32[unitnum].h, p, blocksize, &dtotal, 0)) {
				reseterrormode (unitnum);
				if (win32_error (unitnum, L"ReadFile") < 0)
					continue;
				return 0;
			}
			if (dtotal == 0) {
				/* ESS Mega (CDTV) "fake" data area returns zero bytes and no error.. */
				*ptr = spti_read (unitnum, sector, 2048);
				if (log_scsi)
					write_log (L"IOCTL unit %d, sector %d: ReadFile()==0. SPTI=%d\n", unitnum, sector, *ptr == 0 ? GetLastError () : 0);
				return 1;
#if 0
				DWORD len = CD_RAW_SECTOR_WITH_SUBCODE_SIZE, err = -1;
				RAW_READ_INFO rri = { 0 };
				rri.DiskOffset.QuadPart = sector * 2048;
				rri.SectorCount = 1;
				rri.TrackMode = RawWithSubCode;
				memset (p, 0, blocksize);
				if (!DeviceIoControl(ciw32[unitnum].h, IOCTL_CDROM_RAW_READ, &rri, sizeof rri,
					p, len, &len, NULL)) {
						err = GetLastError (); /* returns ERROR_IO_DEVICE and still succeeds?! */
				}
				p += 16; /* skip raw header */
				write_log (L"ioctl_command_read(%d,%d)==0, IOCTL_CDROM_RAW_READ = d\n",
					sector, blocksize, err);
#endif
			}
		}
		reseterrormode (unitnum);
		gui_flicker_led (LED_CD, unitnum, 1);
		break;
	}
	*ptr = p;
	return 1;
}

static int ioctl_command_write (int unitnum, int sector)
{
	uae_u8 *ptr;
	return ioctl_command_readwrite (unitnum, sector, 1, ciw32[unitnum].blocksize, &ptr);
}

static uae_u8 *ioctl_command_read (int unitnum, int sector)
{
	uae_u8 *ptr;
	if (ioctl_command_readwrite (unitnum, sector, 0, ciw32[unitnum].blocksize, &ptr) > 0)
		return ptr;
	return NULL;
}

static int fetch_geometry (int unitnum, struct device_info *di)
{
	DISK_GEOMETRY geom;
	DWORD len;
	int cnt = 3;

	if (!open_createfile (unitnum, 0))
		return 0;
	while (cnt-- > 0) {
		seterrormode (unitnum);
		if (!DeviceIoControl (ciw32[unitnum].h, IOCTL_CDROM_GET_DRIVE_GEOMETRY, NULL, 0, &geom, sizeof(geom), &len, NULL)) {
			reseterrormode (unitnum);
			if (win32_error (unitnum, L"IOCTL_CDROM_GET_DRIVE_GEOMETRY") < 0)
				continue;
			return 0;
		}
		reseterrormode (unitnum);
		break;
	}
	if (di) {
		di->cylinders = geom.Cylinders.LowPart;
		di->sectorspertrack = geom.SectorsPerTrack;
		di->trackspercylinder = geom.TracksPerCylinder;
		di->bytespersector = geom.BytesPerSector;
	}
	return 1;
}

static int ismedia (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (ciw->mciid > 0) {

		DWORD err;
		MCI_STATUS_PARMS mciStatusParms;

		memset (&mciStatusParms, 0, sizeof mciStatusParms);
		mciStatusParms.dwItem = MCI_STATUS_MEDIA_PRESENT;
		err = mciSendCommand (ciw->mciid, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, (DWORD_PTR)&mciStatusParms);
		if (err != MMSYSERR_NOERROR)
			return 0;
		if (mciStatusParms.dwReturn)
			return 1;
		return 0;

	} else {

		struct device_info di;
		memset (&di, 0, sizeof di);
		return fetch_geometry (unitnum, &di);

	}
}

/* read toc */
static uae_u8 *ioctl_command_toc (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];
	DWORD len;
	int i;
	uae_u8 *p = ciw->tempbuffer;
	int cnt = 3;
	CDROM_TOC *toc = &ciw->toc;

	if (!open_createfile (unitnum, 0))
		return 0;
	cdda_stop (unitnum);
	ciw32[unitnum].cd_last_pos = 0;
	gui_flicker_led (LED_CD, unitnum, 1);
	while (cnt-- > 0) {
		seterrormode (unitnum);
		if (!DeviceIoControl (ciw32[unitnum].h, IOCTL_CDROM_READ_TOC, NULL, 0, toc, sizeof(CDROM_TOC), &len, NULL)) {
			reseterrormode (unitnum);
			if (win32_error (unitnum, L"IOCTL_CDROM_READ_TOC") < 0)
				continue;
			return 0;
		}
		reseterrormode (unitnum);
		break;
	}

	p[0] = ((toc->LastTrack + 4) * 11) >> 8;
	p[1] = ((toc->LastTrack + 4) * 11) & 0xff;
	p[2] = 1;
	p[3] = toc->LastTrack;
	p += 4;
	memset (p, 0, 11);
	p[0] = 1;
	p[1] = (toc->TrackData[0].Control << 0) | (toc->TrackData[0].Adr << 4);
	p[3] = 0xa0;
	p[8] = 1;
	p += 11;
	memset (p, 0, 11);
	p[0] = 1;
	p[1] = 0x10;
	p[3] = 0xa1;
	p[8] = toc->LastTrack;
	p += 11;
	memset (p, 0, 11);
	p[0] = 1;
	p[1] = 0x10;
	p[3] = 0xa2;
	p[8] = toc->TrackData[toc->LastTrack].Address[1];
	p[9] = toc->TrackData[toc->LastTrack].Address[2];
	p[10] = toc->TrackData[toc->LastTrack].Address[3];
	p += 11;
	for (i = 0; i < toc->LastTrack; i++) {
		memset (p, 0, 11);
		p[0] = 1;
		p[1] = (toc->TrackData[i].Control << 0) | (toc->TrackData[i].Adr << 4);
		p[2] = 0;
		p[3] = i + 1;
		p[8] = toc->TrackData[i].Address[1];
		p[9] = toc->TrackData[i].Address[2];
		p[10] = toc->TrackData[i].Address[3];
		p += 11;
	}
	gui_flicker_led (LED_CD, unitnum, 1);
	return ciw32[unitnum].tempbuffer;
}

/* open device level access to cd rom drive */
static int sys_cddev_open (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	ciw->cdda = 1;
	ciw->cdda_volume = 0xffff;
	ciw->cdda_volume_main = currprefs.sound_volume;
	/* buffer must be page aligned for device access */
	ciw->tempbuffer = (uae_u8*)VirtualAlloc (NULL, IOCTL_DATA_BUFFER, MEM_COMMIT, PAGE_READWRITE);
	if (!ciw->tempbuffer) {
		write_log (L"IOCTL: failed to allocate buffer");
		return 1;
	}
	if (!open_createfile (unitnum, 0)) {
		write_log (L"IOCTL: failed to open '%s', err=%d\n", ciw->devname, GetLastError ());
		goto error;
	}
	ciw->mediainserted = ioctl_command_toc (unitnum) ? 1 : 0;
	write_log (L"IOCTL: device '%s' opened succesfully (unit number=%d,media=%d)\n", ciw->devname, unitnum, ciw->mediainserted);
	ioctl_command_stop (unitnum);
	return 0;
error:
	win32_error (unitnum, L"CreateFile");
	VirtualFree (ciw->tempbuffer, 0, MEM_RELEASE);
	ciw->tempbuffer = NULL;
	CloseHandle (ciw->h);
	ciw->h = NULL;
	return -1;
}

static int unitcheck (int unitnum)
{
	if (unitnum >= MAX_TOTAL_DEVICES) {
		if (unitnum < 'A' || unitnum > 'Z')
			return 0;
		return 1;
	}
	if (ciw32[unitnum].drvletter == 0)
		return 0;
	return 1;
}

/* close device handle */
void sys_cddev_close (int unitnum)
{
	struct dev_info_ioctl *ciw = &ciw32[unitnum];

	if (!unitcheck (unitnum))
		return;
	cdda_stop (unitnum);
	close_createfile (unitnum);
	close_mci (unitnum);
	VirtualFree (ciw->tempbuffer, 0, MEM_RELEASE);
	ciw->tempbuffer = NULL;
}

static int open_device (int unitnum)
{
	if (!unitcheck (unitnum))
		return 0;
	if (sys_cddev_open (unitnum) == 0)
		return 1;
	return 0;
}
static void close_device (int unitnum)
{
	sys_cddev_close (unitnum);
}

static void close_bus (void)
{
}

static int total_devices;

static int open_bus (int flags)
{
	int dwDriveMask;
	int drive, i;
	TCHAR tmp[10];

	for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
		memset (&ciw32[i], 0, sizeof (struct dev_info_ioctl));
		ciw32[i].h = INVALID_HANDLE_VALUE;
	}
	MCICDA = 1;//os_vista ? 1 : 0;
	total_devices = 0;
	dwDriveMask = GetLogicalDrives ();
	if (log_scsi)
		write_log (L"IOCTL: drive mask = %08X\n", dwDriveMask);
	dwDriveMask >>= 2; // Skip A and B drives...
	for (drive = 'C'; drive <= 'Z' && total_devices < MAX_TOTAL_DEVICES; drive++) {
		if (dwDriveMask & 1) {
			int dt;
			_stprintf (tmp, L"%c:\\", drive);
			dt = GetDriveType (tmp);
			if (log_scsi)
				write_log (L"IOCTL: drive %c type %d\n", drive, dt);
			if (((flags & (1 << INQ_ROMD)) && dt == DRIVE_CDROM) || ((flags & (1 << INQ_DASD)) && dt == DRIVE_FIXED)) {
				if (log_scsi)
					write_log (L"IOCTL: drive %c: = unit %d\n", drive, total_devices);
				ciw32[total_devices].drvletter = drive;
				_tcscpy (ciw32[total_devices].drvlettername, tmp);
				ciw32[total_devices].type = dt;
				ciw32[total_devices].blocksize = 2048;
				_stprintf (ciw32[total_devices].devname, L"\\\\.\\%c:", drive);
				total_devices++;
			}
		}
		dwDriveMask >>= 1;
	}
	return total_devices;
}

static int ioctl_ismedia (int unitnum, int quick)
{
	if (!unitcheck (unitnum))
		return -1;
	if (quick) {
		struct dev_info_ioctl *ciw = &ciw32[unitnum];
		return ciw->mediainserted;
	}
	return ismedia (unitnum);
}

static struct device_info *info_device (int unitnum, struct device_info *di)
{
	if (!unitcheck (unitnum))
		return 0;
	di->bus = unitnum;
	di->target = 0;
	di->lun = 0;
	di->media_inserted = 0;
	di->bytespersector = 2048;
	_stprintf (di->mediapath, L"\\\\.\\%c:", ciw32[unitnum].drvletter);
	if (fetch_geometry (unitnum, di)) { // || ioctl_command_toc (unitnum))
		di->media_inserted = 1;
		ciw32[unitnum].blocksize = di->bytespersector;
	}
	di->write_protected = ciw32[unitnum].type == DRIVE_CDROM ? 1 : 0;
	di->type = ciw32[unitnum].type == DRIVE_CDROM ? INQ_ROMD : INQ_DASD;
	di->id = ciw32[unitnum].drvletter;
	_tcscpy (di->label, ciw32[unitnum].drvlettername);
	return di;
}

void win32_ioctl_media_change (TCHAR driveletter, int insert)
{
	int i;

	for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
		if (ciw32[i].drvletter == driveletter && ciw32[i].mediainserted != insert) {
			write_log (L"IOCTL: media change %s %d\n", ciw32[i].drvlettername, insert);
			ciw32[i].mediainserted = insert;
			scsi_do_disk_change (driveletter, insert);
#ifdef RETROPLATFORM
			rp_cd_image_change (i, insert ? ciw32[i].drvlettername : NULL);
#endif
		}
	}
}

static struct device_scsi_info *ioctl_scsi_info (int unitnum, struct device_scsi_info *dsi)
{
	if (!unitcheck (unitnum))
		return 0;
	dsi->buffer = ciw32[unitnum].tempbuffer;
	dsi->bufsize = IOCTL_DATA_BUFFER;
	return dsi;
}

struct device_functions devicefunc_win32_ioctl = {
	open_bus, close_bus, open_device, close_device, info_device,
	0, 0, 0,
	ioctl_command_pause, ioctl_command_stop, ioctl_command_play, ioctl_command_volume, ioctl_command_qcode,
	ioctl_command_toc, ioctl_command_read, ioctl_command_rawread, ioctl_command_write,
	0, ioctl_scsi_info, ioctl_ismedia
};

#endif