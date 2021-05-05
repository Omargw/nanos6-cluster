/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2015-2020 Barcelona Supercomputing Center (BSC)
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cassert>
#include <deque>
#include <iostream>
#include <mutex>

#include "BottomMapEntry.hpp"
#include "CPUDependencyData.hpp"
#include "CommutativeScoreboard.hpp"
#include "DataAccess.hpp"
#include "DataAccessRegistration.hpp"
#include "ReductionInfo.hpp"
#include "TaskDataAccesses.hpp"
#include "executors/threads/TaskFinalization.hpp"
#include "executors/threads/ThreadManager.hpp"
#include "executors/threads/WorkerThread.hpp"
#include "hardware/places/ComputePlace.hpp"
#include "memory/directory/Directory.hpp"
#include "scheduling/Scheduler.hpp"
#include "support/Containers.hpp"
#include "tasks/Task.hpp"

#include <ClusterManager.hpp>
#include <ExecutionWorkflow.hpp>
#include <InstrumentComputePlaceId.hpp>
#include <InstrumentDependenciesByAccess.hpp>
#include <InstrumentDependenciesByAccessLinks.hpp>
#include <InstrumentLogMessage.hpp>
#include <InstrumentReductions.hpp>
#include <InstrumentTaskId.hpp>
#include <InstrumentDependencySubsystemEntryPoints.hpp>
#include <ObjectAllocator.hpp>

#pragma GCC visibility push(hidden)

namespace DataAccessRegistration {

#ifndef NDEBUG
	/*
	 * Debugging function to print out the accesses and fragments for a task
	 *
	 * Prints desc, plus task name, then the accesses and fragments
	 */
	void printTaskAccessesAndFragments(const char *desc, Task *task)
	{
		std::cout << desc << task->getLabel() << "\n";

		assert(task != nullptr);
		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

		// Take lock on access structures if not already done
		TaskDataAccesses::spinlock_t *lock = accessStructures._lock.isLockedByThisThread() ?
								nullptr : &accessStructures._lock;
		if (lock)
			lock->lock();

		/*
		 * Print all the task accesses. The task accesses correspond to the
		 * regions accessed by the task and its subtasks, and is essentially
		 * what is "visible" to the parent. The task accesses are fragmented,
		 * if necessary, because of sibling task accesses. This is done by
		 * registerTaskDataAccess. There are some circumstances when the task
		 * accesses are fragmented later, for example due to early release of
		 * dependencies.
		 */
		accessStructures._accesses.processAll(
			/* processor: called for each task access */
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				DataAccess *access = &(*position);
				assert(access != nullptr);
				std::cout << "access: (DataAccess *)" << access << ": "
					<< access->getAccessRegion().getStartAddress() << ":"
					<< access->getAccessRegion().getSize() << "\n";
				return true; /* always continue, don't stop here */
			}
		);

		/*
		 * Print all the task fragments. The task fragments correspond to
		 * regions accessed by child tasks, so is essentially what is "visible"
		 * to the children. A task starts with no fragments.  As child tasks
		 * are submitted, they will create fragments to cover their accesses
		 * (if not already covered by previous sibling tasks), and the (parent)
		 * task's fragments will be fragmented as necessary.
		 */
		accessStructures._accessFragments.processAll(
			/* processor: called for each task access fragment */
			[&](TaskDataAccesses::access_fragments_t::iterator position) -> bool {
				DataAccess *fragment = &(*position);
				assert(fragment != nullptr);
				std::cout << "fragment: (DataAccess *)" << fragment << ": "
					<< fragment->getAccessRegion().getStartAddress() << ":"
					<< fragment->getAccessRegion().getSize() << "\n";
				return true; /* always continue, don't stop here */
			}
		);
		// Release lock if not already done by the caller
		if (lock)
			lock->unlock();
	}
