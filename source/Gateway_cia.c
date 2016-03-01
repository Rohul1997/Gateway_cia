#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <3ds.h>

extern u32 PAYLOAD_TEXTADDR[];
extern u32 PAYLOAD_TEXTMAXSIZE;

extern Handle gspGpuHandle;

u8 *filebuffer;
u32 filebuffer_maxsize;

char regionids_table[7][4] = {//http://3dbrew.org/wiki/Nandrw/sys/SecureInfo_A
"JPN",
"USA",
"EUR",
"JPN", //"AUS"
"CHN",
"KOR",
"TWN"
};

void gxlowcmd_4(u32* inadr, u32* outadr, u32 size, u32 width0, u32 height0, u32 width1, u32 height1, u32 flags)
{
	GX_TextureCopy(inadr, width0 | (height0<<16), outadr, width1 | (height1<<16), size, flags);
}

Result gsp_flushdcache(u8* adr, u32 size)
{
	return GSPGPU_FlushDataCache(adr, size);
}

Result loadsd_payload(char *filepath, u32 *payloadsize)
{
	struct stat filestats;
	FILE *f;
	size_t readsize=0;

	if(stat(filepath, &filestats)==-1)return errno;

	*payloadsize = filestats.st_size;

	if(filestats.st_size==0 || filestats.st_size>PAYLOAD_TEXTMAXSIZE)
	{
		printf("Invalid SD payload size: 0x%08x.\n", (unsigned int)filestats.st_size);
		return -3;
	}

	f = fopen(filepath, "r");
	if(f==NULL)return errno;

	readsize = fread(filebuffer, 1, filestats.st_size, f);
	fclose(f);

	if(readsize!=filestats.st_size)
	{
		printf("fread() failed with the SD payload.\n");
		return -2;
	}

	return 0;
}

Result savesd_payload(char *filepath, u32 payloadsize)
{
	FILE *f;
	size_t writesize=0;

	unlink(filepath);

	f = fopen(filepath, "w+");
	if(f==NULL)
	{
		printf("Failed to open the SD payload for writing.\n");
		return errno;
	}

	writesize = fwrite(filebuffer, 1, payloadsize, f);
	fclose(f);

	if(writesize!=payloadsize)
	{
		printf("fwrite() failed with the SD payload.\n");
		return -2;
	}

	return 0;
}

Result load_gateway()
{
	Result ret = 0;
	u8 region=0;
	u8 new3dsflag = 0;

	OS_VersionBin nver_versionbin;
	OS_VersionBin cver_versionbin;

	u32 payloadsize = 0, payloadsize_aligned = 0;
	u32 payload_src = 0;

	char payload_sysver[32];
	char payloadurl[0x80];
	char payload_sdpath[0x80];

	void (*funcptr)(u32*, u32*) = NULL;
	u32 *paramblk = NULL;

	memset(&nver_versionbin, 0, sizeof(OS_VersionBin));
	memset(&cver_versionbin, 0, sizeof(OS_VersionBin));

	memset(payload_sysver, 0, sizeof(payload_sysver));
	memset(payloadurl, 0, sizeof(payloadurl));
	memset(payload_sdpath, 0, sizeof(payload_sdpath));

	ret = cfguInit();
	if(ret!=0)
	{
		printf("Failed to init cfgu: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	ret = CFGU_SecureInfoGetRegion(&region);
	if(ret!=0)
	{
		printf("Failed to get region from cfgu: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	if(region>=7)
	{
		printf("Region value from cfgu is invalid: 0x%02x.\n", (unsigned int)region);
		ret = -9;
		return ret;
	}
	cfguExit();

	APT_CheckNew3DS(&new3dsflag);

	snprintf(payload_sdpath, sizeof(payload_sdpath)-1, "sdmc:/Cia_shortcut/Gateway_cia.bin", payload_sysver);

	memset(filebuffer, 0, filebuffer_maxsize);

	printf("Loading payload on SD, with the following filepath: \n", payload_sdpath);
	ret = loadsd_payload(payload_sdpath, &payloadsize);

	if(ret==0)
	{
		payload_src = 0;
	}
	else
	{
		printf("Can't find \n");
	}

	printf("Initializing payload data etc...\n");

	payloadsize_aligned = (payloadsize + 0xfff) & ~0xfff;
	if(payloadsize_aligned > PAYLOAD_TEXTMAXSIZE)
	{
		printf("Invalid payload size: 0x%08x.\n", (unsigned int)payloadsize);
		ret = -3;
		return ret;
	}

	memcpy(PAYLOAD_TEXTADDR, filebuffer, payloadsize_aligned);
	memset(filebuffer, 0, filebuffer_maxsize);

	ret = svcFlushProcessDataCache(0xffff8001, PAYLOAD_TEXTADDR, payloadsize_aligned);//Flush dcache for the payload which was copied into .text. Since that area was never executed, icache shouldn't be an issue.
	if(ret!=0)
	{
		printf("svcFlushProcessDataCache failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	paramblk = linearMemAlign(0x10000, 0x1000);
	if(paramblk==NULL)
	{
		ret = 0xfe;
		printf("Failed to alloc the paramblk.\n");
		return ret;
	}

		memset(paramblk, 0, 0x10000);

		paramblk[0x1c>>2] = (u32)gxlowcmd_4;
		paramblk[0x20>>2] = (u32)gsp_flushdcache;
		paramblk[0x48>>2] = 0x8d;//flags
		paramblk[0x58>>2] = (u32)&gspGpuHandle;

	printf("Jumping into the payload...\n");

	funcptr = (void*)PAYLOAD_TEXTADDR;
	funcptr(paramblk, (u32*)(0x10000000-0x1000));

	ret = 0xff;
	printf("The payload returned back into the app.\n");

	return ret;
}

int main(int argc, char **argv)
{
	Result ret = 0;
	u32 pos;
	Handle kproc_handledup=0;

	// Initialize services
	gfxInitDefault();

	consoleInit(GFX_BOTTOM, NULL);

	printf("Gateway Cia\n", VERSION);

	ret = svcDuplicateHandle(&kproc_handledup, 0xffff8001);
	if(ret!=0)printf("svcDuplicateHandle() with the current proc-handle failed: 0x%08x.\n", (unsigned int)ret);

	if(ret==0)
	{
		for(pos=0; pos<PAYLOAD_TEXTMAXSIZE; pos+=0x1000)
		{
			ret = svcControlProcessMemory(kproc_handledup, (u32)&PAYLOAD_TEXTADDR[pos >> 2], 0x0, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
			if(ret!=0)
			{
				printf("svcControlProcessMemory with pos=0x%x failed: 0x%08x.\n", (unsigned int)pos, (unsigned int)ret);
				break;
			}
		}
	}

	if(ret==0)
	{
		filebuffer_maxsize = PAYLOAD_TEXTMAXSIZE;

		filebuffer = (u8*)malloc(filebuffer_maxsize);
		if(filebuffer==NULL)
		{
			printf("Failed to allocate memory.\n");
			ret = -1;
		}
		else
		{
			memset(filebuffer, 0, filebuffer_maxsize);
		}
	}

	ret = load_gateway();

	free(filebuffer);

	if(ret!=0 && ret!=0xd8a0a046)printf("An error occured\n");

	printf("Press the START button to exit.\n");
	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break;
	}

	// Exit services
	gfxExit();
	return 0;
}
