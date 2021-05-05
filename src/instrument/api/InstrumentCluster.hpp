/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2018-2019 Barcelona Supercomputing Center (BSC)
*/

#ifndef INSTRUMENT_CLUSTER_HPP
#define INSTRUMENT_CLUSTER_HPP

#include <InstrumentInstrumentationContext.hpp>
#include <InstrumentThreadInstrumentationContext.hpp>

class Message;

namespace Instrument {

	/* NOTE: this must match the order of the clusterEventType array */
	enum ClusterEventType {
		OffloadedTasksWaiting = 0,
		PendingDataTransfers,
		PendingDataTransferBytes,
		PendingDataTransfersIncoming,
		MaxClusterEventType
	};

	//! This function is called when initiating a Message sending
	//!
	//! \param[in] msg is the Message we are sending
	//! \param[in] receiverId is the index of the receiver node
	void clusterMessageInitSend(
		Message const *msg,
		int receiverId,
		InstrumentationContext const &context =
				ThreadInstrumentationContext::getCurrent()
	);

	//! This function is called when sending a Message has completed
	//!
	//! \param[in] msg is the Message which was sent
	void clusterMessageCompleteSend(
		Message const *msg,
		InstrumentationContext const &context =
				ThreadInstrumentationContext::getCurrent()
	);

	//! This function is called when sending raw data
	void clusterDataSend(
		void *address,
		size_t size,
		int dest,
		InstrumentationContext const &context =
				ThreadInstrumentationContext::getCurrent()
	);

	//! This function is called when we start handling a received Message
	//!
	//! \param[in] msg is the Message we received and currently handling
	//! \param[in] senderId is the index of the sender node
	void enterHandleReceivedMessage(
		Message const *msg,
		int senderId,
		InstrumentationContext const &context =
				ThreadInstrumentationContext::getCurrent()
	);

	//! This function is called after we finished handling a received Message
	//!
	//! \param[in] msg is the Message we finished handling
	void exitHandleReceivedMessage(
		Message const *msg,
		InstrumentationContext const &context =
				ThreadInstrumentationContext::getCurrent()
	);

	//! This function is called when sending raw data
	void clusterDataReceived(
		void *address,
		size_t size,
		int dest,
		InstrumentationContext const &context =
				ThreadInstrumentationContext::getCurrent()
	);

	//! \brief Indicates that the task has been offloaded to another node
	//! \param[in] taskId the task identifier for the offloaded task
	void taskIsOffloaded(
		task_id_t taskId,
	    InstrumentationContext const &context = ThreadInstrumentationContext::getCurrent()
	);

	void stateNodeNamespace(
		int state,
		InstrumentationContext const &context = ThreadInstrumentationContext::getCurrent()
	);

	void emitClusterEvent(ClusterEventType clusterEventType, int value, InstrumentationContext const & = ThreadInstrumentationContext::getCurrent());

	void offloadedTaskCompletes(task_id_t taskId, InstrumentationContext const &context = ThreadInstrumentationContext::getCurrent());
}

#endif //! INSTRUMENT_CLUSTER_HPP