#endif /* NDEBUG */

	typedef CPUDependencyData::removable_task_list_t removable_task_list_t;


	typedef CPUDependencyData::UpdateOperation UpdateOperation;


	struct DataAccessStatusEffects {
		bool _isRegistered: 1 ;
		bool _isSatisfied: 1 ;
		bool _enforcesDependency: 1 ;

		bool _hasNext: 1 ;
		bool _propagatesReadSatisfiabilityToNext: 1 ;
		bool _propagatesWriteSatisfiabilityToNext: 1 ;
		bool _propagatesConcurrentSatisfiabilityToNext: 1 ;
		bool _propagatesCommutativeSatisfiabilityToNext: 1 ;
		bool _propagatesReductionInfoToNext: 1 ;
		bool _propagatesReductionSlotSetToNext: 1 ;
		bool _makesNextTopmost: 1 ;
		bool _propagatesTopLevel: 1 ;
		bool _releasesCommutativeRegion: 1 ;

		bool _propagatesReadSatisfiabilityToFragments: 1 ;
		bool _propagatesWriteSatisfiabilityToFragments: 1 ;
		bool _propagatesConcurrentSatisfiabilityToFragments: 1 ;
		bool _propagatesCommutativeSatisfiabilityToFragments: 1 ;
		bool _propagatesReductionInfoToFragments: 1 ;
		bool _propagatesReductionSlotSetToFragments: 1 ;

		bool _makesReductionOriginalStorageAvailable: 1 ;
		bool _combinesReductionToPrivateStorage: 1 ;
		bool _combinesReductionToOriginal: 1 ;

		bool _linksBottomMapAccessesToNextAndInhibitsPropagation: 1 ;

		bool _isRemovable: 1 ;

		bool _triggersTaskwaitWorkflow: 1 ;

		bool _propagatesDataReleaseStepToNext: 1 ;

		bool _triggersDataRelease: 1 ;
		bool _triggersDataLinkRead: 1 ;
		bool _triggersDataLinkWrite: 1 ;

	public:
		DataAccessStatusEffects() :
			_isRegistered(false),
			_isSatisfied(false),
			_enforcesDependency(false),

			_hasNext(false),
			_propagatesReadSatisfiabilityToNext(false),
			_propagatesWriteSatisfiabilityToNext(false),
			_propagatesConcurrentSatisfiabilityToNext(false),
			_propagatesCommutativeSatisfiabilityToNext(false),
			_propagatesReductionInfoToNext(false),
			_propagatesReductionSlotSetToNext(false),
			_makesNextTopmost(false),
			_propagatesTopLevel(false),
			_releasesCommutativeRegion(false),

			_propagatesReadSatisfiabilityToFragments(false),
			_propagatesWriteSatisfiabilityToFragments(false),
			_propagatesConcurrentSatisfiabilityToFragments(false),
			_propagatesCommutativeSatisfiabilityToFragments(false),
			_propagatesReductionInfoToFragments(false),
			_propagatesReductionSlotSetToFragments(false),

			_makesReductionOriginalStorageAvailable(false),
			_combinesReductionToPrivateStorage(false),
			_combinesReductionToOriginal(false),

			_linksBottomMapAccessesToNextAndInhibitsPropagation(false),

			_isRemovable(false),

			_triggersTaskwaitWorkflow(false),

			_propagatesDataReleaseStepToNext(false),

			_triggersDataRelease(false),
			_triggersDataLinkRead(false),
			_triggersDataLinkWrite(false)
		{
		}

		DataAccessStatusEffects(DataAccess const *access)
		{
			_isRegistered = access->isRegistered();

			_isSatisfied = access->satisfied();
			_enforcesDependency =
				!access->isWeak() && !access->satisfied() &&
				// Reduction accesses can begin as soon as they have a ReductionInfo (even without SlotSet)
				!((access->getType() == REDUCTION_ACCESS_TYPE) && (access->receivedReductionInfo() || access->allocatedReductionInfo())) && (access->getObjectType() == access_type);
			_hasNext = access->hasNext();

			// Propagation to fragments
			if (access->hasSubaccesses()) {
				_propagatesReadSatisfiabilityToFragments = access->readSatisfied();
				_propagatesWriteSatisfiabilityToFragments = access->writeSatisfied();
				_propagatesConcurrentSatisfiabilityToFragments = access->concurrentSatisfied();
				_propagatesCommutativeSatisfiabilityToFragments = access->commutativeSatisfied();
				// If an access allocates a ReductionInfo, its fragments will have the ReductionInfo
				// set as soon as they are created (being created as a copy of the parent access)
				// For this, this trigger is used to propagate to the fragments the information of
				// *having received* (not having allocated) a ReductionInfo, as this is what is actually
				// tracked in the fragment's 'receivedReductionInfo' status bit
				_propagatesReductionInfoToFragments = access->receivedReductionInfo();
				// Non-reduction accesses will propagate received ReductionSlotSet to their fragments
				// to make their status consistent with the access itself
				_propagatesReductionSlotSetToFragments = access->receivedReductionSlotSet();
			} else {
				_propagatesReadSatisfiabilityToFragments = false;
				_propagatesWriteSatisfiabilityToFragments = false;
				_propagatesConcurrentSatisfiabilityToFragments = false;
				_propagatesCommutativeSatisfiabilityToFragments = false;
				_propagatesReductionInfoToFragments = false;
				_propagatesReductionSlotSetToFragments = false;
			}

			// Propagation to next
			if (_hasNext) {
				assert(access->getObjectType() != taskwait_type);
				assert(access->getObjectType() != top_level_sink_type);

				if (access->hasSubaccesses()) {
					assert(access->getObjectType() == access_type);
					_propagatesReadSatisfiabilityToNext =
						access->canPropagateReadSatisfiability() && access->readSatisfied()
						&& ((access->getType() == READ_ACCESS_TYPE) || (access->getType() == NO_ACCESS_TYPE));
					_propagatesWriteSatisfiabilityToNext = false; // Write satisfiability is propagated through the fragments
					_propagatesConcurrentSatisfiabilityToNext =
						access->canPropagateConcurrentSatisfiability() && access->concurrentSatisfied()
						&& (access->getType() == CONCURRENT_ACCESS_TYPE);
					_propagatesCommutativeSatisfiabilityToNext =
						access->canPropagateCommutativeSatisfiability() && access->commutativeSatisfied()
						&& (access->getType() == COMMUTATIVE_ACCESS_TYPE);
					_propagatesReductionInfoToNext =
						access->canPropagateReductionInfo()
						&& (access->receivedReductionInfo() || access->allocatedReductionInfo())
						// For 'write' and 'readwrite' accesses we need to propagate the ReductionInfo through fragments only,
						// in order to be able to propagate a nested reduction ReductionInfo outside
						&& ((access->getType() != WRITE_ACCESS_TYPE) && (access->getType() != READWRITE_ACCESS_TYPE));
					_propagatesReductionSlotSetToNext = false; // ReductionSlotSet is propagated through the fragments
					// Occasionally data release step needs to be propagated here
					_propagatesDataReleaseStepToNext = access->complete() && access->hasDataReleaseStep();
				} else if (
					(access->getObjectType() == fragment_type)
					|| (access->getObjectType() == taskwait_type)
					|| (access->getObjectType() == top_level_sink_type)) {
					_propagatesReadSatisfiabilityToNext =
						access->canPropagateReadSatisfiability()
						&& access->readSatisfied();
					_propagatesWriteSatisfiabilityToNext = access->writeSatisfied();
					_propagatesConcurrentSatisfiabilityToNext =
						access->canPropagateConcurrentSatisfiability()
						&& access->concurrentSatisfied();
					_propagatesCommutativeSatisfiabilityToNext =
						access->canPropagateCommutativeSatisfiability()
						&& access->commutativeSatisfied();
					_propagatesReductionInfoToNext =
						access->canPropagateReductionInfo()
						&& (access->receivedReductionInfo() || access->allocatedReductionInfo());
					_propagatesReductionSlotSetToNext =
						(access->getType() == REDUCTION_ACCESS_TYPE)
						&& access->complete()
						&& access->receivedReductionInfo()
						&& !access->closesReduction()
						&& (access->allocatedReductionInfo()
							|| access->receivedReductionSlotSet());
					_propagatesDataReleaseStepToNext = access->hasDataReleaseStep();
				} else {
					assert(access->getObjectType() == access_type);
					assert(!access->hasSubaccesses());

					// A regular access without subaccesses but with a next
					_propagatesReadSatisfiabilityToNext =
						access->canPropagateReadSatisfiability()
						&& access->readSatisfied()
						// Note: 'satisfied' as opposed to 'readSatisfied', because otherwise read
						// satisfiability could be propagated before reductions are combined
						&& access->satisfied()
						&& ((access->getType() == READ_ACCESS_TYPE) || (access->getType() == NO_ACCESS_TYPE) || access->complete());
					_propagatesWriteSatisfiabilityToNext =
						access->writeSatisfied() && access->complete()
						// Note: This is important for not propagating write
						// satisfiability before reductions are combined
						&& access->satisfied();


					_propagatesConcurrentSatisfiabilityToNext =
						access->canPropagateConcurrentSatisfiability()
						&& access->concurrentSatisfied()
						// Note: If a reduction is to be combined, being the (reduction) access 'satisfied'
						// and 'complete' should allow it to be done before propagating this satisfiability
						&& access->satisfied()
						&& ((access->getType() == CONCURRENT_ACCESS_TYPE) || access->complete());
					_propagatesCommutativeSatisfiabilityToNext =
						access->canPropagateCommutativeSatisfiability()
						&& access->commutativeSatisfied()
						&& ((access->getType() == COMMUTATIVE_ACCESS_TYPE) || access->complete());
					_propagatesReductionInfoToNext =
						access->canPropagateReductionInfo()
						&& (access->receivedReductionInfo() || access->allocatedReductionInfo())
						// For 'write' and 'readwrite' accesses we need to propagate the ReductionInfo to next only when
						// complete, otherwise subaccesses can still appear
						&& (((access->getType() != WRITE_ACCESS_TYPE) && (access->getType() != READWRITE_ACCESS_TYPE))
							|| access->complete());
					_propagatesReductionSlotSetToNext =
						(access->getType() == REDUCTION_ACCESS_TYPE)
						&& access->complete()
						&& !access->closesReduction()
						&& (access->allocatedReductionInfo()
							|| access->receivedReductionSlotSet());
					_propagatesDataReleaseStepToNext =
						access->hasDataReleaseStep() && access->complete();
				}
			} else {
				assert(!access->hasNext());
				_propagatesReadSatisfiabilityToNext = false;
				_propagatesWriteSatisfiabilityToNext = false;
				_propagatesConcurrentSatisfiabilityToNext = false;
				_propagatesCommutativeSatisfiabilityToNext = false;
				_propagatesReductionInfoToNext = false;
				_propagatesReductionSlotSetToNext = false;
				_propagatesDataReleaseStepToNext = false;
			}

			_makesReductionOriginalStorageAvailable =
				access->getObjectType() == access_type
				&& access->allocatedReductionInfo()
				&& access->writeSatisfied();

			_combinesReductionToPrivateStorage =
				access->closesReduction()
				// If there are subaccesses, it's the last subaccess that should combine
				&& !access->hasSubaccesses()
				// Having received 'ReductionSlotSet' implies that previously inserted reduction accesses
				// (forming part of the same reduction) are completed, but access' predecessors are
				// not necessarily so
				&& (access->allocatedReductionInfo()
					|| access->receivedReductionSlotSet())
				&& access->complete();

			_combinesReductionToOriginal =
				_combinesReductionToPrivateStorage
				// Being satisfied implies all predecessors (reduction or not) have been completed
				&& access->satisfied();

			_isRemovable = access->isTopmost()
						   && access->readSatisfied() && access->writeSatisfied()
						   && access->receivedReductionInfo()
						   // Read as: If this (reduction) access is part of its predecessor reduction,
						   // it needs to have received the 'ReductionSlotSet' before being removed
						   && ((access->getType() != REDUCTION_ACCESS_TYPE)
							   || access->allocatedReductionInfo() || access->receivedReductionSlotSet())
						   && access->complete()
						   && (!access->isInBottomMap() || access->hasNext()
							   || (access->getType() == NO_ACCESS_TYPE)
							   || (access->getObjectType() == taskwait_type)
							   || (access->getObjectType() == top_level_sink_type));

			/*
			 *If the access is a taskwait access (from createTaskwait)
			 */
			_triggersTaskwaitWorkflow = (access->getObjectType() == taskwait_type)
										&& access->readSatisfied()
										&& access->writeSatisfied()
										&& access->hasOutputLocation();

			if (access->hasDataReleaseStep()) {
				ExecutionWorkflow::DataReleaseStep *releaseStep =
					access->getDataReleaseStep();

				_triggersDataRelease =
					releaseStep->checkDataRelease(access);
			} else {
				_triggersDataRelease = false;
			}

			_triggersDataLinkRead = access->hasDataLinkStep()
									&& access->readSatisfied();

			_triggersDataLinkWrite = access->hasDataLinkStep()
									 && access->writeSatisfied();

			Task *domainParent;
			assert(access->getOriginator() != nullptr);
			if (access->getObjectType() == access_type) {
				if (access->getType() == NO_ACCESS_TYPE) {
					domainParent = access->getOriginator();
				} else {
					domainParent = access->getOriginator()->getParent();
				}
			} else {
				assert(
					(access->getObjectType() == fragment_type)
					|| (access->getObjectType() == taskwait_type)
					|| (access->getObjectType() == top_level_sink_type));
				domainParent = access->getOriginator();
			}
			assert(domainParent != nullptr);

			/*
			 * Does it make the next access topmost
			 */
			if (_isRemovable && access->hasNext()) {
				Task *nextDomainParent;
				if (access->getNext()._objectType == access_type) {
					nextDomainParent = access->getNext()._task->getParent();
				} else {
					assert(
						(access->getNext()._objectType == fragment_type)
						|| (access->getNext()._objectType == taskwait_type)
						|| (access->getNext()._objectType == top_level_sink_type));
					nextDomainParent = access->getNext()._task;
				}
				assert(nextDomainParent != nullptr);

				_makesNextTopmost = (domainParent == nextDomainParent);
			} else {
				_makesNextTopmost = false;
			}

			_propagatesTopLevel =
				access->isTopLevel()
				&& access->hasNext()
				&& (access->getOriginator()->getParent() == access->getNext()._task->getParent());

			_releasesCommutativeRegion =
				(access->getType() == COMMUTATIVE_ACCESS_TYPE)
				&& !access->isWeak()
				&& access->complete();

			// NOTE: Calculate inhibition from initial status
			_linksBottomMapAccessesToNextAndInhibitsPropagation =
				access->hasNext() && access->complete() && access->hasSubaccesses();
		}

		void setEnforcesDependency()
		{
			assert(_enforcesDependency == false);
			_enforcesDependency = true;
		}
	};


	struct BottomMapUpdateOperation {
		DataAccessRegion _region;
		DataAccessType _parentAccessType;

		bool _linkBottomMapAccessesToNext;

		bool _inhibitReadSatisfiabilityPropagation;
		bool _inhibitConcurrentSatisfiabilityPropagation;
		bool _inhibitCommutativeSatisfiabilityPropagation;
		bool _inhibitReductionInfoPropagation;

		bool _setCloseReduction;

		DataAccessLink _next;

		BottomMapUpdateOperation() :
			_region(),
			_parentAccessType(NO_ACCESS_TYPE),
			_linkBottomMapAccessesToNext(false),
			_inhibitReadSatisfiabilityPropagation(false),
			_inhibitConcurrentSatisfiabilityPropagation(false),
			_inhibitCommutativeSatisfiabilityPropagation(false),
			_inhibitReductionInfoPropagation(false),
			_setCloseReduction(false),
			_next()
		{
		}

		BottomMapUpdateOperation(DataAccessRegion const &region) :
			_region(region),
			_parentAccessType(NO_ACCESS_TYPE),
			_linkBottomMapAccessesToNext(false),
			_inhibitReadSatisfiabilityPropagation(false),
			_inhibitConcurrentSatisfiabilityPropagation(false),
			_inhibitCommutativeSatisfiabilityPropagation(false),
			_inhibitReductionInfoPropagation(false),
			_setCloseReduction(false),
			_next()
		{
		}

		bool empty() const
		{
			return !_linkBottomMapAccessesToNext;
		}
	};


	// Forward declarations
	static inline void processBottomMapUpdate(
		BottomMapUpdateOperation &operation,
		TaskDataAccesses &accessStructures, Task *task,
		/* OUT */ CPUDependencyData &hpDependencyData);
	static inline void removeBottomMapTaskwaitOrTopLevelSink(
		DataAccess *access, TaskDataAccesses &accessStructures, __attribute__((unused)) Task *task);
	static inline BottomMapEntry *fragmentBottomMapEntry(
		BottomMapEntry *bottomMapEntry, DataAccessRegion region,
		TaskDataAccesses &accessStructures, bool removeIntersection = false);
	static void handleRemovableTasks(
		/* inout */ CPUDependencyData::removable_task_list_t &removableTasks);
	static void handleCompletedTaskwaits(
		CPUDependencyData::satisfied_taskwait_accesses_t &completedTaskwaits,
		__attribute__((unused)) ComputePlace *computePlace);
	static inline DataAccess *fragmentAccess(
		DataAccess *dataAccess, DataAccessRegion const &region,
		TaskDataAccesses &accessStructures);


	/*
	 * Make the changes to the data access implied by the differences between
	 * initialStatus and updatedStatus. This is called with the lock for the
	 * tasks's data accesses (accessStructures). Any changes that cannot be
	 * done while this lock is held (as they need a different lock and taking
	 * it could cause a deadlock) will be added to hpDependencyData and done
	 * later (in
	 * processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks).
	 */
	static inline void handleDataAccessStatusChanges(
		DataAccessStatusEffects const &initialStatus,
		DataAccessStatusEffects const &updatedStatus,
		DataAccess *access, TaskDataAccesses &accessStructures, Task *task,
		/* OUT */ CPUDependencyData &hpDependencyData
	) {
		/* Check lock on task's access structures already taken by caller */
		assert(task->getDataAccesses()._lock.isLockedByThisThread());

		// Registration
		if (initialStatus._isRegistered != updatedStatus._isRegistered) {
			assert(!initialStatus._isRegistered);

			// Count the access
			if (!initialStatus._isRemovable) {
				if (accessStructures._removalBlockers == 0) {
					// The blocking count is decreased once all the accesses become removable
					task->increaseRemovalBlockingCount();
				}
				accessStructures._removalBlockers++;

				/*
				 * Count the registered taskwait fragments, so know when they
				 * have all been handled.
				 */
				if (access->getObjectType() == taskwait_type) {
					accessStructures._liveTaskwaitFragmentCount++;
				}
			}

			// (Strong) Commutative accounting
			if (!access->isWeak() && (access->getType() == COMMUTATIVE_ACCESS_TYPE)) {
				accessStructures._totalCommutativeBytes += access->getAccessRegion().getSize();
			}

			if (updatedStatus._enforcesDependency) {
				task->increasePredecessors();
			}
		}

		if (!updatedStatus._isRegistered) {
			return;
		}

		// NOTE: After this point, all actions assume the access is registered

		// Satisfiability
		if (initialStatus._isSatisfied != updatedStatus._isSatisfied) {
			assert(!initialStatus._isSatisfied);
			Instrument::dataAccessBecomesSatisfied(
				access->getInstrumentationId(),
				true,
				task->getInstrumentationTaskId());
		}

		// Link to Next
		if (initialStatus._hasNext != updatedStatus._hasNext) {
			assert(!initialStatus._hasNext);
			Instrument::linkedDataAccesses(
				access->getInstrumentationId(),
				access->getNext()._task->getInstrumentationTaskId(),
				(Instrument::access_object_type_t)access->getNext()._objectType,
				access->getAccessRegion(),
				/* direct */ true, /* unidirectional */ false);
		}

		// Dependency updates
		if (initialStatus._enforcesDependency != updatedStatus._enforcesDependency) {
			if (updatedStatus._enforcesDependency) {
				// A new access that enforces a dependency.
				// Only happens when the task is first registered, and has already been
				// counted as part of the registration status change.
				assert(!initialStatus._isRegistered && updatedStatus._isRegistered);
			} else {
				// The access no longer enforces a dependency (has become satisfied)
				if (task->decreasePredecessors()) {
					// The task becomes ready
					if (accessStructures._totalCommutativeBytes != 0UL) {
						hpDependencyData._satisfiedCommutativeOriginators.push_back(task);
					} else {
						hpDependencyData._satisfiedOriginators.push_back(task);
					}
				}
			}
		}

		// Notify reduction original storage has become available
		if (initialStatus._makesReductionOriginalStorageAvailable != updatedStatus._makesReductionOriginalStorageAvailable) {
			assert(!initialStatus._makesReductionOriginalStorageAvailable);
			assert(access->getObjectType() == access_type);

			ReductionInfo *reductionInfo = access->getReductionInfo();
			assert(reductionInfo != nullptr);

			reductionInfo->makeOriginalStorageRegionAvailable(access->getAccessRegion());
		}

		// Reduction combination to a private reduction storage
		if ((initialStatus._combinesReductionToPrivateStorage != updatedStatus._combinesReductionToPrivateStorage)
			// If we can already combine to the original region directly, we just skip this step
			&& (initialStatus._combinesReductionToOriginal == updatedStatus._combinesReductionToOriginal)) {
			assert(!initialStatus._combinesReductionToPrivateStorage);
			assert(!initialStatus._combinesReductionToOriginal);

			assert(!access->hasBeenDiscounted());

			assert(access->getType() == REDUCTION_ACCESS_TYPE);
			assert(access->allocatedReductionInfo() || (access->receivedReductionInfo() && access->receivedReductionSlotSet()));

			ReductionInfo *reductionInfo = access->getReductionInfo();
			assert(reductionInfo != nullptr);
			__attribute__((unused)) bool wasLastCombination =
				reductionInfo->combineRegion(access->getAccessRegion(), access->getReductionSlotSet(), /* canCombineToOriginalStorage */ false);
			assert(!wasLastCombination);
		}

		// Reduction combination to original region
		if (initialStatus._combinesReductionToOriginal != updatedStatus._combinesReductionToOriginal) {
			assert(!initialStatus._combinesReductionToOriginal);
			assert(updatedStatus._combinesReductionToPrivateStorage);

			assert(!access->hasBeenDiscounted());

			assert(access->getType() == REDUCTION_ACCESS_TYPE);
			assert(access->receivedReductionInfo());
			assert(access->allocatedReductionInfo() || access->receivedReductionSlotSet());

			ReductionInfo *reductionInfo = access->getReductionInfo();
			assert(reductionInfo != nullptr);
			bool wasLastCombination = reductionInfo->combineRegion(access->getAccessRegion(), access->getReductionSlotSet(), /* canCombineToOriginalStorage */ true);

			if (wasLastCombination) {
				const DataAccessRegion &originalRegion = reductionInfo->getOriginalRegion();

				ObjectAllocator<ReductionInfo>::deleteObject(reductionInfo);

				Instrument::deallocatedReductionInfo(
					access->getInstrumentationId(),
					reductionInfo,
					originalRegion);
			}
		}

		// Release of commutative region
		if (initialStatus._releasesCommutativeRegion != updatedStatus._releasesCommutativeRegion) {
			assert(!initialStatus._releasesCommutativeRegion);
			hpDependencyData._releasedCommutativeRegions.emplace_back(task, access->getAccessRegion());
		}

		// Propagation to Next
		if (access->hasNext()) {
			/*
			 * Prepare an update operation that will affect the next task.
			 */
			UpdateOperation updateOperation(access->getNext(), access->getAccessRegion());

			if (initialStatus._propagatesReadSatisfiabilityToNext != updatedStatus._propagatesReadSatisfiabilityToNext) {
				assert(!initialStatus._propagatesReadSatisfiabilityToNext);
				updateOperation._makeReadSatisfied = true; /* make next task read satisfied */
				assert(access->hasLocation());
				updateOperation._location = access->getLocation();
			}

			if (initialStatus._propagatesWriteSatisfiabilityToNext != updatedStatus._propagatesWriteSatisfiabilityToNext) {
				assert(!initialStatus._propagatesWriteSatisfiabilityToNext);

				/*
				 * This assertion happens occasionally. Temporarily disable it.
				 */
				// assert(!access->canPropagateReductionInfo() || updatedStatus._propagatesReductionInfoToNext);
				updateOperation._makeWriteSatisfied = true;
			}

			if (initialStatus._propagatesConcurrentSatisfiabilityToNext != updatedStatus._propagatesConcurrentSatisfiabilityToNext) {
				assert(!initialStatus._propagatesConcurrentSatisfiabilityToNext);
				updateOperation._makeConcurrentSatisfied = true;
			}
			if (initialStatus._propagatesCommutativeSatisfiabilityToNext != updatedStatus._propagatesCommutativeSatisfiabilityToNext) {
				assert(!initialStatus._propagatesCommutativeSatisfiabilityToNext);
				updateOperation._makeCommutativeSatisfied = true;
			}

			if (initialStatus._propagatesReductionInfoToNext != updatedStatus._propagatesReductionInfoToNext) {
				assert(!initialStatus._propagatesReductionInfoToNext);
				assert((access->getType() != REDUCTION_ACCESS_TYPE) || (access->receivedReductionInfo() || access->allocatedReductionInfo()));
				updateOperation._setReductionInfo = true;
				updateOperation._reductionInfo = access->getReductionInfo();
			}

			if (initialStatus._propagatesReductionSlotSetToNext != updatedStatus._propagatesReductionSlotSetToNext) {
				assert(!initialStatus._propagatesReductionSlotSetToNext);

				// Reduction slot set computation

				assert(access->getType() == REDUCTION_ACCESS_TYPE);
				assert(access->receivedReductionInfo() || access->allocatedReductionInfo());
				assert(access->getReductionSlotSet().size() > 0);
				assert(access->isWeak() || task->isFinal() || access->getReductionSlotSet().any());

				updateOperation._reductionSlotSet = access->getReductionSlotSet();
			}

			if (initialStatus._propagatesDataReleaseStepToNext != updatedStatus._propagatesDataReleaseStepToNext) {
				assert(!initialStatus._propagatesDataReleaseStepToNext);

				updateOperation._releaseStep = access->getDataReleaseStep();
				access->unsetDataReleaseStep();
			}

			// Make Next Topmost
			if (initialStatus._makesNextTopmost != updatedStatus._makesNextTopmost) {
				assert(!initialStatus._makesNextTopmost);
				updateOperation._makeTopmost = true;
			}

			if (initialStatus._propagatesTopLevel != updatedStatus._propagatesTopLevel) {
				assert(!initialStatus._propagatesTopLevel);
				updateOperation._makeTopLevel = true;
			}

			if (!updateOperation.empty()) {
				hpDependencyData._delayedOperations.emplace_back(updateOperation);
			}
		}

		// Propagation to Fragments
		if (access->hasSubaccesses()) {
			UpdateOperation updateOperation(DataAccessLink(task, fragment_type), access->getAccessRegion());

			if (initialStatus._propagatesReadSatisfiabilityToFragments != updatedStatus._propagatesReadSatisfiabilityToFragments) {
				assert(!initialStatus._propagatesReadSatisfiabilityToFragments);
				updateOperation._makeReadSatisfied = true;
				assert(access->hasLocation());
				updateOperation._location = access->getLocation();
			}

			if (initialStatus._propagatesWriteSatisfiabilityToFragments != updatedStatus._propagatesWriteSatisfiabilityToFragments) {
				assert(!initialStatus._propagatesWriteSatisfiabilityToFragments);
				updateOperation._makeWriteSatisfied = true;
			}

			if (initialStatus._propagatesConcurrentSatisfiabilityToFragments != updatedStatus._propagatesConcurrentSatisfiabilityToFragments) {
				assert(!initialStatus._propagatesConcurrentSatisfiabilityToFragments);
				updateOperation._makeConcurrentSatisfied = true;
			}

			if (initialStatus._propagatesCommutativeSatisfiabilityToFragments != updatedStatus._propagatesCommutativeSatisfiabilityToFragments) {
				assert(!initialStatus._propagatesCommutativeSatisfiabilityToFragments);
				updateOperation._makeCommutativeSatisfied = true;
			}

			if (initialStatus._propagatesReductionInfoToFragments != updatedStatus._propagatesReductionInfoToFragments) {
				assert(!initialStatus._propagatesReductionInfoToFragments);
				assert(!(access->getType() == REDUCTION_ACCESS_TYPE) || (access->receivedReductionInfo() || access->allocatedReductionInfo()));
				updateOperation._setReductionInfo = true;
				updateOperation._reductionInfo = access->getReductionInfo();
			}

			if (initialStatus._propagatesReductionSlotSetToFragments != updatedStatus._propagatesReductionSlotSetToFragments) {
				assert(!initialStatus._propagatesReductionSlotSetToFragments);

				assert(access->receivedReductionSlotSet() || ((access->getType() == REDUCTION_ACCESS_TYPE) && access->allocatedReductionInfo()));
				assert(access->getReductionSlotSet().size() > 0);

				updateOperation._reductionSlotSet = access->getReductionSlotSet();
			}

			if (!updateOperation.empty()) {
				hpDependencyData._delayedOperations.emplace_back(updateOperation);
			}
		}

		// Bottom Map Updates
		if (access->hasSubaccesses()) {
			if (
				initialStatus._linksBottomMapAccessesToNextAndInhibitsPropagation
				!= updatedStatus._linksBottomMapAccessesToNextAndInhibitsPropagation) {
				BottomMapUpdateOperation bottomMapUpdateOperation(access->getAccessRegion());

				bottomMapUpdateOperation._parentAccessType = access->getType();

				bottomMapUpdateOperation._linkBottomMapAccessesToNext = true;
				bottomMapUpdateOperation._next = access->getNext();

				bottomMapUpdateOperation._inhibitReadSatisfiabilityPropagation = (access->getType() == READ_ACCESS_TYPE);
				assert(!updatedStatus._propagatesWriteSatisfiabilityToNext);
				bottomMapUpdateOperation._inhibitConcurrentSatisfiabilityPropagation = (access->getType() == CONCURRENT_ACCESS_TYPE);
				bottomMapUpdateOperation._inhibitCommutativeSatisfiabilityPropagation = (access->getType() == COMMUTATIVE_ACCESS_TYPE);
				// 'write' and 'readwrite' accesses can have a nested reduction that is combined outside the parent task itself, and thus
				// their ReductionInfo needs to be propagates through the bottom map
				// Subaccesses of an access that can't have a nested reduction which is visible outside
				// should never propagate the ReductionInfo (it is already propagated by the parent access)
				bottomMapUpdateOperation._inhibitReductionInfoPropagation =
					(access->getType() != WRITE_ACCESS_TYPE) && (access->getType() != READWRITE_ACCESS_TYPE);

				bottomMapUpdateOperation._setCloseReduction = (access->getType() != REDUCTION_ACCESS_TYPE) || access->closesReduction();

				processBottomMapUpdate(bottomMapUpdateOperation, accessStructures, task, hpDependencyData);
			}
		}

		if (initialStatus._triggersTaskwaitWorkflow != updatedStatus._triggersTaskwaitWorkflow) {
			assert(!initialStatus._triggersTaskwaitWorkflow);
			assert(access->getObjectType() == taskwait_type);
			assert(access->readSatisfied());
			assert(access->writeSatisfied());
			assert(!access->complete());
			assert(!access->hasNext());
			assert(access->isInBottomMap());

			hpDependencyData._completedTaskwaits.emplace_back(access);
		}

		// DataReleaseStep triggers
		if (initialStatus._triggersDataRelease != updatedStatus._triggersDataRelease) {
			assert(!initialStatus._triggersDataRelease);

			ExecutionWorkflow::DataReleaseStep *step = access->getDataReleaseStep();
			access->unsetDataReleaseStep();
			step->releaseRegion(access->getAccessRegion(), access->getLocation());
		}

		const bool linksRead = initialStatus._triggersDataLinkRead != updatedStatus._triggersDataLinkRead;
		const bool linksWrite = initialStatus._triggersDataLinkWrite != updatedStatus._triggersDataLinkWrite;
		if (linksRead || linksWrite) {
			assert(access->hasDataLinkStep());

			ExecutionWorkflow::DataLinkStep *step = access->getDataLinkStep();

			/*
			 * Send satisfiability through the workflow. For Nanos6@cluster, this will
			 * send a MessageSatisfiability to a remote node.
			 * NOTE: it is possible for access->getLocation() to be nullptr only
			 * in the rare case that write satisfiability is propagated before read
			 * satisfiability.
			 */
			step->linkRegion(
				access->getAccessRegion(),
				access->getLocation(),
				linksRead,  /* propagate change, not the new value */
				linksWrite  /* propagate change, not the new value */
			);

			if (updatedStatus._triggersDataLinkRead && updatedStatus._triggersDataLinkWrite) {
				access->unsetDataLinkStep();
			}
		}

		// Access becomes removable
		if (initialStatus._isRemovable != updatedStatus._isRemovable) {
			assert(!initialStatus._isRemovable);

			assert(accessStructures._removalBlockers > 0);
			accessStructures._removalBlockers--;

			/*
			 * Discounted means that it is no longer blocking the removal of
			 * the task (?)
			 */
			access->markAsDiscounted();

			if (access->getObjectType() == taskwait_type) {
				// Update parent data access ReductionSlotSet with information from its subaccesses
				// collected at the taskwait fragment
				// Note: This shouldn't be done for top-level sink fragments, as their presence
				// in the bottomMap just means that there is no matching access in the parent
				// (the reduction is local and not waited for)
				if (access->getType() == REDUCTION_ACCESS_TYPE) {
					assert(access->getReductionSlotSet().size() > 0);

					accessStructures._accesses.processIntersecting(
						access->getAccessRegion(),
						[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
							DataAccess *dataAccess = &(*position);
							assert(dataAccess != nullptr);
							assert(!dataAccess->hasBeenDiscounted());

							assert(dataAccess->getType() == REDUCTION_ACCESS_TYPE);
							assert(dataAccess->isWeak());

							assert(dataAccess->receivedReductionInfo() || dataAccess->allocatedReductionInfo());
							assert(access->receivedReductionInfo());
							assert(dataAccess->getReductionInfo() == access->getReductionInfo());

							assert(dataAccess->getReductionSlotSet().size() ==
								access->getReductionSlotSet().size()
							);

							dataAccess->getReductionSlotSet() |= access->getReductionSlotSet();

							return true;
						});
				}

				// The last taskwait fragment that finishes removes the blocking over the task
				assert(accessStructures._liveTaskwaitFragmentCount > 0);
				accessStructures._liveTaskwaitFragmentCount--;

				if (accessStructures._liveTaskwaitFragmentCount == 0) {
					if (task->decreaseBlockingCount())
						hpDependencyData._satisfiedOriginators.push_back(task);
				}
			}

			if (access->hasNext()) {
				Instrument::unlinkedDataAccesses(
					access->getInstrumentationId(),
					access->getNext()._task->getInstrumentationTaskId(),
					(Instrument::access_object_type_t)access->getNext()._objectType,
					/* direct */ true);
			} else {
				/*
				 * The access has no next access, so actually delete it.
				 */
				if ((access->getObjectType() == taskwait_type)
					|| (access->getObjectType() == top_level_sink_type))
				{
					removeBottomMapTaskwaitOrTopLevelSink(access, accessStructures, task);
				} else {
					assert(access->getObjectType() == access_type
						   && access->getType() == NO_ACCESS_TYPE);

					Instrument::removedDataAccess(access->getInstrumentationId());
					accessStructures._accesses.erase(access);
					ObjectAllocator<DataAccess>::deleteObject(access);
				}
			}

			/*
			 * This removable access is no longer blocking the removal of the
			 * task itself. Decrement the task's removal blocking count (of
			 * accesses) and, if it becomes zero, list the task as removable.
			 */
			if (accessStructures._removalBlockers == 0) {
				if (task->decreaseRemovalBlockingCount()) {
					hpDependencyData._removableTasks.push_back(task);
				}
			}
		}
	}


	static inline void removeBottomMapTaskwaitOrTopLevelSink( DataAccess *access, TaskDataAccesses &accessStructures,
		__attribute__((unused)) Task *task
	) {
		assert(access != nullptr);
		assert(task != nullptr);
		assert(access->getOriginator() == task);
		assert(accessStructures._lock.isLockedByThisThread());
		assert((access->getObjectType() == taskwait_type) || (access->getObjectType() == top_level_sink_type));

		accessStructures._subaccessBottomMap.processIntersecting(
			access->getAccessRegion(),
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);
				assert(access->getAccessRegion().fullyContainedIn(bottomMapEntry->getAccessRegion()));
				assert(bottomMapEntry->_link._task == task);
				assert(bottomMapEntry->_link._objectType == access->getObjectType());

				if (access->getAccessRegion() == bottomMapEntry->getAccessRegion()) {
					accessStructures._subaccessBottomMap.erase(bottomMapEntry);
					ObjectAllocator<BottomMapEntry>::deleteObject(bottomMapEntry);
				} else {
					fragmentBottomMapEntry(
						bottomMapEntry, access->getAccessRegion(),
						accessStructures,
						/* remove intersection */ true);
				}

				return true;
			});

		//! We are about to delete the taskwait fragment. Before doing so,
		//! move the location info and data release step back to the original access
		ExecutionWorkflow::DataReleaseStep *dataReleaseStep = access->getDataReleaseStep();
		if (dataReleaseStep) {
			access->unsetDataReleaseStep();
		}
		accessStructures._accesses.processIntersecting(
			access->getAccessRegion(),
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				DataAccess *originalAccess = &(*position);
				assert(originalAccess != nullptr);
				assert(!originalAccess->hasBeenDiscounted());

				originalAccess =
					fragmentAccess(originalAccess,
						access->getAccessRegion(),
						accessStructures);
				originalAccess->setLocation(access->getLocation());

				if (dataReleaseStep) {
					originalAccess->setDataReleaseStep(dataReleaseStep);
				}

				return true;
			});

		accessStructures._taskwaitFragments.erase(access);
		ObjectAllocator<DataAccess>::deleteObject(access);
	}


	/*
	 * Internal function to create a new data access.
	 */
	static inline DataAccess *createAccess(
		Task *originator,
		DataAccessObjectType objectType,
		DataAccessType accessType, bool weak, DataAccessRegion region,
		reduction_type_and_operator_index_t reductionTypeAndOperatorIndex = no_reduction_type_and_operator,
		reduction_index_t reductionIndex = -1,
		MemoryPlace const *location = nullptr,
		MemoryPlace const *outputLocation = nullptr,
		ExecutionWorkflow::DataReleaseStep *dataReleaseStep = nullptr,
		ExecutionWorkflow::DataLinkStep *dataLinkStep = nullptr,
		DataAccess::status_t status = 0, DataAccessLink next = DataAccessLink()
	) {
		// Not sure why this was previously commented as "Regular object duplication"
		DataAccess *dataAccess = ObjectAllocator<DataAccess>::newObject(
			objectType,
			accessType, weak, originator, region,
			reductionTypeAndOperatorIndex,
			reductionIndex,
			location,
			outputLocation,
			dataReleaseStep,
			dataLinkStep,
			Instrument::data_access_id_t(),
			status, next);

		return dataAccess;
	}

	/*
	 * Upgrade an access: called by registerTaskDataAccess when a task's access
	 * intersects a previously-registered access of the same task.
	 */
	static inline void upgradeAccess(
		DataAccess *dataAccess,
		DataAccessType accessType,
		bool weak,
		reduction_type_and_operator_index_t reductionTypeAndOperatorIndex
	) {
		assert(dataAccess != nullptr);
		assert(!dataAccess->hasBeenDiscounted());

		/* Only weak if both accesses are weak */
		bool newWeak = dataAccess->isWeak() && weak;

		DataAccessType newDataAccessType = accessType;
		if (accessType != dataAccess->getType()) {
			FatalErrorHandler::failIf(
				(accessType == REDUCTION_ACCESS_TYPE) || (dataAccess->getType() == REDUCTION_ACCESS_TYPE),
				"Task ",
				(dataAccess->getOriginator()->getTaskInfo()->implementations[0].task_label != nullptr ? dataAccess->getOriginator()->getTaskInfo()->implementations[0].task_label : dataAccess->getOriginator()->getTaskInfo()->implementations[0].declaration_source),
				" has non-reduction accesses that overlap a reduction");
			if (
				((accessType == COMMUTATIVE_ACCESS_TYPE) && (dataAccess->getType() == CONCURRENT_ACCESS_TYPE))
				|| ((accessType == CONCURRENT_ACCESS_TYPE) && (dataAccess->getType() == COMMUTATIVE_ACCESS_TYPE))) {
				newDataAccessType = COMMUTATIVE_ACCESS_TYPE;
			} else {
				/*
				 * Every other remaining case is READWRITE
				 *    (# means same, X means invalid, commutative handled above
				 *
				 *  		        READ WRITE READWRITE  CONCURRENT COMMUTATIVE REDUCTION
				 *  READ               #    RW        RW         RW?         RW?         X
				 *  WRITE             RW     #        RW         RW?         RW?         X
				 *  READWRITE         RW    RW         #         RW?         RW?         X
				 *  CONCURRENT       RW?   RW?       RW?           # commutative         X
				 *  COMMUTATIVE      RW?   RW?       RW? commutative           #         X
				 *  REDUCTION          X     X         X           X           X         #
				 */
				newDataAccessType = READWRITE_ACCESS_TYPE;
			}
		} else {
			FatalErrorHandler::failIf(
				(accessType == REDUCTION_ACCESS_TYPE)
					&& (dataAccess->getReductionTypeAndOperatorIndex() != reductionTypeAndOperatorIndex),
				"Task ",
				(dataAccess->getOriginator()->getTaskInfo()->implementations[0].task_label != nullptr ? dataAccess->getOriginator()->getTaskInfo()->implementations[0].task_label : dataAccess->getOriginator()->getTaskInfo()->implementations[0].declaration_source),
				" has two overlapping reductions over different types or with different operators");
		}

		dataAccess->upgrade(newWeak, newDataAccessType);
	}


	// NOTE: locking should be handled from the outside
	static inline DataAccess *duplicateDataAccess(
		DataAccess const &toBeDuplicated,
		__attribute__((unused)) TaskDataAccesses &accessStructures)
	{
		assert(toBeDuplicated.getOriginator() != nullptr);
		assert(!accessStructures.hasBeenDeleted());
		assert(!toBeDuplicated.hasBeenDiscounted());

		// Regular object duplication
		DataAccess *newFragment = ObjectAllocator<DataAccess>::newObject(toBeDuplicated);

		// Copy symbols
		newFragment->addToSymbols(toBeDuplicated.getSymbols()); // TODO: Consider removing the pointer from declaration and make it a reference

		newFragment->clearRegistered();

		return newFragment;
	}


