/* AudioSafetyHookSEH.c — Pure C wrapper for waveOutOpen with SEH
 *
 * MSVC's /EHsc flag (default for C++) can cause __try/__except frames to be
 * stripped under /O2 + LTCG optimizations.  By putting the SEH wrapper in a
 * plain .c translation unit, we guarantee the C compiler always emits the
 * proper SEH prologue/epilogue regardless of optimization level.
 *
 * Returns 0xDEAD06BA as a sentinel if exception 0x6BA was caught.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

typedef MMRESULT(WINAPI *PFN_waveOutOpen)(
    LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen);

#pragma optimize("", off)          /* no optimizations — ensure SEH frame is emitted */

MMRESULT WINAPI CallWaveOutOpenSafe(
    PFN_waveOutOpen pfnOriginal,
    LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen)
{
    __try {
        return pfnOriginal(phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen);
    }
    __except (GetExceptionCode() == 0x000006BA   /* RPC_S_SERVER_UNAVAILABLE */
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH)
    {
        return (MMRESULT)0xDEAD06BAul;           /* sentinel — caller converts to MMSYSERR_NODRIVER */
    }
}

#pragma optimize("", on)
