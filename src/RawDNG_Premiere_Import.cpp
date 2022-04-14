

//////////////////////////////////////////////////////////////////////////////
// 
// Copyright (c) 2015, Brendan Bolles
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
// 
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
//////////////////////////////////////////////////////////////////////////////

//------------------------------------------
//
// OpenEXR_Premiere_Import.cpp
// 
// OpenEXR plug-in for Adobe Premiere
//
//------------------------------------------

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include "RawDNG_Premiere_Import.h"

#include "RawSpeed-API.h"
#include "rawspeedconfig.h"

//#include "amaze/rtengine/array2D.h"
#include "amaze/rtengine/amaze_demosaic_RT.h"

#include <string>
#include <map>

#include <stdio.h>
#include <assert.h>

#include <regex>

// Just for debugging:
#include <iostream>
#include <fstream>

#ifdef PRMAC_ENV
	#include <mach/mach.h>
#endif

#include <future>
#include <mutex>
#include <iomanip>
#include "common.h"

typedef  unsigned long ulong;

int rawspeed_get_number_of_processor_cores(){
	int numberOfCores = 1;
	if (numberOfCores <= 1)
	{
#ifdef PRMAC_ENV
		// get number of CPUs using Mach calls
		host_basic_info_data_t hostInfo;
		mach_msg_type_number_t infoCount;

		infoCount = HOST_BASIC_INFO_COUNT;
		host_info(mach_host_self(), HOST_BASIC_INFO,
			(host_info_t)&hostInfo, &infoCount);

		numberOfCores = hostInfo.max_cpus;
#else
		SYSTEM_INFO systemInfo;
		GetSystemInfo(&systemInfo);

		numberOfCores = systemInfo.dwNumberOfProcessors;
#endif
	}
	return numberOfCores;
}


using namespace std;


static  int callCounter = 0;

#ifdef DEBUGOUTPUT
std::ofstream abasc;
#else
std::ofstream nothing;
//#define abasc if(0) nothing
#endif
static bool inited = false;

#define BUFFER_TIMEOUT 30.0

regex pathregex(R"((.*?)(\d+)([^\d]*?\.[^.]*?$))", // prefix, number and suffix (including extension)
	regex_constants::icase | regex_constants::optimize | regex_constants::ECMAScript);

static class CalculatingFrame {
	const double maxValue = pow(2.0, 16.0) - 1;
	int width;
	int height;
	std::thread* worker;
	std::promise<bool> finishPromise;
	std::future<bool> finishFuture = finishPromise.get_future();
	bool finished;
	bool success;
	std::string path;
	std::chrono::system_clock::time_point startTime;
	float* outputBuffer;
	int outputBufferLength;
	int rowBytes;

	void doProcess() {

#ifdef DEBUGBLACK
		width = 3300;
		height = 3000; 
		rowBytes = width * sizeof(float) * 4;
		outputBufferLength = height * rowBytes / 4;
		outputBuffer = new float[outputBufferLength];

		int rowFloats = rowBytes / 4;
		for (size_t y = 0; y < height; y++) {
			for (size_t x = 0; x < width; x++) {
				outputBuffer[y * rowFloats + x * 4] = (float)y/(float)height + static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
				outputBuffer[y * rowFloats + x * 4 + 1] = (float)y / (float)height + static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
				outputBuffer[y * rowFloats + x * 4 + 2] = (float)y / (float)height + static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
				outputBuffer[y * rowFloats + x * 4 + 3] = 1.0f;
			}
		}

		finishPromise.set_value(true);

#else

		array2D<float>* red;
		array2D<float>* green;
		array2D<float>* blue;

		abasc << "before read file" << "\n";
		abasc.flush();

		// Check if file even exists
		if (FILE* file = fopen(path.c_str(), "r")) {
			fclose(file); // All good
		}
		else {
			abasc << "file doesnt exist, thats ok tho " << path << "\n";
			finishPromise.set_value(false);
			return; // Yeah no. Not even gonna try.
		}

		rawspeed::FileReader reader(path.c_str());
		const rawspeed::Buffer* map = NULL;
		try {
			map = reader.readFile().release();
		}
		catch (rawspeed::IOException& e) {
			abasc << "file read error gg " << path << "\n";
			// Handle errors
			finishPromise.set_value(false);
			return;
		}

		abasc << "before parse" << "\n";
		abasc.flush();

		rawspeed::RawParser parser(map);
		rawspeed::RawDecoder* decoder = parser.getDecoder().release();

		rawspeed::CameraMetaData* metadata = new rawspeed::CameraMetaData();

		//decoder->uncorrectedRawValues = true;
		decoder->decodeRaw();
		decoder->decodeMetaData(metadata);
		rawspeed::RawImage raw = decoder->mRaw;

		delete metadata;
		delete map;

		int components_per_pixel = raw->getCpp();
		rawspeed::RawImageType type = raw->getDataType();
		bool is_cfa = raw->isCFA;

		int dcraw_filter = 0;
		if (true == is_cfa) {
			rawspeed::ColorFilterArray cfa = raw->cfa;
			dcraw_filter = cfa.getDcrawFilter();
			//rawspeed::CFAColor c = cfa.getColorAt(0, 0);
		}
		abasc << "dcraw filter " << dcraw_filter << "\n";

		unsigned char* data = raw->getData(0, 0);
		int rawwidth = raw->dim.x;
		int rawheight = raw->dim.y;
		int pitch_in_bytes = raw->pitch;

		unsigned int bpp = raw->getBpp();

		width = rawwidth;
		height = rawheight;

		if (rowBytes == 0) {
			// Seemingly must be 64-byte aligned from observation. Ideally we pass this info in here straight from Premiere/AE
			// But if we don't have the info (like when called from GetInfo function), we assume 64-byte alignment.
			rowBytes = width * sizeof(float) * 4;
			if (rowBytes % 64) {
				rowBytes = (rowBytes / 64 + 1) * 64;
			}
		}

		uint16_t* dataAs16bit = reinterpret_cast<uint16_t*>(data);

		double maxValue = pow(2.0, 16.0) - 1;
		double tmp;

		abasc << "before rawimage" << "\n";
		abasc.flush();

		rtengine::RawImage* ri = new rtengine::RawImage();
		ri->filters = dcraw_filter;
		rtengine::RawImageSource* rawImageSource = new rtengine::RawImageSource();
		rawImageSource->ri = ri;


		array2D<float>* demosaicSrcData = new array2D<float>(width, height, 0U);
		red = new array2D<float>(width, height, 0U);
		green = new array2D<float>(width, height, 0U);
		blue = new array2D<float>(width, height, 0U);

#ifdef _OPENMP
	#pragma omp for
#endif
		for (int y = 0; y < height; y++) {
#ifdef _OPENMP
	#pragma omp simd
#endif
			for (size_t x = 0; x < width; x++) {
				//tmp = ;// maxValue;
				(*demosaicSrcData)[y][x] = dataAs16bit[y * pitch_in_bytes / 2 + x];
			}
		}

		delete decoder;

		rawImageSource->amaze_demosaic_RT(0, 0, width, height, *demosaicSrcData, *red, *green, *blue);

		delete ri;
		delete rawImageSource;
		demosaicSrcData->free();
		delete demosaicSrcData;

		abasc << "before deleting" << "\n";
		abasc.flush();

		outputBufferLength = height*rowBytes/4;
		outputBuffer = new float[outputBufferLength];

		int rowFloats = rowBytes / 4;
#ifdef _OPENMP
	#pragma omp for
#endif
		for (int y = 0; y < height; y++) {
			float* outputBufferHere = outputBuffer + y * rowFloats;
#ifdef _OPENMP
	#pragma omp simd
#endif
			for (int x = 0; x < width; x++) {
				outputBufferHere[x * 4] = (float)( (*blue)[y][x] / maxValue);
				outputBufferHere[x * 4 + 1] = (float)((*green)[y][x] / maxValue);
				outputBufferHere[x * 4 + 2] = (float)((*red)[y][x] / maxValue);
				outputBufferHere[x * 4 + 3] = 1.0f;
			}
		}

		red->free();
		green->free();
		blue->free();
		delete red;
		delete green;
		delete blue;

		finishPromise.set_value(true);
#endif
	}