#ifndef NDEBUG

	/*
	 * Debug function to check that none of the accesses is marked as
	 * reachable.  If no access is reachable then no locking is necessary on
	 * the access structures. Accesses become reachable in linkTaskAccesses,
	 * when the task's accesses are linked into the dependency system, and in
	 * createTaskwait and createTopLevelSink, which do the same for taskwaits
	 * and top-level sinks. They are then reachable until they are destroyed.
	 */
	static bool noAccessIsReachable(TaskDataAccesses &accessStructures)
	{
		assert(!accessStructures.hasBeenDeleted());
		return accessStructures._accesses.processAll(
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				return !position->isReachable();
			});
	}
#endif


	static inline BottomMapEntry *fragmentBottomMapEntry(
		BottomMapEntry *bottomMapEntry, DataAccessRegion region,
		TaskDataAccesses &accessStructures, bool removeIntersection)
	{
		if (bottomMapEntry->getAccessRegion().fullyContainedIn(region)) {
			// Nothing to fragment
			return bottomMapEntry;
		}

		assert(!accessStructures.hasBeenDeleted());
		assert(accessStructures._lock.isLockedByThisThread());

		TaskDataAccesses::subaccess_bottom_map_t::iterator position =
			accessStructures._subaccessBottomMap.iterator_to(*bottomMapEntry);
		position = accessStructures._subaccessBottomMap.fragmentByIntersection(
			position, region,
			removeIntersection,
			[&](BottomMapEntry const &toBeDuplicated) -> BottomMapEntry * {
				return ObjectAllocator<BottomMapEntry>::newObject(DataAccessRegion(), toBeDuplicated._link,
					toBeDuplicated._accessType, toBeDuplicated._reductionTypeAndOperatorIndex);
			},
			[&](__attribute__((unused)) BottomMapEntry *fragment, __attribute__((unused)) BottomMapEntry *originalBottomMapEntry) {
			});

		if (!removeIntersection) {
			bottomMapEntry = &(*position);
			assert(bottomMapEntry != nullptr);
			assert(bottomMapEntry->getAccessRegion().fullyContainedIn(region));

			return bottomMapEntry;
		} else {
			return nullptr;
		}
	}


	static inline void setUpNewFragment(
		DataAccess *fragment, DataAccess *originalDataAccess,
		TaskDataAccesses &accessStructures)
	{
		if (fragment != originalDataAccess) {
			CPUDependencyData hpDependencyData;

			DataAccessStatusEffects initialStatus(fragment);
			fragment->setUpNewFragment(originalDataAccess->getInstrumentationId());
			fragment->setRegistered();
			DataAccessStatusEffects updatedStatus(fragment);

			handleDataAccessStatusChanges(
				initialStatus, updatedStatus,
				fragment, accessStructures, fragment->getOriginator(),
				hpDependencyData);

			/* Do not expect any delayed operations */
			assert (hpDependencyData.empty());
		}
	}


	/*
	 * fragmentAccessObject: Fragment an access if necessary to match a region.
	 *
	 * The task access structures must be either locked or not reachable.
	 *
	 */

	static inline DataAccess *fragmentAccessObject(
		DataAccess *dataAccess, DataAccessRegion const &region,
		TaskDataAccesses &accessStructures)
	{
		assert(!dataAccess->hasBeenDiscounted());
		assert(dataAccess->getObjectType() == access_type);

		if (dataAccess->getAccessRegion().fullyContainedIn(region)) {
			// Nothing to fragment
			return dataAccess;
		}

		TaskDataAccesses::accesses_t::iterator position =
			accessStructures._accesses.iterator_to(*dataAccess);
		position = accessStructures._accesses.fragmentByIntersection(
			position, region,
			/* removeIntersection */ false,
			/* duplicator */
			[&](DataAccess const &toBeDuplicated) -> DataAccess * {
				assert(toBeDuplicated.isRegistered());
				return duplicateDataAccess(toBeDuplicated, accessStructures);
			},
			/* postprocessor */
			[&](DataAccess *fragment, DataAccess *originalDataAccess) {
				setUpNewFragment(fragment, originalDataAccess, accessStructures);
			});

		/*
		 * Return the part of this access that is fully inside the given region
		 */
		dataAccess = &(*position);
		assert(dataAccess != nullptr);
		assert(dataAccess->getAccessRegion().fullyContainedIn(region));

		return dataAccess;
	}


	static inline DataAccess *fragmentFragmentObject(
		DataAccess *dataAccess, DataAccessRegion const &region,
		TaskDataAccesses &accessStructures)
	{
		assert(!dataAccess->hasBeenDiscounted());
		assert(dataAccess->getObjectType() == fragment_type);

		if (dataAccess->getAccessRegion().fullyContainedIn(region)) {
			// Nothing to fragment
			return dataAccess;
		}

		TaskDataAccesses::access_fragments_t::iterator position =
			accessStructures._accessFragments.iterator_to(*dataAccess);
		position = accessStructures._accessFragments.fragmentByIntersection(
			position, region,
			/* removeIntersection */ false,
			/* duplicator */
			[&](DataAccess const &toBeDuplicated) -> DataAccess * {
				assert(toBeDuplicated.isRegistered());
				return duplicateDataAccess(toBeDuplicated, accessStructures);
			},
			/* postprocessor */
			[&](DataAccess *fragment, DataAccess *originalDataAccess) {
				setUpNewFragment(fragment, originalDataAccess, accessStructures);
			});

		/*
		 * Return the part of this fragment that is fully inside the given region
		 */
		dataAccess = &(*position);
		assert(dataAccess != nullptr);
		assert(dataAccess->getAccessRegion().fullyContainedIn(region));

		return dataAccess;
	}


	static inline DataAccess *fragmentTaskwaitFragmentObject(
		DataAccess *dataAccess, DataAccessRegion const &region,
		TaskDataAccesses &accessStructures)
	{
		assert(!dataAccess->hasBeenDiscounted());
		assert((dataAccess->getObjectType() == taskwait_type) || (dataAccess->getObjectType() == top_level_sink_type));

		if (dataAccess->getAccessRegion().fullyContainedIn(region)) {
			// Nothing to fragment
			return dataAccess;
		}

		TaskDataAccesses::taskwait_fragments_t::iterator position =
			accessStructures._taskwaitFragments.iterator_to(*dataAccess);
		position = accessStructures._taskwaitFragments.fragmentByIntersection(
			position, region,
			/* removeIntersection */ false,
			/* duplicator */
			[&](DataAccess const &toBeDuplicated) -> DataAccess * {
				assert(toBeDuplicated.isRegistered());
				return duplicateDataAccess(toBeDuplicated, accessStructures);
			},
			/* postprocessor */
			[&](DataAccess *fragment, DataAccess *originalDataAccess) {
				setUpNewFragment(fragment, originalDataAccess, accessStructures);
			});

		/*
		 * Return the part of this taskwait fragment that is fully inside the given region
		 */
		dataAccess = &(*position);
		assert(dataAccess != nullptr);
		assert(dataAccess->getAccessRegion().fullyContainedIn(region));

		return dataAccess;
	}


	/*
	 * fragmentAccess: Fragment a data access, fragment or taskwait as
	 * necessary to match a region.
	 *
	 * The task access structures must be either locked or not reachable.
	 *
	 * This function should be called inside one of the processors for the
	 * access structures (e.g. processAll, processIntersecting, ...). If
	 * fragmentation is necessary, then the this function will split the
	 * access/fragment/taskwait into multiple parts. The return value will be
	 * the first part; the other parts will be visited later by the iterator
	 * and processed by a subsequent call to the supplied lambda function.
	 */
	static inline DataAccess *fragmentAccess(
		DataAccess *dataAccess, DataAccessRegion const &region,
		TaskDataAccesses &accessStructures)
	{
		assert(dataAccess != nullptr);
		// assert(accessStructures._lock.isLockedByThisThread()); // Not necessary when fragmenting an access that is not reachable
		assert(accessStructures._lock.isLockedByThisThread() || noAccessIsReachable(accessStructures));
		assert(&dataAccess->getOriginator()->getDataAccesses() == &accessStructures);
		assert(!accessStructures.hasBeenDeleted());

		// This following assert did once fail, when called indirectly from
		// unregisterTaskDataAccesses => ... => processUpdateOperation => ...
		// (but only with two runtimes on a node):
		// salloc -q debug -c 48 -n 2 -t 01:00:00
		// ./nasty.py --nodes 2 --tasks 40 --nesting 4 --seed 135
		// mcc -fsanitize=address -fno-omit-frame-pointer -ggdb -o nasty --ompss-2 nasty.c
		// mpirun -np 4 ./nasty
		// assert(!dataAccess->hasBeenDiscounted());
#ifndef NDEBUG
		if (dataAccess->hasBeenDiscounted()) {
			std::cerr << "Warning: dataAccess->hasBeenDiscounted at " << __FILE__ << ":" << __LINE__ << std::endl;
		}
#endif

		if (dataAccess->getAccessRegion().fullyContainedIn(region)) {
			// Nothing to fragment: this task access/fragment is fully contained inside the region
			return dataAccess;
		}

		if (dataAccess->getObjectType() == access_type) {
			return fragmentAccessObject(dataAccess, region, accessStructures);
		} else if (dataAccess->getObjectType() == fragment_type) {
			return fragmentFragmentObject(dataAccess, region, accessStructures);
		} else {
			assert((dataAccess->getObjectType() == taskwait_type) || (dataAccess->getObjectType() == top_level_sink_type));
			return fragmentTaskwaitFragmentObject(dataAccess, region, accessStructures);
		}
	}


	static inline void processSatisfiedCommutativeOriginators(/* INOUT */ CPUDependencyData &hpDependencyData)
	{
		if (!hpDependencyData._satisfiedCommutativeOriginators.empty()) {
			CommutativeScoreboard::_lock.lock();
			for (Task *satisfiedCommutativeOriginator : hpDependencyData._satisfiedCommutativeOriginators) {
				assert(satisfiedCommutativeOriginator != 0);

				bool acquiredCommutativeSlots =
					CommutativeScoreboard::addAndEvaluateTask(satisfiedCommutativeOriginator, hpDependencyData);
				if (acquiredCommutativeSlots) {
					hpDependencyData._satisfiedOriginators.push_back(satisfiedCommutativeOriginator);
				}
			}
			CommutativeScoreboard::_lock.unlock();

			hpDependencyData._satisfiedCommutativeOriginators.clear();
		}
	}


	//! Process all the originators that have become ready
	static inline void processSatisfiedOriginators(
		/* INOUT */ CPUDependencyData &hpDependencyData,
		ComputePlace *computePlace,
		bool fromBusyThread)
	{
		processSatisfiedCommutativeOriginators(hpDependencyData);

		// NOTE: This is done without the lock held and may be slow since it can enter the scheduler
		for (Task *satisfiedOriginator : hpDependencyData._satisfiedOriginators) {
			assert(satisfiedOriginator != 0);

			ComputePlace *computePlaceHint = nullptr;
			if (computePlace != nullptr) {
				if (computePlace->getType() == satisfiedOriginator->getDeviceType()) {
					computePlaceHint = computePlace;
				}
			}

			ReadyTaskHint schedulingHint = SIBLING_TASK_HINT;
			if (fromBusyThread || !computePlaceHint || !computePlaceHint->isOwned()) {
				schedulingHint = BUSY_COMPUTE_PLACE_TASK_HINT;
			}

			Scheduler::addReadyTask(satisfiedOriginator, computePlaceHint, schedulingHint);
		}

		hpDependencyData._satisfiedOriginators.clear();
	}


	static void applyUpdateOperationOnAccess(
		UpdateOperation const &updateOperation,
		DataAccess *access, TaskDataAccesses &accessStructures,
		/* OUT */ CPUDependencyData &hpDependencyData)
	{
		// Fragment if necessary
		access = fragmentAccess(access, updateOperation._region, accessStructures);
		assert(access != nullptr);

		DataAccessStatusEffects initialStatus(access);


		if (updateOperation._makeReadSatisfied) {
			access->setReadSatisfied(updateOperation._location);
		}
		if (updateOperation._makeWriteSatisfied) {
			/*
			 * NOTE: although normally an access becomes read satisified before
			 * (or at the same time as) it becomes write satisfied, it is valid
			 * for the write satisfiability to arrive first. This reordering
			 * happens for example due to the race between setting
			 * _make{Read/Write}Satisfied and calling
			 * applyUpdateOperationOnAccess as a delayed operation.
			 */
			access->setWriteSatisfied();
		}

		// Concurrent Satisfiability
		if (updateOperation._makeConcurrentSatisfied) {
			access->setConcurrentSatisfied();
		}
		if (updateOperation._makeCommutativeSatisfied) {
			access->setCommutativeSatisfied();
		}
		if (updateOperation._releaseStep != nullptr) {
			access->setDataReleaseStep(updateOperation._releaseStep);
		}

		// ReductionInfo
		if (updateOperation._setReductionInfo) {
			access->setPreviousReductionInfo(updateOperation._reductionInfo);

			// ReductionInfo can be already assigned for partially overlapping reductions
			if (access->getReductionInfo() != nullptr) {
				assert(access->getType() == REDUCTION_ACCESS_TYPE);
				assert(access->allocatedReductionInfo());
			} else if ((access->getType() == REDUCTION_ACCESS_TYPE)
					   && (updateOperation._reductionInfo != nullptr)
					   && (access->getReductionTypeAndOperatorIndex() == updateOperation._reductionInfo->getTypeAndOperatorIndex())) {
				// Received compatible ReductionInfo
				access->setReductionInfo(updateOperation._reductionInfo);

				Instrument::receivedCompatibleReductionInfo(
					access->getInstrumentationId(),
					*updateOperation._reductionInfo);
			}

			access->setReceivedReductionInfo();
		}

		// ReductionSlotSet
		if (updateOperation._reductionSlotSet.size() > 0) {
			assert((access->getObjectType() == access_type) || (access->getObjectType() == fragment_type) || (access->getObjectType() == taskwait_type));
			assert(access->getType() == REDUCTION_ACCESS_TYPE);
			assert(access->getReductionSlotSet().size() == updateOperation._reductionSlotSet.size());

			access->getReductionSlotSet() |= updateOperation._reductionSlotSet;
			access->setReceivedReductionSlotSet();
		}

		// Topmost
		if (updateOperation._makeTopmost) {
			access->setTopmost();
		}

		// Top Level
		if (updateOperation._makeTopLevel) {
			access->setTopLevel();
		}

		DataAccessStatusEffects updatedStatus(access);

		handleDataAccessStatusChanges(
			initialStatus, updatedStatus,
			access, accessStructures, updateOperation._target._task,
			hpDependencyData);
	}

	/*
	 * Process an update operation.
	 *
	 * The lock must already be taken on the target task's access structures. The
	 * target task is updateOperation._target._task.
	 */
	static void processUpdateOperation(
		UpdateOperation const &updateOperation,
		/* OUT */ CPUDependencyData &hpDependencyData)
	{
		assert(!updateOperation.empty());

		TaskDataAccesses &accessStructures = updateOperation._target._task->getDataAccesses();

		/* Check lock on access structures already taken by caller */
		assert(accessStructures._lock.isLockedByThisThread());

		if (updateOperation._target._objectType == access_type) {
			// Update operation for accesses
			accessStructures._accesses.processIntersecting(
				updateOperation._region,
				[&](TaskDataAccesses::accesses_t::iterator accessPosition) -> bool {
					DataAccess *access = &(*accessPosition);

					applyUpdateOperationOnAccess(updateOperation, access, accessStructures, hpDependencyData);

					return true;
				});
		} else if (updateOperation._target._objectType == fragment_type) {
			// Update operation for fragments
			accessStructures._accessFragments.processIntersecting(
				updateOperation._region,
				[&](TaskDataAccesses::access_fragments_t::iterator fragmentPosition) -> bool {
					DataAccess *fragment = &(*fragmentPosition);

					applyUpdateOperationOnAccess(updateOperation, fragment, accessStructures, hpDependencyData);

					return true;
				});
		} else {
			// Update operation for taskwait Fragments
			assert((updateOperation._target._objectType == taskwait_type) || (updateOperation._target._objectType == top_level_sink_type));
			accessStructures._taskwaitFragments.processIntersecting(
				updateOperation._region,
				[&](TaskDataAccesses::access_fragments_t::iterator position) -> bool {
					DataAccess *taskwaitFragment = &(*position);

					applyUpdateOperationOnAccess(updateOperation, taskwaitFragment, accessStructures, hpDependencyData);

					return true;
				});
		}
	}


	/*
	 * Process the delayed operations. These are operations that are triggered
	 * by handleDataAccessStatusChanges. A lock was already taken on a task's
	 * access structures before calling handleDataAccessStatusChanges. Since
	 * these delayed operations require taking a lock on a different task's
	 * access structures, they couldn't have been done at the time without
	 * risking a deadlock.
	 */
	static inline void processDelayedOperations(
		/* INOUT */ CPUDependencyData &hpDependencyData)
	{
		Task *lastLocked = nullptr;

		while (!hpDependencyData._delayedOperations.empty()) {
			UpdateOperation &delayedOperation = hpDependencyData._delayedOperations.front();

			assert(delayedOperation._target._task != nullptr);
			if (delayedOperation._target._task != lastLocked) {
				if (lastLocked != nullptr) {
					lastLocked->getDataAccesses()._lock.unlock();
				}
				lastLocked = delayedOperation._target._task;
				lastLocked->getDataAccesses()._lock.lock();
			}

			processUpdateOperation(delayedOperation, hpDependencyData);

			hpDependencyData._delayedOperations.pop_front();
		}

		if (lastLocked != nullptr) {
			lastLocked->getDataAccesses()._lock.unlock();
		}
	}


	static inline void processReleasedCommutativeRegions(
		/* INOUT */ CPUDependencyData &hpDependencyData)
	{
		if (!hpDependencyData._releasedCommutativeRegions.empty()) {
			CommutativeScoreboard::_lock.lock();
			CommutativeScoreboard::processReleasedCommutativeRegions(hpDependencyData);
			CommutativeScoreboard::_lock.unlock();
		}
	}


	static void processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(
		CPUDependencyData &hpDependencyData,
		ComputePlace *computePlace,
		bool fromBusyThread)
	{
		processReleasedCommutativeRegions(hpDependencyData);

#if NO_DEPENDENCY_DELAYED_OPERATIONS
#else
		processDelayedOperations(hpDependencyData);
#endif

		handleCompletedTaskwaits(hpDependencyData._completedTaskwaits, computePlace);
		processSatisfiedOriginators(hpDependencyData, computePlace, fromBusyThread);
		assert(hpDependencyData._satisfiedOriginators.empty());

		handleRemovableTasks(hpDependencyData._removableTasks);
	}


	/*
	 * Create an initial fragment due to an access of a newly-submitted child
	 * task that was not yet in the bottom map, but is covered by the parent
	 * task's accesses. The parent task's fragments correspond to child task
	 * accesses (after fragmentation) within the parent's accesses. This method
	 * creates the new fragment, but not the bottom map entry corresponding to
	 * it.
	 */
	static inline DataAccess *createInitialFragment(
		TaskDataAccesses::accesses_t::iterator accessPosition,
		TaskDataAccesses &accessStructures,       /* Access structures for the parent task */
		DataAccessRegion subregion                /* Subregion accessed by the new child task */
	) {
		DataAccess *dataAccess = &(*accessPosition);
		assert(dataAccess != nullptr);
		assert(!accessStructures.hasBeenDeleted());

		assert(!accessStructures._accessFragments.contains(dataAccess->getAccessRegion()));

		Instrument::data_access_id_t instrumentationId =
			Instrument::createdDataSubaccessFragment(dataAccess->getInstrumentationId());
		DataAccess *fragment = ObjectAllocator<DataAccess>::newObject(
			fragment_type,
			dataAccess->getType(),
			dataAccess->isWeak(),
			dataAccess->getOriginator(),
			dataAccess->getAccessRegion(),
			dataAccess->getReductionTypeAndOperatorIndex(),
			dataAccess->getReductionIndex(),
			dataAccess->getLocation(),
			dataAccess->getOutputLocation(),
			dataAccess->getDataReleaseStep(),
			dataAccess->getDataLinkStep(),
			instrumentationId);

		fragment->inheritFragmentStatus(dataAccess); //TODO is it necessary?

#ifndef NDEBUG
		fragment->setReachable();
#endif

		// This assertion is wrong: it is in fact possible for write satisfiability
		// to arrive before read satisfiable. This is due to race conditions in the
		// runtime system.
		// assert(fragment->readSatisfied() || !fragment->writeSatisfied());

		accessStructures._accessFragments.insert(*fragment);
		fragment->setInBottomMap();

		// NOTE: This may in the future need to be included in the common status changes code
		dataAccess->setHasSubaccesses();

		//! The DataReleaseStep of the access will be propagated through the fragment(s).
		//! Unset it here so we avoid needless (and possibly wrong) checks for this access.
		if (dataAccess->hasDataReleaseStep()) {
			dataAccess->unsetDataReleaseStep();
		}

		if (subregion != dataAccess->getAccessRegion()) {
			dataAccess->getAccessRegion().processIntersectingFragments(
				subregion,
				/* thisOnlyProcessor */
				[&](DataAccessRegion excludedSubregion) {
					BottomMapEntry *bottomMapEntry = ObjectAllocator<BottomMapEntry>::newObject(
						excludedSubregion,
						DataAccessLink(dataAccess->getOriginator(), fragment_type),
						dataAccess->getType(),
						dataAccess->getReductionTypeAndOperatorIndex());
					accessStructures._subaccessBottomMap.insert(*bottomMapEntry);
				},
				/* intersectingProcessor */
				[&](__attribute__((unused)) DataAccessRegion intersection) {
				},
				/* otherOnlyProcessor */
				[&](__attribute__((unused)) DataAccessRegion unmatchedRegion) {
					// This part is not covered by the access
				});
		}

		return fragment;
	}

	/*
	 * Fragment the linked object (access, fragment or taskwait) against the
	 * given region, and call the supplied function on the fragment of the
	 * access fully contained inside the region. It needs to check the type of
	 * the object and fragment it in the appropriate way (using the correct
	 * function to fragment it and correct processor to iterate over the
	 * corresponding list.
	 */
	template <typename ProcessorType>
	static inline bool followLink(
		DataAccessLink const &link,
		DataAccessRegion const &region,
		ProcessorType processor)
	{
		Task *task = link._task;
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(accessStructures._lock.isLockedByThisThread());

		if (link._objectType == access_type) {
			/*
			 * An access, iterate over accessStructures._accesses and
			 * fragment using fragmentAccessObject.
			 */
			return accessStructures._accesses.processIntersecting(
				region,
				[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
					DataAccess *access = &(*position);
					assert(!access->hasBeenDiscounted());

					/* Fragment it */
					access = fragmentAccessObject(access, region, accessStructures);

					/* call the processor on each access fragment */
					return processor(access);
				});
		} else if (link._objectType == fragment_type) {
			/*
			 * An fragment, iterate over accessStructures._accessFragments and
			 * fragment using fragmentFragmentObject.
			 */
			return accessStructures._accessFragments.processIntersecting(
				region,
				[&](TaskDataAccesses::access_fragments_t::iterator position) -> bool {
					DataAccess *access = &(*position);
					assert(!access->hasBeenDiscounted());

					/* Fragment it */
					access = fragmentFragmentObject(access, region, accessStructures);

					/* call the processor on each fragment fragment */
					return processor(access);
				});
		} else {
			/*
			 * A taskwait fragment, iterate over accessStructures._taskwaitFragments and
			 * fragment using fragmentTaskwaitFragmentObject.
			 */
			assert((link._objectType == taskwait_type) || (link._objectType == top_level_sink_type));
			return accessStructures._taskwaitFragments.processIntersecting(
				region,
				[&](TaskDataAccesses::taskwait_fragments_t::iterator position) -> bool {
					DataAccess *access = &(*position);
					assert(!access->hasBeenDiscounted());

					/* Fragment it */
					access = fragmentTaskwaitFragmentObject(access, region, accessStructures);

					/* call the processor on each taskwait fragment */
					return processor(access);
				});
		}
	}

