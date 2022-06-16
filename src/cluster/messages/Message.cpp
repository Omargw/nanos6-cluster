/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2018 Barcelona Supercomputing Center (BSC)
*/

#include "Message.hpp"
#include "MessageId.hpp"

#include <ClusterManager.hpp>
#include <ClusterNode.hpp>

Message::Message(MessageType type, size_t size)
	: TransferBase(nullptr), _deliverable((Deliverable *) calloc(1, sizeof(msg_header) + size))
{
	FatalErrorHandler::failIf(_deliverable == nullptr, "Could not allocate for creating message");

	_deliverable->header.type = type;
	_deliverable->header.size = size;
	/*! initialize the message id to 0 for now. In the
	 * future, it will probably be something related to
	 * the Task related with this message. */
	_deliverable->header.id = MessageId::nextMessageId();
	_deliverable->header.senderId = ClusterManager::getCurrentClusterNode()->getIndex();
}