	void waitForFinish() {
		if (!finished) {
			success = finishFuture.get();
			finished = true;

			abasc << "finished " << success << "\n";
			abasc.flush();
		}
	}
public:
	CalculatingFrame(std::string pathA,int rowBytesA) {
		path = pathA;
		success = false;
		finished = false;
		width = height = 0;
		worker = new std::thread(&CalculatingFrame::doProcess, this);

		rowBytes = rowBytesA ? rowBytesA : 0;

		startTime = std::chrono::system_clock::now();
	}
	void refreshStartTime() { // Call to avoid timeout while the image is still active.
		startTime = std::chrono::system_clock::now();
	}
	double getAgeInSeconds() {
		std::chrono::duration<double> diff = std::chrono::system_clock::now() - startTime;
		return diff.count();
	}
	int getWidth() {
		waitForFinish();
		return width;
	}
	int getHeight() {
		waitForFinish();
		return height;
	}
	void getFrame(float* outputBufferOut,int rowBytesA) {
		waitForFinish();
		if (success) {
			if (rowBytes == rowBytesA) {
				memcpy(outputBufferOut, outputBuffer, outputBufferLength * sizeof(float));
			}
			else {
				abasc << "converting from " << rowBytes << " to " << rowBytesA << " rowbytes" << "\n";
				abasc.flush();
				// This shouldn't happen, but just in case it does, we convert to a different
				// rowbytes (stride) amount.
				int rowFloats = rowBytes / 4;
				int rowFloatsA = rowBytesA / 4;
#ifdef _OPENMP
	#pragma omp for
#endif
				for (int y = 0; y < height; y++) {
					float* outputBufferOutHere = outputBufferOut + y * rowFloatsA;
					float* outputBufferHere = outputBuffer + y * rowFloats;
#ifdef _OPENMP
#pragma omp simd
#endif
					for (int x = 0; x < width; x++) {
						outputBufferOutHere[x * 4] = outputBufferHere[x * 4];
						outputBufferOutHere[x * 4 + 1] = outputBufferHere[x * 4 + 1];
						outputBufferOutHere[x * 4 + 2] = outputBufferHere[x * 4 + 2];
						outputBufferOutHere[x * 4 + 3] = outputBufferHere[x * 4 + 3];
					}
				}
			}
		}
	}
	~CalculatingFrame() {
		abasc << "before delete worker " << success << "\n";
		abasc.flush();
		waitForFinish();
		worker->join();
		delete worker;
		if (success) {
			abasc << "before delete red green blue " << success << "\n";
			abasc.flush();
			delete[] outputBuffer;
			abasc << "after delete red green blue " << success << "\n";
			abasc.flush();
		}
	}
};



typedef std::map<int, CalculatingFrame> bufferCollection_t;
typedef std::map<int, CalculatingFrame>::iterator bufferCollectionIterator;

static std::map<std::string, bufferCollection_t> bufferCollection;
typedef std::map<std::string, bufferCollection_t>::iterator globalBufferCollectionIterator;
std::mutex bufferCollectionMutex;


static const csSDK_int32 RawDNG_ID = 'rDNG';

//extern unsigned int gNumCPUs;
unsigned int gNumCPUs = 1;


typedef struct
{	
	csSDK_int32				width;
	csSDK_int32				height;
	csSDK_int32				importerID;
	PlugMemoryFuncsPtr		memFuncs;
	SPBasicSuite			*BasicSuite;
	PrSDKPPixCreatorSuite	*PPixCreatorSuite;
#ifdef PREMIERE_CACHE_NOT_CLEARING
	PrSDKPPixCacheSuite		*PPixCacheSuite;
#endif
	PrSDKPPixSuite			*PPixSuite;
	PrSDKTimeSuite			*TimeSuite;
	csSDK_int32				callCounter;
} ImporterLocalRec8, *ImporterLocalRec8Ptr, **ImporterLocalRec8H;


typedef struct
{
	char			magic[4];
} ImporterPrefs, *ImporterPrefsPtr, **ImporterPrefsH;


static prMALError 
SDKInit(
	imStdParms		*stdParms, 
	imImportInfoRec *importInfo)
{


	abasc.open("C:/tmptest/blah.txt", std::ios::out | std::ios::app);
	inited = true;

	PrSDKAppInfoSuite *appInfoSuite = NULL;
	stdParms->piSuites->utilFuncs->getSPBasicSuite()->AcquireSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion, (const void**)&appInfoSuite);
	
	if(appInfoSuite)
	{
		int fourCC = 0;
	
		appInfoSuite->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, (void *)&fourCC);
	
		stdParms->piSuites->utilFuncs->getSPBasicSuite()->ReleaseSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion);
		
		//if(fourCC == kAppAfterEffects)
		//	return imOtherErr;
	}


	importInfo->importerType		= RawDNG_ID;

	importInfo->canSave				= kPrFalse;		// Can 'save as' files to disk, real file only
	
	// imDeleteFile8 is broken on MacOS when renaming a file using the Save Captured Files dialog
	// So it is not recommended to set this on MacOS yet (bug 1627325)