/*
 * Matching processor used to put a new task's data access into the bottom map.
 */
	template <typename MatchingProcessorType, typename MissingProcessorType>
	static inline bool foreachBottomMapMatchPossiblyCreatingInitialFragmentsAndMissingRegion(
		Task *parent, TaskDataAccesses &parentAccessStructures,
		DataAccessRegion region,
		MatchingProcessorType matchingProcessor, MissingProcessorType missingProcessor)
	{
		assert(parent != nullptr);
		assert((&parentAccessStructures) == (&parent->getDataAccesses()));
		assert(!parentAccessStructures.hasBeenDeleted());

		return parentAccessStructures._subaccessBottomMap.processIntersectingAndMissing(
			region,

			/*
			 * A region of the new task's data access is already in the bottom map.
			 */
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);

				DataAccessRegion subregion = region.intersect(bottomMapEntry->getAccessRegion());
				BottomMapEntryContents bmeContents = *bottomMapEntry;

				DataAccessLink target = bmeContents._link;
				assert(target._task != nullptr);

				bool result = true;
				if (target._task != parent) {
					TaskDataAccesses &subtaskAccessStructures = target._task->getDataAccesses();

					subtaskAccessStructures._lock.lock();

					// For each access of the subtask (predecessor on the bottom map) that matches
					result = followLink(
						target, subregion,
						[&](DataAccess *previous) -> bool {
							assert(!previous->hasNext());
							assert(previous->isInBottomMap());

							return matchingProcessor(previous, bmeContents);
						});

					subtaskAccessStructures._lock.unlock();
				} else {
					// A fragment
					assert(target._objectType == fragment_type);

					// For each fragment of the parent that matches
					result = followLink(
						target, subregion,
						[&](DataAccess *previous) -> bool {
							assert(!previous->hasNext());
							assert(previous->isInBottomMap());

							return matchingProcessor(previous, bmeContents);
						});
				}

				bottomMapEntry = fragmentBottomMapEntry(bottomMapEntry, subregion, parentAccessStructures);

				/*
				 * The (first part of) the bottom map entry has been fully
				 * handled, and it will be covered by the new bottom map entry
				 * for the new task. So we can delete the old bottom map entry
				 * now.
				 */
				parentAccessStructures._subaccessBottomMap.erase(*bottomMapEntry);
				ObjectAllocator<BottomMapEntry>::deleteObject(bottomMapEntry);

				return result;
			},

			/*
			 * A region of the new task's data access that is not (yet) in the bottom map.
			 * Iterate over the parent's accesses and divide into those parts that intersect
			 * parent accesses and those that do not.
			 */
			[&](DataAccessRegion missingRegion) -> bool {
				parentAccessStructures._accesses.processIntersectingAndMissing(
					missingRegion,
					/*
					 * intersectingProcessor: it's not in the bottom map yet, but it
					 * is part of one of the parent task's accesses. Create a new
					 * bottom map entry and now that it exists, give it to the
					 * matching processor.
					 */
					[&](TaskDataAccesses::accesses_t::iterator superaccessPosition) -> bool {
						DataAccessStatusEffects initialStatus;

						/* Create an initial fragment */
						DataAccess *previous = createInitialFragment(
							superaccessPosition, parentAccessStructures,
							missingRegion);
						assert(previous != nullptr);
						assert(previous->getObjectType() == fragment_type);

						previous->setTopmost(); /* it's top-most: in the parent? */
						previous->setRegistered(); /* register it immediately */

						DataAccessStatusEffects updatedStatus(previous);

						BottomMapEntryContents bmeContents(
							DataAccessLink(parent, fragment_type),
							previous->getType(),
							previous->getReductionTypeAndOperatorIndex());

						{
							CPUDependencyData hpDependencyData;
							handleDataAccessStatusChanges(
								initialStatus, updatedStatus,
								previous, parentAccessStructures, parent,
								hpDependencyData);
							assert(hpDependencyData.empty());
						}

						previous = fragmentAccess(previous, missingRegion, parentAccessStructures);

						/*
						 *Now that the bottom map entry has been created, pass it
						 * to the matching processor
						 */
						return matchingProcessor(previous, bmeContents);
					},

					/*
					 * missingProcessor: the region isn't in the bottom map or
					 * the parent task's accesses. Pass this "hole" up to the
					 * missing processor to deal with.
					 */
					[&](DataAccessRegion regionUncoveredByParent) -> bool {
						return missingProcessor(regionUncoveredByParent);
					});

				/* Keep going, with other regions of the bottom map */
				return true;
			});
	}


	template <typename ProcessorType, typename BottomMapEntryProcessorType>
	static inline void foreachBottomMapMatch(
		DataAccessRegion const &region,
		TaskDataAccesses &accessStructures, Task *task,
		ProcessorType processor,
		BottomMapEntryProcessorType bottomMapEntryProcessor = [](BottomMapEntry *) {})
	{
		assert(!accessStructures.hasBeenDeleted());
		assert(accessStructures._lock.isLockedByThisThread());

		accessStructures._subaccessBottomMap.processIntersecting(
			region,
			/* processor: called with each part of the bottom map that intersects region */
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);

				DataAccessLink target = bottomMapEntry->_link;
				assert(target._task != nullptr);

				DataAccessRegion subregion = region.intersect(bottomMapEntry->getAccessRegion());

				if (target._task != task) {
					// An access from a subtask

					TaskDataAccesses &subtaskAccessStructures = target._task->getDataAccesses();
					subtaskAccessStructures._lock.lock();

					// For each access of the subtask that matches
					followLink(
						target, subregion,
						[&](DataAccess *subaccess) -> bool {
							assert(subaccess->isReachable());
							assert(subaccess->isInBottomMap());

							processor(subaccess, subtaskAccessStructures, target._task);

							return true;
						});

					subtaskAccessStructures._lock.unlock();
				} else {
					// A fragment from the current task, a taskwait fragment, or a top level sink
					assert(
						(target._objectType == fragment_type)
						|| (target._objectType == taskwait_type)
						|| (target._objectType == top_level_sink_type));

					followLink(
						target, subregion,
						[&](DataAccess *fragment) -> bool {
							assert(fragment->isReachable());
							assert(fragment->isInBottomMap());

							processor(fragment, accessStructures, task);

							return true;
						});
				}

				bottomMapEntryProcessor(bottomMapEntry);

				/* always continue through the bottom map*/
				return true;
			});
	}


	template <typename ProcessorType, typename BottomMapEntryProcessorType>
	static inline void foreachBottomMapEntry(
		TaskDataAccesses &accessStructures, Task *task,
		ProcessorType processor,
		BottomMapEntryProcessorType bottomMapEntryProcessor = [](BottomMapEntry *) {})
	{
		assert(!accessStructures.hasBeenDeleted());
		assert(accessStructures._lock.isLockedByThisThread());

		accessStructures._subaccessBottomMap.processAll(
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);

				DataAccessLink target = bottomMapEntry->_link;
				assert(target._task != nullptr);

				DataAccessRegion const &subregion = bottomMapEntry->getAccessRegion();

				if (target._task != task) {
					// An access from a subtask

					TaskDataAccesses &subtaskAccessStructures = target._task->getDataAccesses();
					subtaskAccessStructures._lock.lock();

					// For each access of the subtask that matches
					followLink(
						target, subregion,
						[&](DataAccess *subaccess) -> bool {
							assert(subaccess->isReachable());
							assert(subaccess->isInBottomMap());

							processor(subaccess, subtaskAccessStructures, target._task);

							return true;
						});

					subtaskAccessStructures._lock.unlock();
				} else {
					// A fragment from the current task
					assert(target._objectType == fragment_type);

					followLink(
						target, subregion,
						[&](DataAccess *fragment) -> bool {
							assert(fragment->isReachable());
							assert(fragment->isInBottomMap());

							processor(fragment, accessStructures, task);

							return true;
						});
				}

				bottomMapEntryProcessor(bottomMapEntry);

				return true;
			});
	}


	static inline void processBottomMapUpdate(
		BottomMapUpdateOperation &operation,
		TaskDataAccesses &accessStructures, Task *task,
		/* OUT */ CPUDependencyData &hpDependencyData)
	{
		assert(task != nullptr);
		assert(!operation.empty());
		assert(!operation._region.empty());
		assert(!accessStructures.hasBeenDeleted());
		assert(accessStructures._lock.isLockedByThisThread());

		assert(operation._linkBottomMapAccessesToNext);
		foreachBottomMapMatch(
			operation._region,
			accessStructures, task,
			[&](DataAccess *access, TaskDataAccesses &currentAccessStructures, Task *currentTask) {
				FatalErrorHandler::failIf(
					((operation._parentAccessType == CONCURRENT_ACCESS_TYPE) || (operation._parentAccessType == COMMUTATIVE_ACCESS_TYPE))
						&& access->getType() == REDUCTION_ACCESS_TYPE,
					"Task '",
					(access->getOriginator()->getTaskInfo()->implementations[0].task_label != nullptr) ? access->getOriginator()->getTaskInfo()->implementations[0].task_label : access->getOriginator()->getTaskInfo()->implementations[0].declaration_source,
					"' declares a reduction within a region registered as ",
					(operation._parentAccessType == CONCURRENT_ACCESS_TYPE) ? "concurrent" : "commutative",
					" by task '",
					(task->getTaskInfo()->implementations[0].task_label != nullptr) ? task->getTaskInfo()->implementations[0].task_label : task->getTaskInfo()->implementations[0].declaration_source,
					"' without a taskwait");

				DataAccessStatusEffects initialStatus(access);

				if (operation._inhibitReadSatisfiabilityPropagation) {
					access->unsetCanPropagateReadSatisfiability();
				}

				if (operation._inhibitConcurrentSatisfiabilityPropagation) {
					access->unsetCanPropagateConcurrentSatisfiability();
				}

				if (operation._inhibitCommutativeSatisfiabilityPropagation) {
					access->unsetCanPropagateCommutativeSatisfiability();
				}

				if (operation._inhibitReductionInfoPropagation) {
					access->unsetCanPropagateReductionInfo();
				}

				if (operation._setCloseReduction) {
					// Note: It is currently unsupported that a strong reduction access has
					// subaccesses, as this implies a task-scheduling point.
					// Even if this becomes supported in the future, the following scenario
					// needs to be controlled, possibly by inserting a nested taskwait
					FatalErrorHandler::failIf(
						(operation._parentAccessType == REDUCTION_ACCESS_TYPE) && (access->getType() != REDUCTION_ACCESS_TYPE),
						"Task '",
						(access->getOriginator()->getTaskInfo()->implementations[0].task_label != nullptr) ? access->getOriginator()->getTaskInfo()->implementations[0].task_label : access->getOriginator()->getTaskInfo()->implementations[0].declaration_source,
						"' declares a non-reduction access within a region registered as reduction by task '",
						(task->getTaskInfo()->implementations[0].task_label != nullptr) ? task->getTaskInfo()->implementations[0].task_label : task->getTaskInfo()->implementations[0].declaration_source,
						"'");

					if (access->getType() == REDUCTION_ACCESS_TYPE) {
						access->setClosesReduction();
					}
				}

				assert(!access->hasNext());
				access->setNext(operation._next);

				DataAccessStatusEffects updatedStatus(access);

				handleDataAccessStatusChanges(
					initialStatus, updatedStatus,
					access, currentAccessStructures, currentTask,
					hpDependencyData);
			},
			[](BottomMapEntry *) {});
	}


	static inline void allocateReductionInfo(DataAccess &dataAccess, const Task &task)
	{
		assert(dataAccess.getType() == REDUCTION_ACCESS_TYPE);

		Instrument::enterAllocateReductionInfo(
			dataAccess.getInstrumentationId(),
			dataAccess.getAccessRegion());

		nanos6_task_info_t *taskInfo = task.getTaskInfo();
		assert(taskInfo != nullptr);

		reduction_index_t reductionIndex = dataAccess.getReductionIndex();

		ReductionInfo *newReductionInfo = ObjectAllocator<ReductionInfo>::newObject(
			dataAccess.getAccessRegion(),
			dataAccess.getReductionTypeAndOperatorIndex(),
			taskInfo->reduction_initializers[reductionIndex],
			taskInfo->reduction_combiners[reductionIndex]);

		// Note: ReceivedReductionInfo flag is not set, as the access will still receive
		// an (invalid) reduction info from the propagation system
		dataAccess.setReductionInfo(newReductionInfo);
		dataAccess.setAllocatedReductionInfo();

		Instrument::exitAllocateReductionInfo(
			dataAccess.getInstrumentationId(),
			*newReductionInfo);
	}

	/*
	 * Update the bottom map with a data access for a new task. This function
	 * is called by linkTaskAccesses.
	 */
	static inline void replaceMatchingInBottomMapLinkAndPropagate(
		DataAccessLink const &next,                /* link to the new task */
		TaskDataAccesses &accessStructures,        /* access structures for the new task */
		DataAccess *dataAccess,                    /* data access for the new task */
		Task *parent,                              /* parent of the new task */
		TaskDataAccesses &parentAccessStructures,  /* access structures for the parent */
		/* inout */ CPUDependencyData &hpDependencyData
	) {
		assert(dataAccess != nullptr);
		assert(parent != nullptr);
		assert(next._task != nullptr);
		assert(!accessStructures.hasBeenDeleted());
		assert(!parentAccessStructures.hasBeenDeleted());

		DataAccessRegion region = dataAccess->getAccessRegion();

		bool hasAllocatedReductionInfo = false;
		ReductionInfo *previousReductionInfo = nullptr;
		Container::vector<DataAccess *> previousReductionAccesses;

		bool local = false;
#ifndef NDEBUG
		bool lastWasLocal = false;
		bool first = true;
#endif

		DataAccessType parentAccessType = NO_ACCESS_TYPE;
		reduction_type_and_operator_index_t parentReductionTypeAndOperatorIndex = no_reduction_type_and_operator;

		/*
		 * Put the new data access (dataAccess) in the bottom map.
		 *
		 * There may be multiple entries in the bottom map that intersect the
		 * new data access ("foreachBottomMapMatch"). The new data access may
		 * alternatively be contained within accesses of the parent task that
		 * are not yet in the bottom map because no child task has accessed
		 * them yet ("PossiblyCreatingInitialFragments").  Finally the new data
		 * access may not be contained within any access of the parent task
		 * ("AndMissingRegion").
		 *
		 * Processing of the first two cases is done by the first big lambda
		 * (matchingProcessor) and processing of the last case is done by the
		 * second big lambda (missingProcessor).
		 */

		foreachBottomMapMatchPossiblyCreatingInitialFragmentsAndMissingRegion(
			parent,
			parentAccessStructures,  /* contains the parent's bottom map */
			region,

			/* matchingProcessor: handle a region (part of the new overall
			 * data access) that was already in the bottom map. The existing
			 * bottom map access is 'previous'. */
			[&](DataAccess *previous, BottomMapEntryContents const &bottomMapEntryContents) -> bool {
				assert(previous != nullptr);
				assert(previous->isReachable());
				assert(!previous->hasBeenDiscounted());
				assert(!previous->hasNext());       /* no next access yet, as it was in the bottom map */

				Task *previousTask = previous->getOriginator();
				assert(previousTask != nullptr);

				parentAccessType = bottomMapEntryContents._accessType;
				parentReductionTypeAndOperatorIndex = bottomMapEntryContents._reductionTypeAndOperatorIndex;
				local = (bottomMapEntryContents._accessType == NO_ACCESS_TYPE);

				/*
				 * Handle reductions
				 */
				if ((dataAccess->getType() == REDUCTION_ACCESS_TYPE) && !hasAllocatedReductionInfo) {
					bool allocatesReductionInfo = false;

					if (previous->getReductionTypeAndOperatorIndex() != dataAccess->getReductionTypeAndOperatorIndex()) {
						// When a reduction access is to be linked with any non-matching access, we want to
						// allocate a new reductionInfo to it before it gets fragmented by propagation operations
						allocatesReductionInfo = true;
					} else {
						if (previousReductionInfo == nullptr) {
							previousReductionInfo = previous->getReductionInfo();
						} else if (previous->getReductionInfo() != previousReductionInfo) {
							// Has multiple previous reductions, need to allocate new reduction info
							allocatesReductionInfo = true;
						}
					}

					if (allocatesReductionInfo) {
						hasAllocatedReductionInfo = true;

						DataAccessStatusEffects initialStatus(dataAccess);
						allocateReductionInfo(*dataAccess, *next._task);
						DataAccessStatusEffects updatedStatus(dataAccess);

						handleDataAccessStatusChanges(
							initialStatus, updatedStatus,
							dataAccess, accessStructures, next._task,
							hpDependencyData);
					}
				}

#ifndef NDEBUG
				if (!first) {
					assert((local == lastWasLocal) && "This fails with wrongly nested regions");
				}
				first = false;
				lastWasLocal = local;
#endif

				TaskDataAccesses &previousAccessStructures = previousTask->getDataAccesses();
				assert(!previousAccessStructures.hasBeenDeleted());
				assert(previous->getAccessRegion().fullyContainedIn(region));

				DataAccessStatusEffects initialStatus(previous);

				// Mark end of reduction
				if (previous->getType() == REDUCTION_ACCESS_TYPE) {
					if (dataAccess->getReductionTypeAndOperatorIndex() != previous->getReductionTypeAndOperatorIndex()) {
						// When any access is to be linked with a non-matching reduction access,
						// we want to mark the preceding reduction access so that it is the
						// last access of its reduction chain
						previous->setClosesReduction();
					} else {
						assert(dataAccess->getType() == REDUCTION_ACCESS_TYPE);
						// When a reduction access is to be linked with a matching reduction
						// access, we don't know whether a ReductionInfo will be allocated yet
						// (it can partially overlap), so we want to keep track of the preceding
						// reduction access so that it can be later marked for closure if needed
						previousReductionAccesses.push_back(previous);
					}
				}
				/*
				 * Finished handling of reductions
				 */

				/*
				 * Link the dataAccess and unset
				 */
				previous->setNext(next);
				previous->unsetInBottomMap();  /* only unsets the status bit, doesn't actually remove it */

				DataAccessStatusEffects updatedStatus(previous);

				/*
				 * Handle the data access status changes in the difference
				 * between initialStatus and updatedStatus.
				 */
				handleDataAccessStatusChanges(
					initialStatus, updatedStatus,
					previous, previousAccessStructures, previousTask,
					hpDependencyData);

				/* Keep going with other bottom map entries (don't stop here) */
				return true;
			},

			/* missingProcessor: handle a region (part of the overall data access)
			 * that is not yet in the parent's bottom map */
			[&](DataAccessRegion missingRegion) -> bool {
				assert(!parentAccessStructures._accesses.contains(missingRegion));

				// Not part of the parent
				local = true;

#ifndef NDEBUG
				if (!first) {
					assert((local == lastWasLocal) && "This fails with wrongly nested regions");
				}
				first = false;
				lastWasLocal = local;
#endif

				// NOTE: holes in the parent bottom map that are not in the parent accesses become fully satisfied

				accessStructures._accesses.processIntersecting(
					missingRegion,
					[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
						DataAccess *targetAccess = &(*position);
						assert(targetAccess != nullptr);
						assert(!targetAccess->hasBeenDiscounted());

						// We need to allocate the reductionInfo before fragmenting the access
						if ((dataAccess->getType() == REDUCTION_ACCESS_TYPE) && !hasAllocatedReductionInfo) {
							hasAllocatedReductionInfo = true;

							DataAccessStatusEffects initialStatus(dataAccess);
							allocateReductionInfo(*dataAccess, *next._task);
							DataAccessStatusEffects updatedStatus(dataAccess);

							handleDataAccessStatusChanges(
								initialStatus, updatedStatus,
								dataAccess, accessStructures, next._task,
								hpDependencyData);
						}

						targetAccess = fragmentAccess(targetAccess, missingRegion, accessStructures);

						DataAccessStatusEffects initialStatus(targetAccess);
						//! If this is a remote task, we will receive satisfiability
						//! information later on, otherwise this is a local access,
						//! so no location is setup yet.
						//! For now we set it to the Directory MemoryPlace.
						if (!targetAccess->getOriginator()->isRemoteTask()) {
							/* TBD? Is this an access from e.g. a malloc inside the parent task? */
							targetAccess->setReadSatisfied(Directory::getDirectoryMemoryPlace());
							targetAccess->setWriteSatisfied();
						}
						targetAccess->setConcurrentSatisfied();
						targetAccess->setCommutativeSatisfied();
						targetAccess->setReceivedReductionInfo();
						// Note: setting ReductionSlotSet as received is not necessary, as its not always propagated
						targetAccess->setTopmost();
						targetAccess->setTopLevel();
						DataAccessStatusEffects updatedStatus(targetAccess);

						// TODO: We could mark in the task that there are local accesses (and remove the mark in taskwaits)

						handleDataAccessStatusChanges(
							initialStatus, updatedStatus,
							targetAccess, accessStructures, next._task,
							hpDependencyData);

						/* keep going with the other task data accesses that intersect this
						 * hole in the bottom map */
						return true;
					});

				/* Keep going with other bottom map entries (don't stop here) */
				return true;
			});

		if (hasAllocatedReductionInfo && !previousReductionAccesses.empty()) {
			assert(dataAccess->getType() == REDUCTION_ACCESS_TYPE);

			for (DataAccess *&previousAccess : previousReductionAccesses) {
				assert(previousAccess->getType() == REDUCTION_ACCESS_TYPE);
				previousAccess->setClosesReduction();
			}
		}

		// Add the entry to the bottom map
		BottomMapEntry *bottomMapEntry = ObjectAllocator<BottomMapEntry>::newObject(
			region, next, parentAccessType, parentReductionTypeAndOperatorIndex);
		parentAccessStructures._subaccessBottomMap.insert(*bottomMapEntry);
	}


	/*
	 * This is called by registerTaskDataAccesses to actually link the task
	 * data accesses into the dependency system.
	 */
	static inline void linkTaskAccesses(
		/* OUT */ CPUDependencyData &hpDependencyData,
		Task *task)
	{
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

		// No accesses: do nothing.
		if (accessStructures._accesses.empty()) {
			return;
		}

		Task *parent = task->getParent();
		assert(parent != nullptr);

		TaskDataAccesses &parentAccessStructures = parent->getDataAccesses();
		assert(!parentAccessStructures.hasBeenDeleted());

		/*
		 * Lock the parent and the task itself.
		 */
		std::lock_guard<TaskDataAccesses::spinlock_t> parentGuard(parentAccessStructures._lock);
		std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

		// Create any initial missing fragments in the parent, link the previous accesses
		// and possibly some parent fragments to the new task, and create propagation
		// operations from the previous accesses to the new task.
		//
		// The new task cannot be locked since it may have a predecessor multiple times,
		// and that could produce a dead lock if the latter is finishing (this one would
		// lock the new task first, and the predecessor later; the finishing task would
		// do the same in the reverse order). However, we need to protect the traversal
		// itself, since an already linked predecessor may produce fragmentation and thus
		// may rebalance the tree. Therefore, we just lock for advancing the iteration.
		accessStructures._accesses.processAll(
			/* processor: called for each task access */
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				DataAccess *dataAccess = &(*position);
				assert(dataAccess != nullptr);
				assert(!dataAccess->hasBeenDiscounted());

				DataAccessStatusEffects initialStatus(dataAccess);
				dataAccess->setNewInstrumentationId(task->getInstrumentationTaskId());

				/* New task accesses always in the bottom map */
				dataAccess->setInBottomMap();

				/* This is the step where accesses become registered */
				dataAccess->setRegistered();

#ifndef NDEBUG
				/*
				 * The accesses now become reachable so for most updates the
				 * task data access structures will need locking.
				 */
				dataAccess->setReachable();
#endif
				DataAccessStatusEffects updatedStatus(dataAccess);

				/* Process the changes between initialStatus and updatedStatus */
				handleDataAccessStatusChanges(
					initialStatus, updatedStatus,
					dataAccess, accessStructures, task,
					hpDependencyData);

				/* Update bottom map */
				replaceMatchingInBottomMapLinkAndPropagate(
					DataAccessLink(task, access_type),
					accessStructures,
					dataAccess,
					parent, parentAccessStructures,
					hpDependencyData);

				return true;
			});
	}


	/*
	 * Called by handleEnterTaskwait.
	 */
	static inline void finalizeFragments(
		Task *task, TaskDataAccesses &accessStructures,
		/* OUT */ CPUDependencyData &hpDependencyData)
	{
		assert(task != nullptr);
		assert(!accessStructures.hasBeenDeleted());

		// Mark the fragments as completed and propagate topmost property
		accessStructures._accessFragments.processAll(
			/* processor: set every fragment as complete (if not already) */
			[&](TaskDataAccesses::access_fragments_t::iterator position) -> bool {
				DataAccess *fragment = &(*position);
				assert(fragment != nullptr);
				assert(!fragment->hasBeenDiscounted());

				// The fragment may already be complete due to the use of the "release" directive
				if (fragment->complete()) {
					return true;
				}

				/* Set access as complete */
				DataAccessStatusEffects initialStatus(fragment);
				fragment->setComplete();
				DataAccessStatusEffects updatedStatus(fragment);

				/* Handle consequences of access becoming complete */
				handleDataAccessStatusChanges(
					initialStatus, updatedStatus,
					fragment, accessStructures, task,
					hpDependencyData);

				return true;
			});
	}


	/*
	 * Apply a lambda function (processor) to a region of a single task access
	 * and all the fragments that intersect it (fragmenting them if needed).
	 */
	template <typename ProcessorType>
	static inline void applyToAccessAndFragments(
		DataAccess *dataAccess,            /* DataAccess containing the region */
		DataAccessRegion const &region,
		TaskDataAccesses &accessStructures,
		ProcessorType processor
	) {
		// Fragment the data access if necessary (then continue with the first
		// fragment - remaining fragments will be processed later)
		dataAccess = fragmentAccess(dataAccess, region, accessStructures);
		assert(dataAccess != nullptr);

		bool hasSubaccesses = dataAccess->hasSubaccesses();
		DataAccessRegion finalRegion = dataAccess->getAccessRegion();
		bool alsoSubaccesses = processor(dataAccess);

		if (alsoSubaccesses && hasSubaccesses) {
			accessStructures._accessFragments.processIntersecting(
				finalRegion,
				[&](TaskDataAccesses::access_fragments_t::iterator position) -> bool {
					DataAccess *fragment = &(*position);
					assert(fragment != nullptr);
					assert(!fragment->hasBeenDiscounted());

					fragment = fragmentAccess(fragment, finalRegion, accessStructures);
					assert(fragment != nullptr);

					processor(fragment);

					return true;
				});
		}
	}


	static inline void releaseReductionStorage(
		__attribute__((unused)) Task *finishedTask, DataAccess *dataAccess,
		__attribute__((unused)) DataAccessRegion region,
		ComputePlace *computePlace)
	{
		assert(finishedTask != nullptr);
		assert(dataAccess != nullptr);
		assert(computePlace != nullptr);

		assert(dataAccess->getOriginator() == finishedTask);
		assert(!region.empty());

		// Release reduction slots (only when necessary)
		// Note: Remember weak accesses in final tasks will be promoted to strong
		if ((dataAccess->getType() == REDUCTION_ACCESS_TYPE) && !dataAccess->isWeak()) {
			assert(computePlace->getType() == nanos6_device_t::nanos6_host_device);

#ifdef NDEBUG
			CPU *cpu = static_cast<CPU *>(computePlace);
#else
			CPU *cpu = dynamic_cast<CPU *>(computePlace);
			assert(cpu != nullptr);
#endif

			ReductionInfo *reductionInfo = dataAccess->getReductionInfo();
			assert(reductionInfo != nullptr);

			reductionInfo->releaseSlotsInUse(cpu->getIndex());
		}
	}


	static inline void finalizeAccess(
		Task *finishedTask, DataAccess *dataAccess, DataAccessRegion region,
		MemoryPlace const *location, /* OUT */ CPUDependencyData &hpDependencyData)
	{
		assert(finishedTask != nullptr);
		assert(dataAccess != nullptr);
		assert((location != nullptr) || dataAccess->isWeak());

		assert(dataAccess->getOriginator() == finishedTask);
		assert(!region.empty());

		// The access may already have been released through the "release" directive
		if (dataAccess->complete()) {
			return;
		}
		assert(!dataAccess->hasBeenDiscounted());

		/*
		 * Set complete and update location for the access itself and all
		 * (child task) fragments.
		 */
		applyToAccessAndFragments(
			dataAccess, region,
			finishedTask->getDataAccesses(),
			[&](DataAccess *accessOrFragment) -> bool {
				assert(!accessOrFragment->complete());
				assert(accessOrFragment->getOriginator() == finishedTask);

				DataAccessStatusEffects initialStatus(accessOrFragment);
				accessOrFragment->setComplete();
				if (location != nullptr) {
					accessOrFragment->setLocation(location);
				}
				DataAccessStatusEffects updatedStatus(accessOrFragment);

				handleDataAccessStatusChanges(
					initialStatus, updatedStatus,
					accessOrFragment, finishedTask->getDataAccesses(), finishedTask,
					hpDependencyData);

				return true; // Apply also to subaccesses if any
			});
	}


	static void handleRemovableTasks(
		/* inout */ CPUDependencyData::removable_task_list_t &removableTasks)
	{
		for (Task *removableTask : removableTasks) {
			TaskFinalization::disposeTask(removableTask);
		}
		removableTasks.clear();
	}

	static void handleCompletedTaskwaits(
		CPUDependencyData::satisfied_taskwait_accesses_t &completedTaskwaits,
		__attribute__((unused)) ComputePlace *computePlace)
	{
		for (DataAccess *taskwait : completedTaskwaits) {
			assert(taskwait->getObjectType() == taskwait_type);
			ExecutionWorkflow::setupTaskwaitWorkflow(
				taskwait->getOriginator(),
				taskwait);
		}
		completedTaskwaits.clear();
	}


	/*
	 * Create a taskwait. The lock should already be taken on the task's
	 * access structures.
	 */
	static void createTaskwait(
		Task *task, TaskDataAccesses &accessStructures, ComputePlace *computePlace,
		/* OUT */ CPUDependencyData &hpDependencyData)
	{
		assert(task != nullptr);
		assert(accessStructures._lock.isLockedByThisThread());

		if (accessStructures._subaccessBottomMap.empty()) {
			return;
		}

		// The last taskwait fragment will decrease the blocking count.
		// This is necessary to force the task to wait until all taskwait fragments have finished.
		task->increaseBlockingCount();

		/*
		 * There should not already be any taskwait fragments.
		 */
		assert(accessStructures._taskwaitFragments.empty());

		/*
		 * The accesses to wait for are precisely those in the bottom map.
		 * Iterate through the bottom map and for each subaccess in the bottom
		 * map, create a new taskwait fragment that depends on it.
		 *
		 */

		accessStructures._subaccessBottomMap.processAll(
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);

				DataAccessLink previous = bottomMapEntry->_link;
				DataAccessRegion region = bottomMapEntry->_region;
				DataAccessType accessType = bottomMapEntry->_accessType;
				reduction_type_and_operator_index_t reductionTypeAndOperatorIndex =
					bottomMapEntry->_reductionTypeAndOperatorIndex;

				// Create the taskwait fragment
				{
					DataAccess *taskwaitFragment = createAccess(
						task,
						taskwait_type,
						accessType, /* not weak */ false, region,
						reductionTypeAndOperatorIndex);

					// No need for symbols in a taskwait

					DataAccessStatusEffects initialStatus(taskwaitFragment);
					taskwaitFragment->setNewInstrumentationId(task->getInstrumentationTaskId());
					taskwaitFragment->setInBottomMap();
					taskwaitFragment->setRegistered();
					if (computePlace != nullptr) {
						taskwaitFragment->setOutputLocation(computePlace->getMemoryPlace(0));
					} else {
						taskwaitFragment->setComplete();
					}

				// NOTE: For now we create it as completed, but we could actually link
				// that part of the status to any other actions that needed to be carried
				// out. For instance, data transfers.
				// taskwaitFragment->setComplete();
#ifndef NDEBUG
					taskwaitFragment->setReachable();
#endif
					accessStructures._taskwaitFragments.insert(*taskwaitFragment);

					// Update the bottom map entry to now be of taskwait type
					bottomMapEntry->_link._objectType = taskwait_type;
					bottomMapEntry->_link._task = task;

					DataAccessStatusEffects updatedStatus(taskwaitFragment);

					handleDataAccessStatusChanges(
						initialStatus, updatedStatus,
						taskwaitFragment, accessStructures, task,
						hpDependencyData);
				}

				/*
				 * Previous task (that was previously in the bottom map)
				 */
				TaskDataAccesses &previousAccessStructures = previous._task->getDataAccesses();

				// Unlock parent task to avoid potential deadlock
				if (previous._task != task) {
					accessStructures._lock.unlock();
					previousAccessStructures._lock.lock();
				}

				followLink(
					previous, region,
					[&](DataAccess *previousAccess) -> bool {
						DataAccessStatusEffects initialStatus(previousAccess);
						// Mark end of reduction
						if ((previousAccess->getType() == REDUCTION_ACCESS_TYPE)
							&& (previousAccess->getReductionTypeAndOperatorIndex()
								!= reductionTypeAndOperatorIndex)) {
							// When a reduction access is to be linked with a taskwait, we want to mark the
							// reduction access so that it is the last access of its reduction chain
							//
							// Note: This should only be done when the reductionType of the parent access
							// (obtained by 'reductionTypeAndOperatorIndex')
							// is different from the reduction access reductionType.
							// Ie. The reduction in which the subaccess participates is different from its
							// parent's reduction, and thus it should be closed by the nested taskwait
							previousAccess->setClosesReduction();
						}

						/*
						 * Link to the taskwait and unset flag indicating that it was in bottom map.
						 */
						previousAccess->setNext(DataAccessLink(task, taskwait_type));
						previousAccess->unsetInBottomMap();
						DataAccessStatusEffects updatedStatus(previousAccess);

						handleDataAccessStatusChanges(
							initialStatus, updatedStatus,
							previousAccess, previousAccessStructures, previous._task,
							hpDependencyData);

						return true;
					});

				// Relock to advance the iterator
				if (previous._task != task) {
					previousAccessStructures._lock.unlock();
					accessStructures._lock.lock();
				}

				/* Always continue with the rest of the bottom map */
				return true;
			});
	}

	/*
	 * createTopLevelSink:
	 *
	 * This function is called by unregisterTaskDataAccesses when the task
	 * finishes. For each entry in the bottom map, a new taskwait fragment is
	 * created (of top_level_sink_type), which is the successor (next access)
	 * of the access that was in the bottom map.
	 *
	 * The task data accesses must already be locked by the caller.
	 */

	static void createTopLevelSink(
		Task *task, TaskDataAccesses &accessStructures, /* OUT */ CPUDependencyData &hpDependencyData)
	{
		assert(task != nullptr);
		assert(accessStructures._lock.isLockedByThisThread());

		// For each bottom map entry
		accessStructures._subaccessBottomMap.processAll(
			/* processor: called for each bottom map entry */
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);

				if (bottomMapEntry->_accessType != NO_ACCESS_TYPE) {
					// Not a local access
					return true;
				}

				DataAccessLink previous = bottomMapEntry->_link;
				DataAccessRegion region = bottomMapEntry->_region;
				DataAccessType accessType = bottomMapEntry->_accessType;
				assert(bottomMapEntry->_reductionTypeAndOperatorIndex == no_reduction_type_and_operator);

				// Create the top level sink fragment
				{
					DataAccess *topLevelSinkFragment = createAccess(
						task,
						top_level_sink_type,
						accessType, /* not weak */ false, region);

					// TODO, top level sink fragment, what to do with the symbols?

					DataAccessStatusEffects initialStatus(topLevelSinkFragment);
					topLevelSinkFragment->setNewInstrumentationId(task->getInstrumentationTaskId());
					topLevelSinkFragment->setInBottomMap();
					topLevelSinkFragment->setRegistered();

					// NOTE: For now we create it as completed, but we could actually link
					// that part of the status to any other actions that needed to be carried
					// out. For instance, data transfers.
					topLevelSinkFragment->setComplete();
#ifndef NDEBUG
					topLevelSinkFragment->setReachable();
#endif
					accessStructures._taskwaitFragments.insert(*topLevelSinkFragment);

					// Update the bottom map entry
					bottomMapEntry->_link._objectType = top_level_sink_type;
					bottomMapEntry->_link._task = task;

					DataAccessStatusEffects updatedStatus(topLevelSinkFragment);

					handleDataAccessStatusChanges(
						initialStatus, updatedStatus,
						topLevelSinkFragment, accessStructures, task,
						hpDependencyData);
				}

				TaskDataAccesses &previousAccessStructures = previous._task->getDataAccesses();

				// Unlock to avoid potential deadlock
				if (previous._task != task) {
					accessStructures._lock.unlock();
					previousAccessStructures._lock.lock();
				}

				/*
				 * Process every access of the previous task (that was in the
				 * bottom map) that intersects the current region, as its next
				 * access will be the new top-level sink taskwait fragment.
				 * Since previous is a DataAccessLink, followLink will apply
				 * the lambda function to the right kind of accesses (access,
				 * fragment or taskwait).
				 */
				followLink(
					previous,    /* previous task was in bottom map */
					region,
					/* processor: called for every intersecting access of the previous task */
					[&](DataAccess *previousAccess) -> bool
					{
						DataAccessStatusEffects initialStatus(previousAccess);
						// Mark end of reduction
						if (previousAccess->getType() == REDUCTION_ACCESS_TYPE) {
							// When a reduction access is to be linked with a top-level sink, we want to mark the
							// reduction access so that it is the last access of its reduction chain
							//
							// Note: This is different from the taskwait above in that a top-level sink will
							// _always_ mean the reduction is to be closed
							previousAccess->setClosesReduction();
						}

						/*
						 * Link to the top-level sink and unset flag indicating that it was in bottom map.
						 */
						previousAccess->setNext(DataAccessLink(task, taskwait_type));
						previousAccess->unsetInBottomMap();
						DataAccessStatusEffects updatedStatus(previousAccess);

						/* Handle the consequences */
						handleDataAccessStatusChanges(
							initialStatus, updatedStatus,
							previousAccess, previousAccessStructures, previous._task,
							hpDependencyData);

						/* Continue with all intersecting accesses of previous task */
						return true;
					});

				// Relock to advance the iterator
				if (previous._task != task) {
					previousAccessStructures._lock.unlock();
					accessStructures._lock.lock();
				}

				/* Always continue with the rest of the bottom map */
				return true;
			});
	}


	/*
	 * Register a single task data access.
	 *
	 * This function does not link the data access with the parent and sibling
	 * tasks.  Linking is done later inside registerTaskDataAccesses. This
	 * function, registerTaskDataAccess, is (indirectly) called from the
	 * callback given to Nanos6 when the task was created.
	 */
	void registerTaskDataAccess(
		Task *task, DataAccessType accessType, bool weak, DataAccessRegion region, int symbolIndex,
		reduction_type_and_operator_index_t reductionTypeAndOperatorIndex, reduction_index_t reductionIndex)
	{
		assert(task != nullptr);

		DataAccess::symbols_t symbol_list; //TODO consider alternative to vector

		if (symbolIndex >= 0)
			symbol_list.set(symbolIndex);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

		/*
		 * This access may fragment an existing access.
		 *
		 * Also collect all symbols used by all intersecting access (?)
		 */
		accessStructures._accesses.fragmentIntersecting(
			region,
			/* duplicator */
			[&](DataAccess const &toBeDuplicated) -> DataAccess * {
				assert(!toBeDuplicated.isRegistered());
				return duplicateDataAccess(toBeDuplicated, accessStructures);
			},
			/* postprocessor */
			[&](__attribute__((unused)) DataAccess *newAccess, DataAccess *originalAccess) {
				symbol_list |= originalAccess->getSymbols();
			});

		/*
		 * The new access may overlap existing accesses. After fragmentation
		 * (above), the region divides up into parts that either match an
		 * existing access ("intersecting") or that are new ("missing"). Handle
		 * the two cases separately using the two lambdas below.
		 */
		accessStructures._accesses.processIntersectingAndMissing(
			region,
			/* intersectingProcessor: may need to upgrade (e.g. existing READ to READWRITE) */
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				DataAccess *oldAccess = &(*position);
				assert(oldAccess != nullptr);

				upgradeAccess(oldAccess, accessType, weak, reductionTypeAndOperatorIndex);
				oldAccess->addToSymbols(symbol_list);

				return true;
			},
			/* missingProcessor: create a new access for it */
			[&](DataAccessRegion missingRegion) -> bool {
				DataAccess *newAccess = createAccess(task, access_type, accessType, weak, missingRegion,
					reductionTypeAndOperatorIndex, reductionIndex);
				newAccess->addToSymbols(symbol_list);

				accessStructures._accesses.insert(*newAccess);

				return true;
			}
		);
	}

	/*
	 * This function is called by submitTask to register a task and its
	 * dependencies in the dependency system. The function starts by calling
	 * the callback * "_taskInfo->register_depinfo" that came with the args
	 * block.  The callback registers each data access by a call to Nanos6,
	 * which results in a call to registerTaskDataAccess for each data access.
	 * After registering all the individual task data accesses in this way,
	 * they are linked to existing parent and sibling accesses.
	 */
	bool registerTaskDataAccesses(
		Task *task,
		ComputePlace *computePlace,
		CPUDependencyData &hpDependencyData
	) {
		bool ready; /* return value: true if task is ready immediately */

		assert(task != nullptr);
		assert(computePlace != nullptr);

		Instrument::enterRegisterTaskDataAcesses();

		/*
		 * This part creates the DataAccesses and calculates any possible upgrade.
		 * It calls _taskInfo->register_depinfo with the args block, which results
		 * in a call to registerTaskDataAccess for each access.
		 */
		task->registerDependencies();

		/*
		 * Now that the task accesses have been registered, they need to
		 * be linked to the parent and sibling accesses.
		 */
		if (!task->getDataAccesses()._accesses.empty()) {

			/* Two extra predecessors, so cannot become ready early */
			task->increasePredecessors(2);

#ifndef NDEBUG
			{
				bool alreadyTaken = false;
				assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, true));
			}
