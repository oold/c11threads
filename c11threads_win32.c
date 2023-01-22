/*
Win32 implementation for c11threads.

Authors:
John Tsiombikas <nuclear@member.fsf.org>
Oliver Old <oliver.old@outlook.com>

I place this piece of code in the public domain. Feel free to use as you see
fit. I'd appreciate it if you keep my name at the top of the code somewhere, but
whatever.

Main project site: https://github.com/jtsiomb/c11threads
*/

#if defined(_WIN32) && !defined(C11THREADS_PTHREAD_WIN32)

#include "include/c11threads.h"

#include <stddef.h>
#include <stdlib.h>

#define WINVER _WIN32_WINNT_NT4
#define _WIN32_WINNT _WIN32_WINNT_NT4
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define _WIN32_WINNT_VISTA 0x0600
#define THREAD_QUERY_LIMITED_INFORMATION (0x0800)


/* ---- library ---- */

static int _c11threads_win32_init(void);
static void _c11threads_win32_destroy(void);
static void _c11threads_win32_thrd_run_tss_dtors(void);
int __stdcall DllMain(void *instance, unsigned long reason, void *reserved)
{
	(void)instance;

	switch (reason) {
	case DLL_PROCESS_ATTACH:
		return _c11threads_win32_init();

	case DLL_PROCESS_DETACH:
		if (reserved) {
			_c11threads_win32_destroy();
		}
		break;

	case DLL_THREAD_DETACH:
		_c11threads_win32_thrd_run_tss_dtors();
		break;
	}

	return 0;
}

typedef void (__stdcall *_c11threads_win32_InitializeConditionVariable_t)(void*);
typedef void (__stdcall *_c11threads_win32_WakeConditionVariable_t)(void*);
typedef void (__stdcall *_c11threads_win32_WakeAllConditionVariable_t)(void*);
typedef int (__stdcall *_c11threads_win32_SleepConditionVariableCS_t)(void*, PCRITICAL_SECTION, unsigned long);
typedef int (__stdcall *_c11threads_win32_InitOnceExecuteOnce_t)(void*, const void*, void*, void**);

struct _c11threads_win32_thrd_entry_t {
	struct _c11threads_win32_thrd_entry_t *next;
	void *h;
	thrd_t thrd;
};

struct _c11threads_win32_tss_dtor_entry_t {
	struct _c11threads_win32_tss_dtor_entry_t *next;
	tss_dtor_t dtor;
	tss_t key;
};

static unsigned short _c11threads_win32_winver;
static _c11threads_win32_InitializeConditionVariable_t _c11threads_win32_InitializeConditionVariable;
static _c11threads_win32_WakeConditionVariable_t _c11threads_win32_WakeConditionVariable;
static _c11threads_win32_WakeAllConditionVariable_t _c11threads_win32_WakeAllConditionVariable;
static _c11threads_win32_SleepConditionVariableCS_t _c11threads_win32_SleepConditionVariableCS;
static _c11threads_win32_InitOnceExecuteOnce_t _c11threads_win32_InitOnceExecuteOnce;
static CRITICAL_SECTION _c11threads_win32_thrd_list_critical_section;
static struct _c11threads_win32_thrd_entry_t *_c11threads_win32_thrd_list = NULL;
static CRITICAL_SECTION _c11threads_win32_tss_dtor_list_critical_section;
static struct _c11threads_win32_tss_dtor_entry_t *_c11threads_win32_tss_dtor_list = NULL;

