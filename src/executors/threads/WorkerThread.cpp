/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.
	
	Copyright (C) 2015-2017 Barcelona Supercomputing Center (BSC)
*/

#include "CPUActivation.hpp"
#include "TaskFinalization.hpp"
#include "TaskFinalizationImplementation.hpp"
#include "ThreadManager.hpp"
#include "WorkerThread.hpp"
#include "scheduling/Scheduler.hpp"
#include "system/If0Task.hpp"
#include "system/PollingAPI.hpp"
#include "tasks/Task.hpp"
#include "tasks/TaskImplementation.hpp"

#include <DataAccessRegistration.hpp>
#include <ExecutionWorkflow.hpp>

#include <InstrumentComputePlaceManagement.hpp>
#include <InstrumentTaskExecution.hpp>
#include <InstrumentTaskStatus.hpp>
#include <InstrumentInstrumentationContext.hpp>
#include <InstrumentThreadInstrumentationContext.hpp>
#include <InstrumentThreadInstrumentationContextImplementation.hpp>
#include <InstrumentThreadManagement.hpp>
#include <instrument/support/InstrumentThreadLocalDataSupport.hpp>
#include <instrument/support/InstrumentThreadLocalDataSupportImplementation.hpp>

#include <atomic>

#include <alloca.h>
#include <pthread.h>
#include <cstring>

void WorkerThread::initialize()
{
	Instrument::createdThread(_instrumentationId, getComputePlace()->getInstrumentationId());
	
	assert(getComputePlace() != nullptr);
	
	Instrument::ThreadInstrumentationContext instrumentationContext(Instrument::task_id_t(), getComputePlace()->getInstrumentationId(), _instrumentationId);
	
	markAsCurrentWorkerThread();
	
	// This is needed for kernel-level threads to stop them after initialization 
	synchronizeInitialization();
	
	Instrument::threadHasResumed(_instrumentationId, getComputePlace()->getInstrumentationId());
}


void WorkerThread::body()
{
	initialize();
	
	CPU *cpu = getComputePlace();
	Instrument::ThreadInstrumentationContext instrumentationContext(Instrument::task_id_t(), cpu->getInstrumentationId(), _instrumentationId);
	
	bool myCPUShouldBecomeIdleIfNoTask = false;
	while (!_mustShutDown) {
		CPUActivation::activationCheck(this);
		
		cpu = getComputePlace();
		instrumentationContext.updateComputePlace(cpu->getInstrumentationId());
		
		if (_task == nullptr) {
			_task = Scheduler::getReadyTask(cpu, nullptr, myCPUShouldBecomeIdleIfNoTask, true);
		} else {
			// The thread has been preassigned a task before being resumed
		}
		
		if (_task != nullptr) {
			myCPUShouldBecomeIdleIfNoTask = false;
			WorkerThread *assignedThread = _task->getThread();
			
			// A task already assigned to another thread
			if (assignedThread != nullptr) {
				_task = nullptr;
				
				ThreadManager::addIdler(this);
				switchTo(assignedThread);
			} else {
				if (_task->isIf0()) {
					// An if0 task executed outside of the implicit taskwait of its parent (i.e. not inline)
					Task *if0Task = _task;
					
					// This is needed, since otherwise the semantics would be that the if0Task task is being launched from within its own execution
					_task = nullptr;
					
					If0Task::executeNonInline(this, if0Task, cpu);
				} else {
					handleTask(cpu);
				}
				
				_task = nullptr;
			}
		} else if (!myCPUShouldBecomeIdleIfNoTask) {
			// Try to advance work before going to sleep
			PollingAPI::handleServices();
			myCPUShouldBecomeIdleIfNoTask = true;
		} else {
			// The code below is protected by a condition because under certain CPU activation/deactivation
			// cases, the call to CPUActivation::activationCheck may have put the thread in the idle queue
			// and the shutdown mechanism may have waken up the thread. In that case we do not want the
			// thread to go back to the idle queue. The previous case does not need the condition because
			// there is a task to be run and thus the program cannot be performing (a regular) shutdown.
			if (!_mustShutDown) {
				ThreadManager::addIdler(this);
				
				Instrument::suspendingComputePlace(cpu->getInstrumentationId());
				switchTo(nullptr);
				cpu = getComputePlace();
				Instrument::resumedComputePlace(cpu->getInstrumentationId());
				myCPUShouldBecomeIdleIfNoTask = false;
			}
		}
	}
	
	Instrument::threadWillShutdown();
	
	shutdownSequence();
}


void WorkerThread::handleTask(CPU *cpu)
{
	size_t NUMAId = cpu->_NUMANodeId;
	//MemoryPlace *targetPlace = cpu->getMemoryPlace(NUMAId);
	MemoryPlace *targetMemoryPlace = HardwareInfo::getMemoryPlace(nanos6_host_device, NUMAId);
	assert(targetMemoryPlace != nullptr);

	ExecutionWorkflow::executeTask(_task, cpu, targetMemoryPlace);

	_task = nullptr;
}