#endif
			/*
			 * This part actually inserts the accesses into the dependency system
			 */
			linkTaskAccesses(hpDependencyData, task);
			processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(hpDependencyData, computePlace, true);

#ifndef NDEBUG
			{
				bool alreadyTaken = true;
				assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, false));
			}
#endif
			/*
			 * Remove the two extra predecessors. The task may become ready.
			 */
			ready = task->decreasePredecessors(2);

			// Special handling for tasks with commutative accesses
			if (ready && (task->getDataAccesses()._totalCommutativeBytes > 0UL)) {
				assert(hpDependencyData._satisfiedCommutativeOriginators.empty());
				assert(hpDependencyData._satisfiedOriginators.empty());

				hpDependencyData._satisfiedCommutativeOriginators.push_back(task);
				processSatisfiedCommutativeOriginators(hpDependencyData);

				if (!hpDependencyData._satisfiedOriginators.empty()) {
					assert(hpDependencyData._satisfiedOriginators.front() == task);
					hpDependencyData._satisfiedOriginators.clear();
				} else {
					// Failed to acquire all the commutative entries
					ready = false;
				}
			}
		} else {
			/*
			 * No accesses: so the task is immediately ready.
			 */
			ready = true;
		}

		Instrument::exitRegisterTaskDataAcesses();
		return ready;
	}


	/*
	 * Release a region accessed by a task
	 *
	 * It is used to (a) support the release directive and (b) for Nanos6@cluster,
	 * handle the receipt of a MessageReleaseAccess when a remote task releases
	 * an access.
	 *
	 */
	void releaseAccessRegion(
		Task *task,  /* The task that is releasing the region */
		DataAccessRegion region,
		__attribute__((unused)) DataAccessType accessType, __attribute__((unused)) bool weak,
		ComputePlace *computePlace,
		CPUDependencyData &hpDependencyData,
		MemoryPlace const *location)
	{
		assert(task != nullptr);

		//! The compute place may be none if it is released from inside a
		//! polling service
		//! assert(computePlace != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();

		printf("Node %d: Release acceses for Task: %p -> %p\n",
			nanos6_get_cluster_node_id(), task, &accessStructures);

		assert(!accessStructures.hasBeenDeleted());
		TaskDataAccesses::accesses_t &accesses = accessStructures._accesses;

#ifndef NDEBUG
		{
			bool alreadyTaken = false;
			assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, true));
		}