#ifdef PRWIN_ENV
	importInfo->canDelete			= kPrFalse;		// File importers only, use if you only if you have child files
#endif
	
	importInfo->canResize			= kPrFalse;
	importInfo->canDoSubsize		= kPrFalse;
	
	
	importInfo->dontCache			= kPrFalse;		// Don't let Premiere cache these files
	importInfo->hasSetup			= kPrTrue;		// Set to kPrTrue if you have a setup dialog
	importInfo->setupOnDblClk		= kPrFalse;		// If user dbl-clicks file you imported, pop your setup dialog
	importInfo->keepLoaded			= kPrTrue;		// If you MUST stay loaded use, otherwise don't: play nice
	importInfo->priority			= 10;			// Original ProEXR plug-in had a priority of 0
	importInfo->canTrim				= kPrFalse;
	importInfo->canCalcSizes		= kPrFalse;
	

	if(gNumCPUs <= 1)
	{
	#ifdef PRMAC_ENV
		// get number of CPUs using Mach calls
		host_basic_info_data_t hostInfo;
		mach_msg_type_number_t infoCount;
		
		infoCount = HOST_BASIC_INFO_COUNT;
		host_info(mach_host_self(), HOST_BASIC_INFO, 
				  (host_info_t)&hostInfo, &infoCount);
		
		gNumCPUs = hostInfo.max_cpus;
	#else
		SYSTEM_INFO systemInfo;
		GetSystemInfo(&systemInfo);

		gNumCPUs = systemInfo.dwNumberOfProcessors;
	#endif
	}
	
	
	return malNoError;
}


static prMALError
SDKShutdown(imStdParms* stdParms)
{
	// anything you want to close before Premiere quits
	bufferCollection.clear();
	return malNoError;
}


static prMALError 
SDKGetIndFormat(
	imStdParms		*stdParms, 
	csSDK_size_t	index, 
	imIndFormatRec	*SDKIndFormatRec)
{
	prMALError	result		= malNoError;
	

	switch(index)
	{
		//	Add a case for each filetype.
		
		case 0:
			do{
				char formatname[255]	= "RawDNG Importer";
				char shortname[32]		= "RawDNG";
				char platformXten[256]	= "dng\0\0";

				SDKIndFormatRec->filetype			= RawDNG_ID;

				SDKIndFormatRec->canWriteTimecode	= kPrTrue;
				SDKIndFormatRec->canWriteMetaData	= kPrFalse;

				SDKIndFormatRec->flags = xfIsStill | xfCanImport; // not used supposedly, but actually enables the numbered stills checkbox

			#ifdef PRWIN_ENV
				strcpy_s(SDKIndFormatRec->FormatName, sizeof (SDKIndFormatRec->FormatName), formatname);				// The long name of the importer
				strcpy_s(SDKIndFormatRec->FormatShortName, sizeof (SDKIndFormatRec->FormatShortName), shortname);		// The short (menu name) of the importer
			#else
				strcpy(SDKIndFormatRec->FormatName, formatname);			// The Long name of the importer
				strcpy(SDKIndFormatRec->FormatShortName, shortname);		// The short (menu name) of the importer
			#endif

				memcpy(SDKIndFormatRec->PlatformExtension, platformXten, 13);

			}while(0);
			
			break;

		default:
			result = imBadFormatIndex;
	}

	return result;
}



prMALError 
SDKOpenFile8(
	imStdParms		*stdParms, 
	imFileRef		*SDKfileRef, 
	imFileOpenRec8	*SDKfileOpenRec8)
{
	prMALError			result = malNoError;
	ImporterLocalRec8H	localRecH;


	if(SDKfileOpenRec8->privatedata)
	{
		localRecH = (ImporterLocalRec8H)SDKfileOpenRec8->privatedata;
	}
	else
	{
		localRecH = (ImporterLocalRec8H)stdParms->piSuites->memFuncs->newHandle(sizeof(ImporterLocalRec8));
		//(*localRecH)->bufferCollection = new bufferCollection();
		//(*localRecH)->bufferCollectionMutex = new mutex();
		SDKfileOpenRec8->privatedata = (PrivateDataPtr)localRecH;
	}
	

#ifdef PRWIN_ENV

	DWORD shareMode;

	if(SDKfileOpenRec8->inReadWrite == kPrOpenFileAccess_ReadOnly)
	{
		shareMode = GENERIC_READ;
	}
	else if(SDKfileOpenRec8->inReadWrite == kPrOpenFileAccess_ReadWrite)
	{
		shareMode = GENERIC_WRITE;
	}
	
	imFileRef fileRef = CreateFileW(SDKfileOpenRec8->fileinfo.filepath,
									shareMode,
									FILE_SHARE_READ,
									NULL,
									OPEN_EXISTING,
									FILE_ATTRIBUTE_NORMAL,
									NULL);
							
	if(fileRef == imInvalidHandleValue/* || !isEXR(fileRef)*/)
	{
		if(fileRef != imInvalidHandleValue)
			CloseHandle(fileRef);
		
		//(*localRecH)->bufferCollection->clear();
		//delete (*localRecH)->bufferCollection;
		//delete (*localRecH)->bufferCollectionMutex;
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<PrMemoryHandle>(SDKfileOpenRec8->privatedata));
		
		result = imBadFile;
	}
	else
	{
		SDKfileOpenRec8->fileinfo.fileref = *SDKfileRef = fileRef;
		SDKfileOpenRec8->fileinfo.filetype = RawDNG_ID;
	}
	
