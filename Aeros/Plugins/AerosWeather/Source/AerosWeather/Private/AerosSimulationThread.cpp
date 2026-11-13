// Copyright Aeros. All Rights Reserved.

#include "AerosSimulationThread.h"
#include "AerosWeatherModule.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DECLARE_CYCLE_STAT(TEXT("Aeros Sim Step"), STAT_AerosSimStep, STATGROUP_Aeros);
DECLARE_CYCLE_STAT(TEXT("Aeros Publish Snapshot"), STAT_AerosPublish, STATGROUP_Aeros);

FAerosSimulationThread::FAerosSimulationThread(const FAerosSimulationSettings& InSettings)
{
	StepSeconds = FMath::Clamp(InSettings.PhysicsStepSeconds, 5.f, 600.f);
	Sim.Initialize(InSettings);

	for (int32 K = 0; K < 3; ++K)
	{
		Pool.Add(MakeShared<FAerosSnapshot, ESPMode::ThreadSafe>());
	}
	Sim.FillSnapshot(*Pool[0]);
	Latest = Pool[0];
}

FAerosSimulationThread::~FAerosSimulationThread()
{
	Shutdown();
}

void FAerosSimulationThread::Start()
{
	if (Thread || !FPlatformProcess::SupportsMultithreading())
	{
		return;
	}
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	Thread = FRunnableThread::Create(this, TEXT("AerosWeatherSim"), 0, TPri_BelowNormal);
}

void FAerosSimulationThread::Shutdown()
{
	if (Thread)
	{
		bStopRequested = true;
		if (WakeEvent) WakeEvent->Trigger();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

void FAerosSimulationThread::SetTargetTime(double InTarget)
{
	TargetTime.store(InTarget);
	if (WakeEvent) WakeEvent->Trigger();
}

void FAerosSimulationThread::Enqueue(FAerosSimCommand&& Command, double ApplyAtSimTime)
{
	Commands.Enqueue(FPendingCommand{ MoveTemp(Command), ApplyAtSimTime });
	if (WakeEvent) WakeEvent->Trigger();
}

FAerosSnapshotPtr FAerosSimulationThread::GetLatestSnapshot() const
{
	FScopeLock Lock(&SnapshotLock);
	return Latest;
}

bool FAerosSimulationThread::Init()
{
	return true;
}

void FAerosSimulationThread::Stop()
{
	bStopRequested = true;
	if (WakeEvent) WakeEvent->Trigger();
}

bool FAerosSimulationThread::ProcessCommands()
{
	bool bAny = false;
	FPendingCommand Cmd;
	while (Commands.Dequeue(Cmd))
	{
		Deferred.Add(MoveTemp(Cmd));
	}
	const double Now = Sim.GetSimTime();
	for (int32 K = 0; K < Deferred.Num();)
	{
		if (Deferred[K].ApplyAt <= Now + 0.001)
		{
			if (Deferred[K].Command) Deferred[K].Command(Sim);
			Deferred.RemoveAt(K);
			bAny = true;
		}
		else
		{
			++K;
		}
	}
	if (bAny)
	{
		bDirty = true;
		CurrentTime.store(Sim.GetSimTime());
	}
	return bAny;
}

void FAerosSimulationThread::StepUntilTarget(double WallBudgetSeconds, int32 MaxSteps)
{
	const double WallStart = FPlatformTime::Seconds();
	int32 Steps = 0;
	while (!bStopRequested && Steps < MaxSteps && Sim.GetSimTime() + StepSeconds <= TargetTime.load())
	{
		{
			SCOPE_CYCLE_COUNTER(STAT_AerosSimStep);
			TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_SimStep);
			Sim.Step(StepSeconds);
		}
		CurrentTime.store(Sim.GetSimTime());
		ProcessCommands(); // timed commands land exactly on step boundaries
		bDirty = true;
		++Steps;
		if (FPlatformTime::Seconds() - WallStart > WallBudgetSeconds) break;
	}
}

void FAerosSimulationThread::Publish()
{
	SCOPE_CYCLE_COUNTER(STAT_AerosPublish);
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_PublishSnapshot);

	// Reuse a pool entry nobody else references (memory stays allocated: no churn)
	TSharedPtr<FAerosSnapshot, ESPMode::ThreadSafe> Target;
	{
		FScopeLock Lock(&SnapshotLock);
		for (const TSharedPtr<FAerosSnapshot, ESPMode::ThreadSafe>& P : Pool)
		{
			if (P.GetSharedReferenceCount() == 1 && P.Get() != Latest.Get())
			{
				Target = P;
				break;
			}
		}
	}
	if (!Target)
	{
		// Every pooled snapshot is still held by readers: allocate one more (rare)
		Target = MakeShared<FAerosSnapshot, ESPMode::ThreadSafe>();
		if (Pool.Num() < 8) Pool.Add(Target);
	}

	Sim.FillSnapshot(*Target);
	Target->Stats.LagSeconds = float(FMath::Max(0.0, TargetTime.load() - Sim.GetSimTime()));

	{
		FScopeLock Lock(&SnapshotLock);
		Latest = Target;
	}
	bDirty = false;
	LastPublishWall = FPlatformTime::Seconds();
}

uint32 FAerosSimulationThread::Run()
{
	while (!bStopRequested)
	{
		ProcessCommands();
		StepUntilTarget(0.1, 16);

		// Publish at most ~30 Hz wall clock (fast-forward does not copy every step)
		if (bDirty && FPlatformTime::Seconds() - LastPublishWall > 0.033)
		{
			Publish();
		}

		if (!bStopRequested && Sim.GetSimTime() + StepSeconds > TargetTime.load())
		{
			WakeEvent->Wait(bDirty ? 5 : 50);
		}
	}
	return 0;
}

void FAerosSimulationThread::TickSynchronous()
{
	if (Thread) return;
	ProcessCommands();
	StepUntilTarget(0.008, 2);
	if (bDirty) Publish();
}
