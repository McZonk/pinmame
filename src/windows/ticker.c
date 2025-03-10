//============================================================
//
//	ticker.c - Win32 timing code
//
//============================================================

// standard windows headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#if _MSC_VER >= 1800
 // Windows 2000 _WIN32_WINNT_WIN2K
 #define _WIN32_WINNT 0x0500
#elif _MSC_VER < 1600
 #define _WIN32_WINNT 0x0400
#else
 #define _WIN32_WINNT 0x0403
#endif
#define WINVER _WIN32_WINNT
#endif
#include <windows.h>
#include <mmsystem.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// MAME headers
#include "driver.h"

//#define USE_LOWLEVEL_PRECISION_SETTING // does allow to pick lower windows timer resolutions than 1ms (usually 0.5ms as of win10/2020) via undocumented API calls, BUT leads to sound distortion on some setups

//============================================================
//	PROTOTYPES
//============================================================

static cycles_t init_cycle_counter(void);
static cycles_t performance_cycle_counter(void);
static cycles_t rdtsc_cycle_counter(void);


//============================================================
//	GLOBAL VARIABLES
//============================================================

// global cycle_counter function and divider
cycles_t		(*cycle_counter)(void) = init_cycle_counter;
cycles_t		cycles_per_sec;


//============================================================
//	STATIC VARIABLES
//============================================================

static cycles_t suspend_adjustment;
static cycles_t suspend_time;


//============================================================
//	init_cycle_counter
//============================================================

static cycles_t init_cycle_counter(void)
{
	LARGE_INTEGER frequency;

	suspend_adjustment = 0;
	suspend_time = 0;

	if (QueryPerformanceFrequency( &frequency ))
	{
		cycle_counter = performance_cycle_counter;
		logerror("using performance counter for timing ... ");
		cycles_per_sec = frequency.QuadPart;
		logerror("cycles/second = %llu\n", cycles_per_sec);
	}
	else
	{
		logerror("NO QueryPerformanceFrequency available");
	}

	// return the current cycle count
	return (*cycle_counter)();
}



//============================================================
//	performance_cycle_counter
//============================================================

static cycles_t performance_cycle_counter(void)
{
	LARGE_INTEGER performance_count;
	QueryPerformanceCounter( &performance_count );
	return (cycles_t)performance_count.QuadPart;
}



//============================================================
//	rdtsc_cycle_counter
//============================================================

#ifdef _MSC_VER

#ifndef __LP64__
static cycles_t rdtsc_cycle_counter(void)
{
	INT64 result;
	INT64 *presult = &result;

	__asm {

		rdtsc
		mov ebx, presult
		mov [ebx],eax
		mov [ebx+4],edx
	}

	return result;
}
#else
#if defined(_M_ARM64)
// See https://docs.microsoft.com/en-us/cpp/intrinsics/arm64-intrinsics?view=vs-2019
// and https://reviews.llvm.org/D53115
#include <stdint.h>
static cycles_t rdtsc_cycle_counter(void)
{
	const int64_t virtual_timer_value = _ReadStatusReg(ARM64_CNTVCT);
	return virtual_timer_value;
}
#else
static cycles_t rdtsc_cycle_counter(void)
{
	return __rdtsc();
}
#endif
#endif

#else

static cycles_t rdtsc_cycle_counter(void)
{
	INT64 result;

	// use RDTSC
	__asm__ __volatile__ (
		"rdtsc"
		: "=A" (result)
	);

	return result;
}

#endif


//============================================================
//	osd_cycles
//============================================================

cycles_t osd_cycles(void)
{
	return suspend_time ? suspend_time : (*cycle_counter)() - suspend_adjustment;
}



//============================================================
//	osd_cycles_per_second
//============================================================

cycles_t osd_cycles_per_second(void)
{
	return cycles_per_sec;
}



//============================================================
//	osd_profiling_ticks
//============================================================

cycles_t osd_profiling_ticks(void)
{
	return rdtsc_cycle_counter(); //!! meh, but only used for profiling
}



//============================================================
//	win_timer_enable
//============================================================

void win_timer_enable(int enabled)
{
	cycles_t actual_cycles = (*cycle_counter)();
	if (!enabled)
	{
		suspend_time = actual_cycles;
	}
	else if (suspend_time > 0)
	{
		suspend_adjustment += actual_cycles - suspend_time;
		suspend_time = 0;
	}
}

//

static unsigned int sTimerInit = 0;
static LARGE_INTEGER TimerFreq;
static LARGE_INTEGER sTimerStart;
static LONGLONG OneMSTimerTicks;
static LONGLONG TwoMSTimerTicks;
static char highrestimer;