#ifdef _MSC_VER
#pragma warning(push)
/* Warning C4996: 'GetVersion': was declared deprecated */
/* Warning C28125: The function 'InitializeCriticalSection' must be called from within a try/except block. */
/* Warning C28159: Consider using 'IsWindows*' instead of 'GetVersion'. Reason: Deprecated. Use VerifyVersionInfo* or IsWindows* macros from VersionHelpers. */
#pragma warning(disable: 4996 28125 28159)
#endif
static int _c11threads_win32_init(void)
{
	unsigned short os_version;

	os_version = (unsigned short)GetVersion(); /* Keep in mind: Maximum version for unmanifested apps is Windows 8 (0x0602). */
	_c11threads_win32_winver = (os_version << 8) | (os_version >> 8);
	if (_c11threads_win32_winver >= _WIN32_WINNT_VISTA) {
		void *kernel32;
		kernel32 = GetModuleHandleW(L"kernel32.dll");
		if (!kernel32) {
			return 0;
		}
		_c11threads_win32_InitializeConditionVariable = (_c11threads_win32_InitializeConditionVariable_t)GetProcAddress(kernel32, "InitializeConditionVariable");
		if (!_c11threads_win32_InitializeConditionVariable) {
			return 0;
		}
		_c11threads_win32_WakeConditionVariable = (_c11threads_win32_WakeConditionVariable_t)GetProcAddress(kernel32, "WakeConditionVariable");
		if (!_c11threads_win32_WakeConditionVariable) {
			return 0;
		}
		_c11threads_win32_WakeAllConditionVariable = (_c11threads_win32_WakeAllConditionVariable_t)GetProcAddress(kernel32, "WakeAllConditionVariable");
		if (!_c11threads_win32_WakeAllConditionVariable) {
			return 0;
		}
		_c11threads_win32_SleepConditionVariableCS = (_c11threads_win32_SleepConditionVariableCS_t)GetProcAddress(kernel32, "SleepConditionVariableCS");
		if (!_c11threads_win32_SleepConditionVariableCS) {
			return 0;
		}
		_c11threads_win32_InitOnceExecuteOnce = (_c11threads_win32_InitOnceExecuteOnce_t)GetProcAddress(kernel32, "InitOnceExecuteOnce");
		if (!_c11threads_win32_InitOnceExecuteOnce) {
			return 0;
		}
	}
	InitializeCriticalSection(&_c11threads_win32_thrd_list_critical_section);
	InitializeCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);
	return 1;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static void _c11threads_win32_destroy(void)
{
	struct _c11threads_win32_thrd_entry_t *thrd_entry;
	struct _c11threads_win32_thrd_entry_t *thrd_entry_temp;
	struct _c11threads_win32_tss_dtor_entry_t *tss_dtor_entry;
	struct _c11threads_win32_tss_dtor_entry_t *tss_dtor_entry_temp;

	_c11threads_win32_thrd_run_tss_dtors();

	DeleteCriticalSection(&_c11threads_win32_thrd_list_critical_section);
	DeleteCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);

	thrd_entry = _c11threads_win32_thrd_list;
	while (thrd_entry) {
		thrd_entry_temp = thrd_entry->next;
		CloseHandle(thrd_entry->h);
		free(thrd_entry);
		thrd_entry = thrd_entry_temp;
	}

	tss_dtor_entry = _c11threads_win32_tss_dtor_list;
	while (tss_dtor_entry) {
		tss_dtor_entry_temp = tss_dtor_entry->next;
		TlsFree(tss_dtor_entry->key);
		free(tss_dtor_entry);
		tss_dtor_entry = tss_dtor_entry_temp;
	}
}

/* ---- utilities ---- */

static int _c11threads_win32_util_is_timespec32_valid(const struct _c11threads_win32_timespec32_t *ts)
{
	return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec <= 999999999;
}

static int _c11threads_win32_util_is_timespec64_valid(const struct _c11threads_win32_timespec64_t *ts)
{
	return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec <= 999999999;
}

/* Precondition: 'ts' validated. */
static __int64 _c11threads_win32_util_timespec32_to_file_time(const struct _c11threads_win32_timespec32_t *ts)
{
	unsigned __int64 sec_res;
	unsigned __int64 nsec_res;

	sec_res = (unsigned __int64)ts->tv_sec * (unsigned __int64)10000000;

	/* Add another 100 ns if division yields remainder. */
	nsec_res = (unsigned long)ts->tv_nsec / 100UL + !!((unsigned long)ts->tv_nsec % 100UL);

	return sec_res + nsec_res;
}

/* Precondition: 'ts' validated. */
static __int64 _c11threads_win32_util_timespec64_to_file_time(const struct _c11threads_win32_timespec64_t *ts, size_t *periods)
{
	unsigned __int64 sec_res;
	unsigned __int64 nsec_res;
	unsigned __int64 res;

	*periods = (unsigned long)((unsigned __int64)ts->tv_sec / (unsigned __int64)922337203685);
	sec_res = ((unsigned __int64)ts->tv_sec % (unsigned __int64)922337203685) * (unsigned __int64)10000000;

	/* Add another 100 ns if division yields remainder. */
	nsec_res = (unsigned long)ts->tv_nsec / 100UL + !!((unsigned long)ts->tv_nsec % 100UL);

	/* 64-bit time_t may cause overflow. */
	if (nsec_res > (unsigned __int64) - 1 - sec_res) {
		++*periods;
		nsec_res -= (unsigned __int64) - 1 - sec_res;
		sec_res = 0;
	}

	res = sec_res + nsec_res;

	if (*periods && !res) {
		--*periods;
		return (__int64)9223372036850000000;
	}

	return res;
}