#endif

		{
			std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

			accesses.processIntersecting(
				region,
				/* processor: called for each access that intersects the region */
				[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
					DataAccess *dataAccess = &(*position);
					assert(dataAccess != nullptr);
					assert(dataAccess->isWeak() == weak);

					FatalErrorHandler::failIf(dataAccess->getType() != accessType,
						"The 'release' construct does not currently support the type downgrade of dependencies; ",
						"the dependency type specified at that construct must be its complete type");

					if (dataAccess->getType() == REDUCTION_ACCESS_TYPE && task->isRunnable()) {
						releaseReductionStorage(task, dataAccess, region, computePlace);
					}

					//! If a valid location has not been provided then we use
					//! the MemoryPlace assigned to the Task but only for non-weak
					//! accesses. For weak accesses we do not want to update the
					//! location of the access
					MemoryPlace const *releaseLocation;
					if ((location == nullptr) && !dataAccess->isWeak()) {
						assert(task->hasMemoryPlace());
						releaseLocation = task->getMemoryPlace();
					} else {
						releaseLocation = location;
					}

					dataAccess = fragmentAccess(dataAccess, region, accessStructures);
					finalizeAccess(task, dataAccess, region, releaseLocation, /* OUT */ hpDependencyData);

					return true;
				});
		}
		processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(hpDependencyData, computePlace, true);

