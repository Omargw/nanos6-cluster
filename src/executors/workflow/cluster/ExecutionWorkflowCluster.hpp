/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019 Barcelona Supercomputing Center (BSC)
*/

#ifndef EXECUTION_WORKFLOW_CLUSTER_HPP
#define EXECUTION_WORKFLOW_CLUSTER_HPP

#include <functional>

#include "../ExecutionStep.hpp"

#include <ClusterManager.hpp>
#include <ClusterTaskContext.hpp>
#include <DataAccess.hpp>
#include <Directory.hpp>
#include <InstrumentLogMessage.hpp>
#include <SatisfiabilityInfo.hpp>
#include <TaskOffloading.hpp>
#include <VirtualMemoryManagement.hpp>
#include <tasks/Task.hpp>

class ComputePlace;
class MemoryPlace;

namespace ExecutionWorkflow {
	class ClusterDataLinkStep : public DataLinkStep {
		//! The MemoryPlace that holds the data at the moment
		MemoryPlace const *_sourceMemoryPlace;

		//! The MemoryPlace that requires the data
		MemoryPlace const *_targetMemoryPlace;

		//! DataAccessRegion that the Step covers
		DataAccessRegion _region;

		//! The task in which the access belongs to
		Task *_task;

		//! read satisfiability at creation time
		bool _read;

		//! write satisfiability at creation time
		bool _write;

		bool _started;
	public:
		ClusterDataLinkStep(
			MemoryPlace const *sourceMemoryPlace,
			MemoryPlace const *targetMemoryPlace,
			DataAccess *access
		) : DataLinkStep(access),
			_sourceMemoryPlace(sourceMemoryPlace),
			_targetMemoryPlace(targetMemoryPlace),
			_region(access->getAccessRegion()),
			_task(access->getOriginator()),
			_read(access->readSatisfied()),
			_write(access->writeSatisfied()),
			_started(false)
		{
			access->setDataLinkStep(this);
		}

		void linkRegion(
			DataAccessRegion const &region,
			MemoryPlace const *location,
			bool read,
			bool write
		) override;

		//! Start the execution of the Step
		void start() override;
	};

	class ClusterDataCopyStep : public Step {
		//! The MemoryPlace that the data will be copied from.
		MemoryPlace const *_sourceMemoryPlace;

		//! The MemoryPlace that the data will be copied to.
		MemoryPlace const *_targetMemoryPlace;

		//! A mapping of the address range in the source node to the target node.
		DataAccessRegion const _region;

		//! The task on behalf of which we perform the data copy
		Task *_task;

		//! The data copy is for a taskwait
		bool _isTaskwait;

		//! An actual data transfer is required
		bool _needsTransfer;

	public:
		ClusterDataCopyStep(
			MemoryPlace const *sourceMemoryPlace,
			MemoryPlace const *targetMemoryPlace,
			DataAccessRegion const &region,
			Task *task,
			bool isTaskwait,
			bool needsTransfer
		) : Step(),
			_sourceMemoryPlace(sourceMemoryPlace),
			_targetMemoryPlace(targetMemoryPlace),
			_region(region),
			_task(task),
			_isTaskwait(isTaskwait),
			_needsTransfer(needsTransfer)
		{
		}

		//! Start the execution of the Step
		void start() override;
	};

	class ClusterDataReleaseStep : public DataReleaseStep {
		//! identifier of the remote task
		void *_remoteTaskIdentifier;

		//! the cluster node we need to notify
		ClusterNode const *_offloader;

	public:
		ClusterDataReleaseStep(TaskOffloading::ClusterTaskContext *context, DataAccess *access)
			: DataReleaseStep(access),
			_remoteTaskIdentifier(context->getRemoteIdentifier()),
			_offloader(context->getRemoteNode())
		{
			access->setDataReleaseStep(this);
		}

		void releaseRegion(DataAccessRegion const &region, MemoryPlace const *location) override
		{
			Instrument::logMessage(
				Instrument::ThreadInstrumentationContext::getCurrent(),
				"releasing remote region:", region
			);

			TaskOffloading::sendRemoteAccessRelease(
				_remoteTaskIdentifier, _offloader, region, _type, _weak, location
			);

			_bytesToRelease -= region.getSize();
			if (_bytesToRelease == 0) {
				delete this;
			}
		}

		bool checkDataRelease(DataAccess const *access) override
		{
			const bool releases = (access->getObjectType() == taskwait_type)
				&& access->getOriginator()->isSpawned()
				&& access->readSatisfied()
				&& access->writeSatisfied();

			Instrument::logMessage(
				Instrument::ThreadInstrumentationContext::getCurrent(),
				"Checking DataRelease access:", access->getInstrumentationId(),
				" object_type:", access->getObjectType(),
				" spawned originator:", access->getOriginator()->isSpawned(),
				" read:", access->readSatisfied(),
				" write:", access->writeSatisfied(),
				" releases:", releases
			);

			return releases;
		}