/* Precondition: 'ts' validated. */
static int _c11threads_win32_util_timespec32_to_milliseconds(const struct _c11threads_win32_timespec32_t *ts, unsigned long *ms)
{
	unsigned long sec_res;
	unsigned long nsec_res;

	/* Overflow. */
	if ((unsigned long)ts->tv_sec > (INFINITE - 1UL) / 1000UL) {
		return 0;
	}

	sec_res = (unsigned long)ts->tv_sec * 1000UL;
	/* Add another millisecond if division yields remainder. */
	nsec_res = (unsigned long)ts->tv_nsec / 1000000UL + !!((unsigned long)ts->tv_nsec % 1000000UL);

	/* Overflow. */
	if (nsec_res > INFINITE - 1UL - sec_res) {
		return 0;
	}

	*ms = sec_res + nsec_res;
	return 1;
}

/* Precondition: 'ts' validated. */
static int _c11threads_win32_util_timespec64_to_milliseconds(const struct _c11threads_win32_timespec64_t *ts, unsigned long *ms)
{
	unsigned long sec_res;
	unsigned long nsec_res;

	/* Overflow. */
	if ((unsigned __int64)ts->tv_sec > (INFINITE - 1UL) / 1000UL) {
		return 0;
	}

	sec_res = (unsigned long)ts->tv_sec * 1000UL;
	/* Add another millisecond if division yields remainder. */
	nsec_res = (unsigned long)ts->tv_nsec / 1000000UL + !!((unsigned long)ts->tv_nsec % 1000000UL);

	/* Overflow. */
	if (nsec_res > INFINITE - 1UL - sec_res) {
		return 0;
	}

	*ms = sec_res + nsec_res;
	return 1;
}

/* Precondition: 'current_time' and 'end_time' validated. */
static unsigned long _c11threads_win32_util_timepoint_to_millisecond_timespan32(const struct _c11threads_win32_timespec32_t *current_time, const struct _c11threads_win32_timespec32_t *end_time, int *clamped) {
	unsigned long wait_time;
	struct _c11threads_win32_timespec32_t ts;

	*clamped = 0;
	if (current_time->tv_sec > end_time->tv_sec || (current_time->tv_sec == end_time->tv_sec && current_time->tv_nsec >= end_time->tv_nsec)) {
		wait_time = 0;
	} else {
		ts.tv_sec = end_time->tv_sec - current_time->tv_sec;
		ts.tv_nsec = end_time->tv_nsec - current_time->tv_nsec;
		if (ts.tv_nsec < 0) {
			--ts.tv_sec;
			ts.tv_nsec += 1000000000;
		}

		if (!_c11threads_win32_util_timespec32_to_milliseconds(&ts, &wait_time)) {
			/* Clamp wait_time. Pretend we've had a spurious wakeup if expired. */
			wait_time = INFINITE - 1;
			*clamped = 1;
		}
	}

	return wait_time;
}

/* Precondition: 'current_time' and 'end_time' validated. */
static unsigned long _c11threads_win32_util_timepoint_to_millisecond_timespan64(const struct _c11threads_win32_timespec64_t *current_time, const struct _c11threads_win32_timespec64_t *end_time, int *clamped) {
	unsigned long wait_time;
	struct _c11threads_win32_timespec64_t ts;

	*clamped = 0;
	if (current_time->tv_sec > end_time->tv_sec || (current_time->tv_sec == end_time->tv_sec && current_time->tv_nsec >= end_time->tv_nsec)) {
		wait_time = 0;
	} else {
		ts.tv_sec = end_time->tv_sec - current_time->tv_sec;
		ts.tv_nsec = end_time->tv_nsec - current_time->tv_nsec;
		if (ts.tv_nsec < 0) {
			--ts.tv_sec;
			ts.tv_nsec += 1000000000;
		}

		if (!_c11threads_win32_util_timespec64_to_milliseconds(&ts, &wait_time)) {
			/* Clamp wait_time. Pretend we've had a spurious wakeup if expired. */
			wait_time = INFINITE - 1;
			*clamped = 1;
		}
	}

	return wait_time;
}

