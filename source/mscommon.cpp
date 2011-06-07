#include "mscommon.h"

static int audioInitCell(void);

volatile bool s_receivedExitGameRequest;

#ifndef MS_THREADED_SAMPLE

// SPURS information
#define				SPURS_SPU_NUM	1
#define				SPU_THREAD_GROUP_PRIORITY		128
CellSpurs			spurs __attribute__((aligned (128)));
#endif

sys_ppu_thread_t     s_MultiStreamPuThread   = 0;
void *               s_pMultiStreamMemory    = NULL;


#define   CHANNEL   CELL_AUDIO_PORT_8CH
#define   BLOCK     CELL_AUDIO_BLOCK_8

/**********************************************************************************/
// MultiStream thread defines
/**********************************************************************************/
#define STACK_SIZE              (0x4000) // 16 kb
#define EXIT_CODE               (0xbee)
#define SERVER_PRIO             (128)


/**********************************************************************************

audioInitCell
	Initialises low level video and audio.
	This is not MultiStream specific.

	Returns: audio port number returned from cellAudioPortOpen(..)

**********************************************************************************/
static int audioInitCell(void)
{
	s_receivedExitGameRequest = false;
	int ret = 0;
	unsigned int portNum = -1;


	//	cellMSSystemConfigureSysUtil returns the following data:
	//	Bits 0-3:	Number of available output channels
	//	Bit    4:	Dolby On status
	//	unsigned int retSysUtil = cellMSSystemConfigureSysUtil();
	//	unsigned int numChans = (retSysUtil & 0x0000000F);
	//	unsigned int dolbyOn = (retSysUtil & 0x00000010) >> 4;
	//	printf("Number Of Channels: %u\n", numChans);
	//	printf("Dolby enabled: %u\n", dolbyOn);

	ret = cellAudioInit();
	if (ret !=CELL_OK)	return -1;

	// audio port open.
	audioParam.nChannel = CHANNEL;
	audioParam.nBlock   = BLOCK;

	ret = cellAudioPortOpen(&audioParam, &portNum);
	if (ret != CELL_OK)
	{
		cellAudioQuit();
		return -1;
	}
	 
	// get port config.
	ret = cellAudioGetPortConfig(portNum, &portConfig);
	if (ret != CELL_OK)
	{
		cellAudioQuit();
		return -1;
	}

	cellMSSystemConfigureLibAudio(&audioParam, &portConfig);

	return portNum;
}


/**********************************************************************************

InitialiseAudio

	This function sets up the audio system.

	Requires:	nStreams		Maximum number of streams to be active at any time
				nmaxSubs		Maximum number of sub channels to init in MultiStream
				_nPortNumber	Reference to int - Returns port number from CELL audio init
				_audioParam		Reference to CellAudioPortParam - Returns audio params from CELL audio init
				_portConfig		Reference to CellAudioPortConfig - Returns port configuration from CELL audio init

	Returns:	0	OK
				-1	Error


**********************************************************************************/
long InitialiseAudio( const long nStreams, const long nmaxSubs, int &_nPortNumber , CellAudioPortParam &_audioParam, CellAudioPortConfig &_portConfig)
{
	CellMSSystemConfig cfg;

#ifndef MS_THREADED_SAMPLE
	uint8_t prios[8] = {1, 0, 0, 0, 0, 0, 0, 0};
#endif


	cfg.channelCount=nStreams;
	cfg.subCount=nmaxSubs;
	cfg.dspPageCount=2;//5;
	cfg.flags=0;//3;//CELL_MS_STREAM_AUTOCLOSE;//CELL_MS_ROUTABLE_STREAMS_FLAG;//CELL_MS_NOFLAGS;


    _nPortNumber = audioInitCell();
	if(_nPortNumber < 0)
	{
//		printf("InitialiseAudio: Failed to find valid port number!\n");
		return -1;
	}

	_audioParam = audioParam;
	_portConfig = portConfig;

	// Initialise MultiStream

	int nMemoryNeeded = cellMSSystemGetNeededMemorySize(&cfg);
	s_pMultiStreamMemory = memalign(128, nMemoryNeeded);

#ifndef MS_THREADED_SAMPLE
	InitSPURS();

	// Initialise SPURS MultiStream version
	cellMSSystemInitSPURS(s_pMultiStreamMemory, &cfg, &spurs, &prios[0]);
#else
	// Initialise Threaded MultiStream version (define MS_THREADED_SAMPLE in proprocessor flags)
	cellMSSystemInitSPUThread(s_pMultiStreamMemory, &cfg, 100);
#endif

    return 0;
}