#ifndef NDEBUG
		{
			bool alreadyTaken = true;
			assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, false));
		}
#endif
	}

	void releaseTaskwaitFragment(
		Task *task,
		DataAccessRegion region,
		ComputePlace *computePlace,
		CPUDependencyData &hpDependencyData)
	{
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

#ifndef NDEBUG
		{
			bool alreadyTaken = false;
			assert(hpDependencyData._inUse.compare_exchange_strong(
				alreadyTaken, true));
		}
#endif

		{
			std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);
			accessStructures._taskwaitFragments.processIntersecting(
				region,
				/* processor: called for each taskwait fragment that intersects the region */
				[&](TaskDataAccesses::taskwait_fragments_t::iterator position) -> bool {
					DataAccess *taskwait = &(*position);

					// Should be fully fragmented already, so the whole fragment becomes complete
					assert(taskwait->getAccessRegion().fullyContainedIn(region));

					/*
					 * Set the taskwait fragment as complete.
					 */
					DataAccessStatusEffects initialStatus(taskwait);
					taskwait->setComplete();
					DataAccessStatusEffects updatedStatus(taskwait);

					handleDataAccessStatusChanges(
						initialStatus, updatedStatus,
						taskwait, accessStructures, task,
						hpDependencyData);

					return true;
				});
		}

		processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(
			hpDependencyData,
			computePlace,
			true);

#ifndef NDEBUG
		{
			bool alreadyTaken = true;
			assert(hpDependencyData._inUse.compare_exchange_strong(
				alreadyTaken, false));
		}
