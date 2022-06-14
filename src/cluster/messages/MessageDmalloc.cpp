/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019 Barcelona Supercomputing Center (BSC)
*/

#include "MessageDmalloc.hpp"

#include <list>

#include <ClusterManager.hpp>
#include "ClusterMemoryManagement.hpp"
#include <DistributionPolicy.hpp>
#include <VirtualMemoryManagement.hpp>

MessageDmalloc::MessageDmalloc(const ClusterNode *from,
	const DataAccessRegion &region, size_t clusterSize,
	nanos6_data_distribution_t policy, size_t nrDim, const size_t *dimensions
)
	: Message(DMALLOC,
		2 * sizeof(size_t)
		+ sizeof(MessageDmallocDataInfo) + nrDim * sizeof(size_t),
		from)
{
	_content = reinterpret_cast<DmallocMessageContent *>(_deliverable->payload);
	_content->_ndmallocs = 1;
	_content->getOffsetPtr()[0] = 0;
	MessageDmallocDataInfo *ptr = _content->getData(0);

	new (ptr) MessageDmallocDataInfo(region, clusterSize, policy, nrDim, dimensions);
}

MessageDmalloc::MessageDmalloc(const ClusterNode *from,
	std::list<MessageDmalloc::MessageDmallocDataInfo *> &dmallocs
)
	: Message(DMALLOC,
		sizeof(size_t)
		+ dmallocs.size() * sizeof(size_t)
		+ ClusterMemoryManagement::getSerializedDmallocsSize(),
		from)
{
	_content = reinterpret_cast<DmallocMessageContent *>(_deliverable->payload);
	_content->_ndmallocs = dmallocs.size();

	size_t i = 0;
	size_t offset = 0;
	for (const MessageDmalloc::MessageDmallocDataInfo *it : dmallocs) {
		_content->getOffsetPtr()[i] = offset;
		MessageDmalloc::MessageDmallocDataInfo *data = _content->getData(i);

		new (data) MessageDmallocDataInfo(
			it->_region,
			it->_clusterSize,
			it->_policy,
			it->_nrDim,
			it->_dimensions
		);

		assert(it->getSize() == data->getSize());

		offset += it->getSize();
		++i;
	}
}

bool MessageDmalloc::handleMessage()
{
	if (ClusterManager::isMasterNode()) {
		assert(_content->_ndmallocs == 1);

		MessageDmallocDataInfo *data = this->getContent()->getData(0);

		assert(data->_region.getStartAddress() == nullptr);

		const size_t allocationSize = data->_region.getSize();

		void *dptr = VirtualMemoryManagement::allocDistrib(allocationSize);
		FatalErrorHandler::failIf(dptr == nullptr,
			"Master node couldn't allocate distributed memory with size: ", allocationSize);

		data->_region = DataAccessRegion(dptr, allocationSize);

		const ClusterNode *node = ClusterManager::getClusterNode(this->getSenderId());
		assert(node != nullptr);

		// We don't change the message so it will keep the original sender and won't be sent to it
		// by the ping-pong protection.
		ClusterManager::sendMessageToAll(this, true);

		// And send only address to the original sender.
		DataAccessRegion region(&dptr, sizeof(void *));
		ClusterManager::sendDataRaw(region, node->getMemoryNode(), this->getId(), true);
	}

	ClusterMemoryManagement::handleDmallocMessage(this, nullptr);

	return true;
}

static const bool __attribute__((unused))_registered_dmalloc =
	Message::RegisterMSGClass<MessageDmalloc>(DMALLOC);
