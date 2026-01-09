#undef  _UNICODE
#define _UNICODE
#undef  UNICODE
#define UNICODE
#define USEWIN32IOAPI

#include <Windows.h>
#include <shlwapi.h>
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include "minizip\zip.h"
#include "minizip\unzip.h"
#include "minizip\iowin32.h"
#include "minizip\zlib.h"
#include "ReflectiveLoader.h"
#include "zipper.h"

#define FOPEN_FUNC(filename, mode) fopen64(filename, mode)
#define FTELLO_FUNC(stream) ftello64(stream)
#define FSEEKO_FUNC(stream, offset, origin) fseeko64(stream, offset, origin)

#define ARG_MAX 8191
#define BUF_SIZE 4 * 2048 * 1024
#define ZIP64 1
#define WRITEBUFFERSIZE (16384)
#define MAXFILENAME (256)

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "zlib.lib")

BOOL bIsUNCPath = FALSE;
DWORD dwFilesCompressed = 0;
DWORD dwFoldersCompressed = 0;

// You can use this value as a pseudo hinstDLL value (defined and set via ReflectiveLoader.c)
extern HINSTANCE hAppInstance;

uLong filetime(f, tmzip, dt)
    char *f;                /* name of file to get info on */
    tm_zip *tmzip;             /* return value: access, modific. and creation times */
    uLong *dt;             /* dostime */
{
  int ret = 0;
  {
      FILETIME ftLocal;
      HANDLE hFind;
      WIN32_FIND_DATAA ff32;

      hFind = FindFirstFileA(f,&ff32);
      if (hFind != INVALID_HANDLE_VALUE)
      {
        FileTimeToLocalFileTime(&(ff32.ftLastWriteTime),&ftLocal);
        FileTimeToDosDateTime(&ftLocal,((LPWORD)dt)+1,((LPWORD)dt)+0);
        FindClose(hFind);
        ret = 1;
      }
  }
  return ret;
}

LPSTR Utf16ToUtf8(LPWSTR lpwWideString) {
	INT strLen = WideCharToMultiByte(CP_UTF8, 0, lpwWideString, -1, NULL, 0, NULL, NULL);
	if (!strLen) {
		return NULL;
	}
	LPSTR lpMultiByteString = (LPSTR)calloc(1, strLen + 1);
	if (!lpMultiByteString) {
		return NULL;
	}
	WideCharToMultiByte(CP_UTF8, 0, lpwWideString, -1, lpMultiByteString, strLen, NULL, NULL);

	return lpMultiByteString;
}

void GenRandomStringW(LPWSTR lpFileName, INT len) {
	static const wchar_t AlphaNum[] =
		L"0123456789"
		L"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		L"abcdefghijklmnopqrstuvwxyz";
	srand(GetTickCount());
	for (INT i = 0; i < len; ++i) {
		lpFileName[i] = AlphaNum[rand() % (_countof(AlphaNum) - 1)];
	}
	lpFileName[len] = 0;
}

BOOL InfoZip(LPWSTR lpwZipName) {
	zlib_filefunc64_def ffunc;
	fill_win32_filefunc64W(&ffunc);

	unzFile uzFile = unzOpen2_64(lpwZipName, &ffunc);
	if (uzFile == NULL) {
		return FALSE;
	}

	unz_file_info64 uzFinfo;
	INT Result = unzGetCurrentFileInfo64(uzFile, &uzFinfo, NULL, 0, NULL, 0, NULL, 0);
	if (Result != UNZ_OK) {
		unzClose(uzFile);
		return FALSE;
	}

	wprintf(L"[+] Uncompressed file size:\t %llu Bytes\n", uzFinfo.uncompressed_size);
	wprintf(L"[+] Compressed file size:\t\t %llu Bytes\n", uzFinfo.compressed_size);

	unzClose(uzFile);

	return TRUE;
}

zipFile CreateZip(LPWSTR lpwZipName) {
	zlib_filefunc64_def ffunc;
	fill_win32_filefunc64W(&ffunc);

	zipFile zFile = zipOpen2_64(lpwZipName, APPEND_STATUS_CREATE, NULL, &ffunc);

	return zFile;
}

/* calculate the CRC32 of a file,
   because to encrypt a file, we need known the CRC32 of the file before */