#endif
	}

	/*
	 * Update the location information for all data accesses that intersect the
	 * region, fragmenting them if necessary. For clusters this is done when
	 * a data copy completes.
	 */
	void updateTaskDataAccessLocation(Task *task,
		DataAccessRegion const &region,
		MemoryPlace const *location,
		bool isTaskwait)
	{
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

		// Take the lock on the task data accesses (all locking in
		// DataAccessRegistration is done on the task data accesses).
		std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

		auto &accesses = (isTaskwait) ? accessStructures._taskwaitFragments : accessStructures._accesses;

		// At this point the region must be included in DataAccesses of the task
		assert(accesses.contains(region));

		accesses.processIntersecting(region,
			/* processor: lambda called for every task data access that intersects
			   the region */
			[&](TaskDataAccesses::accesses_t::iterator accessPosition) -> bool {
				DataAccess *access = &(*accessPosition);
				assert(access != nullptr);

				/* fragment the access (if not fully contained inside the region) */
				access = fragmentAccess(access, region, accessStructures);
				access->setLocation(location);

				/* always continue with remaining accesses: don't stop here */
				return true;
			});
	}

	/*
	 * Register a new data access on a task (after it has started). This
	 * is necessary for dmallocs, because all child data accesses should
	 * be contained within the parent data accesses (?).
	 */
	void registerLocalAccess(Task *task, DataAccessRegion const &region)
	{
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

		Instrument::registerTaskAccess(
			task->getInstrumentationTaskId(),
			NO_ACCESS_TYPE,
			false,
			region.getStartAddress(),
			region.getSize()
		);

		/* Create a new access */
		DataAccess *newLocalAccess = createAccess(
			task,
			access_type,
			NO_ACCESS_TYPE,
			/* not weak */ false,
			region
		);

		/* Modifications to be done after the lock is taken  */
		DataAccessStatusEffects initialStatus(newLocalAccess);
		newLocalAccess->setNewInstrumentationId(task->getInstrumentationTaskId());
		newLocalAccess->setReadSatisfied(Directory::getDirectoryMemoryPlace());
		newLocalAccess->setWriteSatisfied();
		newLocalAccess->setConcurrentSatisfied();
		newLocalAccess->setCommutativeSatisfied();
		newLocalAccess->setReceivedReductionInfo();
		newLocalAccess->setRegistered();
		newLocalAccess->setTopmost();
		newLocalAccess->setTopLevel();
#ifndef NDEBUG
		newLocalAccess->setReachable();
#endif
		DataAccessStatusEffects updatedStatus(newLocalAccess);
		//! This is an exception to avoid decreasing predecessor and it
		//! is not used anywhere else.
		updatedStatus.setEnforcesDependency();

		/* Take the lock on the task data accesses */
		std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

		/* Insert the new access (with the lock) */
		accessStructures._accesses.insert(*newLocalAccess);

		/* Handle the above data access status changes */
		CPUDependencyData hpDependencyData;
		handleDataAccessStatusChanges(
			initialStatus,
			updatedStatus,
			newLocalAccess,
			accessStructures,
			task,
			hpDependencyData
		);

		/* Do not expect any delayed operations */
		assert (hpDependencyData.empty());
	}

	/*
	 * Unregister a new data access on a task (after it has started). This
	 * is necessary after a dfree.
	 */
	void unregisterLocalAccess(Task *task, DataAccessRegion const &region)
	{
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

		using spinlock_t = TaskDataAccesses::spinlock_t;
		using access_fragments_t = TaskDataAccesses::access_fragments_t;
		using accesses_t = TaskDataAccesses::accesses_t;

		std::lock_guard<spinlock_t> guard(accessStructures._lock);

		//! Mark all the access fragments intersecting the given region as complete
		accessStructures._accessFragments.processIntersecting(region,
			[&](access_fragments_t::iterator position) -> bool {
				DataAccess *fragment = &(*position);
				assert(fragment != nullptr);
				assert(fragment->getType() == NO_ACCESS_TYPE);

				/* Fragment the access (if not fully contained inside the region).
				   Given that the use case is dmalloc/dfree it seems unlikely. */
				fragment = fragmentAccess(fragment, region,
					accessStructures);

				/* Set access as complete */
				DataAccessStatusEffects initialStatus(fragment);
				fragment->setComplete();
				DataAccessStatusEffects updatedStatus(fragment);

				/* Handle consequences of access becoming complete */
				CPUDependencyData hpDependencyData;
				handleDataAccessStatusChanges(initialStatus,
					updatedStatus, fragment, accessStructures,
					task, hpDependencyData);

				/* Do not expect any delayed operations */
				assert (hpDependencyData.empty());
				return true;
			});

		//! By now all fragments intersecting the local region should be removed
		assert(!accessStructures._accessFragments.contains(region));

		//! Mark all the accesses intersecting the given region as complete
		accessStructures._accesses.processIntersecting(region,
			[&](accesses_t::iterator position) -> bool {
				DataAccess *access = &(*position);
				assert(access != nullptr);
				assert(!access->hasBeenDiscounted());
				assert(access->getType() == NO_ACCESS_TYPE);

				/*
				 * Fragment access, as only part inside region becomes complete.
				 */
				access = fragmentAccess(access, region,
					accessStructures);

				/* Set access as complete */
				DataAccessStatusEffects initialStatus(access);
				access->setComplete();
				DataAccessStatusEffects updatedStatus(access);

				/* Handle consequences of access becoming complete */
				CPUDependencyData hpDependencyData;
				handleDataAccessStatusChanges(initialStatus,
					updatedStatus, access, accessStructures,
					task, hpDependencyData);

				/* Do not expect any delayed operations */
				assert (hpDependencyData.empty());
				return true;
			});

		//! By now all accesses intersecting the local region should be removed
		assert(!accessStructures._accesses.contains(region));
	}

	void combineTaskReductions(Task *task, ComputePlace *computePlace)
	{
		assert(task != nullptr);
		assert(computePlace != nullptr);
		assert(task->isRunnable());

		if (task->isTaskfor()) {
			// Loop callaborators only
			TaskDataAccesses &parentAccessStructures = task->getParent()->getDataAccesses();

			assert(!parentAccessStructures.hasBeenDeleted());
			TaskDataAccesses::accesses_t &parentAccesses = parentAccessStructures._accesses;

			std::lock_guard<TaskDataAccesses::spinlock_t> guard(parentAccessStructures._lock);

			// Process parent reduction access and release their storage
			parentAccesses.processAll(
				[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
					DataAccess *dataAccess = &(*position);
					assert(dataAccess != nullptr);

					if (dataAccess->getType() == REDUCTION_ACCESS_TYPE) {
						releaseReductionStorage(task->getParent(), dataAccess, dataAccess->getAccessRegion(), computePlace);
					}
					return true;
				});
		}

		TaskDataAccesses &accessStructures = task->getDataAccesses();

		assert(!accessStructures.hasBeenDeleted());
		TaskDataAccesses::accesses_t &accesses = accessStructures._accesses;

		std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

		accesses.processAll(
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				DataAccess *dataAccess = &(*position);
				assert(dataAccess != nullptr);

				if (dataAccess->getType() == REDUCTION_ACCESS_TYPE) {
					releaseReductionStorage(task, dataAccess, dataAccess->getAccessRegion(), computePlace);
				}
				return true;
			});
	}

	/*
	 * Unregister all the task data accesses (when the task completes).
	 */
	void unregisterTaskDataAccesses(Task *task,
									ComputePlace *computePlace,
									CPUDependencyData &hpDependencyData,
									MemoryPlace *location,
									bool fromBusyThread)
	{
		assert(task != nullptr);

		Instrument::enterUnregisterTaskDataAcesses();

		TaskDataAccesses &accessStructures = task->getDataAccesses();

		assert(!accessStructures.hasBeenDeleted());
		TaskDataAccesses::accesses_t &accesses = accessStructures._accesses;

		//! If a valid location has not been provided then we use
		//! the MemoryPlace assigned to the Task
		if (location == nullptr) {
			assert(task->hasMemoryPlace());
			location = task->getMemoryPlace();
		}
#ifndef NDEBUG
		{
			bool alreadyTaken = false;
			assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, true));
		}
#endif

		{
			std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

			createTopLevelSink(task, accessStructures, hpDependencyData);

			accesses.processAll(
				[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
					DataAccess *dataAccess = &(*position);
					assert(dataAccess != nullptr);

					MemoryPlace *accessLocation = (dataAccess->isWeak()) ? nullptr : location;

					finalizeAccess(task, dataAccess, dataAccess->getAccessRegion(), accessLocation, /* OUT */ hpDependencyData);

					return true;
				});
		}

		processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(hpDependencyData, computePlace, fromBusyThread);

#ifndef NDEBUG
		{
			bool alreadyTaken = true;
			assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, false));
		}
#endif
		Instrument::exitUnregisterTaskDataAcesses();
	}

	/*
	 * Called on receipt of MessageSatisfiability. Propagates satisfiability
	 * from the workflow into the dependency system.
	 */
	void propagateSatisfiability(Task *task, DataAccessRegion const &region,
		ComputePlace *computePlace, CPUDependencyData &hpDependencyData,
		bool readSatisfied,    /* Change in read satisfiability (not new value) */
		bool writeSatisfied,   /* Change in write satisfiability (not new value) */
		MemoryPlace const *location)
	{
		assert(task != nullptr);

		/* At least one of read or write satisfied (maybe both) must be changing */
		assert(readSatisfied || writeSatisfied);

		/*
		 * Create an update operation with the satisfiability information.
		 * It affects the task's accesses ("access_type"), not the fragments
		 * (which relate to its children) or taskwaits.
		 */
		UpdateOperation updateOperation;
		updateOperation._target = DataAccessLink(task, access_type);
		updateOperation._region = region;

		updateOperation._makeReadSatisfied = readSatisfied;
		updateOperation._makeWriteSatisfied = writeSatisfied;

		updateOperation._location = location;

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());

#ifndef NDEBUG
		{
			/* No other code should be using this hpDependencyData */
			bool alreadyTaken = false;
			assert(hpDependencyData._inUse.compare_exchange_strong(
				alreadyTaken, true));
		}
#endif

		{
			/*
			 * Process the update operation (which requires the lock
			 * to be taken on the task's access structures).
			 */
			std::lock_guard<TaskDataAccesses::spinlock_t>
				guard(accessStructures._lock);
			processUpdateOperation(updateOperation, hpDependencyData);
		}

		/*
		 * Finish processing with the operations that require locks other than
		 * the task's access structures.
		 */
		processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(
			hpDependencyData,
			computePlace,
			/* fromBusyThread */ true
		);

#ifndef NDEBUG
		{
			/* Allow other code to use this hpDependencyData */
			bool alreadyTaken = true;
			assert(hpDependencyData._inUse.compare_exchange_strong(
				alreadyTaken, false));
		}
#endif
	}

	/*
	 * Enter a taskwait (called from nanos6_taskwait).
	 *
	 * It creates taskwait fragments for all entries in the bottom map.
	 */
	void handleEnterTaskwait(Task *task, ComputePlace *computePlace, CPUDependencyData &hpDependencyData)
	{
		assert(task != nullptr);

#ifndef NDEBUG
		{
			bool alreadyTaken = false;
			assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, true));
		}
#endif

		{
			TaskDataAccesses &accessStructures = task->getDataAccesses();
			assert(!accessStructures.hasBeenDeleted());
			std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

			/* Create a taskwait fragment for each entry in the bottom map */
			createTaskwait(task, accessStructures, computePlace, hpDependencyData);

			finalizeFragments(task, accessStructures, hpDependencyData);
		}
		processDelayedOperationsSatisfiedOriginatorsAndRemovableTasks(hpDependencyData, computePlace, true);

#ifndef NDEBUG
		{
			bool alreadyTaken = true;
			assert(hpDependencyData._inUse.compare_exchange_strong(alreadyTaken, false));
		}
#endif
	}


	void handleExitTaskwait(Task *task, ComputePlace *, CPUDependencyData &)
	{
		assert(task != nullptr);

		TaskDataAccesses &accessStructures = task->getDataAccesses();
		assert(!accessStructures.hasBeenDeleted());
		std::lock_guard<TaskDataAccesses::spinlock_t> guard(accessStructures._lock);

		if (!accessStructures._accesses.empty()) {
			// Mark all accesses as not having subaccesses (meaning fragments,
			// as they will all be deleted below
			accessStructures._accesses.processAll(
				/* processor: called for every access */
				[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
					DataAccess *dataAccess = &(*position);
					assert(dataAccess != nullptr);
					assert(!dataAccess->hasBeenDiscounted());

					if (dataAccess->hasSubaccesses()) {
						dataAccess->unsetHasSubaccesses();
					}

					/* continue, to process all accesses */
					return true;
				});

			/*
			 * Delete all fragments. These are created when child task accesses
			 * when the tasks are submitted, and are no longer needed now that all
			 * child tasks have finished.
			 */
			accessStructures._accessFragments.processAll(
				/* processor: called for every fragment */
				[&](TaskDataAccesses::access_fragments_t::iterator position) -> bool {
					DataAccess *dataAccess = &(*position);
					assert(dataAccess != nullptr);

					Instrument::removedDataAccess(dataAccess->getInstrumentationId());
					accessStructures._accessFragments.erase(dataAccess);
					ObjectAllocator<DataAccess>::deleteObject(dataAccess);

					/* continue, to process all access fragments */
					return true;
				});
			accessStructures._accessFragments.clear();

			// Delete all taskwait fragments
			accessStructures._taskwaitFragments.processAll(
				[&](TaskDataAccesses::taskwait_fragments_t::iterator position) -> bool {
					DataAccess *dataAccess = &(*position);
					assert(dataAccess != nullptr);

#ifndef NDEBUG
					DataAccessStatusEffects currentStatus(dataAccess);
					assert(currentStatus._isRemovable);
#endif

					Instrument::removedDataAccess(dataAccess->getInstrumentationId());
					accessStructures._taskwaitFragments.erase(dataAccess);
					ObjectAllocator<DataAccess>::deleteObject(dataAccess);

					/* continue, to process all taskwait fragments */
					return true;
				});
			accessStructures._taskwaitFragments.clear();
		}

		// Clean up the bottom map
		accessStructures._subaccessBottomMap.processAll(
			/* processor: called for every bottom map entry */
			[&](TaskDataAccesses::subaccess_bottom_map_t::iterator bottomMapPosition) -> bool {
				/*
				 * Erase the bottom map entry.
				 */
				BottomMapEntry *bottomMapEntry = &(*bottomMapPosition);
				assert(bottomMapEntry != nullptr);
				assert((bottomMapEntry->_link._objectType == taskwait_type) || (bottomMapEntry->_link._objectType == top_level_sink_type));

				accessStructures._subaccessBottomMap.erase(bottomMapEntry);
				ObjectAllocator<BottomMapEntry>::deleteObject(bottomMapEntry);

				/* continue, to process all bottom map entries */
				return true;
			});
		assert(accessStructures._subaccessBottomMap.empty());
	}

	void translateReductionAddresses(
		Task *task, ComputePlace *computePlace,
		nanos6_address_translation_entry_t *translationTable,
		int totalSymbols
	) {
		assert(task != nullptr);
		assert(computePlace != nullptr);
		assert(translationTable != nullptr);

		// Initialize translationTable
		for (int i = 0; i < totalSymbols; ++i)
			translationTable[i] = {0, 0};

		TaskDataAccesses &accessStruct = task->getDataAccesses();

		assert(!accessStruct.hasBeenDeleted());
		accessStruct._lock.lock();

		accessStruct._accesses.processAll(
			[&](TaskDataAccesses::accesses_t::iterator position) -> bool {
				DataAccess *dataAccess = &(*position);
				assert(dataAccess != nullptr);

				if (dataAccess->getType() == REDUCTION_ACCESS_TYPE && !dataAccess->isWeak()) {
					FatalErrorHandler::failIf(computePlace->getType() != nanos6_host_device,
						"Region dependencies do not support CUDA reductions");

					ReductionInfo *reductionInfo = dataAccess->getReductionInfo();
					assert(reductionInfo != nullptr);

					size_t slotIndex = reductionInfo->getFreeSlotIndex(computePlace->getIndex());

					// Register assigned slot in the data access
					dataAccess->setReductionAccessedSlot(slotIndex);

					void *address = dataAccess->getAccessRegion().getStartAddress();
					void *translation = nullptr;
					const DataAccessRegion &originalFullRegion = reductionInfo->getOriginalRegion();
					translation = ((char *)reductionInfo->getFreeSlotStorage(slotIndex).getStartAddress()) + ((char *)address - (char *)originalFullRegion.getStartAddress());

					// As we're iterating accesses that might have been split by sibling tasks, it is
					// possible that we translate the same symbol twice. However, this is not an issue
					// because symbol translation is relative and it is not mandatory for "address"
					// to be equal to the first position of the translated symbol
					for (int j = 0; j < totalSymbols; ++j) {
						if (dataAccess->isInSymbol(j))
							translationTable[j] = {(size_t)address, (size_t)translation};
					}
				}

				return true;
			});

		accessStruct._lock.unlock();
	}
}; // namespace DataAccessRegistration

#pragma GCC visibility pop
