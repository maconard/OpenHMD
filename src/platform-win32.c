// Copyright 2013, Fredrik Hultin.
// Copyright 2013, Jakob Bornecrantz.
// SPDX-License-Identifier: BSL-1.0
/*
 * OpenHMD - Free and Open Source API and drivers for immersive technology.
 */

/* Platform Specific Functions, Win32 Implementation */


#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define WIN32_EXTRA_LEAN

#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "openhmdi.h"

double ohmd_get_tick()
{
	double high, low;
	FILETIME filetime;

	GetSystemTimeAsFileTime(&filetime);

	high = filetime.dwHighDateTime;
	low = filetime.dwLowDateTime;

	return (high * 4294967296.0 + low) / 10000000;
}

static const uint64_t NUM_1_000_000_000 = 1000000000;
static LARGE_INTEGER qpc_freq;

void ohmd_monotonic_init(ohmd_context* ctx)
{
	QueryPerformanceFrequency(&qpc_freq);
	ctx->monotonic_ticks_per_sec = NUM_1_000_000_000;
}

/* Nanoseconds, like CLOCK_MONOTONIC on POSIX; the Rift tracker assumes that unit. */
uint64_t ohmd_monotonic_get(ohmd_context* ctx)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	return ohmd_monotonic_conv(now.QuadPart, qpc_freq.QuadPart, NUM_1_000_000_000);
}

// TODO higher resolution
OHMD_APIENTRYDLL void OHMD_APIENTRY ohmd_sleep(double seconds)
{
	Sleep((DWORD)(seconds * 1000));
}

// threads

struct ohmd_thread {
	HANDLE handle;
	void* arg;
	unsigned int (*routine)(void* arg);
};

struct ohmd_mutex {
	CRITICAL_SECTION lock;
};

struct ohmd_cond {
	CONDITION_VARIABLE cond;
};

DWORD __stdcall ohmd_thread_wrapper(void* t)
{
	ohmd_thread* thread = (ohmd_thread*)t;
	return thread->routine(thread->arg);
}

ohmd_thread* ohmd_create_thread(ohmd_context* ctx, unsigned int (*routine)(void* arg), void* arg)
{
	ohmd_thread* thread = ohmd_alloc(ctx, sizeof(ohmd_thread));
	if(!thread)
		return NULL;

	thread->routine = routine;
	thread->arg = arg;

	thread->handle = CreateThread(NULL, 0, ohmd_thread_wrapper, thread, 0, NULL);

	return thread;
}

void ohmd_destroy_thread(ohmd_thread* thread)
{
	WaitForSingleObject(thread->handle, INFINITE);
	CloseHandle(thread->handle);
	free(thread);
}

ohmd_mutex* ohmd_create_mutex(ohmd_context* ctx)
{
	ohmd_mutex* mutex = ohmd_alloc(ctx, sizeof(ohmd_mutex));
	if(!mutex)
		return NULL;
	
	InitializeCriticalSection(&mutex->lock);

	return mutex;
}

void ohmd_destroy_mutex(ohmd_mutex* mutex)
{
	DeleteCriticalSection(&mutex->lock);
	free(mutex);
}

void ohmd_lock_mutex(ohmd_mutex* mutex)
{
	if(mutex)
		EnterCriticalSection(&mutex->lock);
}

void ohmd_unlock_mutex(ohmd_mutex* mutex)
{
	if(mutex)
		LeaveCriticalSection (&mutex->lock);
}

ohmd_cond* ohmd_create_cond(ohmd_context* ctx)
{
	ohmd_cond* cond = ohmd_alloc(ctx, sizeof(ohmd_cond));
	if(!cond)
		return NULL;
	
	InitializeConditionVariable (&cond->cond);
	return cond;
}

void ohmd_destroy_cond(ohmd_cond* cond)
{
  /* The condition variable must not be in use. In that case,
   * there's no need to do anything to clear condition variables, just free it */
	free(cond);
}

void ohmd_cond_wait(ohmd_cond* cond, ohmd_mutex* mutex)
{
	if(cond && mutex)
		SleepConditionVariableCS(&cond->cond, &mutex->lock, INFINITE);
}

void ohmd_cond_signal(ohmd_cond* cond)
{
	if (cond)
		WakeConditionVariable(&cond->cond);
}

void ohmd_cond_broadcast(ohmd_cond* cond)
{
	if (cond)
		WakeAllConditionVariable(&cond->cond);
}

