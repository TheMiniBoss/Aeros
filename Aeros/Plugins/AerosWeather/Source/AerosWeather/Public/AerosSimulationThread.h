// Copyright Aeros. All Rights Reserved.
//
// Runs FAerosSimulation on a dedicated background thread (roadmap: FRunnable).
// The game thread only:
//   - moves a target time forward (atomic),
//   - pushes commands (lock-free MPSC queue),
//   - grabs the latest immutable snapshot (shared pointer swap, no copy).

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "Templates/Function.h"
#include "Core/AerosSimulation.h"
#include "Core/AerosSnapshot.h"
#include <atomic>

class FRunnableThread;
class FEvent;

using FAerosSimCommand = TUniqueFunction<void(FAerosSimulation&)>;

class AEROSWEATHER_API FAerosSimulationThread : public FRunnable
{
public:
	explicit FAerosSimulationThread(const FAerosSimulationSettings& InSettings);
	virtual ~FAerosSimulationThread() override;

	/** Starts the worker thread, or runs synchronously when the platform has no threads. */
	void Start();
	void Shutdown();

	/** Game thread: simulated time the sim should reach. */
	void SetTargetTime(double InTarget);
	double GetTargetTime() const { return TargetTime.load(); }
	double GetSimTime() const { return CurrentTime.load(); }

	/** Queue a command. If ApplyAtSimTime >= 0 it runs at the first step boundary at/after that time (net sync). */
	void Enqueue(FAerosSimCommand&& Command, double ApplyAtSimTime = -1.0);

	/** Latest published snapshot (never null after Start). */
	FAerosSnapshotPtr GetLatestSnapshot() const;

	/** Synchronous fallback (no multithreading). Call from the game thread. */
	void TickSynchronous();

	float GetStepSeconds() const { return StepSeconds; }
	bool IsThreaded() const { return Thread != nullptr; }

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	bool ProcessCommands();
	void StepUntilTarget(double WallBudgetSeconds, int32 MaxSteps);
	void Publish();

	struct FPendingCommand
	{
		FAerosSimCommand Command;
		double ApplyAt = -1.0;
	};

	FAerosSimulation Sim;
	float StepSeconds = 30.f;

	TQueue<FPendingCommand, EQueueMode::Mpsc> Commands;
	TArray<FPendingCommand> Deferred; // sim thread only

	std::atomic<double> TargetTime{ 0.0 };
	std::atomic<double> CurrentTime{ 0.0 };
	std::atomic<bool> bStopRequested{ false };

	FEvent* WakeEvent = nullptr;
	FRunnableThread* Thread = nullptr;

	mutable FCriticalSection SnapshotLock;
	FAerosSnapshotPtr Latest;
	TArray<TSharedPtr<FAerosSnapshot, ESPMode::ThreadSafe>> Pool;
	bool bDirty = true;
	double LastPublishWall = 0.0;
};
