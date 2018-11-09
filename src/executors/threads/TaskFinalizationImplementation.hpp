/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.
	
	Copyright (C) 2015-2017 Barcelona Supercomputing Center (BSC)
*/

#ifndef TASK_FINALIZATION_IMPLEMENTATION_HPP
#define TASK_FINALIZATION_IMPLEMENTATION_HPP

#include "DataAccessRegistration.hpp"
#include "MemoryAllocator.hpp"
#include "TaskFinalization.hpp"
#include "tasks/Taskloop.hpp"

#include <InstrumentTaskStatus.hpp>


void TaskFinalization::disposeOrUnblockTask(Task *task, ComputePlace *computePlace)
{
	bool readyOrDisposable = true;
	
	CPUDependencyData localHpDependencyData;
	CPUDependencyData &hpDependencyData = (computePlace != nullptr) ?
		computePlace->getDependencyData() : localHpDependencyData;
	
	// Follow up the chain of ancestors and dispose them as needed and wake up any in a taskwait that finishes in this moment
	while ((task != nullptr) && readyOrDisposable) {
		Task *parent = task->getParent();
		
		// Complete the delayed release of dependencies of the task if it has a wait clause
		if (task->hasFinished() && task->mustDelayRelease()) {
			readyOrDisposable = false;
			if (task->markAllChildrenAsFinished(computePlace)) {
				DataAccessRegistration::unregisterTaskDataAccesses(
					task, computePlace,
					hpDependencyData
				);
				
				if (task->markAsReleased()) {
					readyOrDisposable = true;
				}
			}
			assert(!task->mustDelayRelease());
			if (!readyOrDisposable)
				break;
		}
		
		if (task->hasFinished()) {
			// NOTE: Handle task removal before unlinking from parent
			DataAccessRegistration::handleTaskRemoval(task, computePlace);
			
			readyOrDisposable = task->unlinkFromParent();
			Instrument::destroyTask(task->getInstrumentationTaskId());
			
			// NOTE: The memory layout is defined in nanos6_create_task
			void *disposableBlock = task->getArgsBlock();
			assert(disposableBlock != nullptr);
			
			size_t disposableBlockSize = (char *)task - (char *)disposableBlock;
				
			if (task->isTaskloop()) {
				disposableBlockSize += sizeof(Taskloop);
			} else {
				disposableBlockSize += sizeof(Task);
			}
			
			Instrument::taskIsBeingDeleted(task->getInstrumentationTaskId());
			
			// Call the taskinfo destructor if not null
			nanos6_task_info_t * taskInfo = task->getTaskInfo();
			if (taskInfo->destroy_args_block != nullptr) {
				taskInfo->destroy_args_block(task->getArgsBlock());
			}
			
			bool isSpawned = task->isSpawned();
			
			task->~Task();
			MemoryAllocator::free(disposableBlock, disposableBlockSize);
			task = parent;
			
			if (isSpawned) {
				SpawnedFunctions::_pendingSpawnedFunctions--;
			}
		} else {
			assert(!task->hasFinished());
			
			// An ancestor in a taskwait that finishes at this point
			Scheduler::addReadyTask(task, computePlace, SchedulerInterface::UNBLOCKED_TASK_HINT);
			readyOrDisposable = false;
			
			bool resumeIdleCPU = true;
			if (computePlace != nullptr) {
				resumeIdleCPU = (computePlace->getType() != nanos6_device_t::nanos6_host_device);
			}
			
			if (resumeIdleCPU) {
				CPU *idleCPU = (CPU *) Scheduler::getIdleComputePlace();
				if (idleCPU != nullptr) {
					ThreadManager::resumeIdle(idleCPU);
				}
			}
		}
	}
}


#endif // TASK_FINALIZATION_IMPLEMENTATION_HPP
