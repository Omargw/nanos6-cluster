/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019 Barcelona Supercomputing Center (BSC)
*/

#ifndef CLUSTER_TASK_CONTEXT_HPP
#define CLUSTER_TASK_CONTEXT_HPP

#include <mutex>
#include <vector>

#include <DataAccessRegion.hpp>
#include <MessageTaskNew.hpp>
#include <ClusterManager.hpp>
#include <TaskOffloading.hpp>
#include <ClusterShutdownCallback.hpp>

class ClusterNode;
class Task;

namespace TaskOffloading {

	//! \brief This class describes the remote context of a task.
	//!
	//! Remote context in this case is the minimum context necessary in
	//! order to be able to identify the counterpart of a Task, on a
	//! remote node. This task can be either an offloaded task, in which
	//! case the ClusterTaskContext object describes the remote task, or it
	//! can be a remote task, so the ClusterTaskContext describes the
	//! offloaded task on the original node.
	//!
	//! The remoteTaskIdentifier is an opaque descriptor that identifies
	//! uniquely, to the user of the ClusterTaskContext object, the remote
	//! task on the remote node. It is opaque so that the user can define
	//! whatever makes sense as a descriptor on each case.
	class ClusterTaskContext {
		//! A descriptro that identifies the remote task at the remote
		//! node
		void *_remoteTaskIdentifier;

		//! The cluster node on which the remote task is located
		ClusterNode *_remoteNode;

		ClusterTaskCallback *_hook;

	public:
		//! \brief Create a Cluster Task context
		//!
		//! \param[in] remoteTaskIdentifier is an identifier of the task
		//!		on the remote node
		//! \param[in] remoteNode is the ClusterNode where the remote
		//!		task is located
		ClusterTaskContext(
			void *remoteTaskIdentifier = nullptr,
			ClusterNode *remoteNode = nullptr
		) : _remoteTaskIdentifier(remoteTaskIdentifier),
			_remoteNode(remoteNode),
			_hook(nullptr)
		{
		}

		~ClusterTaskContext()
		{
			// This asserts that the callback was already called. Previously the callback was called
			// here, but it was moved to TaskFinalization::disposeTask to implement the
			// task-finalization grouping optimization because it requires some extra conditions.
			assert(_hook == nullptr);
		}


		//! Call this before the destructor always because it is the function that sends (opr
		//! prepare to send) the finalization message.
		bool runHook()
		{
			if (_hook != nullptr) {
				_hook->execute();

				delete _hook;
				_hook = nullptr;
				return true;
			}
			return false;
		}

		//! \brief Get the remote task descriptor
		inline void *getRemoteIdentifier() const
		{
			return _remoteTaskIdentifier;
		}

		//! \brief Get the ClusterNode of the remote task
		inline ClusterNode *getRemoteNode() const
		{
			return _remoteNode;
		}

		inline void setCallback(SpawnFunction::function_t callback, MessageTaskNew *callbackArgs)
		{
			assert(callback != nullptr);
			assert(callbackArgs != nullptr);

			_hook = new ClusterTaskCallback(callback, callbackArgs);
			assert(_hook != nullptr);
		}

	};
}

#endif /* CLUSTER_TASK_CONTEXT_HPP */
