/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "System.h"

#include "common/PersistentThread.h"
#include "common/emitter/tools.h"
#include "PINE.h"


using namespace Threading;

typedef SafeArray<u8> VmStateBuffer;

enum SystemsMask : uint8_t
{
	System_GS = 1 << 0,
	System_SPU2 = 1 << 1,
	System_PAD = 1 << 2,
	System_FW = 1 << 3,
	System_CDVD = 1 << 4,
	System_USB = 1 << 5,
	System_DEV9 = 1 << 6,
	System_MCD = 1 << 7,
};

// --------------------------------------------------------------------------------------
//  SysThreadBase
// --------------------------------------------------------------------------------------

class SysThreadBase : public pxThread
{
	typedef pxThread _parent;

public:
	// Important: The order of these enumerations matters!  Optimized tests are used for both
	// Closed and Paused states.
	enum ExecutionMode
	{
		// Thread has not been created yet.  Typically this is the same as IsRunning()
		// returning FALSE.
		ExecMode_NoThreadYet,

		// Thread is safely paused, with subcomponents in a "closed" state, and waiting for a
		// resume/open signal.
		ExecMode_Closed,

		// Thread is safely paused, with subcomponents in an "open" state, and waiting for a
		// resume/open signal.
		ExecMode_Paused,

		// Thread is active and running, with pluigns in an "open" state.
		ExecMode_Opened,

		// Close signal has been sent to the thread, but the thread's response is still
		// pending (thread is busy/running).
		ExecMode_Closing,

		// Pause signal has been sent to the thread, but the thread's response is still
		// pending (thread is busy/running).
		ExecMode_Pausing,
	};

protected:
	std::atomic<ExecutionMode> m_ExecMode;
	std::atomic<SystemsMask> m_SystemsToTearDown {};

	// This lock is used to avoid simultaneous requests to Suspend/Resume/Pause from
	// contending threads.
	MutexRecursive m_ExecModeMutex;

	// Used to wake up the thread from sleeping when it's in a suspended state.
	Semaphore m_sem_Resume;

	// Used to synchronize inline changes from paused to suspended status.
	Semaphore m_sem_ChangingExecMode;

	// Locked whenever the thread is not in a suspended state (either closed or paused).
	// Issue a Wait against this mutex for performing actions that require the thread
	// to be suspended.
	Mutex m_RunningLock;

public:
	explicit SysThreadBase();
	virtual ~SysThreadBase() = default;

	// Thread safety for IsOpen / IsClosed: The execution mode can change at any time on
	// any thread, so the actual status may have already changed by the time this function
	// returns its result.  Typically this isn't of major concern.  However if you need
	// more assured execution mode status, issue a lock against the ExecutionModeMutex()
	// first.
	bool IsOpen() const
	{
		return IsRunning() && (m_ExecMode > ExecMode_Closed);
	}

	bool IsClosed() const { return !IsOpen(); }
	bool IsPaused() const { return !IsRunning() || (m_ExecMode <= ExecMode_Paused); }

	bool IsClosing() const
	{
		return !IsRunning() || (m_ExecMode <= ExecMode_Closed) || (m_ExecMode == ExecMode_Closing);
	}

	bool HasPendingStateChangeRequest() const
	{
		return m_ExecMode >= ExecMode_Closing;
	}

	ExecutionMode GetExecutionMode() const { return m_ExecMode.load(); }
	Mutex& ExecutionModeMutex() { return m_ExecModeMutex; }

	virtual void Suspend(bool isBlocking = true);
	virtual void Resume();
	virtual void Pause(SystemsMask systemsToTearDown, bool debug = false);
	virtual void PauseSelf();
	virtual void PauseSelfDebug();

protected:
	virtual void OnStart();

	// This function is called by Resume immediately prior to releasing the suspension of
	// the core emulation thread.  You should overload this rather than Resume(), since
	// Resume() has a lot of checks and balances to prevent re-entrance and race conditions.
	virtual void OnResumeReady() {}
	virtual void OnPause() {}
	virtual void OnPauseDebug() {}

	virtual bool StateCheckInThread();
	virtual void OnCleanupInThread();
	virtual void OnStartInThread();

	// Used internally from Resume(), so let's make it private here.
	virtual void Start();

	// Extending classes should implement this, but should not call it.  The parent class
	// handles invocation by the following guidelines: Called *in thread* from StateCheckInThread()
	// prior to suspending the thread (ie, when Suspend() has been called on a separate
	// thread, requesting this thread suspend itself temporarily).  After this is called,
	// the thread enters a waiting state on the m_sem_Resume semaphore.
	virtual void OnSuspendInThread() = 0;