#else

	SInt8 filePermissions;	

	if(SDKfileOpenRec8->inReadWrite == kPrOpenFileAccess_ReadOnly)
	{
		filePermissions = fsRdPerm;
	}
	else if(SDKfileOpenRec8->inReadWrite == kPrOpenFileAccess_ReadWrite)
	{
		filePermissions = fsRdWrPerm;
	}
	
	CFStringRef filePathCFSR = CFStringCreateWithCharacters(NULL,
												SDKfileOpenRec8->fileinfo.filepath,
												prUTF16CharLength(SDKfileOpenRec8->fileinfo.filepath));
												
	CFURLRef filePathURL = CFURLCreateWithFileSystemPath(NULL, filePathCFSR, kCFURLPOSIXPathStyle, false);
	
	FSRef fileRef;
	CFURLGetFSRef(filePathURL, &fileRef);
	
	HFSUniStr255 dataForkName;	
	FSGetDataForkName(&dataForkName);
	
	
	FSIORefNum refNum;
	OSErr err = FSOpenFork( &fileRef,
							dataForkName.length,
							dataForkName.unicode,
							filePermissions,
							&refNum);
							
							
	CFRelease(filePathURL);
	CFRelease(filePathCFSR);
	
	if(err)
	{
		if(!err)
			FSCloseFork(refNum);

		//(*localRecH)->bufferCollection->clear();
		//delete (*localRecH)->bufferCollection;
		//delete (*localRecH)->bufferCollectionMutex;
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<PrMemoryHandle>(SDKfileOpenRec8->privatedata));
		
		result = imBadFile;
	}
	else
	{
		SDKfileOpenRec8->fileinfo.fileref = *SDKfileRef = CAST_FILEREF(refNum);
		SDKfileOpenRec8->fileinfo.filetype = RawDNG_ID;
	}
	
#endif

	return result;
}


static prMALError 
SDKQuietFile(
	imStdParms			*stdParms, 
	imFileRef			*SDKfileRef, 
	void				*privateData)
{
	// If file has not yet been closed
	if(SDKfileRef && *SDKfileRef != imInvalidHandleValue)
	{
	#ifdef PRWIN_ENV
		CloseHandle(*SDKfileRef);
		*SDKfileRef = imInvalidHandleValue;
	#else
		FSCloseFork(CAST_REFNUM(*SDKfileRef));
		*SDKfileRef = imInvalidHandleValue;
	#endif
	}

	return malNoError; 
}


static prMALError 
SDKCloseFile(
	imStdParms			*stdParms, 
	imFileRef			*SDKfileRef,
	void				*privateData) 
{
	ImporterLocalRec8H ldataH	= reinterpret_cast<ImporterLocalRec8H>(privateData);
	ImporterLocalRec8Ptr ldataP = *ldataH;

	// If file has not yet been closed
	if (SDKfileRef && *SDKfileRef != imInvalidHandleValue)
	{
		SDKQuietFile(stdParms, SDKfileRef, privateData);
	}

	// Remove the privateData handle.
	// CLEANUP - Destroy the handle we created to avoid memory leaks
	if (ldataH && ldataP && ldataP->BasicSuite)
	{
		ldataP->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
	#ifdef PREMIERE_CACHE_NOT_CLEARING
		ldataP->BasicSuite->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion);
	#endif
		ldataP->BasicSuite->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
		ldataP->BasicSuite->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
		//ldataP->bufferCollection->clear();
		//delete ldataP->bufferCollection;
		//delete ldataP->bufferCollectionMutex;
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(ldataH));
	}

	return malNoError;
}


// Go ahead and overwrite any existing file. Premiere will have already checked and warned the user if file will be overwritten.
// Of course, if there are child files, you should check and return imSaveErr if appropriate
static prMALError 
SDKSaveFile8(
	imStdParms			*stdParms, 
	imSaveFileRec8		*SDKSaveFileRec8) 
{
	prMALError	result = malNoError;
	
#ifdef PRMAC_ENV
	CFStringRef			sourceFilePathCFSR,
						destFilePathCFSR,
						destFolderCFSR,
						destFileNameCFSR;
	CFRange				destFileNameRange,
						destFolderRange;
	CFURLRef			sourceFilePathURL,
						destFolderURL;
	FSRef				sourceFileRef,
						destFolderRef;
												
	// Convert prUTF16Char filePaths to FSRefs for paths
	sourceFilePathCFSR = CFStringCreateWithCharacters(	kCFAllocatorDefault,
														SDKSaveFileRec8->sourcePath,
														prUTF16CharLength(SDKSaveFileRec8->sourcePath));
	destFilePathCFSR = CFStringCreateWithCharacters(	kCFAllocatorDefault,
														SDKSaveFileRec8->destPath,
														prUTF16CharLength(SDKSaveFileRec8->destPath));
														
	// Separate the folder path from the file name
	destFileNameRange = CFStringFind(	destFilePathCFSR,
										CFSTR("/"),
										kCFCompareBackwards);
	destFolderRange.location = 0;
	destFolderRange.length = destFileNameRange.location;
	destFileNameRange.location += destFileNameRange.length;
	destFileNameRange.length = CFStringGetLength(destFilePathCFSR) - destFileNameRange.location;
	destFolderCFSR = CFStringCreateWithSubstring(	kCFAllocatorDefault,
													destFilePathCFSR,
													destFolderRange);
	destFileNameCFSR = CFStringCreateWithSubstring( kCFAllocatorDefault,
													destFilePathCFSR,
													destFileNameRange);
		
	// Make FSRefs
	sourceFilePathURL = CFURLCreateWithFileSystemPath(	kCFAllocatorDefault,
														sourceFilePathCFSR,
														kCFURLPOSIXPathStyle,
														false);
	destFolderURL = CFURLCreateWithFileSystemPath(	kCFAllocatorDefault,
													destFolderCFSR,
													kCFURLPOSIXPathStyle,
													true);
	CFURLGetFSRef (sourceFilePathURL, &sourceFileRef);
	CFURLGetFSRef (destFolderURL, &destFolderRef);						

	if(SDKSaveFileRec8->move)
	{
		if( FSCopyObjectSync(	&sourceFileRef,
								&destFolderRef,
								destFileNameCFSR,
								NULL,
								kFSFileOperationOverwrite))
		{
			result = imSaveErr;
		}
	}
	else
	{
		if( FSMoveObjectSync(	&sourceFileRef,
								&destFolderRef,
								destFileNameCFSR,
								NULL,
								kFSFileOperationOverwrite) )
		{
			result = imSaveErr;
		}
	}


#else

	// gotta admit, this is a lot easier on Windows

	if(SDKSaveFileRec8->move)
	{
		if( MoveFileW(SDKSaveFileRec8->sourcePath, SDKSaveFileRec8->destPath) == 0 )
		{
			result = imSaveErr;
		}
	}
	else
	{
		if( CopyFileW(SDKSaveFileRec8->sourcePath, SDKSaveFileRec8->destPath, kPrTrue) == 0 )
		{
			result = imSaveErr;
		}
	}
	
#endif
	
	return result;
}