		void start() override
		{
			releaseSuccessors();
		}
	};

	class ClusterExecutionStep : public Step {
	private:
		std::vector<TaskOffloading::SatisfiabilityInfo> _satInfo;
		ClusterNode *_remoteNode;
		Task *_task;

	public:
		ClusterExecutionStep(Task *task, ComputePlace *computePlace);

		//! Inform the execution Step about the existence of a
		//! pending data copy.
		//!
		//! \param[in] source is the id of the MemoryPlace that the data
		//!            is currently located
		//! \param[in] region is the memory region being copied
		//! \param[in] size is the size of the region being copied.
		//! \param[in] read is true if access is read-satisfied
		//! \param[in] write is true if access is write-satisfied
		void addDataLink(int source, DataAccessRegion const &region, bool read, bool write);

		//! Start the execution of the Step
		void start() override;
	};

	class ClusterNotificationStep : public Step {
	private:
		std::function<void ()> const _callback;

	public:
		ClusterNotificationStep(std::function<void ()> const &callback)
			: Step(), _callback(callback)
		{
		}

		//! Start the execution of the Step
		void start() override;
	};

	inline Step *clusterFetchData(
		MemoryPlace const *source,
		MemoryPlace const *target,
		DataAccessRegion const &inregion,
		DataAccess *access
	) {
		assert(source != nullptr);
		nanos6_device_t sourceType = source->getType();
		assert(target == ClusterManager::getCurrentMemoryNode());

		//! Currently, we cannot have a cluster data copy where the source
		//! location is in the Directory. This would mean that the data
		//! have not been written yet (that's why they're not in a
		//! non-directory location), so we are reading something that is
		//! not initialized yet
		assert(!Directory::isDirectoryMemoryPlace(source) &&
			"You're probably trying to read something "
			"that has not been initialized yet!"
		);

		//! The source device is a host MemoryPlace of the current
		//! ClusterNode. We do not really need to perform a
		//! DataTransfer
		if ((sourceType == nanos6_host_device)) {
			return new Step();
		}

		assert(source->getType() == nanos6_cluster_device);
		DataAccessObjectType objectType = access->getObjectType();
		DataAccessType type = access->getType();
		DataAccessRegion region = access->getAccessRegion();
		bool isDistributedRegion = VirtualMemoryManagement::isDistributedRegion(region);

		bool needsTransfer =
			(
			 	//! We need a DataTransfer for a taskwait access
				//! in the following cases:
				//! 1) the access is not a NO_ACCESS_TYPE, so it
				//!    is part of the calling task's dependencies,
				//!    which means that the latest version of
				//!    the region needs to be present in the
				//!    context of the task at all times.
				//! 2) the access is a NO_ACCESS_TYPE access, so
				//!    it represents a region allocated within
				//!    the context of the Task but it is local
				//!    memory, so it needs to be present in the
				//!    context of the Task after the taskwait.
				//!    Distributed memory regions, do not need
				//!    to trigger a DataCopy, since anyway can
				//!    only be accessed from within subtasks.
				//!
				//! In both cases, we can avoid the copy if the
				//! access is a read-only access.
			 	(objectType == taskwait_type)
				&& (type != READ_ACCESS_TYPE)
				&& ((type != NO_ACCESS_TYPE) || !isDistributedRegion)
			) ||
			(
				//! We need a DataTransfer for an access_type
				//! access, if the access is not read-only
			 	(objectType == access_type)
				&& (type != WRITE_ACCESS_TYPE)
			);

		return new ClusterDataCopyStep(
			source,
			target,
			inregion,
			access->getOriginator(),
			(objectType == taskwait_type),
			needsTransfer
		);

	}

	inline Step *clusterCopy(
		MemoryPlace const *source,
		MemoryPlace const *target,
		DataAccessRegion const &region,
		DataAccess *access
	) {
		assert(target != nullptr);
		assert(access != nullptr);

		ClusterMemoryNode *current = ClusterManager::getCurrentMemoryNode();

		if (source != nullptr && (source->getType() != nanos6_cluster_device)) {
			assert(source->getType() == nanos6_host_device);
			if (!Directory::isDirectoryMemoryPlace(source)) {
				source = current;
			}
		}

		if (target->getType() != nanos6_cluster_device) {
			//! At the moment cluster copies take into account only
			//! Cluster and host devices
			assert(target->getType() == nanos6_host_device);
			assert(!Directory::isDirectoryMemoryPlace(target));
			target = current;
		}

		if (target == current) {
			return clusterFetchData(source, target, region, access);
		}

		assert(access->getObjectType() == access_type);
		return new ClusterDataLinkStep(source, target, access);
	}
}


#endif // EXECUTION_WORKFLOW_CLUSTER_HPP