int findEndPoint(char* path, int endpoint)
{
	char comp[8];
	sprintf(comp,"mi_0%d",endpoint);

	if (strstr(path, comp) != NULL) {
		return 1;
	}

	return 0;
}

int ohmd_read_file(const char* filename, char **out_buf, unsigned long *out_len)
{
	FILE* f = fopen(filename, "rb");
	long file_len;
	char *buffer;

	if (f == NULL)
		return errno;

	if (fseek(f, 0, SEEK_END) != 0 || (file_len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return EIO;
	}

	buffer = malloc(file_len + 1);
	if (buffer == NULL || fread(buffer, 1, file_len, f) != (size_t)file_len) {
		free(buffer);
		fclose(f);
		return EIO;
	}
	buffer[file_len] = 0;
	fclose(f);

	*out_buf = buffer;
	*out_len = (unsigned long) file_len;
	return 0;
}

int ohmd_ensure_path(const char* pathname)
{
	char pathbuf[OHMD_STR_SIZE+1];
	char *cur;
	DWORD attr;

	strncpy(pathbuf, pathname, OHMD_STR_SIZE);
	pathbuf[OHMD_STR_SIZE] = 0;

	for (cur = pathbuf + 1;; cur++) {
		char c = *cur;
		if (c != '/' && c != '\\' && c != 0)
			continue;
		*cur = 0;
		attr = GetFileAttributesA(pathbuf);
		if (attr == INVALID_FILE_ATTRIBUTES) {
			if (!CreateDirectoryA(pathbuf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
				return EACCES;
		} else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
			return ENOTDIR;
		}
		*cur = c;
		if (c == 0)
			return 0;
	}
}

int ohmd_write_file(const char* filename, char *buf, unsigned long buf_len)
{
	char tmp[OHMD_STR_SIZE];
	FILE* f;
	int ret;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", filename) >= (int)sizeof(tmp))
		return ENAMETOOLONG;

	f = fopen(tmp, "wb");
	if (f == NULL)
		return errno;

	if (fwrite(buf, 1, buf_len, f) != buf_len) {
		fclose(f);
		DeleteFileA(tmp);
		return EIO;
	}

	ret = fclose(f);
	if (ret != 0) {
		DeleteFileA(tmp);
		return ret;
	}

	if (!MoveFileExA(tmp, filename, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileA(tmp);
		return EIO;
	}
	return 0;
}

const char *ohmd_get_config_dir(ohmd_context *ctx)
{
	const char *env;

	if (ctx->config_dir[0] != 0)
		return ctx->config_dir;

	env = getenv("LOCALAPPDATA");
	if (env == NULL)
		env = getenv("TEMP");
	snprintf(ctx->config_dir, OHMD_STR_SIZE, "%s\\openhmd", env ? env : ".");
	return ctx->config_dir;
}

/// Handling ovr service
static int _enable_ovr_service = 0;

void ohmd_toggle_ovr_service(int state) //State is 0 for Disable, 1 for Enable
{
	SC_HANDLE serviceDbHandle = OpenSCManager(NULL,NULL,SC_MANAGER_ALL_ACCESS);
	SC_HANDLE serviceHandle = OpenService(serviceDbHandle, "OVRService", SC_MANAGER_ALL_ACCESS);

	SERVICE_STATUS_PROCESS status;
	DWORD bytesNeeded;
	QueryServiceStatusEx(serviceHandle, SC_STATUS_PROCESS_INFO,(LPBYTE) &status,sizeof(SERVICE_STATUS_PROCESS), &bytesNeeded);

	if (state == 0 || status.dwCurrentState == SERVICE_RUNNING)
	{
		// Stop it
		BOOL b = ControlService(serviceHandle, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS) &status);
		if (b)
		{
			printf("OVRService stopped\n");
			_enable_ovr_service = 1;
		}
		else 
			printf("Error: OVRService failed to stop, please try running with Administrator rights\n");
	}
	else if (state == 1 && _enable_ovr_service)
	{
		// Start it 
		BOOL b = StartService(serviceHandle, 0, NULL);
		if (b) 
			printf("OVRService started\n");
		else 
			printf("Error: OVRService failed to start, please try running with Administrator rights\n");
	} 
	CloseServiceHandle(serviceHandle); 
	CloseServiceHandle(serviceDbHandle); 
}
#endif