int getFileCrc(const char* filenameinzip,void*buf,unsigned long size_buf,unsigned long* result_crc)
{
   unsigned long calculate_crc=0;
   int err=ZIP_OK;
   FILE * fin = FOPEN_FUNC(filenameinzip,"rb");

   unsigned long size_read = 0;
   unsigned long total_read = 0;
   if (fin==NULL)
   {
       err = ZIP_ERRNO;
   }

    if (err == ZIP_OK)
        do
        {
            err = ZIP_OK;
            size_read = (int)fread(buf,1,size_buf,fin);
            if (size_read < size_buf)
                if (feof(fin)==0)
            {
                printf("error in reading %s\n",filenameinzip);
                err = ZIP_ERRNO;
            }

            if (size_read>0)
                calculate_crc = crc32(calculate_crc,buf,size_read);
            total_read += size_read;

        } while ((err == ZIP_OK) && (size_read>0));

    if (fin)
        fclose(fin);

    *result_crc=calculate_crc;
    // printf("file %s crc %lx\n", filenameinzip, calculate_crc);
    return err;
}

int isLargeFile(const char* filename)
{
  int largeFile = 0;
  ZPOS64_T pos = 0;
  FILE* pFile = FOPEN_FUNC(filename, "rb");

  if(pFile != NULL)
  {
    int n = FSEEKO_FUNC(pFile, 0, SEEK_END);
    pos = FTELLO_FUNC(pFile);

    // printf("File : %s is %lld bytes\n", filename, pos);

    if(pos >= 0xffffffff)
     largeFile = 1;

                fclose(pFile);
  }

 return largeFile;
}