#if _WIN32_WINNT < 0x0600
typedef HANDLE(WINAPI* pCWTEA)(LPSECURITY_ATTRIBUTES lpTimerAttributes, LPCSTR lpTimerName, DWORD dwFlags, DWORD dwDesiredAccess);
static pCWTEA CreateWaitableTimerEx = NULL;
#endif

static void wintimer_init(void)
{
	sTimerInit = 1;

	QueryPerformanceFrequency(&TimerFreq);
	OneMSTimerTicks = (1000 * TimerFreq.QuadPart) / 1000000ull;
	TwoMSTimerTicks = (2000 * TimerFreq.QuadPart) / 1000000ull;
	QueryPerformanceCounter(&sTimerStart);

#if _WIN32_WINNT >= 0x0600
	HANDLE timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); // ~0.5msec resolution (unless usec < ~10 requested, which most likely triggers a spin loop then), Win10 and above only, note that this timer variant then also would not require to call timeBeginPeriod(1) before!
	highrestimer = !!timer;
	if (timer)
		CloseHandle(timer);
#else
	CreateWaitableTimerEx = (pCWTEA)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "CreateWaitableTimerExA");
	if (CreateWaitableTimerEx)
	{
		HANDLE timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); // ~0.5msec resolution (unless usec < ~10 requested, which most likely triggers a spin loop then), Win10 and above only, note that this timer variant then also would not require to call timeBeginPeriod(1) before!
		highrestimer = !!timer;
		if (timer)
			CloseHandle(timer);
	}
	else
		highrestimer = 0;
#endif
}

// tries(!) to be as exact as possible at the cost of potentially causing trouble with other threads/cores due to OS madness
// needs timeBeginPeriod(1) before calling 1st time to make the Sleep(1) in here behave more or less accurately (and timeEndPeriod(1) after not needing that precision anymore)
// but MAME code does this already
void uSleep(const UINT64 u)
{
	LARGE_INTEGER TimerEnd;
	LARGE_INTEGER TimerNow;

	if (sTimerInit == 0)
		wintimer_init();

	QueryPerformanceCounter(&TimerNow);
	TimerEnd.QuadPart = TimerNow.QuadPart + ((u * TimerFreq.QuadPart) / 1000000ull);

	while (TimerNow.QuadPart < TimerEnd.QuadPart)
	{
		if ((TimerEnd.QuadPart - TimerNow.QuadPart) > TwoMSTimerTicks)
			Sleep(1); // really pause thread for 1-2ms (depending on OS)
		else if (highrestimer && ((TimerEnd.QuadPart - TimerNow.QuadPart) > OneMSTimerTicks)) // pause thread for 0.5-1ms
		{
			HANDLE timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); // ~0.5msec resolution (unless usec < ~10 requested, which most likely triggers a spin loop then), Win10 and above only, note that this timer variant then also would not require to call timeBeginPeriod(1) before!
			LARGE_INTEGER ft;
			ft.QuadPart = -10 * 500; // 500 usec //!! we could go lower if some future OS (>win10) actually supports this
			SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
			WaitForSingleObject(timer, INFINITE);
			CloseHandle(timer);
		}
		else
#ifdef __MINGW32__
			{__asm__ __volatile__("pause");}
#else
			YieldProcessor(); // was: "SwitchToThread() let other threads on same core run" //!! could also try Sleep(0) or directly use _mm_pause() instead of YieldProcessor() here
#endif

		QueryPerformanceCounter(&TimerNow);
	}
}

// can sleep too long by 500-1000 (=0.5 to 1ms) or 1000-2000 (=1 to 2ms) on older windows versions
// needs timeBeginPeriod(1) before calling 1st time to make the Sleep(1) in here behave more or less accurately (and timeEndPeriod(1) after not needing that precision anymore)
// but MAME code does this already
void uOverSleep(const UINT64 u)
{
	LARGE_INTEGER TimerEnd;
	LARGE_INTEGER TimerNow;

	if (sTimerInit == 0)
		wintimer_init();

	QueryPerformanceCounter(&TimerNow);
	TimerEnd.QuadPart = TimerNow.QuadPart + ((u * TimerFreq.QuadPart) / 1000000ull);

	while (TimerNow.QuadPart < TimerEnd.QuadPart)
	{
		if (!highrestimer || (TimerEnd.QuadPart - TimerNow.QuadPart) > TwoMSTimerTicks)
			Sleep(1); // really pause thread for 1-2ms (depending on OS)
		else // pause thread for 0.5-1ms
		{
			HANDLE timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); // ~0.5msec resolution (unless usec < ~10 requested, which most likely triggers a spin loop then), Win10 and above only, note that this timer variant then also would not require to call timeBeginPeriod(1) before!
			LARGE_INTEGER ft;
			ft.QuadPart = -10 * 500; // 500 usec //!! we could go lower if some future OS (>win10) actually supports this
			SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
			WaitForSingleObject(timer, INFINITE);
			CloseHandle(timer);
		}

		QueryPerformanceCounter(&TimerNow);
	}
}