#if defined(C11THREADS_NO_TIMESPEC_GET) || !defined(_MSC_VER)
int _c11threads_win32_timespec32_get(struct _c11threads_win32_timespec32_t *ts, int base)
{
	FILETIME file_time;
	ULARGE_INTEGER li;

	if (base != TIME_UTC) {
		return 0;
	}

	GetSystemTimeAsFileTime(&file_time);

	li.LowPart = file_time.dwLowDateTime;
	li.HighPart = file_time.dwHighDateTime;

	/* Also subtract difference between FILETIME and UNIX time epoch. It's 369 years by the way. */
	ts->tv_sec = (long)(li.QuadPart / (unsigned __int64)10000000 - (unsigned __int64)11644473600);
	ts->tv_nsec = (long)(li.QuadPart % (unsigned __int64)10000000) * 100;

	return base;
}

int _c11threads_win32_timespec64_get(struct _c11threads_win32_timespec64_t *ts, int base)
{
	FILETIME file_time;
	ULARGE_INTEGER li;

	if (base != TIME_UTC) {
		return 0;
	}

	GetSystemTimeAsFileTime(&file_time);

	li.LowPart = file_time.dwLowDateTime;
	li.HighPart = file_time.dwHighDateTime;

	/* Also subtract difference between FILETIME and UNIX time epoch. It's 369 years by the way. */
	ts->tv_sec = li.QuadPart / (unsigned __int64)10000000 - (unsigned __int64)11644473600;
	ts->tv_nsec = (long)(li.QuadPart % (unsigned __int64)10000000) * 100;

	return base;
}
#else
int _c11threads_win32_timespec32_get(struct _c11threads_win32_timespec32_t *ts, int base)
{
	return _timespec32_get((struct _timespec32*)ts, base);
}

int _c11threads_win32_timespec64_get(struct _c11threads_win32_timespec64_t *ts, int base)
{
	return _timespec64_get((struct _timespec64*)ts, base);
}
#endif

/* ---- thread management ---- */

static void *_c11threads_win32_thrd_pop_entry(thrd_t thrd)
{
	void *h;
	struct _c11threads_win32_thrd_entry_t *prev;
	struct _c11threads_win32_thrd_entry_t *curr;

	h = NULL;
	prev = NULL;

	EnterCriticalSection(&_c11threads_win32_thrd_list_critical_section);
	curr = _c11threads_win32_thrd_list;
	while (curr) {
		if (curr->thrd == thrd) {
			if (prev) {
				prev->next = curr->next;
			} else {
				_c11threads_win32_thrd_list = curr->next;
			}
			h = curr->h;
			free(curr);
			break;
		}
		prev = curr;
		curr = curr->next;
	}
	LeaveCriticalSection(&_c11threads_win32_thrd_list_critical_section);

	return h;
}

static void _c11threads_win32_thrd_run_tss_dtors(void)
{
	int ran_dtor;
	size_t i;
	struct _c11threads_win32_tss_dtor_entry_t *prev;
	struct _c11threads_win32_tss_dtor_entry_t *curr;
	struct _c11threads_win32_tss_dtor_entry_t *temp;
	void *val;

	EnterCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);
	ran_dtor = 1;
	for (i = 0; i < TSS_DTOR_ITERATIONS && ran_dtor; ++i) {
		ran_dtor = 0;
		prev = NULL;
		curr = _c11threads_win32_tss_dtor_list;
		while (curr) {
			val = TlsGetValue(curr->key);
			if (val) {
				TlsSetValue(curr->key, NULL);
				curr->dtor(val);
				ran_dtor = 1;
			} else if (GetLastError() != ERROR_SUCCESS) {
				temp = curr->next;
				free(curr);
				curr = temp;
				if (prev) {
					prev->next = curr;
				} else if (!curr) {
					/* List empty. */
					_c11threads_win32_tss_dtor_list = NULL;
					LeaveCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);
					return;
				}
				continue;
			}
			prev = curr;
			curr = curr->next;
		}
	}
	LeaveCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);
}

struct _c11threads_win32_thrd_start_thunk_parameters_t {
	thrd_start_t func;
	void *arg;
};