static prMALError 
SDKDeleteFile8(
	imStdParms			*stdParms, 
	imDeleteFileRec8	*SDKDeleteFileRec8)
{
	prMALError	result = malNoError;

#ifdef PRWIN_ENV
	if( DeleteFileW(SDKDeleteFileRec8->deleteFilePath) )
	{
		result = imDeleteErr;
	}
#else
	CFStringRef filePathCFSR;
	CFURLRef	filePathURL;
	FSRef		fileRef;

	filePathCFSR = CFStringCreateWithCharacters(kCFAllocatorDefault,
												SDKDeleteFileRec8->deleteFilePath,
												prUTF16CharLength(SDKDeleteFileRec8->deleteFilePath));
	filePathURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
												filePathCFSR,
												kCFURLPOSIXPathStyle,
												false);
	CFURLGetFSRef(filePathURL, &fileRef);
					
	if( FSDeleteObject(&fileRef) )
	{
		result = imDeleteErr;
	}
#endif
	
	return result;
}


static void
InitPrefs(ImporterPrefs *prefs)
{
	memset(prefs, 0, sizeof(ImporterPrefs));

	strcpy(prefs->magic, "rDNG");
}


static prMALError 
SDKGetPrefs8(
	imStdParms			*stdParms, 
	imFileAccessRec8	*fileInfo8, 
	imGetPrefsRec		*prefsRec)
{
	prMALError result = malNoError;
	
	// For a single frame (not a sequence) Premiere will call imGetPrefs8 twice as described
	// in the SDK guide, right as the file is imported.  We don't want a dialog to pop up right then
	// so we don't actually pop the dialog the second time, when prefsRec->firstTime == TRUE.
	//
	// For an image sequence imGetPrefs8 is not called immediately, so we have to create the prefs
	// in imGetInfo8.  See that call for more information.  imGetPrefs8 gets called when the user
	// does a Source Settings.
	
	if(prefsRec->prefsLength == 0)
	{
		prefsRec->prefsLength = sizeof(ImporterPrefs);
	}
	else
	{
		assert(prefsRec->prefsLength == sizeof(ImporterPrefs));
	
	#ifdef PRMAC_ENV
		const char *plugHndl = "com.adobe.PremierePro.OpenEXR";
		const void *mwnd = NULL;
	#else
		const char *plugHndl = NULL;
		const void *mwnd = NULL;

		PrSDKWindowSuite *windowSuite = NULL;
		stdParms->piSuites->utilFuncs->getSPBasicSuite()->AcquireSuite(kPrSDKWindowSuite, kPrSDKWindowSuiteVersion, (const void**)&windowSuite);
		
		if(windowSuite)
		{
			mwnd = windowSuite->GetMainWindow();
			stdParms->piSuites->utilFuncs->getSPBasicSuite()->ReleaseSuite(kPrSDKWindowSuite, kPrSDKWindowSuiteVersion);
		}
	#endif
		
		ImporterPrefsPtr prefs = reinterpret_cast<ImporterPrefsPtr>(prefsRec->prefs);

		if(prefs)
		{
			try
			{
				if(prefsRec->firstTime)
				{
					InitPrefs(prefs);
				}
				else
				{
					if(fileInfo8->fileref != imInvalidHandleValue)
					{
						// already have an open file handle
					}
					else
					{
						// need to re-opne fileInfo8->filepath
					}
					
					// pop a dialog for user to set prefs
				}
			}
			catch(...)
			{
				result = imFileReadFailed;
			}
		}
	}

	return result;
}


static prMALError 
SDKGetIndPixelFormat(
	imStdParms			*stdParms,
	csSDK_size_t		idx,
	imIndPixelFormatRec *SDKIndPixelFormatRec) 
{
	prMALError result = malNoError;

	switch(idx)
	{
		case 0:
			//SDKIndPixelFormatRec->outPixelFormat = PrPixelFormat_BGRA_4444_32f_Linear;
			SDKIndPixelFormatRec->outPixelFormat = PrPixelFormat_BGRA_4444_32f;
			break;
	
		default:
			result = imBadFormatIndex;
			break;
	}

	return result;	
}


static prMALError 
SDKAnalysis(
	imStdParms		*stdParms,
	imFileRef		SDKfileRef,
	imAnalysisRec	*SDKAnalysisRec)
{
	ImporterPrefs *prefs = reinterpret_cast<ImporterPrefs *>(SDKAnalysisRec->prefs);

	try
	{
		string info = "Some sort of file information goes here";
		
		assert(prefs != NULL); // should have this under control now
		
		if(info.size() > SDKAnalysisRec->buffersize - 1)
		{
			info.resize(SDKAnalysisRec->buffersize - 4);
			info += "...";
		}
		
		if(SDKAnalysisRec->buffer != NULL && SDKAnalysisRec->buffersize > 3)
		{
			strcpy(SDKAnalysisRec->buffer, info.c_str());
		}
	}
	catch(...)
	{
		return imBadFile;
	}

	return malNoError;
}


void ReadFileWrap(HANDLE fileRef, void* fileBuffer, unsigned long fileSize, std::ofstream* abasc4) {
	__try {
		uint32_t bytesRead;
		ReadFile(fileRef, (void*)fileBuffer, fileSize, (LPDWORD)&bytesRead, NULL);
	}
	__except (1) {

		*abasc4 << "ERROR reading file: " << GetExceptionCode() << "\n";
	}
}