BOOL AddFile(zipFile zFile, LPWSTR lpwFilename, BOOL bIgnoreFilePath, LPWSTR pwszPassword) {
	BOOL Success;
	LARGE_INTEGER FileSize;
	DWORD dwInputFileSize;
	LPSTR lpFullFilePath = NULL;
	LPSTR lpFileName = NULL;

	_RtlInitUnicodeString RtlInitUnicodeString = (_RtlInitUnicodeString)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "RtlInitUnicodeString");
	if (RtlInitUnicodeString == NULL) {
		return FALSE;
	}

	_NtAllocateVirtualMemory NtAllocateVirtualMemory = (_NtAllocateVirtualMemory)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtAllocateVirtualMemory");
	if (NtAllocateVirtualMemory == NULL) {
		return FALSE;
	}

	_NtFreeVirtualMemory NtFreeVirtualMemory = (_NtFreeVirtualMemory)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtFreeVirtualMemory");
	if (NtFreeVirtualMemory == NULL) {
		exit(FALSE);
	}

	_NtCreateFile NtCreateFile = (_NtCreateFile)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtCreateFile");
	if (NtCreateFile == NULL) {
		return FALSE;
	}

	_NtReadFile NtReadFile = (_NtReadFile)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtReadFile");
	if (NtReadFile == NULL) {
		return FALSE;
	}

	WCHAR chFileName[ARG_MAX] = { 0 };
	if (bIsUNCPath) {
		lstrcat(chFileName, L"\\??\\UNC");
		wcscat_s(chFileName, _countof(chFileName), lpwFilename + 1);
	}
	else {
		lstrcat(chFileName, L"\\??\\");
		wcscat_s(chFileName, _countof(chFileName), lpwFilename);
	}

	UNICODE_STRING uFileName;
	RtlInitUnicodeString(&uFileName, chFileName);

	HANDLE hSrcFile = NULL;
	IO_STATUS_BLOCK IoStatusBlock;
	ZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
	OBJECT_ATTRIBUTES FileObjectAttributes;
	InitializeObjectAttributes(&FileObjectAttributes, &uFileName, OBJ_CASE_INSENSITIVE, NULL, NULL);

	NTSTATUS Status = NtCreateFile(&hSrcFile, (GENERIC_READ | SYNCHRONIZE), &FileObjectAttributes, &IoStatusBlock, 0,
		0, FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);

	if (hSrcFile == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	Success = GetFileSizeEx(hSrcFile, &FileSize);
	if ((!Success) || (FileSize.QuadPart > 0xFFFFFFFF))
	{
		return FALSE;
	}
	dwInputFileSize = FileSize.LowPart;

	FILETIME ft;
	GetFileTime(hSrcFile, NULL, NULL, &ft);

	zip_fileinfo zfi = { 0 };
	FileTimeToDosDateTime(&ft, ((LPWORD)&zfi.dosDate) + 1, ((LPWORD)&zfi.dosDate) + 0);

	zfi.internal_fa = 0;
	zfi.external_fa = GetFileAttributes(lpwFilename);

	LPSTR lpFilePathA = Utf16ToUtf8(lpwFilename);
	if (!lpFilePathA) {
		CloseHandle(hSrcFile);
		return FALSE;
	}

	if (bIgnoreFilePath) {
		lpFileName = PathFindFileNameA(lpFilePathA);
	}
	else {
		lpFullFilePath = lpFilePathA;
		lpFileName = PathSkipRootA(lpFullFilePath);
	}

	INT Result;

	if (!pwszPassword)
	{
		Result = zipOpenNewFileInZip64(
			zFile,
			lpFileName,
			&zfi,
			NULL, 0,
			NULL, 0,
			NULL,
			Z_DEFLATED,
			Z_DEFAULT_COMPRESSION,
			ZIP64
		);
	}
	else
	{
		char szPassword[256];
		WideCharToMultiByte(CP_UTF8, 0, pwszPassword, -1, szPassword, sizeof(szPassword), NULL, NULL);
		int len = WideCharToMultiByte(CP_UTF8, 0, lpwFilename, -1, NULL, 0, NULL, NULL);
		char* filenameinzip = (char*)malloc(len);
		WideCharToMultiByte(CP_UTF8, 0, lpwFilename, -1, filenameinzip, len, NULL, NULL);
		unsigned long crcFile=0;
		int zip64 = 0;
		int size_buf=0;
    	void* buf=NULL;
		int opt_compress_level=Z_DEFAULT_COMPRESSION;
	    size_buf = WRITEBUFFERSIZE;
    	buf = (void*)malloc(size_buf);

		getFileCrc(filenameinzip,buf,size_buf,&crcFile);
		zip64 = isLargeFile(filenameinzip);
		Result = zipOpenNewFileInZip3_64(zFile,lpFileName,&zfi,
					NULL,0,NULL,0,NULL /* comment*/,
					(opt_compress_level != 0) ? Z_DEFLATED : 0,
					opt_compress_level,0,
					/* -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, */
					-MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
					szPassword,crcFile, zip64);
	}

	if (Result != ZIP_OK)
	{
		CloseHandle(hSrcFile);
		return FALSE;
	}

	PVOID lpBuffer = NULL;
	SIZE_T uSize = BUF_SIZE;
	Status = NtAllocateVirtualMemory(NtCurrentProcess(), &lpBuffer, 0, &uSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (Status != 0) {
		zipCloseFileInZip(zFile);
		CloseHandle(hSrcFile);
		return FALSE;
	}

	ULONG uBytesRead = 0;
	ULONG uBytesWritten = 0;

	while (Result == ZIP_OK && uBytesWritten < dwInputFileSize) {
		Status = NtReadFile(hSrcFile, 0, NULL, NULL, &IoStatusBlock, lpBuffer, BUF_SIZE, 0, NULL);
		uBytesRead = IoStatusBlock.Information;
		if (Status != 0) {
			CloseHandle(hSrcFile);
			return FALSE;
		}

		uBytesWritten += uBytesRead;

		if (uBytesRead)
			Result = zipWriteInFileInZip(zFile, lpBuffer, uBytesRead);
		else
			break;
	}

	Status = NtFreeVirtualMemory(NtCurrentProcess(), &lpBuffer, &uSize, MEM_RELEASE);

	zipCloseFileInZip(zFile);
	CloseHandle(hSrcFile);

	dwFilesCompressed++;

	return TRUE;
}

BOOL AddFolder(zipFile zFile, LPWSTR lpwFoldername, LPWSTR pwszPassword) {
	LPSTR lpFolderName = NULL;

	_RtlInitUnicodeString RtlInitUnicodeString = (_RtlInitUnicodeString)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "RtlInitUnicodeString");
	if (RtlInitUnicodeString == NULL) {
		return FALSE;
	}

	_NtCreateFile NtCreateFile = (_NtCreateFile)
		GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtCreateFile");
	if (NtCreateFile == NULL) {
		return FALSE;
	}

	PathAddBackslash(lpwFoldername);

	LPWSTR lpDotPath = StrStr(lpwFoldername, L"\\.\\");
	LPWSTR lpDotDotPath = StrStr(lpwFoldername, L"\\..\\");
	if (lpDotPath || lpDotDotPath) {
		return TRUE;
	}

	WCHAR chFolderName[ARG_MAX] = { 0 };
	if (bIsUNCPath) {
		lstrcat(chFolderName, L"\\??\\UNC");
		wcscat_s(chFolderName, _countof(chFolderName), lpwFoldername + 1);
	}
	else {
		lstrcat(chFolderName, L"\\??\\");
		wcscat_s(chFolderName, _countof(chFolderName), lpwFoldername);
	}

	UNICODE_STRING uFolderName;
	RtlInitUnicodeString(&uFolderName, chFolderName);

	HANDLE hSrcFolder = NULL;
	IO_STATUS_BLOCK IoStatusBlock;
	ZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
	OBJECT_ATTRIBUTES FileObjectAttributes;
	InitializeObjectAttributes(&FileObjectAttributes, &uFolderName, OBJ_CASE_INSENSITIVE, NULL, NULL);

	NTSTATUS Status = NtCreateFile(&hSrcFolder, (GENERIC_READ | SYNCHRONIZE), &FileObjectAttributes, &IoStatusBlock, 0,
		0, FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE, NULL, 0);

	if (hSrcFolder == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	FILETIME ft;
	GetFileTime(hSrcFolder, NULL, NULL, &ft);

	zip_fileinfo zfi = { 0 };
	FileTimeToDosDateTime(&ft, ((LPWORD)&zfi.dosDate) + 1, ((LPWORD)&zfi.dosDate) + 0);

	zfi.internal_fa = 0;
	zfi.external_fa = GetFileAttributes(lpwFoldername);

	LPSTR lpFolderNameA = Utf16ToUtf8(lpwFoldername);
	if (!lpFolderNameA) {
		return FALSE;
	}

	lpFolderName = PathSkipRootA(lpFolderNameA);

	if (lpFolderName != NULL) {
		INT Result;

		Result = zipOpenNewFileInZip64(
			zFile,
			lpFolderName,      // MUST end with "/"
			&zfi,
			NULL, 0,
			NULL, 0,
			NULL,
			Z_DEFLATED,
			Z_DEFAULT_COMPRESSION,
			ZIP64
		);
		
		if (Result != ZIP_OK)
		{
			CloseHandle(hSrcFolder);
			return FALSE;
		}
	}

	zipCloseFileInZip(zFile);
	CloseHandle(hSrcFolder);

	dwFoldersCompressed++;

	WCHAR wcWildCard[ARG_MAX] = { 0 };
	lstrcpy(wcWildCard, lpwFoldername);
	lstrcat(wcWildCard, L"*");

	WIN32_FIND_DATA findData;
	HANDLE hFindFile = FindFirstFile(wcWildCard, &findData);
	if (hFindFile == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	do
	{
		WCHAR wcFullPath[ARG_MAX] = { 0 };
		lstrcpy(wcFullPath, lpwFoldername);
		lstrcat(wcFullPath, findData.cFileName);

		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			AddFolder(zFile, wcFullPath, pwszPassword);
		}
		else {
			AddFile(zFile, wcFullPath, FALSE, pwszPassword);
		}
	} while (FindNextFile(hFindFile, &findData));

	return TRUE;
}

void do_help()
{
	wprintf(L"Usage : zipper [-p password] [-f path to file/folder]\n\n");
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpReserved)
{
	BOOL bReturnValue = TRUE;
	LPWSTR pwszParams = (LPWSTR)calloc(strlen((LPSTR)lpReserved) + 1, sizeof(WCHAR));
	size_t convertedChars = 0;
	size_t newsize = strlen((LPSTR)lpReserved) + 1;

	switch (dwReason)
	{
	case DLL_QUERY_HMODULE:
		if (lpReserved != NULL)
			*(HMODULE *)lpReserved = hAppInstance;
		break;
	case DLL_PROCESS_ATTACH:
		hAppInstance = hinstDLL;

		if (lpReserved != NULL) {
			// convert ANSI → Unicode
			size_t len = strlen((LPSTR)lpReserved) + 1;
			LPWSTR pwszParams = (LPWSTR)calloc(len, sizeof(WCHAR));
			size_t convertedChars = 0;
			mbstowcs_s(&convertedChars, pwszParams, len, (LPSTR)lpReserved, _TRUNCATE);

			// parse parameters
			int argc = 0;
			LPWSTR* argv = CommandLineToArgvW(pwszParams, &argc);

			LPWSTR pwszPassword = NULL;
			LPWSTR pwszInputPath = NULL;

			for (int i = 0; i < argc; i++)
			{
				if (!lstrcmpiW(argv[i], L"-p") && i + 1 < argc)
					pwszPassword = argv[++i];
				else if (!lstrcmpiW(argv[i], L"-f") && i + 1 < argc)
					pwszInputPath = argv[++i];
			}

			if (!pwszInputPath || !PathFileExistsW(pwszInputPath))
			{
				wprintf(L"[!] Invalid or missing input path\n");
				do_help();
				fflush(stdout);
				ExitProcess(0);
			}

			BOOL IsDirectory = FALSE;
			BOOL bIgnoreFullPath = TRUE;

			wprintf(L" __________.__                                   \n");
			wprintf(L" \\____    /|__|_____ ______   ___________       \n");
			wprintf(L"   /     / |  \\____ \\\\____ \\_/ __ \\_  __ \\ \n");
			wprintf(L"  /     /_ |  |  |_> >  |_> >  ___/|  | \\/      \n");
			wprintf(L" /_______ \\|__|   __/|   __/ \\___  >__|        \n");
			wprintf(L"         \\/   |__|   |__|        \\/            \n");
			wprintf(L"                         Outflank Zipper        \n");
			wprintf(L"                    By Cneeliz @Outflank 2020   \n\n");

			if (PathIsDirectory(pwszInputPath)) {
				IsDirectory = TRUE;
			}

			if (PathIsUNC(pwszInputPath)) {
				bIsUNCPath = TRUE;
			}

			WCHAR wcZipContent[ARG_MAX] = { 0 };
			lstrcat(wcZipContent, pwszInputPath);

			WCHAR chZipPath[MAX_PATH];
			DWORD dwRetVal = GetTempPath(MAX_PATH, chZipPath);
			if (dwRetVal == 0) {
				fflush(stdout);
				ExitProcess(0);
			}

			WCHAR chFileName[MAX_PATH] = { 0 };
			GenRandomStringW(chFileName, 12);
			lstrcat(chZipPath, chFileName);
			lstrcat(chZipPath, L".zip");

			zipFile zFile = CreateZip(chZipPath);
			if (zFile == NULL) {
				fflush(stdout);
				ExitProcess(0);
			}
			if (IsDirectory == FALSE) {
				if (!AddFile(zFile, wcZipContent, bIgnoreFullPath, pwszPassword)) {
					wprintf(L"[!] Failed to compress file %ls\n\n", wcZipContent);
					zipClose(zFile, NULL);
					fflush(stdout);
					ExitProcess(0);
				}
			}
			else {
				if (!AddFolder(zFile, wcZipContent, pwszPassword)) {
					wprintf(L"[!] Failed to compress folder %ls\n\n", wcZipContent);
					zipClose(zFile, NULL);
					fflush(stdout);
					ExitProcess(0);
				}
			}

			zipClose(zFile, NULL);

			wprintf(L"[+] Zipfile saved as:\t\t %ls with password %ls\n", chZipPath, pwszPassword);
			if (IsDirectory == FALSE) {
				InfoZip(chZipPath);
			}

			wprintf(L"[+] Total files compressed:\t %d\n", dwFilesCompressed);
			wprintf(L"[+] Total folders compressed:\t %d\n", dwFoldersCompressed);	
		}

		// Flush STDOUT
		fflush(stdout);

		// We're done, so let's exit
		ExitProcess(0);
		break;
	case DLL_PROCESS_DETACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return bReturnValue;
}