static int __stdcall _c11threads_win32_thrd_start_thunk(struct _c11threads_win32_thrd_start_thunk_parameters_t *start_parameters)
{
	struct _c11threads_win32_thrd_start_thunk_parameters_t local_start_params;
	local_start_params = *start_parameters;
	free(start_parameters);
	return local_start_params.func(local_start_params.arg);
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
	struct _c11threads_win32_thrd_start_thunk_parameters_t *thread_start_params;
	struct _c11threads_win32_thrd_entry_t *thread_entry;
	void *h;
	struct _c11threads_win32_thrd_entry_t **curr;

	thread_start_params = malloc(sizeof(*thread_start_params));
	if (!thread_start_params) {
		return thrd_nomem;
	}

	thread_start_params->func = func;
	thread_start_params->arg = arg;

	thread_entry = malloc(sizeof(*thread_entry));
	if (!thread_entry) {
		free(thread_start_params);
		return thrd_nomem;
	}

	thread_entry->next = NULL;

	EnterCriticalSection(&_c11threads_win32_thrd_list_critical_section);
	h = CreateThread(NULL, 0, (PTHREAD_START_ROUTINE)_c11threads_win32_thrd_start_thunk, thread_start_params, 0, thr);
	if (!h) {
		unsigned long error;
		LeaveCriticalSection(&_c11threads_win32_thrd_list_critical_section);
		error = GetLastError();
		free(thread_start_params);
		free(thread_entry);
		return error == ERROR_NOT_ENOUGH_MEMORY ? thrd_nomem : thrd_error;
	}

	thread_entry->h = h;
	thread_entry->thrd = *thr;

	curr = &_c11threads_win32_thrd_list;
	while (*curr) {
		curr = &(*curr)->next;
	}
	*curr = thread_entry;
	LeaveCriticalSection(&_c11threads_win32_thrd_list_critical_section);

	return thrd_success;
}

void thrd_exit(int res)
{
	ExitThread(res);
}

int thrd_join(thrd_t thr, int *res)
{
	int ret;
	void *h;

	ret = thrd_error;
	h = _c11threads_win32_thrd_pop_entry(thr);
	if (h) {
		if (WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && (!res || GetExitCodeThread(h, (unsigned long*)res))) {
			ret = thrd_success;
		}

		CloseHandle(h);
	}

	return ret;
}

int thrd_detach(thrd_t thr)
{
	void *h;
	h = _c11threads_win32_thrd_pop_entry(thr);
	return h && CloseHandle(h) ? thrd_success : thrd_error;
}

thrd_t thrd_current(void)
{
	return GetCurrentThreadId();
}

static int _c11threads_win32_sleep_common(__int64 file_time_in)
{
	void *timer;
	unsigned long error;
	LARGE_INTEGER due_time;

	timer = CreateWaitableTimerW(NULL, 1, NULL);
	if (!timer) {
		error = GetLastError();
		return error > 1 ? -(long)error : -ERROR_INTERNAL_ERROR;
	}

	due_time.QuadPart = -file_time_in;
	if (!SetWaitableTimer(timer, &due_time, 0, NULL, NULL, 0)) {
		error = GetLastError();
		CloseHandle(timer);
		return error > 1 ? -(long)error : -ERROR_INTERNAL_ERROR;
	}

	if (WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) {
		error = GetLastError();
		CloseHandle(timer);
		return error > 1 ? -(long)error : -ERROR_INTERNAL_ERROR;
	}

	CloseHandle(timer);
	return 0; /* Success. */
}

int _c11threads_win32_thrd_sleep32(const struct _c11threads_win32_timespec32_t *ts_in, struct _c11threads_win32_timespec32_t *rem_out)
{
	__int64 file_time;
	int res;

	(void)rem_out;

	if (!_c11threads_win32_util_is_timespec32_valid(ts_in)) {
		return -ERROR_INVALID_PARAMETER;
	}

	file_time = _c11threads_win32_util_timespec32_to_file_time(ts_in);
	if (file_time < 0) {
		return -ERROR_INVALID_PARAMETER;
	}

	res = _c11threads_win32_sleep_common(file_time);

	return res;
}

int _c11threads_win32_thrd_sleep64(const struct _c11threads_win32_timespec64_t *ts_in, struct _c11threads_win32_timespec64_t *rem_out)
{
	__int64 file_time;
	size_t periods;
	int res;

	(void)rem_out;

	if (!_c11threads_win32_util_is_timespec64_valid(ts_in)) {
		return -ERROR_INVALID_PARAMETER;
	}

	file_time = _c11threads_win32_util_timespec64_to_file_time(ts_in, &periods);
	if (file_time < 0) {
		return -ERROR_INVALID_PARAMETER;
	}

restart_sleep:
	res = _c11threads_win32_sleep_common(file_time);

	if (!res && periods) {
		--periods;
		file_time = (__int64)9223372036850000000;
		goto restart_sleep;
	}

	return res;
}

void thrd_yield(void)
{
	SwitchToThread();
}