	// Extending classes should implement this, but should not call it.  The parent class
	// handles invocation by the following guidelines: Called *in thread* from StateCheckInThread()
	// prior to pausing the thread (ie, when Pause() has been called on a separate thread,
	// requesting this thread pause itself temporarily).  After this is called, the thread
	// enters a waiting state on the m_sem_Resume semaphore.
	// Parameter:
	//   systemsToTearDown - a bitmask of systems to call Close functions on.
	virtual void OnPauseInThread(SystemsMask systemsToTearDown) = 0;

	// Extending classes should implement this, but should not call it.  The parent class
	// handles invocation by the following guidelines: Called from StateCheckInThread() after the
	// thread has been suspended and then subsequently resumed.
	// Parameter:
	//   systemsToTearDown - a bitmask of systems to call Open functions on.
	virtual void OnResumeInThread(SystemsMask systemsToReinstate) = 0;
};


// --------------------------------------------------------------------------------------
//  SysCoreThread class
// --------------------------------------------------------------------------------------
class SysCoreThread : public SysThreadBase
{
	typedef SysThreadBase _parent;

protected:
	bool m_resetRecompilers;
	bool m_resetProfilers;
	bool m_resetVsyncTimers;
	bool m_resetVirtualMachine;

	// Stores the state of the PINE thread.
	std::unique_ptr<PINEServer> m_pineServer;

	// Current state of the PINE thread
	enum StatePINE
	{
		OFF,
		ON
	};
	StatePINE m_PineState = OFF;

	// Indicates if the system has an active virtual machine state.  Pretty much always
	// true anytime between subcomponents being initialized and being shutdown.  Gets
	// set false when they are shutdown, the corethread is canceled, or when an error
	// occurs while trying to upload a new state into the VM.
	std::atomic<bool> m_hasActiveMachine;

	wxString m_elf_override;

	SSE_MXCSR m_mxcsr_saved;

public:
	explicit SysCoreThread();
	virtual ~SysCoreThread();

	bool HasPendingStateChangeRequest() const;

	virtual void OnResumeReady() override;
	virtual void Reset();
	virtual void ResetQuick();
	virtual void Cancel(bool isBlocking = true) override;
	virtual bool Cancel(const wxTimeSpan& timeout) override;

	virtual bool StateCheckInThread() override;
	virtual void VsyncInThread();
	virtual void GameStartingInThread();

	virtual void ApplySettings(const Pcsx2Config& src);

	virtual bool HasActiveMachine() const { return m_hasActiveMachine; }

	virtual const wxString& GetElfOverride() const { return m_elf_override; }
	virtual void SetElfOverride(const wxString& elf);

protected:
	void _reset_stuff_as_needed();

	virtual void Start() override;
	virtual void OnStart() override;
	virtual void OnSuspendInThread() override;
	virtual void OnPauseInThread(SystemsMask systemsToTearDown) override { TearDownSystems(systemsToTearDown); }
	virtual void OnResumeInThread(SystemsMask systemsToReinstate) override;
	virtual void OnCleanupInThread() override;
	virtual void ExecuteTaskInThread() override;
	virtual void DoCpuReset();
	virtual void DoCpuExecute();

	void _StateCheckThrows();

private:
	void TearDownSystems(SystemsMask systemsToTearDown);
};


struct SysStateUnlockedParams
{
	SysStateUnlockedParams() {}
};

// --------------------------------------------------------------------------------------
//  IEventListener_SaveStateThread
// --------------------------------------------------------------------------------------
class IEventListener_SysState : public IEventDispatcher<SysStateUnlockedParams>
{
public:
	typedef SysStateUnlockedParams EvtParams;

public:
	IEventListener_SysState() {}
	virtual ~IEventListener_SysState() = default;

	virtual void DispatchEvent(const SysStateUnlockedParams& status)
	{
		SysStateAction_OnUnlocked();
	}

protected:
	virtual void SysStateAction_OnUnlocked();
};

// GetCoreThread() is a required external implementation. This function is *NOT*
// provided by the PCSX2 core library.  It provides an interface for the linking User
// Interface apps or DLLs to reference their own instance of SysCoreThread (also allowing
// them to extend the class and override virtual methods).
//
extern SysCoreThread& GetCoreThread();

extern bool g_CDVDReset;

namespace PINESettings
{
	extern unsigned int slot;
};
