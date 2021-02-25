/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019-2020 Barcelona Supercomputing Center (BSC)
*/

#ifndef CLUSTER_SCHEDULER_HPP
#define CLUSTER_SCHEDULER_HPP

#include <string>

#include "SchedulerInterface.hpp"
#include "LocalScheduler.hpp"
#include "schedulers/cluster/ClusterLocalityScheduler.hpp"
#include "schedulers/cluster/ClusterRandomScheduler.hpp"
#include "system/RuntimeInfo.hpp"

#include <ClusterManager.hpp>

class ClusterScheduler {

public:
	static SchedulerInterface *generate(const std::string &name)
	{
		if (ClusterManager::inClusterMode()) {

			if (name == "random") {
				return new ClusterRandomScheduler();
			}

			if (name == "locality") {
				return new ClusterLocalityScheduler();
			}

			SchedulerInterface *ret = new ClusterLocalityScheduler();
			// This is the default.
			FatalErrorHandler::warn(
				"Unknown cluster scheduler:", name, ". Using default: ", ret->getName()
			);

			return ret;

		} else {
			return new LocalScheduler();
		}
	}

};

#endif // CLUSTER_SCHEDULER_HPP