/* ---- mutexes ---- */

int mtx_init(mtx_t *mtx, int type)
{
	(void)type;
#ifdef _MSC_VER
#pragma warning(suppress: 28125) /* Warning C28125: The function 'InitializeCriticalSection' must be called from within a try/except block. */
#endif
	InitializeCriticalSection((PCRITICAL_SECTION)mtx);
	return thrd_success;
}

void mtx_destroy(mtx_t *mtx)
{
	DeleteCriticalSection((PCRITICAL_SECTION)mtx);
}

int mtx_lock(mtx_t *mtx)
{
	EnterCriticalSection((PCRITICAL_SECTION)mtx);
	return thrd_success;
}

int mtx_trylock(mtx_t *mtx)
{
	return TryEnterCriticalSection((PCRITICAL_SECTION)mtx) ? thrd_success : thrd_busy;
}

int _c11threads_win32_mtx_timedlock32(mtx_t *mtx, const struct _c11threads_win32_timespec32_t *ts)
{
	int success;
	struct _c11threads_win32_timespec32_t ts_current;

	if (!_c11threads_win32_util_is_timespec32_valid(ts)) {
		return thrd_error;
	}

	success = TryEnterCriticalSection((PCRITICAL_SECTION)mtx);
	while (!success) {
		if (!_c11threads_win32_timespec32_get(&ts_current, TIME_UTC)) {
			return thrd_error;
		}

		if (ts_current.tv_sec > ts->tv_sec || (ts_current.tv_sec == ts->tv_sec && ts_current.tv_nsec >= ts->tv_nsec)) {
			return thrd_timedout;
		}

		Sleep(0);

		success = TryEnterCriticalSection((PCRITICAL_SECTION)mtx);
	}

	return thrd_success;
}

int _c11threads_win32_mtx_timedlock64(mtx_t *mtx, const struct _c11threads_win32_timespec64_t *ts)
{
	int success;
	struct _c11threads_win32_timespec64_t ts_current;

	if (!_c11threads_win32_util_is_timespec64_valid(ts)) {
		return thrd_error;
	}

	success = TryEnterCriticalSection((PCRITICAL_SECTION)mtx);
	while (!success) {
		if (!_c11threads_win32_timespec64_get(&ts_current, TIME_UTC)) {
			return thrd_error;
		}

		if (ts_current.tv_sec > ts->tv_sec || (ts_current.tv_sec == ts->tv_sec && ts_current.tv_nsec >= ts->tv_nsec)) {
			return thrd_timedout;
		}

		Sleep(0);

		success = TryEnterCriticalSection((PCRITICAL_SECTION)mtx);
	}

	return thrd_success;
}

int mtx_unlock(mtx_t *mtx)
{
	LeaveCriticalSection((PCRITICAL_SECTION)mtx);
	return thrd_success;
}

/* ---- condition variables ---- */

struct _c11threads_win32_cnd_t {
	void *mutex;
	void *signal_sema;
	void *broadcast_event;
	size_t wait_count;
};

int cnd_init(cnd_t *cond)
{
	if (_c11threads_win32_winver >= _WIN32_WINNT_VISTA) {
		_c11threads_win32_InitializeConditionVariable(cond);
		return thrd_success;
	} else {
		struct _c11threads_win32_cnd_t *cnd;

		cnd = malloc(sizeof(*cnd));
		if (!cnd) {
			return thrd_nomem;
		}

		cnd->mutex = CreateMutexW(NULL, 0, NULL);
		if (cnd->mutex) {
			cnd->signal_sema = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
			if (cnd->signal_sema) {
				cnd->broadcast_event = CreateEventW(NULL, 1, 0, NULL);
				if (cnd->broadcast_event) {
					cnd->wait_count = 0;
					*cond = cnd;
					return thrd_success;
				}
				CloseHandle(cnd->signal_sema);
			}
			CloseHandle(cnd->mutex);
		}

		free(cnd);
		return thrd_error;
	}
}

void cnd_destroy(cnd_t *cond)
{
	if (_c11threads_win32_winver < _WIN32_WINNT_VISTA) {
		struct _c11threads_win32_cnd_t *cnd;
		cnd = *cond;
		CloseHandle(cnd->mutex);
		CloseHandle(cnd->signal_sema);
		CloseHandle(cnd->broadcast_event);
		free(cnd);
	}
}