static CalculatingFrame* getCalculatingFrame(std::string pathString, bool* mustDelete,int rowBytes = 0) {

	CalculatingFrame* calcFrame = 0;
	// Caching stuff
	abasc << "test!24y354yu534th\n";
	abasc.flush();
	if (regex_search(pathString, pathregex)) {
		abasc << "regex found!";
		abasc.flush();
		auto words_begin =
			std::sregex_iterator(pathString.begin(), pathString.end(), pathregex);
		auto words_end = std::sregex_iterator();
		if (words_begin != words_end && (*words_begin).size() >= 4) {// Found base, number and extension. 

			std::smatch match = *words_begin;
			*mustDelete = false;
			abasc << match[1].str() << " ### " << match[2].str() << " ### " << match[3].str() << "\n";
			abasc.flush();

			// Attempt some caching.
			// We want to calculate one frame for each cpu core.
			int numToCache = gNumCPUs - 1; // Single CPU would get no caching. 2 core would get 1 in advance, 4 core would get 3 in advance etc.
			std::string base = match[1].str();
			std::string number = match[2].str();
			std::string extension = match[3].str();

			std::stringstream key;
			key << base << extension;
			std::string keyString = key.str();

			int numInt = atoi(number.c_str());

			std::lock_guard<std::mutex> lock(bufferCollectionMutex); // Lock the list so if there's ever any multiprocessing done by AE, we reduce chance of conflict


			abasc << "Key string: " << keyString << "\n";
			abasc.flush();

			bufferCollection_t* bufferCollectionHere = &bufferCollection[keyString];

			abasc << "Buffer collection lenght before: " << bufferCollectionHere->size() << "\n";
			abasc.flush();

			// Create formatting string for numbers for new files.
			int numberLength = number.size();

			for (int i = 0; i <= numToCache; i++) {
				// First check if its already being calculated/if an entry exists
				bufferCollectionIterator findResult = bufferCollectionHere->find(numInt + i);
				if (findResult == bufferCollectionHere->end()) {
					// Doesn't exist yet. Let's do this.
					std::stringstream assumedAheadFilename;
					assumedAheadFilename << base << std::setfill('0') << std::setw(numberLength) << (numInt + i) << extension;
					abasc << "Attempting to buffer " << assumedAheadFilename.str() << "\n";
					abasc.flush();
					//bufferCollectionHere->emplace(numInt + i, assumedAheadFilename.str(), rowBytes);
					bufferCollectionHere->emplace(std::piecewise_construct, std::make_tuple(numInt + i), std::make_tuple(assumedAheadFilename.str(), rowBytes));
				}
				else {
					findResult->second.refreshStartTime();
					abasc << (numInt + i) << "already buffered, refreshing" << "\n";
					abasc.flush();
				}

			}

			abasc << "clearing stuff now " << "\n";
			abasc.flush();

			// Now iterate through the entire buffer collection and delete any elements that are too old/too far ahead/not needed etc.
			for (globalBufferCollectionIterator itOuter = bufferCollection.begin(); itOuter != bufferCollection.end(); itOuter++) {
				bufferCollection_t* bufferCollectionLocal = &itOuter->second;
				for (bufferCollectionIterator it = bufferCollectionLocal->begin(); it != bufferCollectionLocal->end();) {
					bufferCollectionIterator tmpIt = it;
					it++;
					// How we manage deleting stuff of this particular file we are handling rn
					if (bufferCollectionLocal == bufferCollectionHere && tmpIt->first < numInt || tmpIt->first > numInt + numToCache) { // Too far behind or too far ahead. Get rid of it to free up memory.
						abasc << "Deleting here buffer item " << tmpIt->first << "\n";
						abasc.flush();
						bufferCollectionLocal->erase(tmpIt);
					}
					else if (bufferCollectionLocal != bufferCollectionHere && tmpIt->second.getAgeInSeconds() > BUFFER_TIMEOUT) {
						abasc << "Deleting buffer item (too old) " << tmpIt->first << "\n";
						abasc.flush();
						bufferCollectionLocal->erase(tmpIt);
					}
				}
			}

			// Save current frame CalculatingFrame reference into calcFrame pointer.
			abasc << "Before referencing current: " << bufferCollectionHere->size() << "\n";
			calcFrame = &bufferCollectionHere->find(numInt)->second; // Wow this looks ugly haha. Pretty simple tho really. Just getting a pointer to that specific element in the map.

			abasc << "Buffer collection lenght after: " << bufferCollectionHere->size() << "\n";
			abasc.flush();
		}
		else {
			*mustDelete = true;// Since we are not dealing with a sequence (or can't detect its logic), we won't be doing any caching or persisting across multiple frames, so the CalculatingFrame must be deleted after calculation in this function call.

			calcFrame = new CalculatingFrame(pathString, rowBytes);
		}
	}
	else {
		*mustDelete = true;// Since we are not dealing with a sequence (or can't detect its logic), we won't be doing any caching or persisting across multiple frames, so the CalculatingFrame must be deleted after calculation in this function call.

		calcFrame = new CalculatingFrame(pathString, rowBytes);
	}

	
	return calcFrame;
}