/**********************************************************************************
ShutdownMultiStream

	This function closes the MultiStream system.
	All previously allocated memory is also free'd.
	Note that there is no need to stop any streams here. Just by not calling the
	update functions, no more data will be generated.

**********************************************************************************/
void ShutdownMultiStream()
{
	void* pMemory = cellMSSystemClose();
	assert(pMemory == s_pMultiStreamMemory);

	free( s_pMultiStreamMemory);

	s_pMultiStreamMemory = NULL;
	pMemory = NULL;
}


/**********************************************************************************
OpenFile

	This function opens a file.

	Requires:	pszFilename		Name of file to load
				pnSize			Pointer to pass back the size of data loaded
				nStartOffset	Offset in bytes to skip when reading file

	Returns:	>= 0			File handle
				-1				File not found
				-2				Seek fail
**********************************************************************************/
int OpenFile( const char* pszFilename, long* pnSize, int nStartOffset )
{
int ret;
int fd;
uint64_t	pos=0;

    ret = cellFsOpen (pszFilename, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (ret) return(-1);

	ret = cellFsLseek(fd,0,CELL_FS_SEEK_END, &pos);
	if (ret!=0)
	{
		cellFsClose (fd);
		return(-2);
	}

    *pnSize = (long )pos;

	ret = cellFsLseek(fd, nStartOffset ,CELL_FS_SEEK_SET,&pos);
	if (ret!=0)
	{
		cellFsClose (fd);
		return(-2);
	}

	return(fd);
}

/**********************************************************************************
LoadFile

	This function loads a file into memory.

	Requires:	fd				Handle of file to use
				ppData			Pointer to pass back where file will be loaded
				nReadSize	 	Size of data to load
				nStartOffset	Offset to the read start point from the file beginning
				nEndOffset		Offset from end of the file to stop reading

	Returns:	0				OK
				-1				File load failed
**********************************************************************************/
long LoadFile( const int fd, long ppData, long nReadSize, int nStartOffset, int nEndOffset)
{
	uint64_t nRead = 0;
	uint64_t pos=0;
	int ret;
	int loadSize;

	loadSize=nReadSize;			// Change this to allow file to be loaded in smaller chunks

	while(nReadSize!=0)
	{
		if (loadSize>nReadSize)
			loadSize=nReadSize;

		ret=cellFsRead(fd, (void*)ppData, loadSize, &nRead);
		if (ret!=0)
		{
			cellFsClose (fd);
			return(-1);
		}

	    if( (long)nRead != loadSize )		// Reached end of file and more data still required
	    {
			nRead-=nEndOffset;
			ret = cellFsLseek(fd,nStartOffset,CELL_FS_SEEK_SET,&pos);	// Seek back to start
			if (ret!=0)
			{
				cellFsClose (fd);
				return(-1);
			}
		}
		ppData+=nRead;
		nReadSize-=nRead;
    }
	return(0);
}

/**********************************************************************************
StartMultiStreamUpdateThread
	This function creates the thread to update multistream.
	Requires:
		_thread		Thread function to call to handle MS/libaudio buffer updates

**********************************************************************************/
long StartMultiStreamUpdateThread(void _thread (uint64_t param))
{
   
	// create the MultiStream / libaudio update thread
	int nRet = sys_ppu_thread_create(&s_MultiStreamPuThread, _thread, NULL, SERVER_PRIO, STACK_SIZE, SYS_PPU_THREAD_CREATE_JOINABLE, "MultiStream PU Thread");
	if(nRet)
	{
//		printf("ERROR creating Multistream update thread!!!\n");
		return -1;
	}
//	printf("Multistream thread (%d) created OK.\n", (int)s_MultiStreamPuThread);
    return 0;
}


/**********************************************************************************
InitSPURS
	Initialises SPURS so that SPURS version of MultiStream can then load
**********************************************************************************/
#ifndef MS_THREADED_SAMPLE

void InitSPURS(void)
{
	int ret = -1;
	sys_ppu_thread_t	thread_id;
	int					ppu_thr_prio __attribute__((aligned (128)));  // information for the reverb

	sys_ppu_thread_get_id(&thread_id);
	ret = sys_ppu_thread_get_priority(thread_id, &ppu_thr_prio);
	if(ret != CELL_OK)
	{
//		printf( " ERROR sys_ppu_thread_get_priority = 0x%x\n", ret ); while(1){};
	}
//	printf(" thread_id = %d, ppu_thr_prio = %d\n", (int)thread_id, ppu_thr_prio );
	ret = cellSpursInitialize(&spurs, SPURS_SPU_NUM, SPU_THREAD_GROUP_PRIORITY, ppu_thr_prio-1, 1);
	if(ret != CELL_OK)
	{
//		printf( "******** ERROR cellSpursInitialize = 0x%x\n", ret );
		while(1){};
	}
}

#endif