int cnd_signal(cnd_t *cond)
{
	if (_c11threads_win32_winver >= _WIN32_WINNT_VISTA) {
		_c11threads_win32_WakeConditionVariable(cond);
		return thrd_success;
	} else {
		struct _c11threads_win32_cnd_t *cnd;
		int success;
		unsigned long wait_status;

		cnd = *cond;

		success = 0;
		wait_status = WaitForSingleObject(cnd->mutex, INFINITE);
		if (wait_status == WAIT_OBJECT_0) {
			success = 1;
		} else if (wait_status == WAIT_ABANDONED) {
			abort();
		}

		if (success) {
			if (cnd->wait_count) {
				success = ReleaseSemaphore(cnd->signal_sema, 1, NULL) || GetLastError() == ERROR_TOO_MANY_POSTS;
			}
			if (!ReleaseMutex(cnd->mutex)) {
				success = 0;
			}
		}

		return success ? thrd_success : thrd_error;
	}
}

int cnd_broadcast(cnd_t *cond)
{
	if (_c11threads_win32_winver >= _WIN32_WINNT_VISTA) {
		_c11threads_win32_WakeAllConditionVariable(cond);
		return thrd_success;
	} else {
		struct _c11threads_win32_cnd_t *cnd;
		int success;
		unsigned long wait_status;

		cnd = *cond;

		success = 0;
		wait_status = WaitForSingleObject(cnd->mutex, INFINITE);
		if (wait_status == WAIT_OBJECT_0) {
			success = 1;
		} else if (wait_status == WAIT_ABANDONED) {
			abort();
		}

		if (success) {
			if (cnd->wait_count) {
				success = SetEvent(cnd->broadcast_event);
			}
			if (!ReleaseMutex(cnd->mutex)) {
				success = 0;
			}
		}

		return success ? thrd_success : thrd_error;
	}
}

static int _c11threads_win32_cnd_wait_common(cnd_t *cond, mtx_t *mtx, unsigned long wait_time, int clamped)
{
	if (_c11threads_win32_winver >= _WIN32_WINNT_VISTA) {
		if (_c11threads_win32_SleepConditionVariableCS(cond, (PCRITICAL_SECTION)mtx, wait_time)) {
			return thrd_success;
		}

		if (GetLastError() == ERROR_TIMEOUT) {
			return clamped ? thrd_success : thrd_timedout;
		}

		return thrd_error;
	} else {
		struct _c11threads_win32_cnd_t *cnd;
		unsigned long wait_status;
		unsigned long wait_status_2;
		int res;

		cnd = *cond;

		wait_status = WaitForSingleObject(cnd->mutex, INFINITE);
		if (wait_status == WAIT_ABANDONED) {
			abort();
		} else if (wait_status != WAIT_OBJECT_0) {
			return thrd_error;
		}
		LeaveCriticalSection((PCRITICAL_SECTION)mtx);
		++cnd->wait_count;
		if (!ReleaseMutex(cnd->mutex)) {
			abort();
		}

		wait_status = WaitForMultipleObjects(2, &cnd->signal_sema /* and cnd->broadcast_event */, 0, wait_time);

		if (WaitForSingleObject(cnd->mutex, INFINITE) != WAIT_OBJECT_0) {
			abort();
		}
		--cnd->wait_count;
		if (!cnd->wait_count) {
			do {
				wait_status_2 = WaitForSingleObject(cnd->signal_sema, 0);
			} while (wait_status_2 == WAIT_OBJECT_0);
			if (wait_status_2 != WAIT_TIMEOUT) {
				abort();
			}

			if (!ResetEvent(cnd->broadcast_event)) {
				abort();
			}
		}
		if (!ReleaseMutex(cnd->mutex)) {
			abort();
		}

		res = thrd_success;
		if (wait_status == WAIT_TIMEOUT) {
			if (!clamped) {
				res = thrd_timedout;
			}
		} else if (wait_status != WAIT_OBJECT_0 && wait_status != WAIT_OBJECT_0 + 1) {
			res = thrd_error;
		}

		EnterCriticalSection((PCRITICAL_SECTION)mtx);
		return res;
	}
}

int cnd_wait(cnd_t *cond, mtx_t *mtx)
{
	return _c11threads_win32_cnd_wait_common(cond, mtx, INFINITE, 0);
}