prMALError 
SDKGetInfo8(
	imStdParms			*stdParms, 
	imFileAccessRec8	*fileAccessInfo8, 
	imFileInfoRec8		*SDKFileInfo8)
{
	prMALError					result				= malNoError;
	ImporterLocalRec8H			ldataH				= NULL;
	
	// Get a handle to our private data.  If it doesn't exist, allocate one
	// so we can use it to store our file instance info
	if(SDKFileInfo8->privatedata)
	{
		ldataH = reinterpret_cast<ImporterLocalRec8H>(SDKFileInfo8->privatedata);
	}
	else
	{
		ldataH						= reinterpret_cast<ImporterLocalRec8H>(stdParms->piSuites->memFuncs->newHandle(sizeof(ImporterLocalRec8)));
		//(*ldataH)->bufferCollection = new bufferCollection();
		//(*ldataH)->bufferCollectionMutex = new mutex();
		SDKFileInfo8->privatedata	= reinterpret_cast<PrivateDataPtr>(ldataH);
	}
	
	ImporterLocalRec8Ptr ldataP = *ldataH;

	// Either way, lock it in memory so it doesn't move while we modify it.
	stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));

	// Acquire needed suites
	ldataP->memFuncs = stdParms->piSuites->memFuncs;
	ldataP->BasicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
	if(ldataP->BasicSuite)
	{
		ldataP->BasicSuite->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion, (const void**)&ldataP->PPixCreatorSuite);
	#ifdef PREMIERE_CACHE_NOT_CLEARING
		ldataP->BasicSuite->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion, (const void**)&ldataP->PPixCacheSuite);
	#endif
		ldataP->BasicSuite->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, (const void**)&ldataP->PPixSuite);
		ldataP->BasicSuite->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion, (const void**)&ldataP->TimeSuite);
	}


	try
	{
		unsigned long fileSize = GetFileSize(fileAccessInfo8->fileref, NULL);

		TCHAR filename[MAX_PATH + 1];
		ulong pathLength = GetFinalPathNameByHandleA(fileAccessInfo8->fileref, filename, MAX_PATH + 1, 0);

		std::string pathString = filename;

		CalculatingFrame* calcFrame;
		bool mustDelete = false;
		calcFrame = getCalculatingFrame(pathString, &mustDelete);


		int rawwidth = calcFrame->getWidth();//raw->dim.x;
		int rawheight = calcFrame->getHeight();//raw->dim.y;

		if (mustDelete) {
			delete calcFrame;
		}
		
		const csSDK_int32 width = rawwidth;
		const csSDK_int32 height = rawheight;

		


		const csSDK_int32 pixel_aspect_num = 1;
		const csSDK_int32 pixel_aspect_den = 1;

		const csSDK_int32 fps_num = 24;
		const csSDK_int32 fps_den = 1;
		
		const csSDK_int32 depth = 16;


		SDKFileInfo8->hasVideo = kPrTrue;
		SDKFileInfo8->hasAudio = kPrFalse;
		
		
		// Video information
		//SDKFileInfo8->vidInfo.subType		= PrPixelFormat_BGRA_4444_32f_Linear;
		SDKFileInfo8->vidInfo.subType		= PrPixelFormat_BGRA_4444_32f; // Intuitively linear would naturally make more sense HOWEVER it seems then MediaCore converts it to Rec709 Gamma 2.4, which creates a wrong look in AE.
		SDKFileInfo8->vidInfo.imageWidth	= width;
		SDKFileInfo8->vidInfo.imageHeight	= height;
		SDKFileInfo8->vidInfo.depth			= depth;
		
		SDKFileInfo8->vidInfo.fieldType		= prFieldsNone;
		SDKFileInfo8->vidInfo.hasPulldown	= kPrFalse;
		SDKFileInfo8->vidInfo.isStill		= kPrTrue;
		SDKFileInfo8->vidInfo.noDuration	= imNoDurationStillDefault;

		SDKFileInfo8->vidInfo.alphaType		= (depth == 128 ? alphaBlackMatte : alphaNone);

		SDKFileInfo8->vidInfo.pixelAspectNum	= pixel_aspect_num;
		SDKFileInfo8->vidInfo.pixelAspectDen	= pixel_aspect_den;
		
		SDKFileInfo8->vidInfo.interpretationUncertain = imFieldTypeUncertain;// | imEmbeddedColorProfileUncertain;
		SDKFileInfo8->vidInfo.colorProfileSupport = imColorProfileSupport_Fixed;


		SDKFileInfo8->vidInfo.supportsAsyncIO			= kPrFalse;
		SDKFileInfo8->vidInfo.supportsGetSourceVideo	= kPrTrue;

		// this is optional
		SDKFileInfo8->vidScale = fps_num;
		SDKFileInfo8->vidSampleSize = fps_den;
		
		
		SDKFileInfo8->accessModes = kRandomAccessImport;
		SDKFileInfo8->hasDataRate = kPrFalse;
		
		
		// For a still image (not a sequence), imGetPrefs8 will have already been called twice before
		// we get here (imGetInfo8).  This does not happen for a sequence, so we have to create and
		// initialize the memory here ourselves.  imGetPrefs8 will be called if the user goes to
		// Source Settings.
		
		// Oddly, Premiere will call imGetInfo8 over and over again for a sequence, each time with
		// SDKFileInfo8->prefs == NULL.
		
		if(SDKFileInfo8->prefs == NULL)
		{
			SDKFileInfo8->prefs = stdParms->piSuites->memFuncs->newPtr(sizeof(ImporterPrefs));
			
			InitPrefs((ImporterPrefs *)SDKFileInfo8->prefs);
		}
		
		assert(stdParms->piSuites->memFuncs->getPtrSize((char *)SDKFileInfo8->prefs) == sizeof(ImporterPrefs));
		
		
		ImporterPrefs *prefs = reinterpret_cast<ImporterPrefs *>(SDKFileInfo8->prefs);
		
		
		ldataP->width = SDKFileInfo8->vidInfo.imageWidth;
		ldataP->height = SDKFileInfo8->vidInfo.imageHeight;
		ldataP->importerID = SDKFileInfo8->vidInfo.importerID;

		stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));

	}
	catch(...)
	{
		result = malUnknownError;
	}
	
	return result;
}


static prMALError 
SDKPreferredFrameSize(
	imStdParms					*stdparms, 
	imPreferredFrameSizeRec		*preferredFrameSizeRec)
{
	prMALError			result	= malNoError;
	ImporterLocalRec8H	ldataH	= reinterpret_cast<ImporterLocalRec8H>(preferredFrameSizeRec->inPrivateData);
	ImporterLocalRec8Ptr ldataP = *ldataH;

	// Enumerate formats in order of priority, starting from the most preferred format
	switch(preferredFrameSizeRec->inIndex)
	{
		case 0:
			preferredFrameSizeRec->outWidth = ldataP->width;
			preferredFrameSizeRec->outHeight = ldataP->height;
			// If we supported more formats, we'd return imIterateFrameSizes to request to be called again
			result = malNoError;
			break;
	
		default:
			// We shouldn't be called for anything other than the case above
			result = imOtherErr;
	}

	return result;
}


static prMALError 
SDKGetTimeInfo8(
	imStdParms			*stdParms, 
	imFileRef			SDKfileRef, 
	imTimeInfoRec8		*SDKtimeInfoRec8)
{
	prMALError err = malNoError;

	strncpy(SDKtimeInfoRec8->orgtime, "00:00:00:12", 17);
	
	SDKtimeInfoRec8->orgScale = 24;
	SDKtimeInfoRec8->orgSampleSize = 1;

	return err;
}


static prMALError	
SDKSetTimeInfo(
	imStdParms			*stdParms, 
	imFileRef			SDKfileRef, 
	imTimeInfoRec8		*SDKtimeInfoRec8)
{
	prMALError err = malNoError;

	// When I set SDKIndFormatRec->canWriteTimecode = kPrTrue, I can set the
	// timecode in Premiere, but this never actually gets called.
	// Not that I'd know what to do if it did.	OpenEXR doesn't like
	// modifying files in place.
	assert(FALSE);
	
	try
	{
		
	}
	catch(...)
	{
		err = malUnknownError;
	}

	return err;
}


//regex pathregex(R"([\\\/][^\\\/]*?\..*?$)",
//	regex_constants::icase | regex_constants::optimize | regex_constants::ECMAScript);