// skips sleeping completely if u < 4000 (=4ms), otherwise will undersleep by -3000 to -2000 (=-3 to -2ms)
// needs timeBeginPeriod(1) before calling 1st time to make the Sleep(1) in here behave more or less accurately (and timeEndPeriod(1) after not needing that precision anymore)
// but MAME code does this already
void uUnderSleep(const UINT64 u)
{
	LARGE_INTEGER TimerEndSleep;
	LARGE_INTEGER TimerNow;

	if (sTimerInit == 0)
		wintimer_init();

	if (u <= 4000) // Sleep < 4ms? -> exit
		return;

	QueryPerformanceCounter(&TimerNow);
	TimerEndSleep.QuadPart = TimerNow.QuadPart + (((u - 4000ull) * TimerFreq.QuadPart) / 1000000ull);

	while (TimerNow.QuadPart < TimerEndSleep.QuadPart)
	{
		Sleep(1); // really pause thread for 1-2ms (depending on OS)
		QueryPerformanceCounter(&TimerNow);
	}
}

//

#ifdef USE_LOWLEVEL_PRECISION_SETTING
typedef LONG(CALLBACK* NTSETTIMERRESOLUTION)(IN ULONG DesiredTime,
	IN BOOLEAN SetResolution,
	OUT PULONG ActualTime);
static NTSETTIMERRESOLUTION NtSetTimerResolution;

typedef LONG(CALLBACK* NTQUERYTIMERRESOLUTION)(OUT PULONG MaximumTime,
	OUT PULONG MinimumTime,
	OUT PULONG CurrentTime);
static NTQUERYTIMERRESOLUTION NtQueryTimerResolution;

static HMODULE hNtDll = NULL;
static ULONG win_timer_old_period = -1;
#endif

static TIMECAPS win_timer_caps;
static MMRESULT win_timer_result = TIMERR_NOCANDO;

void set_lowest_possible_win_timer_resolution()
{
	// First crank up the multimedia timer resolution to its max
	// this gives the system much finer timeslices (usually 1-2ms)
	win_timer_result = timeGetDevCaps(&win_timer_caps, sizeof(win_timer_caps));
	if (win_timer_result == TIMERR_NOERROR)
		timeBeginPeriod(win_timer_caps.wPeriodMin);

	// Then try the even finer sliced (usually 0.5ms) low level variant
#ifdef USE_LOWLEVEL_PRECISION_SETTING 
	hNtDll = LoadLibrary("NtDll.dll");
	if (hNtDll) {
		NtQueryTimerResolution = (NTQUERYTIMERRESOLUTION)GetProcAddress(hNtDll, "NtQueryTimerResolution");
		NtSetTimerResolution = (NTSETTIMERRESOLUTION)GetProcAddress(hNtDll, "NtSetTimerResolution");
		if (NtQueryTimerResolution && NtSetTimerResolution) {
			ULONG min_period, tmp;
			NtQueryTimerResolution(&tmp, &min_period, &win_timer_old_period);
			if (min_period < 4500) // just to not screw around too much with the time (i.e. potential timer improvements in future HW/OSs), limit timer period to 0.45ms (picked 0.45 here instead of 0.5 as apparently some current setups can feature values just slightly below 0.5, so just leave them at this native rate then)
				min_period = 5000;
			if (min_period < 10000) // only set this if smaller 1ms, cause otherwise timeBeginPeriod already did the job
				NtSetTimerResolution(min_period, TRUE, &tmp);
			else
				win_timer_old_period = -1;
		}
	}
#endif
}

void restore_win_timer_resolution()
{
	// restore both timer resolutions
#ifdef USE_LOWLEVEL_PRECISION_SETTING
	if (hNtDll) {
		if (win_timer_old_period != -1)
		{
			ULONG tmp;
			NtSetTimerResolution(win_timer_old_period, FALSE, &tmp);
			win_timer_old_period = -1;
		}
		FreeLibrary(hNtDll);
		hNtDll = NULL;
	}
#endif

	if (win_timer_result == TIMERR_NOERROR)
	{
		timeEndPeriod(win_timer_caps.wPeriodMin);
		win_timer_result = TIMERR_NOCANDO;
	}
}