int _c11threads_win32_cnd_timedwait32(cnd_t *cond, mtx_t *mtx, const struct _c11threads_win32_timespec32_t *ts)
{
	struct _c11threads_win32_timespec32_t current_time;
	unsigned long wait_time;
	int clamped;

	if (!_c11threads_win32_util_is_timespec32_valid(ts)) {
		return thrd_error;
	}

	if (!_c11threads_win32_timespec32_get(&current_time, TIME_UTC)) {
		return thrd_error;
	}

	wait_time = _c11threads_win32_util_timepoint_to_millisecond_timespan32(&current_time, ts, &clamped);

	return _c11threads_win32_cnd_wait_common(cond, mtx, wait_time, clamped);
}

int _c11threads_win32_cnd_timedwait64(cnd_t *cond, mtx_t *mtx, const struct _c11threads_win32_timespec64_t *ts)
{
	struct _c11threads_win32_timespec64_t current_time;
	unsigned long wait_time;
	int clamped;

	if (!_c11threads_win32_util_is_timespec64_valid(ts)) {
		return thrd_error;
	}

	if (!_c11threads_win32_timespec64_get(&current_time, TIME_UTC)) {
		return thrd_error;
	}

	wait_time = _c11threads_win32_util_timepoint_to_millisecond_timespan64(&current_time, ts, &clamped);

	return _c11threads_win32_cnd_wait_common(cond, mtx, wait_time, clamped);
}

/* ---- thread-specific data ---- */

static int _c11threads_win32_tss_register(tss_t key, tss_dtor_t dtor) {
	struct _c11threads_win32_tss_dtor_entry_t *tss_dtor_entry;
	struct _c11threads_win32_tss_dtor_entry_t **curr;

	tss_dtor_entry = malloc(sizeof(*tss_dtor_entry));
	if (!tss_dtor_entry) {
		return 0;
	}

	tss_dtor_entry->key = key;
	tss_dtor_entry->dtor = dtor;
	tss_dtor_entry->next = NULL;

	EnterCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);
	curr = &_c11threads_win32_tss_dtor_list;
	while (*curr) {
		curr = &(*curr)->next;
	}
	*curr = tss_dtor_entry;
	LeaveCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);

	return 1;
}

static void _c11threads_win32_tss_deregister(tss_t key) {
	struct _c11threads_win32_tss_dtor_entry_t *prev;
	struct _c11threads_win32_tss_dtor_entry_t *curr;

	prev = NULL;

	EnterCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);
	curr = _c11threads_win32_tss_dtor_list;
	while (curr) {
		if (curr->key == key) {
			if (prev) {
				prev->next = curr->next;
			} else {
				_c11threads_win32_tss_dtor_list = curr->next;
			}
			break;
		}
		prev = curr;
		curr = curr->next;
	}
	LeaveCriticalSection(&_c11threads_win32_tss_dtor_list_critical_section);

	free(curr);
}

int tss_create(tss_t *key, tss_dtor_t dtor)
{
	*key = TlsAlloc();
	if (*key == TLS_OUT_OF_INDEXES) {
		return thrd_error;
	}
	if (dtor && !_c11threads_win32_tss_register(*key, dtor)) {
		TlsFree(*key);
		return thrd_error;
	}
	return thrd_success;
}

void tss_delete(tss_t key)
{
	_c11threads_win32_tss_deregister(key);
	TlsFree(key);
}

int tss_set(tss_t key, void *val)
{
	return TlsSetValue(key, val) ? thrd_success : thrd_error;
}

void *tss_get(tss_t key)
{
	return TlsGetValue(key);
}

/* ---- misc ---- */

static int __stdcall _c11threads_win32_call_once_thunk(void *init_once, void (*func)(void), void **context)
{
	(void)init_once;
	(void)context;
	func();
	return 1;
}

void call_once(once_flag *flag, void (*func)(void))
{
	if (_c11threads_win32_winver >= _WIN32_WINNT_VISTA) {
#ifdef _MSC_VER
#pragma warning(push)
/* Warning C4054: 'type cast' : from function pointer 'int (__stdcall *)(void *,void (__cdecl *)(void),void **)' to data pointer 'const void *' */
/* Warning C4054: 'type cast' : from function pointer 'void (__cdecl *)(void)' to data pointer 'void *' */
#pragma warning(disable: 4054)
#endif
		_c11threads_win32_InitOnceExecuteOnce((void*)flag, (const void*)_c11threads_win32_call_once_thunk, (void*)func, NULL);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	} else {
		if (InterlockedCompareExchange((long*)flag, 1, 0) == 0) {
			func();
			InterlockedExchange((long*)flag, 2);
		} else {
			while (*(volatile long*)flag == 1) {
				Sleep(0);
			}
		}
	}
}

#endif