static prMALError 
SDKGetSourceVideo(
	imStdParms			*stdparms, 
	imFileRef			fileRef, 
	imSourceVideoRec	*sourceVideoRec)
{
	prMALError		result		= malNoError;

	// Get the privateData handle you stored in imGetInfo
	ImporterLocalRec8H ldataH = reinterpret_cast<ImporterLocalRec8H>(sourceVideoRec->inPrivateData);
	ImporterLocalRec8Ptr ldataP = *ldataH;
	ImporterPrefs *prefs = reinterpret_cast<ImporterPrefs *>(sourceVideoRec->prefs);
	

	PPixHand temp_ppix = NULL;

	abasc << "before frame filling" << "\n";
	abasc.flush();

	try
	{
		unsigned long fileSize = GetFileSize(fileRef, NULL);

		TCHAR filename[MAX_PATH + 1];
		ulong pathLength = GetFinalPathNameByHandleA(fileRef, filename, MAX_PATH + 1, 0);

		std::string pathString = filename;


		// make the Premiere buffer
		assert(sourceVideoRec->inNumFrameFormats == 1);
		assert(sourceVideoRec->inFrameFormats[0].inPixelFormat == PrPixelFormat_BGRA_4444_32f);

		imFrameFormat frameFormat = sourceVideoRec->inFrameFormats[0];


		prRect theRect;
		prSetRect(&theRect, 0, 0, frameFormat.inFrameWidth, frameFormat.inFrameHeight);

		RowbyteType rowBytes = 0;
		char* buf = NULL;

		abasc << "before getpixels" << "\n";
		abasc.flush();


		abasc << "before buffer making" << "\n";
		abasc.flush();
		ldataP->PPixCreatorSuite->CreatePPix(sourceVideoRec->outFrame, PrPPixBufferAccess_ReadWrite, frameFormat.inPixelFormat, &theRect);
		ldataP->PPixSuite->GetPixels(*sourceVideoRec->outFrame, PrPPixBufferAccess_WriteOnly, &buf);
		ldataP->PPixSuite->GetRowBytes(*sourceVideoRec->outFrame, &rowBytes);


		CalculatingFrame* calcFrame;
		bool mustDelete = false;

		
		calcFrame = getCalculatingFrame(pathString,&mustDelete, rowBytes);
		
		abasc << "before get dimensions" << "\n";
		abasc.flush();

		const int width = calcFrame->getWidth();
		const int height = calcFrame->getHeight();


		abasc << "width" << width << "\n";
		abasc.flush();
		abasc << "height" << height << "\n";
		abasc.flush();

		assert(frameFormat.inFrameWidth == width);
		assert(frameFormat.inFrameHeight == height);

		

		float* bufFloat = reinterpret_cast<float*>(buf);

		calcFrame->getFrame(bufFloat,rowBytes);


		abasc << "row bytes: " << rowBytes << " width " << width  << "\n";
		abasc << "Call counter private data: " << ((ldataP->callCounter)++) << "\n";
		abasc.flush();

		if (mustDelete) {
			delete calcFrame;
		}

		abasc << "after getframe" << "\n";
		abasc.flush();
		
	}
	catch (const std::exception& e) {
		abasc << "ERROR" << e.what() << "\n";
		abasc.flush();
	}
	catch(...)
	{
		abasc << "ERROR GENERIC" <<  "\n";
		abasc.flush();
		result = malUnknownError;
	}
	
	
	if(temp_ppix)
		ldataP->PPixSuite->Dispose(temp_ppix);
	

	return result;
}



PREMPLUGENTRY DllExport xImportEntry (
	csSDK_int32		selector, 
	imStdParms		*stdParms, 
	void			*param1, 
	void			*param2)
{
	prMALError	result				= imUnsupported;

	if (inited) {

		abasc << "selector" << "\n";
		abasc << selector << "\n";
		abasc.flush();
	}

	switch (selector)
	{
		case imInit:
			result =	SDKInit(stdParms, 
								reinterpret_cast<imImportInfoRec*>(param1));
			break;

		case imShutdown:
			result =	SDKShutdown(stdParms);
			break;
			
		case imGetInfo8:
			result =	SDKGetInfo8(stdParms, 
									reinterpret_cast<imFileAccessRec8*>(param1), 
									reinterpret_cast<imFileInfoRec8*>(param2));
			break;

		case imGetTimeInfo8:
			result =	SDKGetTimeInfo8(stdParms, 
										reinterpret_cast<imFileRef>(param1),
										reinterpret_cast<imTimeInfoRec8*>(param2));
			break;
			
		case imSetTimeInfo8:
			result =	SDKSetTimeInfo( stdParms, 
										reinterpret_cast<imFileRef>(param1),
										reinterpret_cast<imTimeInfoRec8*>(param2));
			break;
			
		case imOpenFile8:
			result =	SDKOpenFile8(	stdParms, 
										reinterpret_cast<imFileRef*>(param1), 
										reinterpret_cast<imFileOpenRec8*>(param2));
			break;
		
		case imGetPrefs8:
			result =	SDKGetPrefs8(	stdParms, 
										reinterpret_cast<imFileAccessRec8*>(param1),
										reinterpret_cast<imGetPrefsRec*>(param2));
			break;

		case imQuietFile:
			result =	SDKQuietFile(	stdParms, 
										reinterpret_cast<imFileRef*>(param1), 
										param2); 
			break;

		case imCloseFile:
			result =	SDKCloseFile(	stdParms, 
										reinterpret_cast<imFileRef*>(param1), 
										param2);
			break;

		case imAnalysis:
			result =	SDKAnalysis(	stdParms,
										reinterpret_cast<imFileRef>(param1),
										reinterpret_cast<imAnalysisRec*>(param2));
			break;

		case imGetIndFormat:
			result =	SDKGetIndFormat(stdParms, 
										reinterpret_cast<csSDK_size_t>(param1),
										reinterpret_cast<imIndFormatRec*>(param2));
			break;

		case imSaveFile8:
			result =	SDKSaveFile8(	stdParms, 
										reinterpret_cast<imSaveFileRec8*>(param1));
			break;
			
		case imDeleteFile8:
			result =	SDKDeleteFile8( stdParms, 
										reinterpret_cast<imDeleteFileRec8*>(param1));
			break;

		case imGetIndPixelFormat:
			result = SDKGetIndPixelFormat(	stdParms,
											reinterpret_cast<csSDK_size_t>(param1),
											reinterpret_cast<imIndPixelFormatRec*>(param2));
			break;

		// Importers that support the Premiere Pro 2.0 API must return malSupports8 for this selector
		case imGetSupports8:
			result = malSupports8;
			break;

		case imGetPreferredFrameSize:
			result =	SDKPreferredFrameSize(	stdParms,
												reinterpret_cast<imPreferredFrameSizeRec*>(param1));
			break;

		case imGetSourceVideo:
			result =	SDKGetSourceVideo(	stdParms,
											reinterpret_cast<imFileRef>(param1),
											reinterpret_cast<imSourceVideoRec*>(param2));
			break;

		case imCreateAsyncImporter:
			result = imUnsupported;
			break;
	}

	return result;
}

